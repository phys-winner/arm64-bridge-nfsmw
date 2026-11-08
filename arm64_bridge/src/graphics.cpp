// Stage 13 graphics options (user request 2026-10-10: render scale 50-100 % in steps of 10, default 100 %; graphics
// quality tiers 1-4 and MAX = tier 4 + the port's own effects; an optional stronger motion blur while the nitrous
// burns, off by default). The rows are port options (options.cpp); this file applies them to the engine.
//
// Render scale. SceneRenderer_CreateOffscreenBuffer 0x1F9C7C makes the scene framebuffer the size of the screen and
// stores the width of the 3D viewport at SceneRenderer+0x90 (screen width x 0.8 in the original, x 1.0 since GU-01).
// SceneRenderer_Render 0x1FAA28 reads that width every frame (0x1FAE1C LDR R6, [R4, #0x90]), sizes the framebuffer's
// viewport to width x width/aspect and draws the result over the whole screen with UV = viewport / texture; the post
// effects read the same field. The hook at 0x1FAE1C scales the engine's width by the option (50..100 %) before that
// read, so a change applies from the next frame, in a race too. The engine's own value is kept as the base: a value
// the hook did not write (a new SceneRenderer, a new surface) becomes the new base.
//
// Nitro motion blur. The race camera's motion blur (SceneRenderer_BuildPostFXMaterials 0x1F8CA0, amount eased in
// 0x1FC8A4 from the car speed between MotionBlurSpeedMin/Max 225/325 km/h, or MotionBlurNitroSpeedMin/Max 50/200 km/h
// while SceneRenderer+0x148 -> +0xB4 says the nitrous burns) is drawn by 0x1FC038 with BlurSize (+0x108, 0.002), the
// motion vectors (+0x114 x +0x118 x amount: the streaks, BlurSize their length) and the radial mask
// smoothstep(NitroBiasA +0x10C, NitroBiasB +0x110 = 28, |motion|^2), the alpha with which the blurred picture covers
// the sharp one (material 0x1FF550: vec4(blurred.xyz, mask), 0x1FFE50 and 0x20142C). With the option on, the hook at
// 0x1FC03C (ADD R11, SP, #0x18 after the PUSH) raises BlurSize to 2.2x while the nitrous burns and widens the mask
// (NitroBiasB 1.15x), so the streaks get much longer at the edges while the car in the middle stays as sharp as
// without the option (b1 halved NitroBiasB instead: the blur reached the car and doubled it), eased in over 0.35 s
// and out over 0.5 s; the nitro speed range becomes 20..120 km/h (Tweaks+0xDA8/+0xDA4), so the full effect starts at
// lower speeds. Off restores the engine's values.
//
// Graphics quality (once per process, from Java before the engine starts; a change in the settings applies at the next
// start). Platform_GetDeviceTier 0x547B40 returns the cached tier dword_ADFD10 without measuring when byte_ADFD0C is
// set (the device-name override path); the bridge sets both to the chosen tier: 1 Low (21), 2 Medium (22), 3 High
// (23), 4 Highest (24); Quality_GetPerformanceLevel maps them to the engine's levels 0, 2, 3, 4 and
// Quality_BuildFeatureMask builds the matching feature mask at its first use. MAX is tier 4 plus the port's effects:
// DE-03 (bit 24 of the level-4 mask, libapp patch 0x3A4FE0), DE-05 lens flare occlusion queries, MSAA 4x, anisotropic
// filtering 8x and the GU-07/GU-08 tweak overlay entries (longer LOD and shadow distances, 1024 car reflection map).
// Below MAX the bridge restores the original MOVT at 0x3A4FE0 and the other effects stay off (thunks_gl.cpp,
// jni_bridge.cpp lensflare, thunks_libc.cpp overlay entries marked "max").
//
// iOS colour grading (stage 16, user request 2026-10-15: the red/grey look of the boss cutscenes, the end of the game
// and the race results is the iOS effect DE-03, wanted as a switch like the nitro blur, off by default). With bit 24
// the SceneRenderer constructor sets +0x9C (bit 24 && GL_EXT_shader_framebuffer_fetch, 0x1F6DA4) and builds two
// framebuffer-fetch colour-ramp materials; SceneRenderer_Render calls the pass 0x1FB958 when +0x9C is set (LDRB R0,
// [R4, #0x9C] at 0x1FAF54, BLNE at 0x1FAF64). The pass draws whenever the engine's ramp tween (+0xA0..+0xBC, advanced
// in 0x1FC8A4 by PhysicsSubSystem+0x70) gives a mix above 0.01: the scene's background is mapped to dark red..red
// (+0xC4/+0xCC/+0xDC/+0xE4) and the stencil-marked cars and track to black..light grey (+0xC0/+0xC8/+0xD8/+0xE0).
// The engine mixes it in for its cinematic scenes (seen: Most Wanted race intros, race results; reported: the ending).
// The hook at 0x1FAF54 replaces that LDRB: R0 = the gate while the option is on, else 0, so the pass is skipped and
// the scene keeps its own colours; it applies from the next frame. Only MAX has bit 24, so the option does nothing
// below MAX (the row's note says so). The rest of bit 24 (depth-stencil offscreen buffer, stencil flags of the car
// and track materials) stays as it is: without the pass it does not change the picture.
//
// Car shader on Adreno (stage 16, b33; reports 2026-10-15: cars dark in the menu and in races since 1.1 on an Adreno
// phone, with a list of affected cars; the Adreno 650 here renders them as 1.0 does). Platform_GetDeviceTier lowers
// every Adreno above 225 by one tier, so the original game never ran the level-4 car shader on Adreno and 1.0 did not
// either (level 3 on the Adreno 650 here); since 1.1, quality 4 and MAX force tier 24 there. Two bits of the level-4
// mask belong to the car shader alone: bit 15 (CarShader+0x60, read at 0x2CF0CC: the sun's per-pixel specular term
// and the smooth damage-swap blend 1.0 - g_DamageSwapArray[i] / smoothstep / mix instead of step) and bit 26
// (CarShaderSubSystem+0xDA, 0x2DEC1C: the blurred vinyl hemisphere map +0xE4 drawn every frame for the matte and
// vinyl paints, the NFS Edition paint among them). On Adreno (ro.hardware.egl or ro.hardware.vulkan "adreno", else
// the vendor's libGLESv2_adreno.so) the bridge builds the level-4 mask without them (MOVW at 0x3A4FDC #0xD7E2 ->
// #0x57E2, MOVT at 0x3A4FE0 without 0x0400): the cars are shaded as in 1.0, the rest of the level (DE-03 at MAX, the
// post effects, the port's effects) stays. setprop debug.nfsbridge.carshading 4 (the level-4 car shader on every GPU)
// or 3 (the level-3 one on every GPU) before launch overrides the GPU check, for A/B runs.
#include "graphics.h"

#include <android/log.h>
#include <strings.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>
#include <utility>

#include "dynarmic_host.h"
#include "options.h"
#include "thunk_manager.h"

#define LOG_TAG "Arm32Bridge_Graphics"
#include "bridge_log.h"

namespace graphics {
namespace {

// libapp 1.3.128 offsets (cfg::kModules base 0x40000000)
constexpr uint32_t kScaleHook = 0x1FAE1C;        // LDR R6, [R4, #0x90] in SceneRenderer_Render
constexpr uint32_t kScaleInsn = 0xE5946090;
constexpr uint32_t kBlurHook = 0x1FC03C;         // ADD R11, SP, #0x18 after PUSH {R4-R8, R10, R11, LR}
constexpr uint32_t kBlurInsn = 0xE28DB018;
constexpr uint32_t kBlurPush = 0xE92D4DF0;
constexpr uint32_t kViewportWidth = 0x90;        // SceneRenderer: 3D viewport width in pixels
constexpr uint32_t kBlurSize = 0x108, kNitroBiasB = 0x110, kNitroCar = 0x148, kNitroActive = 0xB4;
constexpr uint32_t kTweaksPointer = 0xAD6044, kTweaksGuard = 0xAD604C;  // Tweaks singleton (sub_341FB8)
constexpr uint32_t kNitroSpeedMax = 0xDA4, kNitroSpeedMin = 0xDA8;      // MotionBlurNitroSpeedMax/Min, km/h
constexpr uint32_t kTierFunction = 0x547B40, kTierFunctionInsn = 0xE92D4FF0;  // Platform_GetDeviceTier PUSH
constexpr uint32_t kTierOverride = 0xADFD0C, kTierValue = 0xADFD10;
constexpr uint32_t kMaskGuard = 0xAD6528;        // Quality_GetFeatureMask static guard
constexpr uint32_t kMaskMovw = 0x3A4FDC, kMaskMovt = 0x3A4FE0;
constexpr uint32_t kMovwOriginal = 0xE30D07E2;   // MOVW R0, #0xD7E2: low half of the level-4 mask
constexpr uint32_t kMovtOriginal = 0xE3400EBF;   // MOVT R0, #0x0EBF: level-4 mask 0x0EBFD7E2
constexpr uint32_t kMovtDe03 = 0xE3400FBF;       // MOVT R0, #0x0FBF: + bit 24 (DE-03, libapp_de03_colour_ramp.json)
constexpr uint32_t kCarLevel4Bits = 0x04008000;  // bits 26 and 15: the level-4 parts of the car shader (Adreno)
constexpr uint32_t kRampGateHook = 0x1FAF54;     // LDRB R0, [R4, #0x9C] in SceneRenderer_Render before BLNE 0x1FB958
constexpr uint32_t kRampGateInsn = 0xE5D4009C;
constexpr uint32_t kRampCall = 0x1FAF64, kRampCallInsn = 0x1B00027B;  // BLNE sub_1FB958 (the colour-ramp pass)
constexpr uint32_t kRampGate = 0x9C;             // SceneRenderer: bit 24 && framebuffer fetch

constexpr float kScales[] = {0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f};
constexpr int kTiers[] = {21, 22, 23, 24, 24};
constexpr float kBlurBoost = 1.2f;   // BlurSize x (1 + 1.2): streak length
constexpr float kBiasGrow = 0.15f;   // NitroBiasB x (1 + 0.15): the mask starts a little further out
constexpr float kBoostedNitroSpeedMin = 20.0f, kBoostedNitroSpeedMax = 120.0f;

uint32_t g_base = 0;

float AsFloat(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }
uint32_t Bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

int64_t Now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// Per SceneRenderer (GL thread only; the race and the front end each have one).
struct Renderer {
    uint32_t address = 0;
    uint32_t base_width = 0, written_width = 0;
    uint32_t base_blur = 0, written_blur = 0, base_bias = 0, written_bias = 0;
    float boost = 0.0f;
    int64_t last = 0;
};
Renderer g_renderers[4];

Renderer& Find(uint32_t address) {
    for (auto& r : g_renderers)
        if (r.address == address) return r;
    for (auto& r : g_renderers)
        if (!r.address) { r.address = address; return r; }
    // more renderers than slots: reuse the first (a renderer seen before simply re-reads its base)
    g_renderers[0] = Renderer{};
    g_renderers[0].address = address;
    return g_renderers[0];
}

// Render scale (GL thread, every frame of an offscreen scene).
void OnViewportWidth(Dynarmic::A32::Jit& jit, DynarmicHost& h) {
    const uint32_t renderer = jit.Regs()[4];
    const uint32_t value = h.MemoryRead32(renderer + kViewportWidth);
    Renderer& r = Find(renderer);
    if (value != r.written_width || !r.base_width) r.base_width = value;
    const int index = portopts::Value(portopts::kRenderScale);
    uint32_t width = r.base_width;
    if (index >= 0 && index < 5) width = static_cast<uint32_t>(std::lround(r.base_width * kScales[index]));
    if (width < 16) width = r.base_width;
    if (width != value) h.MemoryWrite32(renderer + kViewportWidth, width);
    r.written_width = width;
    jit.Regs()[6] = width;
}

// Nitro speed range in the Tweaks singleton (once it exists).
struct NitroTweaks {
    bool saved = false;
    uint32_t speed_min = 0, speed_max = 0;
    int applied = -1;  // option value the tweaks were last set for
};
NitroTweaks g_nitro_tweaks;

void ApplyNitroTweaks(DynarmicHost& h, int on) {
    if (g_nitro_tweaks.applied == on) return;
    if (!(h.MemoryRead8(g_base + kTweaksGuard) & 1)) return;
    const uint32_t tweaks = h.MemoryRead32(g_base + kTweaksPointer);
    if (!tweaks) return;
    if (!g_nitro_tweaks.saved) {
        g_nitro_tweaks.speed_min = h.MemoryRead32(tweaks + kNitroSpeedMin);
        g_nitro_tweaks.speed_max = h.MemoryRead32(tweaks + kNitroSpeedMax);
        g_nitro_tweaks.saved = true;
    }
    h.MemoryWrite32(tweaks + kNitroSpeedMin, on ? Bits(kBoostedNitroSpeedMin) : g_nitro_tweaks.speed_min);
    h.MemoryWrite32(tweaks + kNitroSpeedMax, on ? Bits(kBoostedNitroSpeedMax) : g_nitro_tweaks.speed_max);
    g_nitro_tweaks.applied = on;
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "nitro motion blur %s: nitro blur speed range %.0f..%.0f km/h", on ? "on" : "off",
                        AsFloat(h.MemoryRead32(tweaks + kNitroSpeedMin)), AsFloat(h.MemoryRead32(tweaks + kNitroSpeedMax)));
}

// Motion blur draw (GL thread, every frame of the race camera's post effect).
void OnMotionBlur(Dynarmic::A32::Jit& jit, DynarmicHost& h) {
    jit.Regs()[11] = jit.Regs()[13] + 0x18;  // the replaced ADD R11, SP, #0x18
    const uint32_t renderer = jit.Regs()[0];
    const int on = portopts::Value(portopts::kNitroBlur) ? 1 : 0;
    ApplyNitroTweaks(h, on);
    Renderer& r = Find(renderer);
    const uint32_t blur = h.MemoryRead32(renderer + kBlurSize), bias = h.MemoryRead32(renderer + kNitroBiasB);
    if (blur != r.written_blur || !r.base_blur) r.base_blur = blur;
    if (bias != r.written_bias || !r.base_bias) r.base_bias = bias;
    bool nitro = false;
    if (on) {
        const uint32_t car = h.MemoryRead32(renderer + kNitroCar);
        nitro = car && h.MemoryRead8(car + kNitroActive);
    }
    const int64_t now = Now();
    const float dt = r.last ? static_cast<float>(now - r.last) * 1e-9f : 0.0f;
    r.last = now;
    if (nitro) r.boost = std::fmin(1.0f, r.boost + dt / 0.35f);
    else r.boost = std::fmax(0.0f, r.boost - dt / 0.5f);
    if (!on) r.boost = 0.0f;
    const uint32_t new_blur = r.boost > 0.0f ? Bits(AsFloat(r.base_blur) * (1.0f + kBlurBoost * r.boost)) : r.base_blur;
    const uint32_t new_bias = r.boost > 0.0f ? Bits(AsFloat(r.base_bias) * (1.0f + kBiasGrow * r.boost)) : r.base_bias;
    if (new_blur != blur) h.MemoryWrite32(renderer + kBlurSize, new_blur);
    if (new_bias != bias) h.MemoryWrite32(renderer + kNitroBiasB, new_bias);
    r.written_blur = new_blur;
    r.written_bias = new_bias;
}

// Colour-ramp gate (GL thread, every frame of a SceneRenderer): the replaced LDRB R0, [R4, #0x9C], 0 while the iOS
// colour grading is off.
std::atomic<int> g_ramp_logged{-1};  // last logged state: -1 none, 0 pass skipped, 1 pass allowed

void OnColourRampGate(Dynarmic::A32::Jit& jit, DynarmicHost& h) {
    const uint32_t gate = h.MemoryRead8(jit.Regs()[4] + kRampGate);
    const int on = portopts::Value(portopts::kColourGrading) ? 1 : 0;
    jit.Regs()[0] = on ? gate : 0;
    if (gate && g_ramp_logged.exchange(on, std::memory_order_relaxed) != on)
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "iOS colour grading (DE-03 colour-ramp pass) %s",
                            on ? "drawn when the engine mixes it in" : "skipped (option off)");
}

// Crash cutscene trace (diagnostic, setprop debug.nfsbridge.cuttrace 1 before launch; stage 14): CrashCutscene::Update
// 0x13E118 advances its clock (+0xD4, ends at Duration +0x94) by the physics step's real time PhysicsSubSystem+0x70
// (physics_update.S). The hook replaces its MOV R4, R0 at 0x13E128 and logs, at most every 0.25 s, the cutscene clock
// against the real time since that cutscene's first update: both advance alike when the timing is right.
constexpr uint32_t kCutsceneHook = 0x13E128, kCutsceneInsn = 0xE1A04000;  // MOV R4, R0
struct CutsceneTrace {
    uint32_t cutscene = 0;
    int64_t first = 0, logged = 0;
    float first_clock = 0.0f;
};
CutsceneTrace g_cutscene_trace;

void OnCrashCutscene(Dynarmic::A32::Jit& jit, DynarmicHost& h) {
    const uint32_t cutscene = jit.Regs()[0];
    jit.Regs()[4] = cutscene;  // the replaced MOV R4, R0
    const float clock = AsFloat(h.MemoryRead32(cutscene + 0xD4)), duration = AsFloat(h.MemoryRead32(cutscene + 0x94));
    const uint32_t scene = h.MemoryRead32(cutscene + 0x14);
    const uint32_t physics = scene ? h.MemoryRead32(scene + 0x88) : 0;
    const float step = physics ? AsFloat(h.MemoryRead32(physics + 0x70)) : 0.0f;
    const float scale = physics ? AsFloat(h.MemoryRead32(physics + 0x6C)) : 0.0f;
    const int64_t now = Now();
    auto& t = g_cutscene_trace;
    if (cutscene != t.cutscene || clock < t.first_clock) { t = {cutscene, now, 0, clock}; }
    if (now - t.logged < 250000000LL) return;
    t.logged = now;
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "crash cutscene 0x%08x: clock %.3f of %.1f s after %.3f s real, step %.2f ms, "
                        "time scale %.3f", cutscene, clock - t.first_clock, duration, (now - t.first) * 1e-9, step * 1e3, scale);
}

// ARM MOVW/MOVT R0, #imm16 and the imm16 of either.
uint32_t MovwR0(uint32_t imm16) { return 0xE3000000u | (imm16 & 0xF000u) << 4 | (imm16 & 0x0FFFu); }
uint32_t MovtR0(uint32_t imm16) { return 0xE3400000u | (imm16 & 0xF000u) << 4 | (imm16 & 0x0FFFu); }
uint32_t Imm16(uint32_t insn) { return (insn >> 4 & 0xF000u) | (insn & 0x0FFFu); }

// Graphics quality: the level-3 car shader at tier 24? Adreno, or debug.nfsbridge.carshading 3 / 4; reason says why.
bool Level3CarShader(char* reason, size_t size) {
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.nfsbridge.carshading", value) > 0 && (value[0] == '3' || value[0] == '4')) {
        snprintf(reason, size, "debug.nfsbridge.carshading %c", value[0]);
        return value[0] == '3';
    }
    for (const char* prop : {"ro.hardware.egl", "ro.hardware.vulkan"}) {
        if (__system_property_get(prop, value) > 0 && strcasecmp(value, "adreno") == 0) {
            snprintf(reason, size, "Adreno: %s=%s", prop, value);
            return true;
        }
    }
    for (const char* lib : {"/vendor/lib64/egl/libGLESv2_adreno.so", "/vendor/lib/egl/libGLESv2_adreno.so",
                            "/system/vendor/lib64/egl/libGLESv2_adreno.so", "/system/vendor/lib/egl/libGLESv2_adreno.so"}) {
        if (access(lib, F_OK) == 0) {
            snprintf(reason, size, "Adreno: %s", lib);
            return true;
        }
    }
    snprintf(reason, size, "not Adreno");
    return false;
}

}  // namespace

void Install(DynarmicHost& host, uint32_t libapp_base) {
    g_base = libapp_base;
    struct Hook { uint32_t offset; uint32_t insn; uint32_t before, before_insn; const char* name; ThunkHandler handler; };
    char cuttrace[PROP_VALUE_MAX]{};
    const bool trace = __system_property_get("debug.nfsbridge.cuttrace", cuttrace) > 0 && cuttrace[0] == '1';
    const Hook hooks[] = {
        {kScaleHook, kScaleInsn, 0, 0, "nfs_render_scale", OnViewportWidth},
        {kBlurHook, kBlurInsn, kBlurHook - 4, kBlurPush, "nfs_nitro_blur", OnMotionBlur},
        {kRampGateHook, kRampGateInsn, kRampCall, kRampCallInsn, "nfs_colour_grading", OnColourRampGate},
        {trace ? kCutsceneHook : 0, kCutsceneInsn, 0, 0, "nfs_crash_cutscene_trace", OnCrashCutscene},
    };
    for (const auto& hook : hooks) {
        if (!hook.offset) continue;
        if (host.MemoryRead32(libapp_base + hook.offset) != hook.insn ||
            (hook.before && host.MemoryRead32(libapp_base + hook.before) != hook.before_insn)) {
            LOGE("graphics: libapp+0x%x is not the expected instruction, %s stays off", hook.offset, hook.name);
            continue;
        }
        const uint32_t trampoline = host.GetThunkManager().RegisterThunk(hook.name, hook.handler);
        host.MemoryWrite32(libapp_base + hook.offset, host.MemoryRead32(trampoline));  // the trampoline's SVC #id
        host.InvalidateGuestCode(libapp_base + hook.offset, 4);
    }
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "render scale, nitro motion blur and colour grading hooks installed");
}

void ApplySessionQuality(DynarmicHost& host, uint32_t libapp_base, int quality) {
    if (quality < 0 || quality > portopts::kQualityMax) return;
    if (host.MemoryRead32(libapp_base + kTierFunction) != kTierFunctionInsn) {
        LOGE("graphics quality: Platform_GetDeviceTier differs from 1.3.128, the engine keeps its own tier");
        return;
    }
    if (host.MemoryRead8(libapp_base + kMaskGuard) & 1)
        LOGE("graphics quality: the feature mask was already built, it keeps the engine's tier until the next start");
    host.MemoryWrite32(libapp_base + kTierValue, static_cast<uint32_t>(kTiers[quality]));
    host.MemoryWrite8(libapp_base + kTierOverride, 1);
    // The level-4 mask (tier 24: quality 4 and MAX): DE-03 only at MAX, on Adreno without the level-4 car shader.
    const char* de03 = "kept";
    uint32_t movt = host.MemoryRead32(libapp_base + kMaskMovt);
    if (quality != portopts::kQualityMax && movt == kMovtDe03) {
        movt = kMovtOriginal;
        de03 = "off (original level-4 mask)";
    } else if (movt != kMovtDe03 && movt != kMovtOriginal) {
        de03 = "unknown instruction, untouched";
    }
    uint32_t movw = host.MemoryRead32(libapp_base + kMaskMovw);
    char reason[96] = "";
    const char* cars = "of the tier";
    if (kTiers[quality] == 24) {
        cars = "level 4";
        if (Level3CarShader(reason, sizeof(reason))) {
            if (movw == kMovwOriginal && (movt == kMovtDe03 || movt == kMovtOriginal)) {
                movw = MovwR0(Imm16(movw) & ~kCarLevel4Bits);
                movt = MovtR0(Imm16(movt) & ~(kCarLevel4Bits >> 16));
                cars = "level 3";
            } else {
                cars = "level 4 (unknown instruction, untouched)";
            }
        }
    }
    for (const auto& [offset, insn] : {std::pair<uint32_t, uint32_t>{kMaskMovw, movw}, {kMaskMovt, movt}}) {
        if (host.MemoryRead32(libapp_base + offset) == insn) continue;
        host.MemoryWrite32(libapp_base + offset, insn);
        host.InvalidateGuestCode(libapp_base + offset, 4);
    }
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "graphics quality %d: engine tier %d forced, DE-03 %s, port effects %s, car shader %s%s%s%s",
                        quality + 1, kTiers[quality], de03, quality == portopts::kQualityMax ? "on" : "off", cars,
                        reason[0] ? " (" : "", reason, reason[0] ? ")" : "");
}

}  // namespace graphics
