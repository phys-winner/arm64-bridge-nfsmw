// Need for Speed Most Wanted (2012) 1.3.128 -- arm64 host for the ARM32 game.
//
// The Java layer is unchanged except for com.ea.ironmonkey.port.NfsNative: on a
// 64-bit process it loads only this library (lib/arm64-v8a/libapp.so, or libapp_legacy.so
// built for Android 7-9 and translated processes) and calls nativeBootGuest(AssetManager,
// translated) where the ARM32 build loads its five libraries.
// Boot maps the ARM32 guests straight out of the APK (assets/guest_libs/, STORED:
// AAsset fd + dup + mmap, never extracted), relocates them in one Dynarmic guest
// address space and runs their constructors and JNI_OnLoads in the ARM32 order.
// Every Java native then forwards to the guest's exported Java_* entry point with
// guest JNI references; NfsVfs is served by the host (thunks_libc.cpp).
#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/system_properties.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "dynarmic_host.h"
#include "elf_loader.h"
#include "framerate.h"
#include "graphics.h"
#include "jni_fake_env.h"
#include "log_gate.h"
#include "options.h"
#include "portstrings.h"
#include "prof.h"
#include "thunk_manager.h"

#define LOG_TAG "Arm32Bridge_JNI"
#include "bridge_log.h"

int BridgeNfsVfsInstall(const char* path, int fd, int64_t offset, int64_t length);
void BridgeNfsVfsStats(int64_t out[3]);

namespace cfg {
// Guest modules in ARM32 load (= constructor) order: GameActivityMain.onCreate loads
// fmodex, fmodevent, c++_shared, Nimble, app -- the game's own libraries only. The port's
// libnfsport halves are host code here: NfsVfs (thunks_libc.cpp) and the QA actions (namespace qa).
// Disjoint reservations below the guest heap (0x50000000) cover each PT_LOAD memsz.
struct Module {
    const char* name;
    uint32_t base;
    uint32_t limit;
};
constexpr Module kModules[] = {
    {"libfmodex.so", 0x40C00000, 0x40E00000},
    {"libfmodevent.so", 0x40E00000, 0x40F00000},
    {"libc++_shared.so", 0x40F00000, 0x41000000},
    {"libNimble.so", 0x41000000, 0x41010000},
    {"libapp.so", 0x40000000, 0x40C00000},
};
constexpr const char* kGuestLibDir = "guest_libs/";
// Guest JNI_OnLoad order of the ARM32 process (libfmodex/libfmodevent export none).
constexpr const char* kJniOnLoadModules[] = {"libNimble.so", "libapp.so"};
constexpr const char* kActivityClass = "com/ea/ironmonkey/GameActivityMain";
}  // namespace cfg

namespace {
JavaVM* g_jvm = nullptr;
std::unique_ptr<DynarmicHost> g_host;
std::mutex g_boot_mutex;
jobject g_assets_ref = nullptr;    // the Activity's AssetManager, kept for the process (N_BootGuest)
AAssetManager* g_assets = nullptr;
}  // namespace

JavaVM* GetHostJavaVM() { return g_jvm; }
// Guest boot keeps the AssetManager: JIT profile and OBB overlay assets (thunks_libc.cpp NfsVfs).
AAssetManager* BridgeAssetManager() { return g_assets; }

namespace {

// ---------------------------------------------------------------------------
// Guest ELF images: read-only mappings of STORED APK assets.
// ---------------------------------------------------------------------------
struct GuestElfMapping {
    std::string path;
    int fd = -1;
    void* mapped_base = MAP_FAILED;
    size_t mapped_span = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;

    GuestElfMapping() = default;
    GuestElfMapping(const GuestElfMapping&) = delete;
    GuestElfMapping& operator=(const GuestElfMapping&) = delete;
    ~GuestElfMapping() {
        if (mapped_base != MAP_FAILED && ::munmap(mapped_base, mapped_span) != 0)
            LOGE("guest munmap failed: asset=assets/%s errno=%d", path.c_str(), errno);
        if (fd >= 0) close(fd);
    }

    bool Open(AAssetManager* manager, const std::string& relative, uint32_t guest_base) {
        path = relative;
        AAsset* asset = AAssetManager_open(manager, relative.c_str(), AASSET_MODE_RANDOM);
        if (!asset) {
            LOGE("guest asset open failed: assets/%s", relative.c_str());
            return false;
        }
        off64_t offset = 0, length = 0;
        const int source = AAsset_openFileDescriptor64(asset, &offset, &length);
        if (source < 0) {
            LOGE("guest asset is not direct-mappable (compressed or unavailable): assets/%s", relative.c_str());
            AAsset_close(asset);
            return false;
        }
        fd = fcntl(source, F_DUPFD_CLOEXEC, 3);
        const int dup_error = errno;
        // AAsset_openFileDescriptor64 hands the caller a descriptor it must close.
        close(source);
        AAsset_close(asset);
        if (fd < 0) {
            LOGE("guest asset dup failed: assets/%s errno=%d", relative.c_str(), dup_error);
            return false;
        }
        struct stat apk{};
        if (fstat(fd, &apk) != 0 || offset < 0 || length <= 0 || offset > apk.st_size ||
            length > apk.st_size - offset || static_cast<uint64_t>(length) > std::numeric_limits<size_t>::max()) {
            LOGE("guest asset fstat/range failed: assets/%s offset=%lld size=%lld apk_size=%lld",
                 relative.c_str(), static_cast<long long>(offset), static_cast<long long>(length),
                 static_cast<long long>(apk.st_size));
            return false;
        }
        const long page = sysconf(_SC_PAGESIZE);
        if (page <= 0 || (page & (page - 1)) != 0) {
            LOGE("guest asset mmap has invalid page size: %ld", page);
            return false;
        }
        const off64_t aligned = offset - (offset % page);
        const size_t bias = static_cast<size_t>(offset - aligned);
        mapped_span = (bias + static_cast<size_t>(length) + static_cast<size_t>(page) - 1) & ~(static_cast<size_t>(page) - 1);
        mapped_base = ::mmap(nullptr, mapped_span, PROT_READ, MAP_PRIVATE, fd, aligned);
        if (mapped_base == MAP_FAILED) {
            LOGE("guest asset mmap failed: assets/%s fd=%d offset=%lld span=%zu errno=%d",
                 relative.c_str(), fd, static_cast<long long>(aligned), mapped_span, errno);
            mapped_span = 0;
            return false;
        }
        data = static_cast<const uint8_t*>(mapped_base) + bias;
        size = static_cast<size_t>(length);
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                            "guest mmap: asset=assets/%s fd=%d offset=%lld size=%lld fstat=%lld -> guest 0x%08X",
                            relative.c_str(), fd, static_cast<long long>(offset), static_cast<long long>(length),
                            static_cast<long long>(apk.st_size), guest_base);
        return true;
    }
};

// ---------------------------------------------------------------------------
// Forwarded natives: Java method -> guest Java_* export in a named module.
// ---------------------------------------------------------------------------
enum Native : unsigned {
    kOnCreate, kOnDestroy, kOnMusicPlayerStateChanged, kOnOrientationChange, kOnPause,
    kOnPhysicalKeyDown, kOnPhysicalKeyUp, kOnPhysicalNavigationVisibilityChanged, kOnRestart,
    kOnResume, kOnStart, kOnStop, kRestoreContext, kSurfaceChanged, kSurfaceCreated,
    kTouchPadEvent, kTouchScreenEvent, kRunLoopTick,
    kMogaKeyEvent, kMogaMotionEvent, kMogaStateEvent,
    kEaioShutdown, kEaioStartup, kStorageShutdown, kStorageStartup,
    kNimbleCallback, kNimbleFinalize,
    kFmodGetInfo, kFmodProcess,
    kNativeCount
};

struct GuestEntry {
    const char* module;
    const char* symbol;
};

constexpr GuestEntry kGuestEntries[kNativeCount] = {
    {"libapp.so", "Java_com_ea_ironmonkey_GameActivityMain_nativeOnCreate"},
    {"libapp.so", "Java_com_ea_ironmonkey_GameActivityMain_nativeOnDestroy"},
    {"libapp.so", "Java_com_ea_ironmonkey_GameActivityMain_nativeOnMusicPlayerStateChanged"},
    {"libapp.so", "Java_com_ea_ironmonkey_GameActivityMain_nativeOnOrientationChange"},
    {"libapp.so", "Java_com_ea_ironmonkey_GameActivityMain_nativeOnPause"},
    {"libapp.so", "Java_com_ea_ironmonkey_GameActivityMain_nativeOnPhysicalKeyDown"},
    {"libapp.so", "Java_com_ea_ironmonkey_GameActivityMain_nativeOnPhysicalKeyUp"},
    {"libapp.so", "Java_com_ea_ironmonkey_GameActivityMain_nativeOnPhysicalNavigationVisibilityChanged"},
    {"libapp.so", "Java_com_ea_ironmonkey_GameActivityMain_nativeOnRestart"},
    {"libapp.so", "Java_com_ea_ironmonkey_GameActivityMain_nativeOnResume"},
    {"libapp.so", "Java_com_ea_ironmonkey_GameActivityMain_nativeOnStart"},
    {"libapp.so", "Java_com_ea_ironmonkey_GameActivityMain_nativeOnStop"},
    {"libapp.so", "Java_com_ea_ironmonkey_GameActivityMain_nativeRestoreContext"},
    {"libapp.so", "Java_com_ea_ironmonkey_GameActivityMain_nativeSurfaceChanged"},
    {"libapp.so", "Java_com_ea_ironmonkey_GameActivityMain_nativeSurfaceCreated"},
    {"libapp.so", "Java_com_ea_ironmonkey_GameGLSurfaceView_nativeTouchPadEvent"},
    {"libapp.so", "Java_com_ea_ironmonkey_GameGLSurfaceView_nativeTouchScreenEvent"},
    {"libapp.so", "Java_com_ea_ironmonkey_RunLoop_nativeOnRunLoopTick"},
    {"libapp.so", "Java_com_ea_ironmonkey_MogaController_nativeOnKeyEvent"},
    {"libapp.so", "Java_com_ea_ironmonkey_MogaController_nativeOnMotionEvent"},
    {"libapp.so", "Java_com_ea_ironmonkey_MogaController_nativeOnStateEvent"},
    {"libapp.so", "Java_com_ea_EAIO_EAIO_Shutdown"},
    {"libapp.so", "Java_com_ea_EAIO_EAIO_StartupNativeImpl"},
    {"libapp.so", "Java_com_ea_EAMIO_StorageDirectory_ShutdownNativeImpl"},
    {"libapp.so", "Java_com_ea_EAMIO_StorageDirectory_StartupNativeImpl"},
    {"libapp.so", "Java_com_ea_nimble_bridge_BaseNativeCallback_nativeCallback"},
    {"libapp.so", "Java_com_ea_nimble_bridge_BaseNativeCallback_nativeFinalize"},
    {"libfmodex.so", "Java_org_fmod_FMODAudioDevice_fmodGetInfo"},
    {"libfmodex.so", "Java_org_fmod_FMODAudioDevice_fmodProcess"},
};

uint32_t g_entry[kNativeCount] = {};

[[noreturn]] void GuestFailed(const char* where) {
    logs::Abort(LOG_TAG, "ARM32 guest failed in %s; the bridge cannot continue", where);
}

// One Java -> guest transition: a guest local-reference frame, the guest JNIEnv*,
// the receiver (jobject or jclass) and the remaining ARM32 argument words in order.
class GuestCall {
public:
    GuestCall(JNIEnv* env, jobject receiver)
        : m_valid(g_host && !g_host->HasFailed() && m_jni.BeginNativeCall(env)) {
        if (m_valid) m_args = {m_jni.EnvTableGuestAddr(), m_jni.AddObjectRef(receiver)};
    }
    ~GuestCall() {
        if (m_valid) m_jni.EndNativeCall();
    }
    GuestCall(const GuestCall&) = delete;
    GuestCall& operator=(const GuestCall&) = delete;

    GuestCall& Int(jint value) { m_args.push_back(static_cast<uint32_t>(value)); return *this; }
    GuestCall& Bool(jboolean value) { m_args.push_back(value ? 1u : 0u); return *this; }
    // Soft-float guest (no Tag_ABI_VFP_args): float arguments travel as core-register words.
    GuestCall& Float(jfloat value) {
        uint32_t bits;
        memcpy(&bits, &value, sizeof(bits));
        m_args.push_back(bits);
        return *this;
    }
    GuestCall& Object(jobject value) { m_args.push_back(m_valid && value ? m_jni.AddObjectRef(value) : 0); return *this; }

    uint32_t Run(Native id) {
        if (!m_valid || !g_entry[id]) {
            LOGE("guest native unavailable: %s", kGuestEntries[id].symbol);
            return 0;
        }
        const uint32_t result = g_host->RunFunction(g_entry[id], m_args);
        if (g_host->HasFailed() || m_jni.HasFailed()) GuestFailed(kGuestEntries[id].symbol);
        return result;
    }

private:
    FakeJniEnv& m_jni = FakeJniEnv::Instance();
    bool m_valid;
    std::vector<uint32_t> m_args;
};

void Void(JNIEnv* env, jobject self, Native id) { GuestCall(env, self).Run(id); }

// ---------------------------------------------------------------------------
// Dynarmic translation hooks (patches/dynarmic_bridge.patch).
// ---------------------------------------------------------------------------
extern "C" void DynarmicBridgeCompileStats(uint64_t* blocks, uint64_t* ns, uint64_t* clears);
extern "C" uint64_t DynarmicBridgeCodeCacheRemaining();
extern "C" void DynarmicBridgeSetBlockRecorder(void (*recorder)(uint64_t descriptor));

uint64_t MonotonicNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000u + uint64_t(ts.tv_nsec);
}

// ---------------------------------------------------------------------------
// JIT warm-up (stage 8). A block the GL thread translates for the first time in the middle of a race
// (first collision, crash camera, race-end cutscene) is a visible hitch. assets/guest_libs/libapp.jitprofile
// lists the IR location descriptors the GL thread translated over recorded play sessions; they are
// translated into the GL thread's depth-0 JIT a slice at a time before each RunLoop tick (2 ms per tick by
// default, setprop debug.nfsbridge.jitwarmup_ms N), during the loading screens and menus, so neither the
// first frame nor the engine's own thread timing changes (a one-shot 5.5 s warm-up at onSurfaceCreated held
// the UI thread in GLSurfaceView.surfaceRedrawNeeded and once exposed an engine loader/GL race on
// bridge_target). Translation reads the guest code as it is, so a stale entry costs cache space, never
// correctness; warm-up stops while 4 MiB of code cache are still free.
// Recording (setprop debug.nfsbridge.jitprofile record before launch): every block the GL thread has to
// translate that the shipped profile does not list is appended, and profile + new blocks are written to
// <external files>/nfs_jitprofile.bin every 10 s. setprop debug.nfsbridge.jitprofile off skips the warm-up.
// ---------------------------------------------------------------------------
constexpr char kJitProfileAsset[] = "guest_libs/libapp.jitprofile";
constexpr char kJitProfileMagic[8] = {'N', 'F', 'S', 'J', 'I', 'T', 'P', '1'};
constexpr char kJitRecordPath[] = "/storage/emulated/0/Android/data/com.ea.games.nfs13_row/files/nfs_jitprofile.bin";
constexpr size_t kJitWarmupReserve = 4u << 20;

struct JitProfile {
    std::vector<uint64_t> order;
    std::unordered_set<uint64_t> seen;
    size_t written = 0;
    uint64_t last_write = 0;
    bool recording = false;

    void Add(uint64_t descriptor) {
        if (seen.insert(descriptor).second) order.push_back(descriptor);
    }
};
JitProfile g_jit_profile;  // GL thread only

void RecordBlock(uint64_t descriptor) { g_jit_profile.Add(descriptor); }

bool LoadJitProfile(std::vector<uint64_t>* out) {
    if (!g_assets) return false;
    AAsset* asset = AAssetManager_open(g_assets, kJitProfileAsset, AASSET_MODE_BUFFER);
    if (!asset) return false;
    const auto* data = static_cast<const uint8_t*>(AAsset_getBuffer(asset));
    const size_t size = static_cast<size_t>(AAsset_getLength(asset));
    uint32_t count = 0;
    bool ok = data && size >= 12 && memcmp(data, kJitProfileMagic, 8) == 0;
    if (ok) {
        memcpy(&count, data + 8, 4);
        ok = size == 12 + size_t(count) * 8;
    }
    if (ok) {
        out->resize(count);
        memcpy(out->data(), data + 12, size_t(count) * 8);
    } else {
        LOGE("JIT profile %s ignored: bad header or size %zu", kJitProfileAsset, size);
    }
    AAsset_close(asset);
    return ok;
}

void WriteJitProfile() {
    FILE* f = fopen(kJitRecordPath, "wb");
    if (!f) {
        LOGE("JIT profile: cannot write %s: %s", kJitRecordPath, strerror(errno));
        g_jit_profile.recording = false;
        DynarmicBridgeSetBlockRecorder(nullptr);
        return;
    }
    const uint32_t count = static_cast<uint32_t>(g_jit_profile.order.size());
    fwrite(kJitProfileMagic, 1, 8, f);
    fwrite(&count, 4, 1, f);
    fwrite(g_jit_profile.order.data(), 8, count, f);
    fclose(f);
    g_jit_profile.written = count;
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "JIT profile: %u blocks written to %s", count, kJitRecordPath);
}

struct JitWarmup {
    std::vector<uint64_t> blocks;
    size_t cursor = 0;
    uint64_t budget_ns = 2000000;
    uint64_t spent_ns = 0;
    uint64_t started = 0;
    uint32_t ticks = 0;
    bool active = false;
};
JitWarmup g_warmup;  // GL thread only

// First onSurfaceCreated, on the GL thread, outside guest execution: load the profile only.
void WarmUpGlThreadJit() {
    static bool done = false;
    if (done || !g_host) return;
    done = true;
    char mode[PROP_VALUE_MAX]{};
    __system_property_get("debug.nfsbridge.jitprofile", mode);
    if (strcmp(mode, "off") == 0) {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "JIT warm-up skipped (debug.nfsbridge.jitprofile off)");
        return;
    }
    char budget[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.nfsbridge.jitwarmup_ms", budget) > 0) {
        const double ms = strtod(budget, nullptr);
        if (ms > 0 && ms <= 50) g_warmup.budget_ns = uint64_t(ms * 1e6);
    }
    if (LoadJitProfile(&g_warmup.blocks) && !g_warmup.blocks.empty()) {
        g_warmup.active = true;
        g_warmup.started = MonotonicNs();
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "JIT warm-up: %zu profile blocks, %.1f ms per RunLoop tick",
                            g_warmup.blocks.size(), g_warmup.budget_ns / 1e6);
    } else {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "JIT warm-up: no profile (%s)", kJitProfileAsset);
    }
    if (strcmp(mode, "record") == 0) {
        for (uint64_t d : g_warmup.blocks) g_jit_profile.Add(d);
        g_jit_profile.written = 0;
        g_jit_profile.recording = true;
        g_jit_profile.last_write = MonotonicNs();
        DynarmicBridgeSetBlockRecorder(RecordBlock);
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "JIT profile recording on (%zu blocks from the shipped profile)", g_jit_profile.order.size());
    }
}

// Before a RunLoop tick on the GL thread: translate profile blocks for up to the per-tick budget.
void WarmUpStep() {
    const uint64_t start = MonotonicNs();
    auto& jit = g_host->CurrentJit();
    constexpr size_t kChunk = 16;
    bool cache_full = false;
    while (g_warmup.cursor < g_warmup.blocks.size() && MonotonicNs() - start < g_warmup.budget_ns) {
        const size_t count = std::min(kChunk, g_warmup.blocks.size() - g_warmup.cursor);
        const size_t done = jit.PrecompileBlocks(g_warmup.blocks.data() + g_warmup.cursor, count, kJitWarmupReserve);
        g_warmup.cursor += done;
        if (done < count) { cache_full = true; break; }
    }
    g_warmup.spent_ns += MonotonicNs() - start;
    ++g_warmup.ticks;
    if (g_warmup.cursor < g_warmup.blocks.size() && !cache_full) return;
    g_warmup.active = false;
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
        "JIT warm-up done: %zu of %zu profile blocks over %u ticks (%.1f s), %.1f ms translating, code cache left %.1f MiB%s",
        g_warmup.cursor, g_warmup.blocks.size(), g_warmup.ticks, (MonotonicNs() - g_warmup.started) / 1e9, g_warmup.spent_ns / 1e6,
        DynarmicBridgeCodeCacheRemaining() / 1048576.0, cache_full ? " (stopped: code cache reserve)" : "");
    std::vector<uint64_t>().swap(g_warmup.blocks);
}

void MaybeWriteJitProfile(uint64_t now) {
    if (!g_jit_profile.recording || now - g_jit_profile.last_write < 10000000000u) return;
    g_jit_profile.last_write = now;
    if (g_jit_profile.order.size() != g_jit_profile.written) WriteJitProfile();
}

// GameActivityMain
void N_OnCreate(JNIEnv* env, jobject self) { Void(env, self, kOnCreate); }
void N_OnDestroy(JNIEnv* env, jobject self) { Void(env, self, kOnDestroy); }
void N_OnMusicPlayerStateChanged(JNIEnv* env, jobject self) { Void(env, self, kOnMusicPlayerStateChanged); }
void N_OnOrientationChange(JNIEnv* env, jobject self, jint value) { GuestCall(env, self).Int(value).Run(kOnOrientationChange); }
void N_OnPause(JNIEnv* env, jobject self) { Void(env, self, kOnPause); }
void N_OnPhysicalKeyDown(JNIEnv* env, jobject self, jint key, jint meta) { GuestCall(env, self).Int(key).Int(meta).Run(kOnPhysicalKeyDown); }
void N_OnPhysicalKeyUp(JNIEnv* env, jobject self, jint key, jint meta) { GuestCall(env, self).Int(key).Int(meta).Run(kOnPhysicalKeyUp); }
void N_OnPhysicalNavigationVisibilityChanged(JNIEnv* env, jobject self, jboolean visible) {
    GuestCall(env, self).Bool(visible).Run(kOnPhysicalNavigationVisibilityChanged);
}
void N_OnRestart(JNIEnv* env, jobject self) { Void(env, self, kOnRestart); }
void N_OnResume(JNIEnv* env, jobject self) { Void(env, self, kOnResume); }
void N_OnStart(JNIEnv* env, jobject self) { Void(env, self, kOnStart); }
void N_OnStop(JNIEnv* env, jobject self) { Void(env, self, kOnStop); }
jboolean N_RestoreContext(JNIEnv* env, jobject self) { return GuestCall(env, self).Run(kRestoreContext) ? JNI_TRUE : JNI_FALSE; }
void N_SurfaceChanged(JNIEnv* env, jobject self, jobject gl, jint width, jint height) {
    ThunkManager::s_screenWidth = static_cast<uint32_t>(width);
    ThunkManager::s_screenHeight = static_cast<uint32_t>(height);
    GuestCall(env, self).Object(gl).Int(width).Int(height).Run(kSurfaceChanged);
}
void N_SurfaceCreated(JNIEnv* env, jobject self, jobject gl, jobject config) {
    WarmUpGlThreadJit();
    GuestCall(env, self).Object(gl).Object(config).Run(kSurfaceCreated);
}

// GameGLSurfaceView (IIFF)V
void N_TouchPadEvent(JNIEnv* env, jobject self, jint a, jint b, jfloat x, jfloat y) {
    GuestCall(env, self).Int(a).Int(b).Float(x).Float(y).Run(kTouchPadEvent);
}
void N_TouchScreenEvent(JNIEnv* env, jobject self, jint a, jint b, jfloat x, jfloat y) {
    GuestCall(env, self).Int(a).Int(b).Float(x).Float(y).Run(kTouchScreenEvent);
}

// ---------------------------------------------------------------------------
// Frame diagnostics (off unless setprop debug.nfsbridge.frametrace 1 before launch): the RunLoop
// tick is the whole guest frame (IsisApp::Update + Render on the GL thread). Every 5 s one line with
// the tick-time distribution and the thread's Dynarmic translation work; every tick over 20 ms is
// logged with the part of it spent translating new guest code and code-cache clears.
// ---------------------------------------------------------------------------
extern "C" uint64_t g_bridge_draw_calls;     // thunks_gl.cpp
extern "C" uint64_t g_bridge_draw_vertices;

struct FrameDiag {
    bool enabled = false;
    uint64_t window_start = 0, last_start = 0;
    uint32_t frames = 0, over_8 = 0, over_16 = 0, over_33 = 0;
    uint64_t busy_ns = 0, max_ns = 0, max_gap_ns = 0, compile_ns = 0, blocks = 0, clears = 0;
    uint64_t draws_at_start = 0, vertices_at_start = 0;

    void Record(uint64_t start, uint64_t end, uint64_t frame_blocks, uint64_t frame_compile_ns, uint64_t frame_clears) {
        const uint64_t tick = end - start;
        if (tick > 20000000u) {
            __android_log_print(ANDROID_LOG_INFO, "Arm32Bridge_Frame",
                "slow tick %.1f ms: translate %.1f ms (%llu blocks), cache clears %llu", tick / 1e6,
                frame_compile_ns / 1e6, (unsigned long long)frame_blocks, (unsigned long long)frame_clears);
        }
        if (!window_start) {
            window_start = start;
            draws_at_start = g_bridge_draw_calls;
            vertices_at_start = g_bridge_draw_vertices;
        }
        if (last_start && start - last_start > max_gap_ns) max_gap_ns = start - last_start;
        last_start = start;
        ++frames;
        busy_ns += tick;
        if (tick > max_ns) max_ns = tick;
        over_8 += tick > 8333333u;
        over_16 += tick > 16666666u;
        over_33 += tick > 33333333u;
        compile_ns += frame_compile_ns;
        blocks += frame_blocks;
        clears += frame_clears;
        if (end - window_start < 5000000000u) return;
        const double span = (end - window_start) / 1e9;
        __android_log_print(ANDROID_LOG_INFO, "Arm32Bridge_Frame",
            "%.1f s: %u ticks (%.1f/s), tick avg %.2f ms max %.1f ms, >8.3 ms %u, >16.7 ms %u, >33 ms %u, "
            "max tick-to-tick %.1f ms | translate %.1f ms, %llu blocks, cache clears %llu, code cache left %.1f MiB | "
            "draws/tick %.0f, vertices/tick %.0f",
            span, frames, frames / span, busy_ns / 1e6 / frames, max_ns / 1e6, over_8, over_16, over_33,
            max_gap_ns / 1e6, compile_ns / 1e6, (unsigned long long)blocks, (unsigned long long)clears,
            DynarmicBridgeCodeCacheRemaining() / 1048576.0, double(g_bridge_draw_calls - draws_at_start) / frames,
            double(g_bridge_draw_vertices - vertices_at_start) / frames);
        *this = FrameDiag{};
        enabled = true;
    }
};
FrameDiag g_frame_diag;

void StartFrameDiag() {
    char value[PROP_VALUE_MAX]{};
    g_frame_diag.enabled = __system_property_get("debug.nfsbridge.frametrace", value) > 0 && value[0] == '1';
    if (g_frame_diag.enabled) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "frame diagnostics on (debug.nfsbridge.frametrace)");
}

// RunLoop, MogaController
namespace renderqueue { void OnGlTick(uint64_t now); }

void N_RunLoopTick(JNIEnv* env, jobject self) {
    if (g_warmup.active) WarmUpStep();
    if (g_jit_profile.recording) MaybeWriteJitProfile(MonotonicNs());
    renderqueue::OnGlTick(MonotonicNs());
    if (!g_frame_diag.enabled) {
        Void(env, self, kRunLoopTick);
        if (g_host) prof::OnGlTick(*g_host, MonotonicNs());
        return;
    }
    uint64_t b0, c0, k0, b1, c1, k1;
    DynarmicBridgeCompileStats(&b0, &c0, &k0);
    const uint64_t start = MonotonicNs();
    Void(env, self, kRunLoopTick);
    const uint64_t end = MonotonicNs();
    DynarmicBridgeCompileStats(&b1, &c1, &k1);
    g_frame_diag.Record(start, end, b1 - b0, c1 - c0, k1 - k0);
    if (g_host) prof::OnGlTick(*g_host, end);
}
void N_MogaKeyEvent(JNIEnv* env, jobject self, jobject event) { GuestCall(env, self).Object(event).Run(kMogaKeyEvent); }
void N_MogaMotionEvent(JNIEnv* env, jobject self, jobject event) { GuestCall(env, self).Object(event).Run(kMogaMotionEvent); }
void N_MogaStateEvent(JNIEnv* env, jobject self, jobject event) { GuestCall(env, self).Object(event).Run(kMogaStateEvent); }

// EAIO, EAMIO StorageDirectory (static)
void N_EaioShutdown(JNIEnv* env, jclass cls) { Void(env, cls, kEaioShutdown); }
void N_EaioStartup(JNIEnv* env, jclass cls, jobject assets, jstring data, jstring files, jstring external) {
    GuestCall(env, cls).Object(assets).Object(data).Object(files).Object(external).Run(kEaioStartup);
}
void N_StorageShutdown(JNIEnv* env, jclass cls) { Void(env, cls, kStorageShutdown); }
void N_StorageStartup(JNIEnv* env, jclass cls) { Void(env, cls, kStorageStartup); }

// Nimble bridge
void N_NimbleCallback(JNIEnv* env, jclass cls, jint id, jobjectArray args) {
    GuestCall(env, cls).Int(id).Object(args).Run(kNimbleCallback);
}
void N_NimbleFinalize(JNIEnv* env, jclass cls, jint id) { GuestCall(env, cls).Int(id).Run(kNimbleFinalize); }

// FMOD Ex Java output (org.fmod.FMODAudioDevice). fmodProcess mixes into a direct
// ByteBuffer that Java allocated, i.e. host memory the guest cannot address. The
// guest gets a direct buffer over an arena scratch block of the same capacity and
// the mixed PCM is copied into the Java buffer after the call. One scratch block
// and one global buffer reference per capacity, reused for every audio period.
struct FmodScratch {
    jlong capacity = 0;
    uint32_t guest = 0;
    jobject buffer = nullptr;
};
std::mutex g_fmod_mutex;
FmodScratch g_fmod_scratch;

jint N_FmodGetInfo(JNIEnv* env, jobject self, jint what) { return static_cast<jint>(GuestCall(env, self).Int(what).Run(kFmodGetInfo)); }
jint N_FmodProcess(JNIEnv* env, jobject self, jobject buffer) {
    if (!g_host) return 0;
    void* host_pcm = buffer ? env->GetDirectBufferAddress(buffer) : nullptr;
    const jlong capacity = buffer ? env->GetDirectBufferCapacity(buffer) : -1;
    if (!host_pcm || capacity <= 0 || capacity > (1 << 22)) {
        LOGE("fmodProcess: unusable Java buffer address=%p capacity=%lld", host_pcm, static_cast<long long>(capacity));
        return 0;
    }
    std::lock_guard lock(g_fmod_mutex);
    if (g_fmod_scratch.capacity != capacity) {
        if (g_fmod_scratch.buffer) env->DeleteGlobalRef(g_fmod_scratch.buffer);
        if (g_fmod_scratch.guest) g_host->GuestFree(g_fmod_scratch.guest);
        g_fmod_scratch = {};
        const uint32_t guest = g_host->GuestAlloc(static_cast<size_t>(capacity), 16);
        jobject local = guest ? env->NewDirectByteBuffer(g_host->GetGuestMemoryBase() + guest, capacity) : nullptr;
        if (!local) {
            if (guest) g_host->GuestFree(guest);
            LOGE("fmodProcess: guest scratch allocation failed (%lld bytes)", static_cast<long long>(capacity));
            return 0;
        }
        g_fmod_scratch = {capacity, guest, env->NewGlobalRef(local)};
        env->DeleteLocalRef(local);
        LOGI("fmodProcess: guest scratch 0x%08X, %lld bytes", guest, static_cast<long long>(capacity));
    }
    const jint rc = static_cast<jint>(GuestCall(env, self).Object(g_fmod_scratch.buffer).Run(kFmodProcess));
    memcpy(host_pcm, g_host->GetGuestMemoryBase() + g_fmod_scratch.guest, static_cast<size_t>(capacity));
    return rc;
}

// ---------------------------------------------------------------------------
// QA catalog / cheat actions (NfsQa.nativeAction): host code since stage 9. Until then the ARM32 guest
// libnfsqa.so (work/native/nfsvfs/nfsqa.c, still the source of the ARM32 build's libnfsport) did the same
// calls. Replays engine sequences that already exist in libapp.so (work/hidden_content.md HC-01/HC-02):
//   cash grant    = StoreScreen_OnUpdate_Grant 0x19FF14..0x19FFC0: profile = 0x77B70(); ChangeSet ctor
//                   0x251D58(&cs, profile); sym = 0x406644("store"); AddCash 0x256FF8(cs.ptr, amount, &sym);
//                   Apply 0x257F84(cs.ptr); Save 0x25FF7C(0x77680(), &dword_AD49C4); release cs.holder
//   unlock cars   = 0x248184() (Metagame/Profile "Car/Unlock/All", applies its own ChangeSet)
//   unlock events = 0x25002C(profile) ("Events/Unlock All")
//   award all     = 0x229E9C(awards = 0x192724()) ("Metagame/Awards/Award All")
//   debug flags   = DebugOptions_Get 0x33FF1C() + byte offset (EverythingIsFree 53, DisableTraffic 72,
//                   InfinitePlayerNitro 73, WeakenOpponents 74, ...)
// Every entry point is checked against the expected libapp.so bytes before first use; on a mismatch every
// action refuses (rc -100). NfsQaMenu queues the calls on the GL thread between frames.
// ---------------------------------------------------------------------------
namespace qa {
constexpr uint32_t kBase = 0x40000000u;  // libapp (cfg::kModules)
enum : int { kGrantCash = 1, kSetDebugByte, kGetDebugByte, kUnlockAllCars, kUnlockAllEvents, kAwardAll, kGetCash, kSave,
             kOpenPauseHelp };
// Help buttons (stage 9): StoreScreen::OnButton 0x1A0C04 and PauseScreen::OnButton 0x18AFD4 get the button's event
// name; "HELP" opened the store help text / the pause menu's flow output HELP (the game's help window). The hooks
// replace "MOV R4, R0" right after the event string is known, call NfsQaMenu.onStoreHelp/onPauseHelp and return
// through the function's own epilogue; every other event runs unchanged. "Controls" in the pause dialog
// (kOpenPauseHelp) later sends HELP to the same pause screen through the base handler 0x1A7920.
constexpr uint32_t kStoreOnButtonHook = 0x1A0C10, kStoreOnButtonEpilogue = 0x1A0E4C;
constexpr uint32_t kPauseOnButtonHook = 0x18AFE4, kPauseOnButtonEpilogue = 0x18B0BC;
constexpr uint32_t kPauseScreenVtable = 0xA9F88C, kScreenEventToOutput = 0x1A7920;
constexpr uint32_t kMovR4R0 = 0xE1A04000, kStoreEpilogueInsn = 0xE24BD018, kPauseEpilogueInsn = 0xE24BD010;
std::atomic<uint32_t> g_pause_screen{0};
jclass g_menu_class = nullptr;
jmethodID g_on_store_help = nullptr, g_on_pause_help = nullptr;
std::atomic<int> g_event_logs{0};
struct Signature { uint32_t offset; uint8_t length; uint8_t bytes[8]; };
constexpr Signature kSignatures[] = {
    {0x077B70, 8, {0xc0, 0x00, 0x9f, 0xe5, 0x00, 0x00, 0x9f, 0xe7}},  // Profile::GetInstance
    {0x077C38, 4, {0x20, 0xae, 0xa5, 0x00}},                          // its literal -> dword_AD299C
    {0x077680, 8, {0xc0, 0x00, 0x9f, 0xe5, 0x00, 0x00, 0x9f, 0xe7}},  // SaveManager::GetInstance
    {0x077748, 4, {0x0c, 0xb3, 0xa5, 0x00}},                          // literal -> dword_AD2998
    {0x192724, 8, {0xc0, 0x00, 0x9f, 0xe5, 0x00, 0x00, 0x9f, 0xe7}},  // Awards::GetInstance
    {0x1927EC, 4, {0x68, 0x1a, 0x94, 0x00}},                          // literal -> dword_AD4198
    {0x251D58, 8, {0x70, 0x4c, 0x2d, 0xe9, 0x10, 0xb0, 0x8d, 0xe2}},  // ChangeSet ctor
    {0x406644, 8, {0x10, 0x4c, 0x2d, 0xe9, 0x08, 0xb0, 0x8d, 0xe2}},  // symbol from C string
    {0x256FF8, 8, {0xf0, 0x4d, 0x2d, 0xe9, 0x18, 0xb0, 0x8d, 0xe2}},  // ChangeSet::AddCash
    {0x257F84, 8, {0x70, 0x4c, 0x2d, 0xe9, 0x10, 0xb0, 0x8d, 0xe2}},  // ChangeSet::Apply
    {0x25FF7C, 8, {0xf0, 0x48, 0x2d, 0xe9, 0x10, 0xb0, 0x8d, 0xe2}},  // SaveManager::Save
    {0x33FF1C, 8, {0x30, 0x48, 0x2d, 0xe9, 0x08, 0xb0, 0x8d, 0xe2}},  // DebugOptions_Get
    {0x248184, 8, {0xf0, 0x4f, 0x2d, 0xe9, 0x1c, 0xb0, 0x8d, 0xe2}},  // Car/Unlock/All
    {0x248308, 8, {0xf0, 0x4f, 0x2d, 0xe9, 0x1c, 0xb0, 0x8d, 0xe2}},  // Car/Purchase/All
    {0x1A7920, 8, {0x30, 0x48, 0x2d, 0xe9, 0x08, 0xb0, 0x8d, 0xe2}},  // flow screen: event -> output
    {0x25002C, 8, {0xf0, 0x4f, 0x2d, 0xe9, 0x1c, 0xb0, 0x8d, 0xe2}},  // Events/Unlock All
    {0x229E9C, 8, {0x30, 0x48, 0x2d, 0xe9, 0x08, 0xb0, 0x8d, 0xe2}},  // Award All
};
constexpr uint32_t kDebugOptionsSize = 0x7C;  // malloc size in DebugOptions_Get
constexpr uint32_t kSaveArgument = 0xAD49C4;  // dword_AD49C4 (value of GOT slot 0xAC61E4)
constexpr uint32_t kProfileCash = 4;          // Profile_DebugAddCash: *(profile + 4) += amount
std::mutex g_mutex;
int g_state = 0;  // 0 unknown, 1 verified, -1 mismatch

bool Verified(DynarmicHost& h) {
    if (g_state) return g_state == 1;
    for (const auto& signature : kSignatures)
        for (uint8_t i = 0; i < signature.length; ++i)
            if (h.MemoryRead8(kBase + signature.offset + i) != signature.bytes[i]) {
                LOGE("QA: signature mismatch at libapp+0x%06x: not the expected 1.3.128 build", signature.offset);
                g_state = -1;
                return false;
            }
    g_state = 1;
    __android_log_print(ANDROID_LOG_INFO, "NfsQa", "libapp.so verified at 0x%08X (QA actions in the bridge)", kBase);
    return true;
}

uint32_t Call(uint32_t function, const std::vector<uint32_t>& args = {}) {
    const uint32_t result = g_host->RunFunction(function, args);
    if (g_host->HasFailed()) GuestFailed("NfsQa.nativeAction");
    return result;
}
uint32_t Engine(uint32_t offset, const std::vector<uint32_t>& args = {}) { return Call(kBase + offset, args); }

// boost::shared_ptr counted base: [vtbl][use][weak][ptr]; dispose on use == 0, destroy on weak == 0.
void ReleaseHolder(DynarmicHost& h, uint32_t holder) {
    if (!holder) return;
    const uint32_t use = h.MemoryRead32(holder + 4) - 1;
    h.MemoryWrite32(holder + 4, use);
    if (use) return;
    const uint32_t vtable = h.MemoryRead32(holder);
    Call(h.MemoryRead32(vtable + 8), {holder});
    const uint32_t weak = h.MemoryRead32(holder + 8) - 1;
    h.MemoryWrite32(holder + 8, weak);
    if (!weak) Call(h.MemoryRead32(vtable + 12), {holder});
}

void Save() { Engine(0x25FF7C, {Engine(0x077680), kBase + kSaveArgument}); }

int GrantCash(DynarmicHost& h, int amount) {
    if (amount <= 0 || amount > 100000000) return -2;
    const uint32_t profile = Engine(0x077B70);
    if (!profile) return -3;
    // Guest scratch: ChangeSet {ptr, holder} at +0, symbol at +8, "store" at +12.
    const uint32_t scratch = h.GuestAlloc(32, 8);
    if (!scratch) return -4;
    for (uint32_t i = 0; i < 12; i += 4) h.MemoryWrite32(scratch + i, 0);
    const char text[] = "store";
    for (uint32_t i = 0; i < sizeof(text); ++i) h.MemoryWrite8(scratch + 12 + i, static_cast<uint8_t>(text[i]));
    Engine(0x251D58, {scratch, profile});
    Engine(0x406644, {scratch + 8, scratch + 12});
    const uint32_t change_set = h.MemoryRead32(scratch);
    Engine(0x256FF8, {change_set, static_cast<uint32_t>(amount), scratch + 8});
    Engine(0x257F84, {change_set});
    Save();
    ReleaseHolder(h, h.MemoryRead32(scratch + 4));
    h.GuestFree(scratch);
    return 0;
}

int Action(DynarmicHost& h, int action, int arg) {
    switch (action) {
        case kGrantCash: {
            const int rc = GrantCash(h, arg);
            __android_log_print(ANDROID_LOG_INFO, "NfsQa", "grant cash %d -> rc=%d", arg, rc);
            return rc;
        }
        case kSetDebugByte: {
            const uint32_t offset = (static_cast<uint32_t>(arg) >> 8) & 0xFF, value = static_cast<uint32_t>(arg) & 0xFF;
            if (offset >= kDebugOptionsSize) return -2;
            const uint32_t options = Engine(0x33FF1C);
            if (!options) return -3;
            h.MemoryWrite8(options + offset, static_cast<uint8_t>(value));
            __android_log_print(ANDROID_LOG_INFO, "NfsQa", "debug byte +%u = %u", offset, value);
            return 0;
        }
        case kGetDebugByte: {
            const uint32_t offset = static_cast<uint32_t>(arg) & 0xFF;
            const uint32_t options = Engine(0x33FF1C);
            return options && offset < kDebugOptionsSize ? h.MemoryRead8(options + offset) : -2;
        }
        case kUnlockAllCars:
            // "Car/Unlock/All" only makes every car purchasable (the garage still counted 0/55 owned);
            // "Car/Purchase/All" (0x248308, same ChangeSet loop with the purchase operation) puts each in the garage.
            Engine(0x248184);
            Engine(0x248308);
            Save();
            __android_log_print(ANDROID_LOG_INFO, "NfsQa", "unlock + purchase all cars applied");
            return 0;
        case kUnlockAllEvents:
            Engine(0x25002C, {Engine(0x077B70)});
            Save();
            __android_log_print(ANDROID_LOG_INFO, "NfsQa", "unlock all events applied");
            return 0;
        case kAwardAll:
            Engine(0x229E9C, {Engine(0x192724)});
            Save();
            __android_log_print(ANDROID_LOG_INFO, "NfsQa", "award all applied");
            return 0;
        case kGetCash: {
            const uint32_t profile = Engine(0x077B70);
            return profile ? static_cast<int>(h.MemoryRead32(profile + kProfileCash)) : -3;
        }
        case kSave:
            Save();
            return 0;
        case kOpenPauseHelp: {
            const uint32_t screen = g_pause_screen.exchange(0);
            if (!screen || h.MemoryRead32(screen) != kBase + kPauseScreenVtable) {
                __android_log_print(ANDROID_LOG_INFO, "NfsQa", "pause help: the pause screen is gone");
                return -3;
            }
            // Event string as the base handler reads it ({begin, end}, begin -> "HELP\0").
            const uint32_t event = h.GuestAlloc(16, 4);
            if (!event) return -4;
            h.MemoryWrite32(event, event + 8);
            h.MemoryWrite32(event + 4, event + 12);
            const char help[] = "HELP";
            for (uint32_t i = 0; i < sizeof(help); ++i) h.MemoryWrite8(event + 8 + i, static_cast<uint8_t>(help[i]));
            Engine(kScreenEventToOutput, {screen, event});
            h.GuestFree(event);
            __android_log_print(ANDROID_LOG_INFO, "NfsQa", "pause help: original help window requested");
            return 0;
        }
        default:
            return -1;
    }
}

bool IsHelpEvent(DynarmicHost& h, uint32_t begin, uint32_t end) {
    if (end - begin != 4) return false;
    return h.MemoryRead8(begin) == 'H' && h.MemoryRead8(begin + 1) == 'E' && h.MemoryRead8(begin + 2) == 'L' &&
           h.MemoryRead8(begin + 3) == 'P';
}

void LogEvent(const char* screen, DynarmicHost& h, uint32_t begin, uint32_t end) {
    if (g_event_logs.fetch_add(1, std::memory_order_relaxed) >= 40 || end < begin || end - begin > 40) return;
    std::string text;
    for (uint32_t p = begin; p < end; ++p) text.push_back(static_cast<char>(h.MemoryRead8(p)));
    __android_log_print(ANDROID_LOG_INFO, "NfsQa", "%s button event %s", screen, text.c_str());
}

void CallMenu(jmethodID method) {
    JNIEnv* env = nullptr;
    if (!g_jvm || !g_menu_class || !method || g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return;
    env->CallStaticVoidMethod(g_menu_class, method);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
}

// JNI_OnLoad (application class loader): the menu entry points the hooks call.
void CacheJava(JNIEnv* env) {
    jclass cls = env->FindClass("com/ea/ironmonkey/port/NfsQaMenu");
    if (!cls) { env->ExceptionClear(); LOGE("QA: NfsQaMenu not found, help hooks inactive"); return; }
    g_menu_class = static_cast<jclass>(env->NewGlobalRef(cls));
    g_on_store_help = env->GetStaticMethodID(cls, "onStoreHelp", "()V");
    g_on_pause_help = env->GetStaticMethodID(cls, "onPauseHelp", "()V");
    if (env->ExceptionCheck()) { env->ExceptionClear(); g_on_store_help = g_on_pause_help = nullptr; }
    env->DeleteLocalRef(cls);
}

// The store's two entry points ask NimbleWrapper whether the network is up (0x7AF980, Network.getStatus() == OK)
// and show NO_CONNECTION_POPUP otherwise; so does the store's error popup (0x19F508) before it picks the text for
// the purchase's error code (a cancel is MTX_ERROR_3333 "Purchase cancelled."). The QA store needs no network, so
// these three calls become "available"; every other caller (content manifest download, online features) still sees
// the offline status.
struct CodePatch { uint32_t offset; uint32_t original; uint32_t patched; const char* what; };
constexpr CodePatch kStorePatches[] = {
    {0x36F3AC, 0xEB110173, 0xE3A00001, "menu STORE button"},  // BL sub_7AF980 -> MOV R0, #1
    {0x16D240, 0xEB1909CE, 0xE3A00001, "garage NEED_CASH -> store"},
    {0x19F558, 0xEB184108, 0xE3A00001, "store error popup text"},
};

void InstallStorePatches(DynarmicHost& host) {
    for (const auto& patch : kStorePatches)
        if (host.MemoryRead32(kBase + patch.offset) != patch.original) {
            LOGE("QA: libapp+0x%x (%s) is not the expected instruction, store network patches skipped", patch.offset, patch.what);
            return;
        }
    for (const auto& patch : kStorePatches) {
        host.MemoryWrite32(kBase + patch.offset, patch.patched);
        host.InvalidateGuestCode(kBase + patch.offset, 4);
    }
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "QA store entry without the network check (%zu sites)", std::size(kStorePatches));
}

void InstallHelpHooks(DynarmicHost& host) {
    InstallStorePatches(host);
    struct Check { uint32_t offset; uint32_t original; };
    const Check checks[] = {{kStoreOnButtonHook, kMovR4R0}, {kPauseOnButtonHook, kMovR4R0},
                            {kStoreOnButtonEpilogue, kStoreEpilogueInsn}, {kPauseOnButtonEpilogue, kPauseEpilogueInsn}};
    for (const auto& check : checks)
        if (host.MemoryRead32(kBase + check.offset) != check.original) {
            LOGE("QA: libapp+0x%x is not the expected instruction, help hooks skipped", check.offset);
            return;
        }
    struct Hook { uint32_t offset; const char* name; ThunkHandler handler; };
    const Hook hooks[] = {
        {kStoreOnButtonHook, "nfs_qa_store_help", [](Dynarmic::A32::Jit& jit, DynarmicHost& h) {
             // R0 = StoreScreen, R1 = event string {begin, end}
             const uint32_t begin = h.MemoryRead32(jit.Regs()[1]), end = h.MemoryRead32(jit.Regs()[1] + 4);
             LogEvent("store", h, begin, end);
             if (IsHelpEvent(h, begin, end) && g_on_store_help) {
                 CallMenu(g_on_store_help);
                 jit.Regs()[0] = 0;
                 jit.Regs()[15] = kBase + kStoreOnButtonEpilogue;
                 return;
             }
             jit.Regs()[4] = jit.Regs()[0];
         }},
        {kPauseOnButtonHook, "nfs_qa_pause_help", [](Dynarmic::A32::Jit& jit, DynarmicHost& h) {
             // R0 = PauseScreen, R6/R7 = event begin/end (LDM R1, {R6, R7} just before)
             const uint32_t begin = jit.Regs()[6], end = jit.Regs()[7];
             LogEvent("pause", h, begin, end);
             if (IsHelpEvent(h, begin, end) && g_on_pause_help) {
                 g_pause_screen.store(jit.Regs()[0]);
                 CallMenu(g_on_pause_help);
                 jit.Regs()[0] = 0;
                 jit.Regs()[15] = kBase + kPauseOnButtonEpilogue;
                 return;
             }
             jit.Regs()[4] = jit.Regs()[0];
         }},
    };
    for (const auto& hook : hooks) {
        const uint32_t trampoline = host.GetThunkManager().RegisterThunk(hook.name, hook.handler);
        host.MemoryWrite32(kBase + hook.offset, host.MemoryRead32(trampoline));
        host.InvalidateGuestCode(kBase + hook.offset, 4);
    }
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "QA help hooks installed (store and pause help buttons)");
}
}  // namespace qa

jint N_QaAction(JNIEnv* env, jclass, jint action, jint arg) {
    std::lock_guard lock(qa::g_mutex);
    if (!g_host || g_host->HasFailed() || !qa::Verified(*g_host)) return -100;
    FakeJniEnv& jni = FakeJniEnv::Instance();
    if (!jni.BeginNativeCall(env)) return -100;  // guest local-reference frame, in case engine code calls back
    const int rc = qa::Action(*g_host, action, arg);
    jni.EndNativeCall();
    return rc;
}

// NfsVfs (host, thunks_libc.cpp)

jint N_NfsVfsInstall(JNIEnv* env, jclass, jstring path, jint fd, jlong offset, jlong length) {
    const char* text = path ? env->GetStringUTFChars(path, nullptr) : nullptr;
    if (!text) return -1;
    const int rc = BridgeNfsVfsInstall(text, fd, offset, length);
    env->ReleaseStringUTFChars(path, text);
    return rc;
}
jlongArray N_NfsVfsStats(JNIEnv* env, jclass) {
    int64_t values[3];
    BridgeNfsVfsStats(values);
    jlongArray result = env->NewLongArray(3);
    if (result) {
        const jlong copy[3] = {values[0], values[1], values[2]};
        env->SetLongArrayRegion(result, 0, 3, copy);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Memory report (diagnostic, off unless setprop debug.nfsbridge.memreport 1): guest heap and
// mapping allocator state plus the resident size of each arena region, every 10 s.
// ---------------------------------------------------------------------------
size_t ResidentKiB(uint32_t begin, uint32_t end) {
    if (!g_host || end <= begin) return 0;
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const uintptr_t first = reinterpret_cast<uintptr_t>(g_host->GetGuestMemoryBase() + begin) & ~(page - 1);
    const uintptr_t last = reinterpret_cast<uintptr_t>(g_host->GetGuestMemoryBase() + end);
    std::vector<unsigned char> resident((last - first + page - 1) / page);
    if (mincore(reinterpret_cast<void*>(first), last - first, resident.data()) != 0) return 0;
    size_t pages = 0;
    for (unsigned char value : resident) pages += value & 1;
    return pages * page / 1024;
}

void StartMemoryReport() {
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.nfsbridge.memreport", value) <= 0 || value[0] != '1') return;
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            const auto stats = g_host->GetHeapStats();
            __android_log_print(ANDROID_LOG_INFO, "Arm32Bridge_Mem",
                "heap hw=%u KiB free_small=%u KiB free_huge=%u KiB/%u blocks dirty=%u KiB | large hw=%u KiB "
                "free=%u KiB/%u blocks dirty=%u KiB | map hw=%u KiB free=%u KiB | failed allocs %u (last %u bytes) | "
                "resident KiB: images=%zu heap=%zu large=%zu stacks=%zu map=%zu | jni refs=%zu",
                stats.heap_high_water / 1024, stats.heap_free_small / 1024, stats.heap_free_huge / 1024,
                stats.huge_free_blocks, stats.huge_dirty / 1024, stats.large_high_water / 1024, stats.large_free / 1024,
                stats.large_free_blocks, stats.large_dirty / 1024, stats.map_high_water / 1024, stats.map_free / 1024,
                stats.alloc_failures, stats.last_failed_size,
                ResidentKiB(0x40000000, 0x41020000),
                ResidentKiB(DynarmicHost::kHeapBase, DynarmicHost::kHeapBase + stats.heap_high_water),
                ResidentKiB(DynarmicHost::kLargeBase, DynarmicHost::kLargeBase + stats.large_high_water),
                ResidentKiB(0x80000000, DynarmicHost::kLargeBase),
                ResidentKiB(DynarmicHost::kMapBase, DynarmicHost::kMapBase + stats.map_high_water),
                FakeJniEnv::Instance().LiveRefCount());
        }
    }).detach();
}

// ---------------------------------------------------------------------------
// DE-05 lens flare occlusion (stage 8). libapp's isis GL query backend is compiled to empty stubs on Android
// (gen 0x54C6B4, delete 0x54C6B8, begin 0x54C6C4, end 0x54C6C8, get-object-uiv 0x54C6D0; called only by the
// RenderQueryGLES methods 0x5658A8.. and the command dispatcher 0x580E9C) and Isis_DetectGlCaps stores
// caps.occlusionQueries = 0 (STRB R0,[R4,#0xD] at 0x564820). With GL_EXT_occlusion_query_boolean the bridge puts
// an SVC to a host thunk in place of each stub's BX LR (the thunk makes the real EXT call and returns to LR) and
// in place of the caps store (the thunk stores 1), so LensFlare_Init_OcclusionGate 0x2E7858 enables the sun flare
// with occlusion; results are read only after QUERY_RESULT_AVAILABLE (0x580E9C case 4), never stalling. Without
// the extension, or with setprop debug.nfsbridge.lensflare off, the caps store keeps the engine's 0 and the stubs
// stay no-ops. Every site is checked against its original instruction first.
// ---------------------------------------------------------------------------
namespace lensflare {
using PfnQueries = void (*)(GLsizei, GLuint*);
using PfnBegin = void (*)(GLenum, GLuint);
using PfnEnd = void (*)(GLenum);
using PfnGetuiv = void (*)(GLuint, GLenum, GLuint*);
std::atomic<int> g_state{-1};  // -1 undecided, 0 off, 1 on
PfnQueries g_gen = nullptr, g_delete = nullptr;
PfnBegin g_begin = nullptr;
PfnEnd g_end = nullptr;
PfnGetuiv g_getuiv = nullptr;

// On the GL thread inside Isis_DetectGlCaps (current context).
bool Enabled() {
    const int state = g_state.load(std::memory_order_acquire);
    if (state >= 0) return state == 1;
    char value[PROP_VALUE_MAX]{};
    const bool below_max = !portopts::PortEffects();  // stage 13: a port effect of the MAX graphics quality
    const bool off = below_max || (__system_property_get("debug.nfsbridge.lensflare", value) > 0 && strcmp(value, "off") == 0);
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    const bool has = extensions && strstr(extensions, "GL_EXT_occlusion_query_boolean");
    if (!off && has) {
        g_gen = reinterpret_cast<PfnQueries>(eglGetProcAddress("glGenQueriesEXT"));
        g_delete = reinterpret_cast<PfnQueries>(eglGetProcAddress("glDeleteQueriesEXT"));
        g_begin = reinterpret_cast<PfnBegin>(eglGetProcAddress("glBeginQueryEXT"));
        g_end = reinterpret_cast<PfnEnd>(eglGetProcAddress("glEndQueryEXT"));
        g_getuiv = reinterpret_cast<PfnGetuiv>(eglGetProcAddress("glGetQueryObjectuivEXT"));
    }
    const bool on = !off && has && g_gen && g_delete && g_begin && g_end && g_getuiv;
    g_state.store(on ? 1 : 0, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "DE-05 lens flare occlusion queries: %s (%s)", on ? "on" : "off",
                        below_max ? "graphics quality below MAX" : off ? "debug.nfsbridge.lensflare off"
                        : has ? "GL_EXT_occlusion_query_boolean" : "no GL_EXT_occlusion_query_boolean");
    return on;
}

bool On() { return g_state.load(std::memory_order_acquire) == 1; }

// The replaced instruction was BX LR.
void ReturnToLr(Dynarmic::A32::Jit& jit) {
    const uint32_t lr = jit.Regs()[14];
    if (lr & 1u) jit.SetCpsr(jit.Cpsr() | (1u << 5));
    jit.Regs()[15] = lr & ~1u;
}

struct Site { uint32_t offset; uint32_t original; const char* name; ThunkHandler handler; };

void Install(DynarmicHost& host, uint32_t libapp_base) {
    const Site sites[] = {
        {0x564820, 0xE5C4000D, "nfs_de05_caps_store", [](Dynarmic::A32::Jit& jit, DynarmicHost& h) {
             // STRB R0,[R4,#0xD]: caps.occlusionQueries (pc already advanced past the SVC)
             h.MemoryWrite8(jit.Regs()[4] + 0xD, Enabled() ? 1 : uint8_t(jit.Regs()[0]));
         }},
        {0x54C6B4, 0xE12FFF1E, "nfs_de05_gen_queries", [](Dynarmic::A32::Jit& jit, DynarmicHost& h) {
             const GLsizei n = static_cast<GLsizei>(jit.Regs()[0]);
             if (On() && n > 0 && n <= 64) {
                 GLuint ids[64] = {};
                 g_gen(n, ids);
                 for (GLsizei i = 0; i < n; ++i) h.MemoryWrite32(jit.Regs()[1] + 4u * i, ids[i]);
             }
             ReturnToLr(jit);
         }},
        {0x54C6B8, 0xE12FFF1E, "nfs_de05_delete_queries", [](Dynarmic::A32::Jit& jit, DynarmicHost& h) {
             const GLsizei n = static_cast<GLsizei>(jit.Regs()[0]);
             if (On() && n > 0 && n <= 64) {
                 GLuint ids[64] = {};
                 for (GLsizei i = 0; i < n; ++i) ids[i] = h.MemoryRead32(jit.Regs()[1] + 4u * i);
                 g_delete(n, ids);
             }
             ReturnToLr(jit);
         }},
        {0x54C6C4, 0xE12FFF1E, "nfs_de05_begin_query", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
             if (On() && jit.Regs()[1]) g_begin(jit.Regs()[0], jit.Regs()[1]);
             ReturnToLr(jit);
         }},
        {0x54C6C8, 0xE12FFF1E, "nfs_de05_end_query", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
             if (On()) g_end(jit.Regs()[0]);
             ReturnToLr(jit);
         }},
        {0x54C6D0, 0xE12FFF1E, "nfs_de05_get_query_uiv", [](Dynarmic::A32::Jit& jit, DynarmicHost& h) {
             if (On() && jit.Regs()[0] && jit.Regs()[2]) {
                 GLuint value = 0;
                 g_getuiv(jit.Regs()[0], jit.Regs()[1], &value);
                 h.MemoryWrite32(jit.Regs()[2], value);
             }
             ReturnToLr(jit);
         }},
    };
    for (const auto& site : sites) {
        const uint32_t address = libapp_base + site.offset;
        if (host.MemoryRead32(address) != site.original) {
            LOGE("DE-05 hook %s: 0x%08X is not the expected instruction, lens flare hooks skipped", site.name, address);
            return;
        }
    }
    for (const auto& site : sites) {
        const uint32_t address = libapp_base + site.offset;
        const uint32_t trampoline = host.GetThunkManager().RegisterThunk(site.name, site.handler);
        host.MemoryWrite32(address, host.MemoryRead32(trampoline));  // the trampoline's SVC #id
        host.InvalidateGuestCode(address, 4);
    }
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "DE-05 lens flare hooks installed (%zu sites)", std::size(sites));
}
}  // namespace lensflare

// ---------------------------------------------------------------------------
// Render command queue race (stage 8). isis records GPU resource work (texture uploads, buffer and shader
// objects, releases) in a resource command queue: sub_4679CC and sub_468650 reserve a command in
// RenderCommandQueue slot 1 under the spinlock *dword_ADC568 and publish it at once, but the caller writes the
// payload after the lock is released, and the GL thread swaps the buffers in sub_467AD8 without the lock and
// executes the swapped buffer right away (sub_468404). A loader thread that stalls between its reservation and
// its payload stores lets the GL thread run a half-written command. Natively the window is a few instructions;
// under the bridge a thread's first run of the code after the reservation is translated inside it (tens to
// hundreds of Ã‚Âµs). b18-host-cold-1 and p18-b22-nr-1 died that way in the texture upload command (sub_473A3C Ã¢â€ â€™
// sub_565B78 with a null texture: RUNAWAY code fetch 0x00000000, lr libapp+0x565c94) while a loader thread ran.
// Fix: a reservation by a thread other than the one that swaps opens a fill window for that thread. The
// producer's saved return address (APCS frame [fp + 4]; the producers and both reservation functions keep R11 as
// the frame pointer) is redirected through a close trampoline, so the window closes when the producer returns
// with its payload written. The swap takes the queue spinlock; while another thread has an open window it
// releases the lock and waits (at most 10 ms until stage 16, now 250 ms, then logs and goes on), and it permutes
// the buffers under the lock. The resource queue starts with 1 MiB buffers instead of 16 KiB, so sub_4688EC does not reallocate a
// buffer under a thread that is still writing into it.
// setprop debug.nfsbridge.rqfix off: engine behaviour; debug.nfsbridge.rqstress N: other threads sleep N Ã‚Âµs right
// after a texture upload reservation (makes the window wide, for A/B).
// Stage 16 (crash report on b30: "invalid guest code fetch at 0x00000000, lr libapp+0x565c94" in
// nativeOnRunLoopTick after races, POCO duchamp / Dimensity 8300): the texture uploads come from short-lived loader
// threads, a new one for each load (one thread at the start, another for the race, both gone afterwards),
// and every new thread starts with an empty JIT code cache, so its first run of the producer code is translated
// inside the window. b23 gave each thread a window slot for good, 64 slots in all: after 64 loader threads (a few
// dozen loads in one session) reservations had no window and the swap no longer waited, which brought the b18/b22
// crash back. Now a slot goes back to the pool when its thread exits (thread_local owner), the limits are longer (a
// cold producer on a busy little core can need more than 10 ms; past the limit the swap still goes on, so a producer
// that waits for the GL thread cannot hang the game), and a buffer that grows (sub_4688EC: malloc, copy, free under
// the queue lock) first waits for the other threads' windows, whose payloads still go into the old buffer.
// ---------------------------------------------------------------------------
namespace renderqueue {
constexpr uint32_t kBase = 0x40000000u;                                        // libapp (cfg::kModules)
constexpr uint32_t kLockPointer = kBase + 0xADC568;                            // dword_ADC568 -> spinlock word
constexpr uint32_t kTextBegin = kBase + 0x66740, kTextEnd = kBase + 0x97E150;  // .plt + .text
constexpr uint64_t kRedirectedLimitNs = 250000000;   // an open window stops holding the swap after this
constexpr uint64_t kUnredirectedLimitNs = 10000000;  // never closed early: the swap waits it out (none seen so far)
constexpr uint64_t kSwapWaitLimitNs = 250000000;
constexpr uint64_t kGrowWaitLimitNs = 250000000;
constexpr int kSlots = 128;

struct Slot {
    std::atomic<int> tid{0};            // 0: free
    std::atomic<uint64_t> deadline{0};  // 0: no open window
};
Slot g_slots[kSlots];
std::atomic<int> g_slot_high{0};        // slots [0, high) have been used
uint32_t g_close_trampoline = 0;
uint32_t g_stress_us = 0;
std::atomic<uint64_t> g_windows{0}, g_unredirected{0}, g_waits{0}, g_wait_ns{0}, g_wait_max_ns{0}, g_timeouts{0},
    g_grows{0}, g_grow_waits{0}, g_threads{0}, g_no_slot{0};
std::atomic<int> g_site_logs{0}, g_event_logs{0};
uint64_t g_reported_windows = 0, g_last_report = 0;  // GL thread

struct Redirect { uint32_t slot; uint32_t original; };
thread_local std::vector<Redirect> t_redirects;
thread_local Slot* t_slot = nullptr;
thread_local bool t_swapper = false;

// Gives the thread's slot back when the thread exits (loader threads are short-lived).
struct SlotOwner {
    ~SlotOwner() {
        if (Slot* slot = t_slot) {
            t_slot = nullptr;
            slot->deadline.store(0, std::memory_order_release);
            slot->tid.store(0, std::memory_order_release);
        }
    }
};
thread_local SlotOwner t_slot_owner;

Slot* ThreadSlot() {
    if (t_slot) return t_slot;
    const int self = gettid();
    for (int i = 0; i < kSlots; ++i) {
        int expected = 0;
        if (g_slots[i].tid.load(std::memory_order_relaxed) == 0 &&
            g_slots[i].tid.compare_exchange_strong(expected, self, std::memory_order_acq_rel)) {
            g_slots[i].deadline.store(0, std::memory_order_release);
            int high = g_slot_high.load(std::memory_order_relaxed);
            while (high < i + 1 && !g_slot_high.compare_exchange_weak(high, i + 1, std::memory_order_acq_rel)) {}
            static_cast<void>(&t_slot_owner);  // first use in this thread registers the owner's destructor
            t_slot = &g_slots[i];
            g_threads.fetch_add(1, std::memory_order_relaxed);
            return t_slot;
        }
    }
    if (g_no_slot.fetch_add(1, std::memory_order_relaxed) == 0)
        LOGE("render queue: more than %d threads reserve at once, thread %d has no fill window", kSlots, self);
    return nullptr;
}

// A return address the redirect may replace: ARM code in libapp right after a call.
bool IsCallReturn(DynarmicHost& h, uint32_t address) {
    if ((address & 3u) || address < kTextBegin + 4 || address >= kTextEnd) return false;
    const uint32_t call = h.MemoryRead32(address - 4);
    return (call & 0x0F000000u) == 0x0B000000u      // BL<c> imm
        || (call & 0xFE000000u) == 0xFA000000u      // BLX imm
        || (call & 0x0FFFFFF0u) == 0x012FFF30u;     // BLX<c> reg
}

// Inside a reservation: the queue lock is held by this thread and the command is already published.
// return_slot: the guest stack word holding the return address of the function that writes the payload.
void OpenWindow(Dynarmic::A32::Jit& jit, DynarmicHost& h, uint32_t return_slot, uint32_t site) {
    if (t_swapper) return;
    const uint32_t sp = jit.Regs()[13];
    bool redirected = false;
    if (!(return_slot & 3u) && return_slot > sp && return_slot - sp < (1u << 20)) {
        const uint32_t original = h.MemoryRead32(return_slot);
        if (original == g_close_trampoline) {
            redirected = true;  // another reservation in a producer frame that is already redirected
        } else if (IsCallReturn(h, original)) {
            t_redirects.push_back({return_slot, original});
            h.MemoryWrite32(return_slot, g_close_trampoline);
            redirected = true;
        }
    }
    g_windows.fetch_add(1, std::memory_order_relaxed);
    if (!redirected) g_unredirected.fetch_add(1, std::memory_order_relaxed);
    if (Slot* slot = ThreadSlot()) {
        const uint64_t deadline = MonotonicNs() + (redirected ? kRedirectedLimitNs : kUnredirectedLimitNs);
        if (deadline > slot->deadline.load(std::memory_order_relaxed)) slot->deadline.store(deadline, std::memory_order_release);
    }
    if (g_site_logs.fetch_add(1, std::memory_order_relaxed) < 24)
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "render queue: thread %d reserves from libapp+0x%x (%s)", gettid(),
                            site - kBase, redirected ? "window until the producer returns" : "time-limited window");
}

// Close trampoline: a redirected producer returned. Its epilogue popped the return address from the slot,
// so SP is slot + 4; entries of frames that were left another way are dropped.
void CloseWindow(Dynarmic::A32::Jit& jit, DynarmicHost& h) {
    const uint32_t sp = jit.Regs()[13];
    while (!t_redirects.empty()) {
        const Redirect top = t_redirects.back();
        t_redirects.pop_back();
        if (top.slot + 4 == sp) {
            if (t_redirects.empty() && t_slot) t_slot->deadline.store(0, std::memory_order_release);
            jit.Regs()[15] = top.original;  // ARM (IsCallReturn)
            return;
        }
        LOGE("render queue: stale return redirect dropped (slot 0x%08X, sp 0x%08X)", top.slot, sp);
    }
    LOGE("render queue: close trampoline reached at sp 0x%08X without a redirected frame", sp);
    h.Fail("render queue close trampoline without a redirected frame");
}

uint32_t* LockWord(DynarmicHost& h) {
    return reinterpret_cast<uint32_t*>(h.GetGuestMemoryBase() + h.MemoryRead32(kLockPointer));
}
void Acquire(uint32_t* word) {
    for (unsigned spins = 0;; ++spins) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(word, &expected, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        if (spins >= 64) std::this_thread::yield();
    }
}
void Release(uint32_t* word) { __atomic_store_n(word, 0u, __ATOMIC_RELEASE); }

bool OtherWindowOpen(uint64_t now, int self) {
    const int count = std::min(g_slot_high.load(std::memory_order_acquire), kSlots);
    for (int i = 0; i < count; ++i) {
        const int tid = g_slots[i].tid.load(std::memory_order_acquire);
        if (!tid || tid == self) continue;
        if (g_slots[i].deadline.load(std::memory_order_acquire) > now) return true;
    }
    return false;
}

// sub_467AD8 before the buffer permutation (replaced instruction LDR R7, [R5]).
void SwapBegin(Dynarmic::A32::Jit& jit, DynarmicHost& h) {
    t_swapper = true;
    uint32_t* word = LockWord(h);
    const int self = gettid();
    const uint64_t start = MonotonicNs();
    uint64_t now = start;
    bool waited = false, timed_out = false;
    for (;;) {
        Acquire(word);
        now = MonotonicNs();
        if (!OtherWindowOpen(now, self)) break;
        if (now - start >= kSwapWaitLimitNs) { timed_out = true; break; }  // go on with the lock held
        Release(word);
        waited = true;
        std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
    if (waited) {
        const uint64_t ns = now - start;
        g_waits.fetch_add(1, std::memory_order_relaxed);
        g_wait_ns.fetch_add(ns, std::memory_order_relaxed);
        if (ns > g_wait_max_ns.load(std::memory_order_relaxed)) g_wait_max_ns.store(ns, std::memory_order_relaxed);
        if (timed_out) g_timeouts.fetch_add(1, std::memory_order_relaxed);
        if ((timed_out || ns > 1000000) && g_event_logs.fetch_add(1, std::memory_order_relaxed) < 20)
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "render queue: swap waited %.2f ms for a fill window%s", ns / 1e6,
                                timed_out ? " (limit reached, swapping anyway)" : "");
    }
    jit.Regs()[7] = h.MemoryRead32(jit.Regs()[5]);
}

// sub_467AD8 after the permutation (replaced instruction LDR R0, [PC, #0x74]: literal at 0x467EC4).
void SwapEnd(Dynarmic::A32::Jit& jit, DynarmicHost& h) {
    Release(LockWord(h));
    jit.Regs()[0] = h.MemoryRead32(kBase + 0x467EC4);
}

// GL thread, before each RunLoop tick: this thread swaps the queue (its own reservations open no window);
// one statistics line every 30 s while cross-thread reservations happen.
void OnGlTick(uint64_t now) {
    t_swapper = true;
    if (now - g_last_report < 30000000000u) return;
    g_last_report = now;
    const uint64_t windows = g_windows.load(std::memory_order_relaxed);
    if (windows == g_reported_windows) return;
    g_reported_windows = windows;
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
        "render queue: %llu cross-thread reservations (%llu time-limited) from %llu threads (%d slots ever used at once, "
        "%llu without a slot), swaps waited %llu times (%.2f ms total, max %.2f ms, %llu at the limit), buffer grows %llu "
        "(%llu waited for windows)",
        (unsigned long long)windows, (unsigned long long)g_unredirected.load(), (unsigned long long)g_threads.load(),
        g_slot_high.load(), (unsigned long long)g_no_slot.load(), (unsigned long long)g_waits.load(), g_wait_ns.load() / 1e6,
        g_wait_max_ns.load() / 1e6, (unsigned long long)g_timeouts.load(), (unsigned long long)g_grows.load(),
        (unsigned long long)g_grow_waits.load());
}

// sub_4688EC (replaced instruction ADD R11, SP, #0x1C; R0 = the buffer that is reallocated, the queue lock held): the
// copy would lose the payload another thread is still writing into the old buffer, so wait for those windows first.
void OnGrow(Dynarmic::A32::Jit& jit, DynarmicHost& h) {
    g_grows.fetch_add(1, std::memory_order_relaxed);
    const int self = gettid();
    const uint64_t start = MonotonicNs();
    uint64_t now = start;
    bool waited = false;
    while (OtherWindowOpen(now, self) && now - start < kGrowWaitLimitNs) {
        waited = true;
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        now = MonotonicNs();
    }
    if (waited) g_grow_waits.fetch_add(1, std::memory_order_relaxed);
    if (g_event_logs.fetch_add(1, std::memory_order_relaxed) < 20)
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "render queue: buffer 0x%08X grows from %u bytes on thread %d%s (%.2f ms)",
                            jit.Regs()[0], h.MemoryRead32(jit.Regs()[0] + 0x1C), self,
                            !waited ? "" : OtherWindowOpen(now, self) ? ", another thread's window still open at the limit"
                                                                     : " after the other threads' windows closed",
                            (now - start) / 1e6);
    jit.Regs()[11] = jit.Regs()[13] + 0x1C;
}

struct Site { uint32_t offset; uint32_t original; const char* name; ThunkHandler handler; };

bool Verify(DynarmicHost& host, const Site* sites, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const uint32_t address = kBase + sites[i].offset;
        if (host.MemoryRead32(address) != sites[i].original) {
            LOGE("render queue hook %s: 0x%08X is not the expected instruction", sites[i].name, address);
            return false;
        }
    }
    return true;
}

void Hook(DynarmicHost& host, const Site* sites, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const uint32_t address = kBase + sites[i].offset;
        const uint32_t trampoline = host.GetThunkManager().RegisterThunk(sites[i].name, sites[i].handler);
        host.MemoryWrite32(address, host.MemoryRead32(trampoline));  // the trampoline's SVC #id
        host.InvalidateGuestCode(address, 4);
    }
}

void Install(DynarmicHost& host) {
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.nfsbridge.rqstress", value) > 0) {
        const long us = strtol(value, nullptr, 10);
        if (us > 0 && us <= 20000) g_stress_us = static_cast<uint32_t>(us);
    }
    if (g_stress_us) {
        const Site stress[] = {
            {0x473934, 0xE2896004, "nfs_rq_stress", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
                 // ADD R6, R9, #4: first instruction after the texture upload reservation (sub_473690)
                 if (!t_swapper) std::this_thread::sleep_for(std::chrono::microseconds(g_stress_us));
                 jit.Regs()[6] = jit.Regs()[9] + 4;
             }},
        };
        if (Verify(host, stress, std::size(stress))) {
            Hook(host, stress, std::size(stress));
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "render queue stress: %u us after texture upload reservations", g_stress_us);
        }
    }
    value[0] = '\0';
    if (__system_property_get("debug.nfsbridge.rqfix", value) > 0 && strcmp(value, "off") == 0) {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "render queue fix off (debug.nfsbridge.rqfix off)");
        return;
    }
    const Site sites[] = {
        {0x467AAC, 0xF57FF05B, "nfs_rq_reserve", [](Dynarmic::A32::Jit& jit, DynarmicHost& h) {
             // DMB ISH before sub_4679CC releases the lock; [R11] = producer frame pointer, [R11 + 4] = producer site
             std::atomic_thread_fence(std::memory_order_seq_cst);
             const uint32_t fp = jit.Regs()[11];
             OpenWindow(jit, h, h.MemoryRead32(fp) + 4, h.MemoryRead32(fp + 4));
         }},
        {0x468718, 0xF57FF05B, "nfs_rq_reserve_release", [](Dynarmic::A32::Jit& jit, DynarmicHost& h) {
             // DMB ISH before sub_468650 releases the lock; it writes its payload itself before returning
             std::atomic_thread_fence(std::memory_order_seq_cst);
             const uint32_t fp = jit.Regs()[11];
             OpenWindow(jit, h, fp + 4, h.MemoryRead32(fp + 4));
         }},
        {0x467BB0, 0xE5957000, "nfs_rq_swap_begin", SwapBegin},
        {0x467E48, 0xE59F0074, "nfs_rq_swap_end", SwapEnd},
        {0x4688F0, 0xE28DB01C, "nfs_rq_grow", OnGrow},
    };
    constexpr uint32_t kCapacity = 0x3EABC4, kCapacityOriginal = 0xE3A01901, kCapacityPatched = 0xE3A01601;  // MOV R1, #0x4000 -> #0x100000
    if (!Verify(host, sites, std::size(sites)) || host.MemoryRead32(kBase + kCapacity) != kCapacityOriginal) {
        LOGE("render queue fix skipped: libapp code differs");
        return;
    }
    g_close_trampoline = host.GetThunkManager().RegisterThunk("nfs_rq_close", CloseWindow);
    Hook(host, sites, std::size(sites));
    host.MemoryWrite32(kBase + kCapacity, kCapacityPatched);
    host.InvalidateGuestCode(kBase + kCapacity, 4);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "render queue fix installed (%zu hooks, resource queue buffers 1 MiB)", std::size(sites));
}
}  // namespace renderqueue

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------
bool RunGuestJniOnLoad(JNIEnv* env, jclass cls, const char* module) {
    const uint32_t entry = g_host->FindLibrarySymbol(module, "JNI_OnLoad");
    if (!entry) {
        LOGE("guest %s exports no JNI_OnLoad", module);
        return false;
    }
    FakeJniEnv& jni = FakeJniEnv::Instance();
    if (!jni.BeginNativeCall(env)) return false;
    const uint32_t version = g_host->RunFunction(entry, {jni.VmTableGuestAddr(), 0});
    jni.EndNativeCall();
    (void)cls;
    if (g_host->HasFailed() || jni.HasFailed()) return false;
    LOGI("guest %s JNI_OnLoad -> 0x%08X", module, version);
    return version == JNI_VERSION_1_2 || version == JNI_VERSION_1_4 || version == JNI_VERSION_1_6;
}

// translated: NfsNative found the process running on another CPU through a native bridge (os.arch is not aarch64,
// e.g. an x86_64 PC emulator); the JIT then keeps its exact settings (DynarmicHost::Initialize).
jboolean N_BootGuest(JNIEnv* env, jclass cls, jobject asset_manager, jboolean translated) {
    std::lock_guard lock(g_boot_mutex);
    if (g_host) return JNI_TRUE;
    if (!asset_manager) {
        LOGE("guest boot refused: no AssetManager");
        return JNI_FALSE;
    }
    g_assets_ref = env->NewGlobalRef(asset_manager);  // process lifetime: JIT profile at the first onSurfaceCreated
    AAssetManager* assets = AAssetManager_fromJava(env, g_assets_ref);
    if (!assets) {
        LOGE("guest boot refused: no AssetManager");
        return JNI_FALSE;
    }
    g_assets = assets;

    std::vector<std::unique_ptr<GuestElfMapping>> mappings;
    std::vector<GuestModuleImage> images;
    for (const auto& module : cfg::kModules) {
        auto mapping = std::make_unique<GuestElfMapping>();
        if (!mapping->Open(assets, std::string(cfg::kGuestLibDir) + module.name, module.base)) return JNI_FALSE;
        images.push_back({module.name, mapping->data, mapping->size, module.base, module.limit});
        mappings.push_back(std::move(mapping));
    }

    auto host = std::make_unique<DynarmicHost>();
    if (!host->Initialize(translated == JNI_TRUE) || !host->RunSelfChecks()) {
        LOGE("Dynarmic host initialization or self-check failed");
        return JNI_FALSE;
    }
    g_host = std::move(host);
    FakeJniEnv& jni = FakeJniEnv::Instance();
    jni.SetJavaVM(g_jvm);
    if (!jni.Init(*g_host) || !jni.RunSelfChecks()) GuestFailed("FakeJniEnv initialization");
    jni.AttachHostEnv(env);

    // Relocate all images, then run their constructors in load order.
    if (!g_host->LoadModulesFromMemory(images)) GuestFailed("guest module loading");
    mappings.clear();  // the images now live in the guest arena
    lensflare::Install(*g_host, 0x40000000u);  // libapp base (cfg::kModules), before any guest code runs
    renderqueue::Install(*g_host);
    qa::InstallHelpHooks(*g_host);
    portopts::Install(*g_host, 0x40000000u);   // settings rows of the port (stages 10 and 13)
    graphics::Install(*g_host, 0x40000000u);   // render scale and nitro motion blur (stage 13)
    portstrings::Install(*g_host, 0x40000000u);  // the rows' strings in the engine's string table (stage 13)

    for (unsigned id = 0; id < kNativeCount; ++id) {
        g_entry[id] = g_host->FindLibrarySymbol(kGuestEntries[id].module, kGuestEntries[id].symbol);
        if (!g_entry[id]) GuestFailed(kGuestEntries[id].symbol);
    }
    for (const char* module : cfg::kJniOnLoadModules)
        if (!RunGuestJniOnLoad(env, cls, module)) GuestFailed(module);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "ARM32 guest ready: %zu modules, %u natives forwarded",
                        std::size(cfg::kModules), static_cast<unsigned>(kNativeCount));
    StartMemoryReport();
    StartFrameDiag();
    return JNI_TRUE;
}

// ---------------------------------------------------------------------------
// Registration. RegisterNatives is all-or-nothing per class: every entry below
// matches a native declared in app/smali and exported by the ARM32 guest (the
// declared-but-never-exported GameActivityMain.nativeOnPhysicalKeyboardVisibilityChanged
// and NimbleCppComponentRegistrar$NimbleCppComponent natives stay unbound, as on ARM32).
// ---------------------------------------------------------------------------
#define NATIVE(name, signature, function) {name, signature, reinterpret_cast<void*>(function)}

const JNINativeMethod kGameActivityMain[] = {
    NATIVE("nativeOnCreate", "()V", N_OnCreate),
    NATIVE("nativeOnDestroy", "()V", N_OnDestroy),
    NATIVE("nativeOnMusicPlayerStateChanged", "()V", N_OnMusicPlayerStateChanged),
    NATIVE("nativeOnOrientationChange", "(I)V", N_OnOrientationChange),
    NATIVE("nativeOnPause", "()V", N_OnPause),
    NATIVE("nativeOnPhysicalKeyDown", "(II)V", N_OnPhysicalKeyDown),
    NATIVE("nativeOnPhysicalKeyUp", "(II)V", N_OnPhysicalKeyUp),
    NATIVE("nativeOnPhysicalNavigationVisibilityChanged", "(Z)V", N_OnPhysicalNavigationVisibilityChanged),
    NATIVE("nativeOnRestart", "()V", N_OnRestart),
    NATIVE("nativeOnResume", "()V", N_OnResume),
    NATIVE("nativeOnStart", "()V", N_OnStart),
    NATIVE("nativeOnStop", "()V", N_OnStop),
    NATIVE("nativeRestoreContext", "()Z", N_RestoreContext),
    NATIVE("nativeSurfaceChanged", "(Ljavax/microedition/khronos/opengles/GL10;II)V", N_SurfaceChanged),
    NATIVE("nativeSurfaceCreated", "(Ljavax/microedition/khronos/opengles/GL10;Ljavax/microedition/khronos/egl/EGLConfig;)V",
           N_SurfaceCreated),
};
const JNINativeMethod kGameGLSurfaceView[] = {
    NATIVE("nativeTouchPadEvent", "(IIFF)V", N_TouchPadEvent),
    NATIVE("nativeTouchScreenEvent", "(IIFF)V", N_TouchScreenEvent),
};
const JNINativeMethod kRunLoop[] = {
    NATIVE("nativeOnRunLoopTick", "()V", N_RunLoopTick),
};
const JNINativeMethod kMogaController[] = {
    NATIVE("nativeOnKeyEvent", "(Lcom/bda/controller/KeyEvent;)V", N_MogaKeyEvent),
    NATIVE("nativeOnMotionEvent", "(Lcom/bda/controller/MotionEvent;)V", N_MogaMotionEvent),
    NATIVE("nativeOnStateEvent", "(Lcom/bda/controller/StateEvent;)V", N_MogaStateEvent),
};
const JNINativeMethod kEaio[] = {
    NATIVE("Shutdown", "()V", N_EaioShutdown),
    NATIVE("StartupNativeImpl", "(Landroid/content/res/AssetManager;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
           N_EaioStartup),
};
const JNINativeMethod kStorageDirectory[] = {
    NATIVE("ShutdownNativeImpl", "()V", N_StorageShutdown),
    NATIVE("StartupNativeImpl", "()V", N_StorageStartup),
};
const JNINativeMethod kBaseNativeCallback[] = {
    NATIVE("nativeCallback", "(I[Ljava/lang/Object;)V", N_NimbleCallback),
    NATIVE("nativeFinalize", "(I)V", N_NimbleFinalize),
};
const JNINativeMethod kFmodAudioDevice[] = {
    NATIVE("fmodGetInfo", "(I)I", N_FmodGetInfo),
    NATIVE("fmodProcess", "(Ljava/nio/ByteBuffer;)I", N_FmodProcess),
};
const JNINativeMethod kNfsQa[] = {
    NATIVE("nativeAction", "(II)I", N_QaAction),
};
const JNINativeMethod kNfsVfs[] = {
    NATIVE("nativeInstall", "(Ljava/lang/String;IJJ)I", N_NfsVfsInstall),
    NATIVE("nativeStats", "()[J", N_NfsVfsStats),
};
const JNINativeMethod kNfsNative[] = {
    NATIVE("nativeBootGuest", "(Landroid/content/res/AssetManager;Z)Z", N_BootGuest),
};
#undef NATIVE

struct NativeTable {
    const char* cls;
    const JNINativeMethod* methods;
    jint count;
    // The guest itself calls RegisterNatives for this class (MogaController init sub_266DD8
    // rebinds the three exported nativeOn*Event functions through a classLoader-loaded class);
    // the fake JNIEnv accepts only registrations of natives declared to it.
    bool guest_registers = false;
};
#define TABLE(cls, methods) {cls, methods, static_cast<jint>(std::size(methods))}
#define GUEST_REGISTERED_TABLE(cls, methods) {cls, methods, static_cast<jint>(std::size(methods)), true}
const NativeTable kTables[] = {
    TABLE("com/ea/ironmonkey/port/NfsNative", kNfsNative),
    TABLE(cfg::kActivityClass, kGameActivityMain),
    TABLE("com/ea/ironmonkey/GameGLSurfaceView", kGameGLSurfaceView),
    TABLE("com/ea/ironmonkey/RunLoop", kRunLoop),
    GUEST_REGISTERED_TABLE("com/ea/ironmonkey/MogaController", kMogaController),
    TABLE("com/ea/EAIO/EAIO", kEaio),
    TABLE("com/ea/EAMIO/StorageDirectory", kStorageDirectory),
    TABLE("com/ea/nimble/bridge/BaseNativeCallback", kBaseNativeCallback),
    TABLE("org/fmod/FMODAudioDevice", kFmodAudioDevice),
    TABLE("com/ea/ironmonkey/port/NfsQa", kNfsQa),
    TABLE("com/ea/ironmonkey/port/NfsVfs", kNfsVfs),
};
#undef TABLE
#undef GUEST_REGISTERED_TABLE

}  // namespace

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    logs::Install(env);  // release builds are silent unless debug.nfsbridge.logs=1 (stage 12)
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "JNI_OnLoad: arm64 host for the ARM32 NFS Most Wanted guest");

    jclass activity = env->FindClass(cfg::kActivityClass);
    if (!activity) {
        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
        LOGE("class not found: %s", cfg::kActivityClass);
        return JNI_ERR;
    }
    FakeJniEnv::Instance().SetJavaVM(vm);
    if (!FakeJniEnv::Instance().CacheClassLoader(env, activity)) {
        LOGE("could not cache the application class loader");
        return JNI_ERR;
    }
    env->DeleteLocalRef(activity);

    for (const auto& table : kTables) {
        jclass cls = env->FindClass(table.cls);
        if (!cls || env->RegisterNatives(cls, table.methods, table.count) != JNI_OK ||
            (table.guest_registers &&
             !FakeJniEnv::Instance().DeclareNatives(env, cls, table.methods, static_cast<size_t>(table.count)))) {
            if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
            LOGE("RegisterNatives failed for %s", table.cls);
            return JNI_ERR;
        }
        env->DeleteLocalRef(cls);
        LOGI("registered %d natives on %s", table.count, table.cls);
    }
    qa::CacheJava(env);
    if (!framerate::Register(env)) LOGE("frame rate limit unavailable (NfsFrameRate)");
    if (!portopts::Register(env)) LOGE("port options unavailable (NfsOptions)");
    return JNI_VERSION_1_6;
}
