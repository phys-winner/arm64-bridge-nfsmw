#include "options.h"
#include "thunk_manager.h"
#include "dynarmic_host.h"
#include "thunk_types.h"
#include <GLES2/gl2.h>
#include <GLES3/gl31.h>
#include <EGL/egl.h>
#include <android/log.h>
#include <string>
#include <vector>
#include <cstring>
#include <memory>
#include <mutex>
#include <atomic>
#include <cstdlib>
#include <sys/system_properties.h>
#include <unordered_map>
#include <unordered_set>


// Mirror of the current buffer bindings. Querying the driver with glGetIntegerv inside
// glDrawElements/glVertexAttribPointer cost a round-trip on every draw call.
static thread_local GLuint s_bound_array_buffer = 0;
static thread_local GLuint s_bound_element_buffer = 0;
static thread_local GLuint s_bound_framebuffer = 0;
static thread_local std::unordered_map<GLenum, GLuint> s_bound_buffers;
static std::mutex s_fbfetch_mutex;
// Frame diagnostics (jni_bridge.cpp FrameDiag): draw calls and vertices/indices submitted by the guest.
extern "C" {
uint64_t g_bridge_draw_calls = 0;
uint64_t g_bridge_draw_vertices = 0;
}
static std::unordered_set<GLuint> s_fbfetch_shaders;
static int s_fbfetch_logged = 0;

// GU-09 anisotropic filtering (stage 8, NFS): the engine never sets anisotropy, so road markings and asphalt
// blur at grazing angles. Whenever the guest gives a texture a mipmapped minification filter, the bridge also
// sets GL_TEXTURE_MAX_ANISOTROPY_EXT on it (GL_EXT_texture_filter_anisotropic only) at
// min(level, GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT). level: setprop debug.nfsbridge.aniso N before launch
// (0 or 1 = off), default 8 at the MAX graphics quality and off below it (stage 13, portopts::PortEffects). Decided
// once, with the first current context.
static constexpr GLenum kTextureMaxAnisotropy = 0x84FE;     // GL_TEXTURE_MAX_ANISOTROPY_EXT
static constexpr GLenum kMaxTextureMaxAnisotropy = 0x84FF;  // GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
static std::atomic<float> s_anisotropy{-1.0f};

static float AnisotropyLevel() {
    const float cached = s_anisotropy.load(std::memory_order_relaxed);
    if (cached >= 0.0f) return cached;
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (!extensions) return 0.0f;  // no current context yet
    float level = portopts::PortEffects() ? 8.0f : 0.0f;
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.nfsbridge.aniso", value) > 0) level = strtof(value, nullptr);
    GLfloat max_level = 0.0f;
    if (!strstr(extensions, "GL_EXT_texture_filter_anisotropic")) {
        level = 0.0f;
    } else if (level > 1.0f) {
        glGetFloatv(kMaxTextureMaxAnisotropy, &max_level);
        if (max_level >= 1.0f && level > max_level) level = max_level;
    }
    if (!(level > 1.0f)) level = 0.0f;
    s_anisotropy.store(level, std::memory_order_relaxed);
    __android_log_print(ANDROID_LOG_INFO, "Arm32Bridge_GL", "anisotropic filtering %.0fx (driver max %.0f)", level, max_level);
    return level;
}


// DE-04/GU-06 MSAA (stage 8, NFS): the scene is drawn into an offscreen framebuffer (colour texture + depth
// renderbuffer at the surface size); the engine's own multisample path exists only for APPLE/EXT/ANGLE
// framebuffer_multisample (iOS/ANGLE). With GL_EXT_multisampled_render_to_texture the bridge makes that
// framebuffer multisampled: a surface-sized depth/stencil renderbuffer gets multisample storage and a
// surface-sized colour texture is attached with glFramebufferTexture2DMultisampleEXT. The tiler resolves into
// the texture when the framebuffer is flushed, so every later read of it (post effects, copies, blits) sees
// the resolved picture. Samples: setprop debug.nfsbridge.msaa N before launch (0 or 1 = off), default 4 at the MAX
// graphics quality and off below it (stage 13), clamped to GL_MAX_SAMPLES_EXT. A framebuffer that is not complete after the change is rebuilt
// single-sampled (logged). Everything else is untouched.
namespace {
using PfnRenderbufferStorageMultisample = void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
using PfnFramebufferTexture2DMultisample = void (*)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
constexpr GLenum kMaxSamplesExt = 0x8D57;  // GL_MAX_SAMPLES_EXT

struct MsaaState {
    std::mutex mutex;
    int samples = -1;  // -1: not decided (needs a current context)
    PfnRenderbufferStorageMultisample storage = nullptr;
    PfnFramebufferTexture2DMultisample attach = nullptr;
    std::unordered_map<GLuint, std::pair<GLsizei, GLsizei>> texture_size;  // GL_TEXTURE_2D level 0
    struct Renderbuffer { GLenum format; GLsizei width, height, samples; };
    std::unordered_map<GLuint, Renderbuffer> renderbuffers;
    struct Framebuffer { GLuint colour_texture = 0; GLuint depth = 0; GLuint stencil = 0; bool colour_ms = false; };
    std::unordered_map<GLuint, Framebuffer> framebuffers;
    int logged = 0;
    bool log_attachments = false;  // debug.nfsbridge.frametrace 1: every framebuffer texture attachment (first 64)
    int logged_attachments = 0;
};
MsaaState& Msaa() {
    static MsaaState state;
    return state;
}
thread_local GLuint t_active_unit = 0;
thread_local GLuint t_texture_2d[32] = {};
thread_local GLuint t_renderbuffer = 0;

// Caller holds the mutex.
int MsaaSamplesLocked(MsaaState& s) {
    if (s.samples >= 0) return s.samples;
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (!extensions) return 0;
    int samples = portopts::PortEffects() ? 4 : 0;
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.nfsbridge.msaa", value) > 0) samples = atoi(value);
    GLint max_samples = 0;
    if (samples > 1 && strstr(extensions, "GL_EXT_multisampled_render_to_texture")) {
        s.storage = reinterpret_cast<PfnRenderbufferStorageMultisample>(eglGetProcAddress("glRenderbufferStorageMultisampleEXT"));
        s.attach = reinterpret_cast<PfnFramebufferTexture2DMultisample>(eglGetProcAddress("glFramebufferTexture2DMultisampleEXT"));
        glGetIntegerv(kMaxSamplesExt, &max_samples);
        if (max_samples > 1 && samples > max_samples) samples = max_samples;
        if (!s.storage || !s.attach || max_samples < 2) samples = 0;
    } else {
        samples = 0;
    }
    if (samples < 2) samples = 0;
    s.samples = samples;
    s.log_attachments = __system_property_get("debug.nfsbridge.frametrace", value) > 0 && value[0] == '1';
    __android_log_print(ANDROID_LOG_INFO, "Arm32Bridge_GL", "MSAA %dx on surface-sized offscreen framebuffers (driver max %d)",
                        samples, max_samples);
    return samples;
}

bool IsSurfaceSize(GLsizei width, GLsizei height) {
    return width > 0 && height > 0 && uint32_t(width) == ThunkManager::s_screenWidth &&
           uint32_t(height) == ThunkManager::s_screenHeight;
}

bool IsDepthOrStencilFormat(GLenum format) {
    switch (format) {
        case GL_DEPTH_COMPONENT16: case 0x81A6 /* DEPTH_COMPONENT24 */: case 0x81A7 /* DEPTH_COMPONENT32_OES */:
        case GL_DEPTH_COMPONENT32F: case 0x88F0 /* DEPTH24_STENCIL8 */: case GL_DEPTH32F_STENCIL8: case GL_STENCIL_INDEX8:
            return true;
        default:
            return false;
    }
}

// The framebuffer bound to target has a multisampled attachment: verify it and fall back to single sampling.
void VerifyMultisampleFramebuffer(MsaaState& s, GLenum target, GLuint fbo) {
    const GLenum status = glCheckFramebufferStatus(target);
    if (status == GL_FRAMEBUFFER_COMPLETE) return;
    auto& f = s.framebuffers[fbo];
    GLsizei w = 0, h = 0;
    if (f.colour_ms && f.colour_texture) {
        glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f.colour_texture, 0);
        f.colour_ms = false;
    }
    for (GLuint rb : {f.depth, f.stencil}) {
        auto it = s.renderbuffers.find(rb);
        if (!rb || it == s.renderbuffers.end() || it->second.samples < 2) continue;
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, it->second.format, it->second.width, it->second.height);
        glBindRenderbuffer(GL_RENDERBUFFER, t_renderbuffer);
        it->second.samples = 0;
        w = it->second.width;
        h = it->second.height;
    }
    __android_log_print(ANDROID_LOG_WARN, "Arm32Bridge_GL", "MSAA framebuffer %u incomplete (0x%04X), rebuilt single-sampled %dx%d -> 0x%04X",
                        fbo, status, w, h, glCheckFramebufferStatus(target));
}
}  // namespace

void BridgeTrackActiveTexture(GLenum unit) { t_active_unit = (unit - GL_TEXTURE0) & 31u; }
void BridgeTrackBindTexture(GLenum target, GLuint texture) {
    if (target == GL_TEXTURE_2D) t_texture_2d[t_active_unit] = texture;
}
void BridgeTrackTextureSize(GLenum target, GLint level, GLsizei width, GLsizei height) {
    if (target != GL_TEXTURE_2D || level != 0) return;
    const GLuint texture = t_texture_2d[t_active_unit];
    if (!texture) return;
    auto& s = Msaa();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.texture_size[texture] = {width, height};
}

void BridgeApplyAnisotropy(GLenum target, GLenum pname, float value) {
    if (pname != GL_TEXTURE_MIN_FILTER) return;
    const GLint filter = static_cast<GLint>(value);
    if (filter < GL_NEAREST_MIPMAP_NEAREST || filter > GL_LINEAR_MIPMAP_LINEAR) return;
    const float level = AnisotropyLevel();
    if (level > 1.0f) glTexParameterf(target, kTextureMaxAnisotropy, level);
}

bool HasGlES3() {
    // eglQueryContext works in ES2 without introducing a GL_INVALID_ENUM.
    const EGLContext context = eglGetCurrentContext();
    if (context == EGL_NO_CONTEXT) return false;
    EGLint version = 0;
    return eglQueryContext(eglGetCurrentDisplay(), context, EGL_CONTEXT_CLIENT_VERSION, &version) && version >= 3;
}

void TrackGlBufferBinding(GLenum target, GLuint buffer) {
    s_bound_buffers[target] = buffer;
    if (target == GL_ARRAY_BUFFER) s_bound_array_buffer = buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) s_bound_element_buffer = buffer;
}

GLuint GetBoundGlBuffer(GLenum target) {
    if (target == GL_ARRAY_BUFFER) return s_bound_array_buffer;
    if (target == GL_ELEMENT_ARRAY_BUFFER) return s_bound_element_buffer;
    const auto it = s_bound_buffers.find(target);
    return it == s_bound_buffers.end() ? 0 : it->second;
}

void ForgetGlBufferBindings(GLsizei count, const GLuint* buffers) {
    if (count <= 0 || !buffers) return;
    for (GLsizei i = 0; i < count; ++i) {
        const GLuint deleted = buffers[i];
        if (!deleted) continue;
        if (s_bound_array_buffer == deleted) s_bound_array_buffer = 0;
        if (s_bound_element_buffer == deleted) s_bound_element_buffer = 0;
        for (auto& binding : s_bound_buffers) {
            if (binding.second == deleted) binding.second = 0;
        }
    }
}

void RefreshGlBindings() {
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        s_bound_array_buffer = s_bound_element_buffer = s_bound_framebuffer = 0;
        s_bound_buffers.clear();
        return;
    }
    GLint array = 0, elements = 0, framebuffer = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elements);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    s_bound_array_buffer = array;
    s_bound_element_buffer = elements;
    s_bound_framebuffer = framebuffer;
    s_bound_buffers.clear();
    if (HasGlES3()) {
        constexpr GLenum targets[] = {GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, GL_PIXEL_PACK_BUFFER,
            GL_PIXEL_UNPACK_BUFFER, GL_UNIFORM_BUFFER, GL_TRANSFORM_FEEDBACK_BUFFER};
        constexpr GLenum queries[] = {GL_COPY_READ_BUFFER_BINDING, GL_COPY_WRITE_BUFFER_BINDING, GL_PIXEL_PACK_BUFFER_BINDING,
            GL_PIXEL_UNPACK_BUFFER_BINDING, GL_UNIFORM_BUFFER_BINDING, GL_TRANSFORM_FEEDBACK_BUFFER_BINDING};
        for (size_t i=0;i<std::size(targets);++i) { GLint value=0;glGetIntegerv(queries[i],&value);s_bound_buffers[targets[i]]=value; }
    }
    s_bound_buffers[GL_ARRAY_BUFFER] = s_bound_array_buffer;
    s_bound_buffers[GL_ELEMENT_ARRAY_BUFFER] = s_bound_element_buffer;
}

bool ResolveGlPixels(DynarmicHost& host, int width, int height, int depth,
                     uint32_t format, uint32_t type, uint32_t address, const void** pixels, bool volume);
void ForgetGuestGlMappings(DynarmicHost& host, GLsizei count, const GLuint* buffers);

// UI scaling, the virtual-resolution way: tell the guest the screen is
// s_virtual{Width,Height} and stretch the result here. The guest sizes its viewport
// in those units while the real default framebuffer is still the surface size, so
// the window-system framebuffer -- and only that one -- gets mapped back. Offscreen
// targets are allocated in the guest's own units and must not be touched, which is
// what the s_bound_framebuffer check is for.
// Leave s_virtualWidth/Height at 0 to disable the whole path.
static void ScaleViewportToSurface(GLint& x, GLint& y, GLsizei& w, GLsizei& h) {
    if (s_bound_framebuffer != 0) return;
    const uint32_t vw = ThunkManager::s_virtualWidth, vh = ThunkManager::s_virtualHeight;
    const uint32_t rw = ThunkManager::s_screenWidth, rh = ThunkManager::s_screenHeight;
    if (!vw || !vh || !rw || !rh || (vw == rw && vh == rh)) return;
    x = static_cast<GLint>((float)x * rw / vw);
    w = static_cast<GLsizei>((float)w * rw / vw);
    y = static_cast<GLint>((float)y * rh / vh);
    h = static_cast<GLsizei>((float)h * rh / vh);
}
#define LOG_TAG "Arm32Bridge_GL"
#include "bridge_log.h"

// GLSL ES requires #extension directives before any non-preprocessor token; the engine's shader generator
// (written against Apple's lenient compiler) emits `#extension GL_EXT_shader_framebuffer_fetch : require` after
// its precision/uniform declarations (DE-03 colour ramp: Mali "P0001: Extension directive must occur before any
// non-preprocessor tokens"). Such directives are moved to the top (after #version); the original line is left
// empty. Returns false when the source needs no change.
static bool HoistLateExtensionDirectives(const std::string& source, std::string* out) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= source.size()) {
        const size_t end = source.find('\n', start);
        lines.push_back(source.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    auto trimmed = [](const std::string& line) {
        const size_t first = line.find_first_not_of(" \t\r");
        return first == std::string::npos ? std::string() : line.substr(first);
    };
    bool code_seen = false, late = false;
    for (const auto& line : lines) {
        const std::string t = trimmed(line);
        if (t.empty() || t.rfind("//", 0) == 0) continue;
        if (t[0] == '#') {
            if (t.rfind("#extension", 0) == 0 && code_seen) late = true;
            continue;
        }
        code_seen = true;
    }
    if (!late) return false;
    std::vector<std::string> directives;
    size_t version_line = std::string::npos;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string t = trimmed(lines[i]);
        if (t.rfind("#version", 0) == 0 && version_line == std::string::npos) version_line = i;
        if (t.rfind("#extension", 0) == 0) {
            directives.push_back(t);
            lines[i].clear();
        }
    }
    out->clear();
    size_t i = 0;
    if (version_line != std::string::npos) {
        for (; i <= version_line; ++i) *out += lines[i] + "\n";
    }
    for (const auto& d : directives) *out += d + "\n";
    for (; i < lines.size(); ++i) {
        *out += lines[i];
        if (i + 1 < lines.size()) *out += "\n";
    }
    return true;
}

void RegisterGles2CoreThunks(ThunkManager& manager);

void ThunkManager::RegisterGLThunks() {
    uint8_t* mem = m_host.GetGuestMemoryBase();

    struct GlStringCache {
        std::mutex mutex;
        std::unordered_map<std::string, uint32_t> values;
    };
    auto gl_strings = std::make_shared<GlStringCache>();

    RegisterThunk("glActiveTexture", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glActiveTexture(static_cast<GLenum>(jit.Regs()[0]));
        BridgeTrackActiveTexture(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glAttachShader", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glAttachShader(jit.Regs()[0], jit.Regs()[1]);
    });

    RegisterThunk("glBindBuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        const GLuint buffer = jit.Regs()[1];
        glBindBuffer(target, buffer);
        TrackGlBufferBinding(target, buffer);
    });

    RegisterThunk("glBindFramebuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        s_bound_framebuffer = jit.Regs()[1];
        glBindFramebuffer(static_cast<GLenum>(jit.Regs()[0]), jit.Regs()[1]);
    });

    RegisterThunk("glBindRenderbuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glBindRenderbuffer(static_cast<GLenum>(jit.Regs()[0]), jit.Regs()[1]);
        t_renderbuffer = jit.Regs()[1];
    });

    RegisterThunk("glBindTexture", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glBindTexture(static_cast<GLenum>(jit.Regs()[0]), jit.Regs()[1]);
        BridgeTrackBindTexture(static_cast<GLenum>(jit.Regs()[0]), jit.Regs()[1]);
    });

    RegisterThunk("glBlendFunc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glBlendFunc(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLenum>(jit.Regs()[1]));
    });

    RegisterThunk("glBufferData", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        GLsizeiptr size = static_cast<GLsizeiptr>(jit.Regs()[1]);
        const void* data = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]);
        GLenum usage = static_cast<GLenum>(jit.Regs()[3]);
        glBufferData(target, size, data, usage);
    });

    RegisterThunk("glCheckFramebufferStatus", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = glCheckFramebufferStatus(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glClear", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glClear(static_cast<GLbitfield>(jit.Regs()[0]));
    });

    RegisterThunk("glClearColor", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float r = *reinterpret_cast<float*>(&jit.Regs()[0]);
        float g = *reinterpret_cast<float*>(&jit.Regs()[1]);
        float b = *reinterpret_cast<float*>(&jit.Regs()[2]);
        float a = *reinterpret_cast<float*>(&jit.Regs()[3]);
        glClearColor(r, g, b, a);
    });

    RegisterThunk("glColorMask", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glColorMask(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], jit.Regs()[3]);
    });

    RegisterThunk("glCompileShader", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLuint shader = jit.Regs()[0];
        glCompileShader(shader);
        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            char log[1024];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            LOGE("Shader %u compile failed: %s", shader, log);
        }
        {
            std::lock_guard<std::mutex> lock(s_fbfetch_mutex);
            if (s_fbfetch_shaders.erase(shader) && s_fbfetch_logged < 8) {
                ++s_fbfetch_logged;
                __android_log_print(ANDROID_LOG_INFO, "Arm32Bridge_GL", "DE-03: framebuffer-fetch (colour ramp) shader %u compiled: %s",
                                    shader, compiled ? "ok" : "FAILED");
            }
        }
    });

    RegisterThunk("glCompressedTexImage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLenum target = jit.Regs()[0];
        GLint level = jit.Regs()[1];
        GLenum internalformat = jit.Regs()[2];
        GLsizei width = jit.Regs()[3];
        uint32_t sp = jit.Regs()[13];
        GLsizei height = host.MemoryRead32(sp);
        GLint border = host.MemoryRead32(sp + 4);
        GLsizei imageSize = host.MemoryRead32(sp + 8);
        uint32_t data_gaddr = host.MemoryRead32(sp + 12);
        GLint pbo = 0;
        if (HasGlES3()) glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
        const void* data = nullptr;
        if (imageSize < 0) return;
        if (pbo != 0) {
            data = reinterpret_cast<const void*>(static_cast<uintptr_t>(data_gaddr));
        } else if (imageSize != 0) {
            if (!GuestRangeValid(host.GetGuestMemorySize(), data_gaddr,
                                 static_cast<size_t>(imageSize))) return;
            data = GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                             data_gaddr, static_cast<size_t>(imageSize));
        }
        glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
    });

    RegisterThunk("glCreateProgram", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = glCreateProgram();
    });

    RegisterThunk("glCreateShader", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = glCreateShader(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glCullFace", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glCullFace(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glDeleteBuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = static_cast<GLsizei>(jit.Regs()[0]);
        const auto* buffers = reinterpret_cast<const GLuint*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        ForgetGuestGlMappings(host,count,buffers);
        glDeleteBuffers(count, buffers);
        ForgetGlBufferBindings(count, buffers);
    });

    RegisterThunk("glDeleteFramebuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = jit.Regs()[0];
        const auto* names = reinterpret_cast<const GLuint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        glDeleteFramebuffers(count, names);
        auto& s = Msaa();
        std::lock_guard<std::mutex> lock(s.mutex);
        for (GLsizei i = 0; names && i < count; ++i) s.framebuffers.erase(names[i]);
    });

    RegisterThunk("glDeleteProgram", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDeleteProgram(jit.Regs()[0]);
    });

    RegisterThunk("glDeleteRenderbuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = jit.Regs()[0];
        const auto* names = reinterpret_cast<const GLuint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        glDeleteRenderbuffers(count, names);
        auto& s = Msaa();
        std::lock_guard<std::mutex> lock(s.mutex);
        for (GLsizei i = 0; names && i < count; ++i) {
            s.renderbuffers.erase(names[i]);
            if (t_renderbuffer == names[i]) t_renderbuffer = 0;
        }
    });

    RegisterThunk("glDeleteShader", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDeleteShader(jit.Regs()[0]);
    });

    RegisterThunk("glDeleteTextures", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = jit.Regs()[0];
        const auto* names = reinterpret_cast<const GLuint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        glDeleteTextures(count, names);
        auto& s = Msaa();
        std::lock_guard<std::mutex> lock(s.mutex);
        for (GLsizei i = 0; names && i < count; ++i) {
            s.texture_size.erase(names[i]);
            for (auto& unit : t_texture_2d) if (unit == names[i]) unit = 0;
        }
    });

    RegisterThunk("glDepthFunc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDepthFunc(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glDepthMask", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDepthMask(static_cast<GLboolean>(jit.Regs()[0]));
    });

    RegisterThunk("glDisable", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDisable(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glDisableVertexAttribArray", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDisableVertexAttribArray(jit.Regs()[0]);
    });

    RegisterThunk("glDrawArrays", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        ++g_bridge_draw_calls;
        g_bridge_draw_vertices += jit.Regs()[2];
        glDrawArrays(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLint>(jit.Regs()[1]), static_cast<GLsizei>(jit.Regs()[2]));
    });

    RegisterThunk("glDrawElements", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t indices_gaddr = jit.Regs()[3];
        const void* indices = (s_bound_element_buffer != 0) ?
            reinterpret_cast<const void*>(static_cast<uintptr_t>(indices_gaddr)) :
            GuestToHost(host.GetGuestMemoryBase(), indices_gaddr);
        glDrawElements(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLsizei>(jit.Regs()[1]),
                       static_cast<GLenum>(jit.Regs()[2]), indices);
        ++g_bridge_draw_calls;
        g_bridge_draw_vertices += jit.Regs()[1];
    });

    RegisterThunk("glEnable", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glEnable(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glEnableVertexAttribArray", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glEnableVertexAttribArray(jit.Regs()[0]);
    });

    RegisterThunk("glFlush", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glFlush();
    });

    RegisterThunk("glFramebufferRenderbuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = jit.Regs()[0], attachment = jit.Regs()[1];
        const GLuint renderbuffer = jit.Regs()[3];
        glFramebufferRenderbuffer(target, attachment, jit.Regs()[2], renderbuffer);
        auto& s = Msaa();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.samples < 2 || !s_bound_framebuffer) return;
        auto& f = s.framebuffers[s_bound_framebuffer];
        if (attachment == GL_DEPTH_ATTACHMENT) f.depth = renderbuffer;
        else if (attachment == GL_STENCIL_ATTACHMENT) f.stencil = renderbuffer;
        else if (attachment == 0x821A /* DEPTH_STENCIL_ATTACHMENT */) f.depth = f.stencil = renderbuffer;
        const auto it = s.renderbuffers.find(renderbuffer);
        if (f.colour_ms || (it != s.renderbuffers.end() && it->second.samples > 1))
            VerifyMultisampleFramebuffer(s, target, s_bound_framebuffer);
    });

    RegisterThunk("glFramebufferTexture2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = jit.Regs()[0], attachment = jit.Regs()[1], textarget = jit.Regs()[2];
        const GLuint texture = jit.Regs()[3];
        const GLint level = host.MemoryRead32(jit.Regs()[13]);
        auto& s = Msaa();
        std::lock_guard<std::mutex> lock(s.mutex);
        const int samples = MsaaSamplesLocked(s);
        const auto size = s.texture_size.find(texture);
        const bool multisample = samples > 1 && s_bound_framebuffer && attachment == GL_COLOR_ATTACHMENT0 &&
                                 textarget == GL_TEXTURE_2D && level == 0 && size != s.texture_size.end() &&
                                 IsSurfaceSize(size->second.first, size->second.second);
        if (multisample) {
            s.attach(target, attachment, textarget, texture, level, samples);
            if (s.logged++ < 16)
                __android_log_print(ANDROID_LOG_INFO, "Arm32Bridge_GL", "MSAA: framebuffer %u colour texture %u %dx%d, %d samples",
                                    s_bound_framebuffer, texture, size->second.first, size->second.second, samples);
        } else {
            glFramebufferTexture2D(target, attachment, textarget, texture, level);
        }
        if (s.log_attachments && s.logged_attachments < 64) {
            ++s.logged_attachments;
            __android_log_print(ANDROID_LOG_INFO, "Arm32Bridge_GL", "framebuffer %u attachment 0x%04X texture %u target 0x%04X %dx%d%s",
                                s_bound_framebuffer, attachment, texture, textarget,
                                size != s.texture_size.end() ? size->second.first : -1,
                                size != s.texture_size.end() ? size->second.second : -1, multisample ? " (MSAA)" : "");
        }
        if (!s_bound_framebuffer || samples < 2) return;
        auto& f = s.framebuffers[s_bound_framebuffer];
        if (attachment == GL_COLOR_ATTACHMENT0) {
            f.colour_texture = texture;
            f.colour_ms = multisample;
        } else if (attachment == GL_DEPTH_ATTACHMENT || attachment == 0x821A /* DEPTH_STENCIL_ATTACHMENT */) {
            f.depth = 0;  // a (single-sampled) depth texture replaces any depth renderbuffer
        }
        const auto depth = s.renderbuffers.find(f.depth);
        if (multisample || (depth != s.renderbuffers.end() && depth->second.samples > 1))
            VerifyMultisampleFramebuffer(s, target, s_bound_framebuffer);
    });

    RegisterThunk("glFrontFace", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glFrontFace(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glGenBuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGenBuffers(jit.Regs()[0], reinterpret_cast<GLuint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1])));
    });

    RegisterThunk("glGenFramebuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGenFramebuffers(jit.Regs()[0], reinterpret_cast<GLuint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1])));
    });

    RegisterThunk("glGenRenderbuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGenRenderbuffers(jit.Regs()[0], reinterpret_cast<GLuint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1])));
    });

    RegisterThunk("glGenTextures", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGenTextures(jit.Regs()[0], reinterpret_cast<GLuint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1])));
    });

    RegisterThunk("glGenerateMipmap", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGenerateMipmap(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glGetActiveAttrib", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t sp = jit.Regs()[13];
        GLsizei* length = reinterpret_cast<GLsizei*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3]));
        GLint* size = reinterpret_cast<GLint*>(GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp)));
        GLenum* type = reinterpret_cast<GLenum*>(GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp + 4)));
        GLchar* name = reinterpret_cast<GLchar*>(GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp + 8)));
        glGetActiveAttrib(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], length, size, type, name);
    });

    RegisterThunk("glGetActiveUniform", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t sp = jit.Regs()[13];
        GLsizei* length = reinterpret_cast<GLsizei*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3]));
        GLint* size = reinterpret_cast<GLint*>(GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp)));
        GLenum* type = reinterpret_cast<GLenum*>(GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp + 4)));
        GLchar* name = reinterpret_cast<GLchar*>(GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp + 8)));
        glGetActiveUniform(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], length, size, type, name);
    });

    RegisterThunk("glGetAttribLocation", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = glGetAttribLocation(jit.Regs()[0], reinterpret_cast<const GLchar*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1])));
    });

    RegisterThunk("glGetError", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = glGetError();
    });

    RegisterThunk("glGetIntegerv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGetIntegerv(static_cast<GLenum>(jit.Regs()[0]),
                      reinterpret_cast<GLint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1])));
    });

    RegisterThunk("glGetProgramInfoLog", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLsizei* length = jit.Regs()[2] ?
            reinterpret_cast<GLsizei*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2])) : nullptr;
        glGetProgramInfoLog(jit.Regs()[0], jit.Regs()[1], length,
                           reinterpret_cast<GLchar*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3])));
    });

    RegisterThunk("glGetProgramiv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGetProgramiv(jit.Regs()[0], static_cast<GLenum>(jit.Regs()[1]),
                       reinterpret_cast<GLint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2])));
    });

    RegisterThunk("glGetRenderbufferParameteriv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGetRenderbufferParameteriv(jit.Regs()[0], jit.Regs()[1],
                                     reinterpret_cast<GLint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2])));
    });

    RegisterThunk("glGetShaderInfoLog", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLsizei* length = jit.Regs()[2] ?
            reinterpret_cast<GLsizei*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2])) : nullptr;
        glGetShaderInfoLog(jit.Regs()[0], jit.Regs()[1], length,
                          reinterpret_cast<GLchar*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3])));
    });

    RegisterThunk("glGetShaderiv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGetShaderiv(jit.Regs()[0], static_cast<GLenum>(jit.Regs()[1]),
                      reinterpret_cast<GLint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2])));
    });

    // Query the current context every time, then cache only successfully copied
    // strings. Caching a pre-MakeCurrent NULL by enum permanently made a later
    // valid context look shader-incompatible and also leaked state across hosts.
    RegisterThunk("glGetString", [gl_strings](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum name = static_cast<GLenum>(jit.Regs()[0]);
        const GLubyte* value = glGetString(name);
        if (!value) {
            jit.Regs()[0] = 0;
            return;
        }

        const std::string text(reinterpret_cast<const char*>(value));
        std::lock_guard<std::mutex> lock(gl_strings->mutex);
        auto found = gl_strings->values.find(text);
        if (found == gl_strings->values.end()) {
            const uint32_t guest_addr = host.GuestAlloc(text.size() + 1, 1);
            if (!guest_addr) {
                jit.Regs()[0] = 0;
                return;
            }
            memcpy(host.GetGuestMemoryBase() + guest_addr, text.c_str(), text.size() + 1);
            found = gl_strings->values.emplace(text, guest_addr).first;
        }
        jit.Regs()[0] = found->second;
    });

    RegisterThunk("glGetUniformLocation", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = glGetUniformLocation(jit.Regs()[0], reinterpret_cast<const GLchar*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1])));
    });

    RegisterThunk("glHint", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glHint(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLenum>(jit.Regs()[1]));
    });

    RegisterThunk("glLineWidth", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glLineWidth(*reinterpret_cast<float*>(&jit.Regs()[0]));
    });

    RegisterThunk("glLinkProgram", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLuint prog = jit.Regs()[0];
        glLinkProgram(prog);
        GLint linked = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[1024];
            glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
            LOGE("Program %u link failed: %s", prog, log);
        }
    });

    RegisterThunk("glPixelStorei", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glPixelStorei(static_cast<GLenum>(jit.Regs()[0]), jit.Regs()[1]);
    });

    RegisterThunk("glPolygonOffset", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glPolygonOffset(*reinterpret_cast<float*>(&jit.Regs()[0]), *reinterpret_cast<float*>(&jit.Regs()[1]));
    });

    RegisterThunk("glReadPixels", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t sp = jit.Regs()[13];
        GLenum format = host.MemoryRead32(sp);
        GLenum type = host.MemoryRead32(sp + 4);
        void* pixels = GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp + 8));
        glReadPixels(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], jit.Regs()[3], format, type, pixels);
    });

    RegisterThunk("glRenderbufferStorage", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = jit.Regs()[0], format = jit.Regs()[1];
        const GLsizei width = jit.Regs()[2], height = jit.Regs()[3];
        auto& s = Msaa();
        std::lock_guard<std::mutex> lock(s.mutex);
        const int samples = MsaaSamplesLocked(s);
        const bool multisample = samples > 1 && target == GL_RENDERBUFFER && t_renderbuffer &&
                                 IsDepthOrStencilFormat(format) && IsSurfaceSize(width, height);
        if (multisample) {
            s.storage(target, samples, format, width, height);
            if (s.logged++ < 16)
                __android_log_print(ANDROID_LOG_INFO, "Arm32Bridge_GL", "MSAA: renderbuffer %u format 0x%04X %dx%d, %d samples",
                                    t_renderbuffer, format, width, height, samples);
        } else {
            glRenderbufferStorage(target, format, width, height);
        }
        if (t_renderbuffer) s.renderbuffers[t_renderbuffer] = {format, width, height, multisample ? samples : 0};
    });

    RegisterThunk("glScissor", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLint x = jit.Regs()[0], y = jit.Regs()[1];
        GLsizei w = jit.Regs()[2], h = jit.Regs()[3];
        ScaleViewportToSurface(x, y, w, h);
        glScissor(x, y, w, h);
    });

    RegisterThunk("glShaderSource", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLuint shader = jit.Regs()[0];
        GLsizei count = jit.Regs()[1];
        uint32_t str_array_gaddr = jit.Regs()[2];
        uint32_t len_array_gaddr = jit.Regs()[3];

        std::vector<const char*> strings(count);
        for (GLsizei i = 0; i < count; ++i) {
            uint32_t str_gaddr = host.MemoryRead32(str_array_gaddr + i * 4);
            strings[i] = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), str_gaddr));
        }

        const GLint* lengths = len_array_gaddr ?
            reinterpret_cast<const GLint*>(GuestToHost(host.GetGuestMemoryBase(), len_array_gaddr)) : nullptr;

        std::string joined;
        bool has_extension = false;
        for (GLsizei i = 0; i < count; ++i) {
            if (!strings[i]) continue;
            const size_t n = lengths && lengths[i] >= 0 ? size_t(lengths[i]) : strlen(strings[i]);
            joined.append(strings[i], n);
            if (!has_extension && joined.find("#extension") != std::string::npos) has_extension = true;
        }
        std::string hoisted;
        if (has_extension && HoistLateExtensionDirectives(joined, &hoisted)) {
            const char* source = hoisted.c_str();
            glShaderSource(shader, 1, &source, nullptr);
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 4)
                __android_log_print(ANDROID_LOG_INFO, "Arm32Bridge_GL", "shader %u: late #extension directives moved to the top", shader);
        } else {
            glShaderSource(shader, count, strings.data(), lengths);
        }
        // DE-03 evidence: the colour-ramp material is the only framebuffer-fetch shader of the engine.
        for (GLsizei i = 0; i < count; ++i) {
            if (strings[i] && strstr(strings[i], "gl_LastFragData")) {
                std::lock_guard<std::mutex> lock(s_fbfetch_mutex);
                s_fbfetch_shaders.insert(shader);
                break;
            }
        }
    });

    RegisterThunk("glTexImage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLenum target = jit.Regs()[0];
        GLint level = jit.Regs()[1];
        GLint internalformat = jit.Regs()[2];
        GLsizei width = jit.Regs()[3];
        uint32_t sp = jit.Regs()[13];
        GLsizei height = host.MemoryRead32(sp);
        GLint border = host.MemoryRead32(sp + 4);
        GLenum format = host.MemoryRead32(sp + 8);
        GLenum type = host.MemoryRead32(sp + 12);
        uint32_t pixels_gaddr = host.MemoryRead32(sp + 16);
        GLint pbo = 0;
        if (HasGlES3()) glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
        const void* pixels = nullptr;
        if ((pbo != 0 || pixels_gaddr != 0) &&
            !ResolveGlPixels(host, width, height, 1, format, type, pixels_gaddr, &pixels, false)) return;
        glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
        BridgeTrackTextureSize(target, level, width, height);
    });

    RegisterThunk("glTexSubImage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLint level = static_cast<GLint>(args.I32());
        const GLint xoffset = static_cast<GLint>(args.I32());
        const GLint yoffset = static_cast<GLint>(args.I32());
        const GLsizei width = static_cast<GLsizei>(args.I32());
        const GLsizei height = static_cast<GLsizei>(args.I32());
        const GLenum format = static_cast<GLenum>(args.U32());
        const GLenum type = static_cast<GLenum>(args.U32());
        const uint32_t pixels_addr = args.Ptr();
        const void* pixels = nullptr;
        if (!ResolveGlPixels(host, width, height, 1, format, type, pixels_addr, &pixels, false)) return;
        glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
    });

    RegisterThunk("glTexParameterf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const float value = *reinterpret_cast<float*>(&jit.Regs()[2]);
        glTexParameterf(jit.Regs()[0], jit.Regs()[1], value);
        BridgeApplyAnisotropy(jit.Regs()[0], jit.Regs()[1], value);
    });

    RegisterThunk("glTexParameteri", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glTexParameteri(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2]);
        BridgeApplyAnisotropy(jit.Regs()[0], jit.Regs()[1], static_cast<float>(static_cast<GLint>(jit.Regs()[2])));
    });

    RegisterThunk("glUniform1f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniform1f(jit.Regs()[0], *reinterpret_cast<float*>(&jit.Regs()[1]));
    });

    RegisterThunk("glUniform1i", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniform1i(jit.Regs()[0], jit.Regs()[1]);
    });

    RegisterThunk("glUniform2f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniform2f(jit.Regs()[0], *reinterpret_cast<float*>(&jit.Regs()[1]), *reinterpret_cast<float*>(&jit.Regs()[2]));
    });

    RegisterThunk("glUniform3f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniform3f(jit.Regs()[0], *reinterpret_cast<float*>(&jit.Regs()[1]),
                    *reinterpret_cast<float*>(&jit.Regs()[2]), *reinterpret_cast<float*>(&jit.Regs()[3]));
    });

    RegisterThunk("glUniform3fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniform3fv(jit.Regs()[0], jit.Regs()[1],
                     reinterpret_cast<const GLfloat*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2])));
    });

    RegisterThunk("glUniform4f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t sp = jit.Regs()[13];
        uint32_t w_bits = host.MemoryRead32(sp);
        float w = *reinterpret_cast<float*>(&w_bits);
        glUniform4f(jit.Regs()[0], *reinterpret_cast<float*>(&jit.Regs()[1]),
                    *reinterpret_cast<float*>(&jit.Regs()[2]), *reinterpret_cast<float*>(&jit.Regs()[3]), w);
    });

    RegisterThunk("glUniform4fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniform4fv(jit.Regs()[0], jit.Regs()[1],
                     reinterpret_cast<const GLfloat*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2])));
    });

    RegisterThunk("glUniformMatrix3fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniformMatrix3fv(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2],
                           reinterpret_cast<const GLfloat*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3])));
    });

    RegisterThunk("glUniformMatrix4fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniformMatrix4fv(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2],
                           reinterpret_cast<const GLfloat*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3])));
    });

    RegisterThunk("glUseProgram", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUseProgram(jit.Regs()[0]);
    });

    RegisterThunk("glVertexAttribPointer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLuint index = jit.Regs()[0];
        GLint size = jit.Regs()[1];
        GLenum type = jit.Regs()[2];
        GLboolean normalized = jit.Regs()[3];
        uint32_t sp = jit.Regs()[13];
        GLsizei stride = host.MemoryRead32(sp);
        uint32_t ptr_gaddr = host.MemoryRead32(sp + 4);

        const void* ptr = (s_bound_array_buffer != 0) ?
            reinterpret_cast<const void*>(static_cast<uintptr_t>(ptr_gaddr)) :
            GuestToHost(host.GetGuestMemoryBase(), ptr_gaddr);

        glVertexAttribPointer(index, size, type, normalized, stride, ptr);
    });

    RegisterThunk("glViewport", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLint x = jit.Regs()[0], y = jit.Regs()[1];
        GLsizei w = jit.Regs()[2], h = jit.Regs()[3];
        ScaleViewportToSurface(x, y, w, h);
        glViewport(x, y, w, h);
    });

    RegisterGles2CoreThunks(*this);
}
