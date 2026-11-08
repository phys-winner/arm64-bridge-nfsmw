// The port's options in the game's own settings pages (stage 10 frame rate limit, stage 13 render scale, graphics
// quality and nitro motion blur, stage 16 iOS colour grading; user requests 2026-10-08, 2026-10-10 and 2026-10-15).
//
// The settings pages are data (published/layouts/layouts.sb in the OBB overlay, built by work/tools/settings_ui.py):
// every port row is a RadioOptionWidget bound to an option name ("FrameRateLimit", "RenderScale", "GraphicsQuality",
// "NitroMotionBlur", "ColourGrading"). The widget finds its option with Options::FindOption 0x24D3A4 (name compare over the 12 options
// of im::app::metagame::Options), casts it to IntOption and then does only three things with it: reads the value
// through the option's database object (sub_7ABF8; with an invalid object it falls back to the default slot +0x44),
// compares it with each child's RadioIndex every frame (Update 0x378AE8) and calls IntOption::Set 0x249EA0 with the
// RadioIndex of a tapped child (0x378BC8). The engine's own options are fields of the save's Options database object;
// a new field there would change the save, so the bridge keeps an IntOption of its own per port option in guest
// memory: the engine's IntOption vtable, an invalid database object (no reads, no writes), the name, the range and
// the current value in the default slot. Hooks (the replaced instruction is emulated on every other path):
//   0x24D3A8  FindOption, ADD R11, SP, #0x18 after the PUSH: a port option's name -> the bridge's option, returned
//             through the function's own epilogue 0x24D414; every other name runs the original search
//   0x249EA4  IntOption::Set, MOV R11, SP after PUSH {R11, LR}: this == a bridge option -> new value, return
//   0x24D36C  Options vtable +8 (ADD R11, SP, #8), the SaveManager broadcast of the DEFAULT SETTINGS answer (0x379184
//             -> 0x260164, flags byte 1 set): the bridge's options go back to their defaults with the engine's options
// The values are not in the save: Java (NfsOptions, NfsFrameRate) keeps them in the port's preferences, passes them in
// at start and saves every change the bridge reports.
#include "options.h"

#include <android/log.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>

#include "dynarmic_host.h"
#include "graphics.h"
#include "thunk_manager.h"

#define LOG_TAG "Arm32Bridge_Options"
#include "bridge_log.h"

JavaVM* GetHostJavaVM();  // jni_bridge.cpp

namespace portopts {
namespace {

struct Spec {
    const char* name;  // the layout's OptionName
    const char* key;   // the debug key string of the IntOption (never shown)
    int last;
    int def;
    const char* (*describe)(int value);
};

const char* DescribeRate(int v) {
    static const char* const kNames[] = {"30 fps", "45 fps", "60 fps", "display maximum"};
    return v >= 0 && v <= 3 ? kNames[v] : "?";
}
const char* DescribeScale(int v) {
    static const char* const kNames[] = {"50%", "60%", "70%", "80%", "90%", "100%"};
    return v >= 0 && v <= 5 ? kNames[v] : "?";
}
const char* DescribeQuality(int v) {
    static const char* const kNames[] = {"tier 1 (Low)", "tier 2 (Medium)", "tier 3 (High)", "tier 4 (Highest)", "MAX"};
    return v >= 0 && v <= 4 ? kNames[v] : "?";
}
const char* DescribeOnOff(int v) { return v ? "on" : "off"; }

constexpr Spec kSpecs[kCount] = {
    {"FrameRateLimit", "Frame Rate Limit", 3, 2, DescribeRate},
    {"RenderScale", "Render Scale", 5, 5, DescribeScale},
    {"GraphicsQuality", "Graphics Quality", 4, kQualityMax, DescribeQuality},
    {"NitroMotionBlur", "Nitro Motion Blur", 1, 0, DescribeOnOff},
    {"ColourGrading", "Colour Grading", 1, 0, DescribeOnOff},
};

constexpr char kJavaClass[] = "com/ea/ironmonkey/port/NfsOptions";

// libapp 1.3.128 offsets (cfg::kModules base 0x40000000)
constexpr uint32_t kFindOptionHook = 0x24D3A8, kFindOptionReturn = 0x24D414;
constexpr uint32_t kSetHook = 0x249EA4;
constexpr uint32_t kResetHook = 0x24D36C;
constexpr uint32_t kIntOptionVtable = 0xAA5D7C;
constexpr uint32_t kIntOptionSize = 0x48;  // ControlMode follows CameraMode at +0x48 in Options
constexpr uint32_t kOptionMin = 0x3C, kOptionMax = 0x40, kOptionValue = 0x44;
struct Site { uint32_t offset; uint32_t insn; };
constexpr Site kSites[] = {
    {0x24D3A4, 0xE92D4DF0}, {0x24D3A8, 0xE28DB018}, {0x24D414, 0xE8BD8DF0},  // PUSH {R4-R8,R10,R11,LR}; ADD R11; POP
    {0x249EA0, 0xE92D4800}, {0x249EA4, 0xE1A0B00D},                          // PUSH {R11,LR}; MOV R11, SP
    {0x24D368, 0xE92D4830}, {0x24D36C, 0xE28DB008}, {0x24D370, 0xE5D11001},  // PUSH; ADD R11, SP, #8; LDRB R1, [R1,#1]
};
// IntOption vtable: destructor, deleting destructor, ResetToDefault, debug-menu registration (relocated entries)
constexpr uint32_t kVtable[] = {0x2496E0, 0x249E14, 0x24A244, 0x24A044};

DynarmicHost* g_host = nullptr;
uint32_t g_base = 0;
std::atomic<uint32_t> g_option[kCount]{};  // guest address of each bridge IntOption
std::atomic<int> g_value[kCount] = {{2}, {5}, {kQualityMax}, {0}, {0}};
std::atomic<Listener> g_listener[kCount]{};
std::atomic<int> g_session_quality{-1};
std::atomic<int> g_bound_logs{0};
jclass g_class = nullptr;
jmethodID g_on_changed = nullptr;

int Clamp(Id id, int value) { return value < 0 ? 0 : value > kSpecs[id].last ? kSpecs[id].last : value; }

// The value the settings row shows (RadioOptionWidget::Update reads the default slot of an option without a database).
void Store(Id id, int value) {
    g_value[id].store(value, std::memory_order_release);
    const uint32_t option = g_option[id].load(std::memory_order_acquire);
    if (option && g_host) g_host->MemoryWrite32(option + kOptionValue, static_cast<uint32_t>(value));
}

// The player changed a row (or reset the settings): the feature module, then Java (preferences and side effects).
void SetFromGame(Id id, int value, const char* why) {
    value = Clamp(id, value);
    const int old = g_value[id].exchange(value);
    Store(id, value);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "%s: %s -> %s (%s)", kSpecs[id].name, kSpecs[id].describe(old),
                        kSpecs[id].describe(value), why);
    if (const Listener listener = g_listener[id].load(std::memory_order_acquire)) listener(value, why);
    JavaVM* vm = GetHostJavaVM();
    JNIEnv* env = nullptr;
    if (!vm || !g_class || !g_on_changed || vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        LOGE("%s: Java callback unavailable on this thread; the choice is not saved", kSpecs[id].name);
        return;
    }
    env->CallStaticVoidMethod(g_class, g_on_changed, static_cast<jint>(id), static_cast<jint>(value));
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
}

int OptionByName(DynarmicHost& h, uint32_t begin, uint32_t end) {
    for (int id = 0; id < kCount; ++id) {
        const char* name = kSpecs[id].name;
        const size_t length = strlen(name);
        if (end - begin != length) continue;
        bool same = true;
        for (uint32_t i = 0; i < length && same; ++i) same = h.MemoryRead8(begin + i) == static_cast<uint8_t>(name[i]);
        if (same) return id;
    }
    return -1;
}

int OptionByAddress(uint32_t address) {
    for (int id = 0; id < kCount; ++id)
        if (address && g_option[id].load(std::memory_order_acquire) == address) return id;
    return -1;
}

// IntOption (OptionBase ctor 0x24953C, IntOption ctor 0x249DD8): vtable, database object {db, id, ...} at +4, name at
// +0x14, debug key string {begin, end, capacity} at +0x18, change callback at +0x2C (empty), min/max/default at
// +0x3C/+0x40/+0x44. The two strings follow the object in the same block.
uint32_t MakeOption(DynarmicHost& host, uint32_t libapp_base, const Spec& spec, int value) {
    const uint32_t name_size = static_cast<uint32_t>(strlen(spec.name) + 1), key_size = static_cast<uint32_t>(strlen(spec.key) + 1);
    const uint32_t size = kIntOptionSize + ((name_size + 3) & ~3u) + key_size;
    const uint32_t block = host.GuestAlloc(size, 8);
    if (!block) return 0;
    for (uint32_t i = 0; i < size; ++i) host.MemoryWrite8(block + i, 0);
    const uint32_t name = block + kIntOptionSize, key = name + ((name_size + 3) & ~3u);
    for (uint32_t i = 0; i < name_size; ++i) host.MemoryWrite8(name + i, static_cast<uint8_t>(spec.name[i]));
    for (uint32_t i = 0; i < key_size; ++i) host.MemoryWrite8(key + i, static_cast<uint8_t>(spec.key[i]));
    host.MemoryWrite32(block + 0x00, libapp_base + kIntOptionVtable);
    host.MemoryWrite32(block + 0x08, 0xFFFFFFFFu);  // database object id: invalid
    host.MemoryWrite32(block + 0x14, name);
    host.MemoryWrite32(block + 0x18, key);
    host.MemoryWrite32(block + 0x1C, key + key_size - 1);
    host.MemoryWrite32(block + 0x20, key + key_size);
    host.MemoryWrite32(block + kOptionMin, 0);
    host.MemoryWrite32(block + kOptionMax, static_cast<uint32_t>(spec.last));
    host.MemoryWrite32(block + kOptionValue, static_cast<uint32_t>(value));
    return block;
}

// JNI: com.ea.ironmonkey.port.NfsOptions
void N_SetOption(JNIEnv*, jclass, jint id, jint value) {
    if (id < 0 || id >= kCount) return;
    const Id option = static_cast<Id>(id);
    Publish(option, value);
    if (option == kGraphicsQuality) {
        int expected = -1;
        const int quality = Clamp(option, value);
        if (g_session_quality.compare_exchange_strong(expected, quality, std::memory_order_acq_rel)) {
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "graphics quality for this session: %s", DescribeQuality(quality));
            if (g_host) graphics::ApplySessionQuality(*g_host, g_base ? g_base : 0x40000000u, quality);
        }
    }
}

}  // namespace

int Value(Id id) { return g_value[id].load(std::memory_order_acquire); }

void SetListener(Id id, Listener listener) { g_listener[id].store(listener, std::memory_order_release); }

void Publish(Id id, int value) { Store(id, Clamp(id, value)); }

int SessionQuality() {
    const int quality = g_session_quality.load(std::memory_order_acquire);
    return quality < 0 ? kQualityMax : quality;
}

bool Register(JNIEnv* env) {
    jclass cls = env->FindClass(kJavaClass);
    if (!cls) {
        env->ExceptionClear();
        LOGE("port options: %s not found", kJavaClass);
        return false;
    }
    const JNINativeMethod methods[] = {
        {"nativeSetOption", "(II)V", reinterpret_cast<void*>(N_SetOption)},
    };
    if (env->RegisterNatives(cls, methods, static_cast<jint>(std::size(methods))) != JNI_OK) {
        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
        LOGE("RegisterNatives failed for %s", kJavaClass);
        env->DeleteLocalRef(cls);
        return false;
    }
    g_class = static_cast<jclass>(env->NewGlobalRef(cls));
    g_on_changed = env->GetStaticMethodID(cls, "onOptionChanged", "(II)V");
    if (env->ExceptionCheck()) { env->ExceptionClear(); g_on_changed = nullptr; }
    env->DeleteLocalRef(cls);
    return g_on_changed != nullptr;
}

void Install(DynarmicHost& host, uint32_t libapp_base) {
    for (const auto& site : kSites)
        if (host.MemoryRead32(libapp_base + site.offset) != site.insn) {
            LOGE("port options: libapp+0x%x is not the expected instruction, the settings rows stay unbound", site.offset);
            return;
        }
    for (size_t i = 0; i < std::size(kVtable); ++i)
        if (host.MemoryRead32(libapp_base + kIntOptionVtable + 4 * i) != libapp_base + kVtable[i]) {
            LOGE("port options: IntOption vtable differs at +%zu, the settings rows stay unbound", 4 * i);
            return;
        }
    uint32_t blocks[kCount]{};
    for (int id = 0; id < kCount; ++id) {
        blocks[id] = MakeOption(host, libapp_base, kSpecs[id], g_value[id].load());
        if (!blocks[id]) {
            LOGE("port options: guest allocation failed");
            return;
        }
    }
    g_host = &host;
    g_base = libapp_base;

    struct Hook { uint32_t offset; const char* name; ThunkHandler handler; };
    const Hook hooks[] = {
        {kFindOptionHook, "nfs_options_find", [](Dynarmic::A32::Jit& jit, DynarmicHost& h) {
             // R0 = Options, R1 = option name {begin, end}
             const uint32_t text = jit.Regs()[1];
             const int id = text ? OptionByName(h, h.MemoryRead32(text), h.MemoryRead32(text + 4)) : -1;
             const uint32_t option = id >= 0 ? g_option[id].load(std::memory_order_acquire) : 0;
             if (option) {
                 if (g_bound_logs.fetch_add(1, std::memory_order_relaxed) < 16)
                     __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "settings row bound to the bridge option %s (value %d)",
                                         kSpecs[id].name, g_value[id].load());
                 jit.Regs()[0] = option;
                 jit.Regs()[15] = g_base + kFindOptionReturn;  // POP {R4-R8, R10, R11, PC}
                 return;
             }
             jit.Regs()[11] = jit.Regs()[13] + 0x18;
         }},
        {kSetHook, "nfs_options_set", [](Dynarmic::A32::Jit& jit, DynarmicHost& h) {
             // R0 = IntOption, R1 = value; PUSH {R11, LR} done
             const int id = OptionByAddress(jit.Regs()[0]);
             if (id >= 0) {
                 SetFromGame(static_cast<Id>(id), static_cast<int>(jit.Regs()[1]), "settings row");
                 const uint32_t sp = jit.Regs()[13];
                 const uint32_t lr = h.MemoryRead32(sp + 4);
                 jit.Regs()[11] = h.MemoryRead32(sp);
                 jit.Regs()[13] = sp + 8;
                 jit.Regs()[0] = 0;
                 if (lr & 1u) jit.SetCpsr(jit.Cpsr() | (1u << 5));
                 jit.Regs()[15] = lr & ~1u;
                 return;
             }
             jit.Regs()[11] = jit.Regs()[13];
         }},
        {kResetHook, "nfs_options_reset", [](Dynarmic::A32::Jit& jit, DynarmicHost& h) {
             // R0 = Options, R1 = broadcast flags; byte 1 set = reset the options to their defaults
             if (g_option[0].load(std::memory_order_acquire) && h.MemoryRead8(jit.Regs()[1] + 1))
                 for (int id = 0; id < kCount; ++id)
                     if (g_value[id].load() != kSpecs[id].def) SetFromGame(static_cast<Id>(id), kSpecs[id].def, "default settings");
             jit.Regs()[11] = jit.Regs()[13] + 8;
         }},
    };
    for (const auto& hook : hooks) {
        const uint32_t trampoline = host.GetThunkManager().RegisterThunk(hook.name, hook.handler);
        host.MemoryWrite32(libapp_base + hook.offset, host.MemoryRead32(trampoline));  // the trampoline's SVC #id
        host.InvalidateGuestCode(libapp_base + hook.offset, 4);
    }
    for (int id = 0; id < kCount; ++id) {
        // a value Java published before the objects existed
        host.MemoryWrite32(blocks[id] + kOptionValue, static_cast<uint32_t>(g_value[id].load()));
        g_option[id].store(blocks[id], std::memory_order_release);
    }
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "port options installed (%d IntOptions, %zu hooks)", int(kCount), std::size(hooks));
}

}  // namespace portopts
