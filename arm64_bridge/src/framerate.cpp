// Frame rate limit (stage 10; user request 2026-10-08: a native-looking "fps limit" switch in the game's settings with
// 30 / 45 / 60 / the display's maximum refresh rate, 60 by default).
//
// Option. The settings row is the port option "FrameRateLimit" (options.cpp, values 0..3 = 30 / 45 / 60 / display
// maximum, default 60; since stage 13 the row sits in the GRAPHICS frame of the settings pages). The value is not in
// the save: Java (NfsFrameRate) keeps it in the port's preferences, passes it in at start (nativeConfigure) and is
// told about every change through NfsOptions.onOptionChanged.
//
// Pacer. GameRenderer.onDrawFrame calls nativeBeforeFrame before the RunLoop tick and nativeAfterFrame after it; the
// GLSurfaceView swaps right after onDrawFrame returns. With a limit below the display rate every frame k has an ideal
// display time ideal_k = ideal_{k-1} + 1/limit (a whole number of vsync periods when the limit divides the rate),
// snapped to the nearest hardware vsync D_k. The frame starts at D_k - presentation deadline - 1/limit, i.e. a whole
// frame period of work before SurfaceFlinger latches for D_k, and is queued with EGL_ANDROID_presentation_time
// D_k - vsync/2, so SurfaceFlinger shows it at D_k and not a vsync early (BufferQueue acquires a buffer once its
// timestamp <= the expected present time): 30 fps on 60 Hz is every second vsync, 45 fps on 60 Hz the steady 1-1-2
// cadence. The vsync phase comes from an AChoreographer thread (its frame time is the hardware vsync plus the app vsync
// offset, Display.getAppVsyncOffsetNanos from Java), sampled twice a second while the pacer asks for it. A frame that
// starts more than one vsync late (the game slower than the limit, a load, a pause) starts at once and re-anchors the
// timeline: the pacer never adds a wait to a slow frame. At "MAX", or with a limit at or above the display rate,
// nothing waits and the timestamps go back to automatic: the GLSurfaceView runs at the vsync rate, as before.
#include "framerate.h"

#include <android/choreographer.h>
#include <android/log.h>
#include <android/looper.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <pthread.h>
#include <sys/system_properties.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iterator>
#include <mutex>
#include <thread>

#include "options.h"

#define LOG_TAG "Arm32Bridge_Fps"
#include "bridge_log.h"

namespace framerate {
namespace {

constexpr int kRates[] = {30, 45, 60, 0};  // RadioIndex -> limit in fps; 0 = the display's maximum
constexpr int kDefaultIndex = 2;           // 60 fps
constexpr int kLastIndex = 3;
constexpr char kJavaClass[] = "com/ea/ironmonkey/port/NfsFrameRate";

std::atomic<int> g_index{kDefaultIndex};
std::atomic<float> g_hz{60.0f};
std::atomic<int64_t> g_app_offset{0}, g_deadline{0};
std::atomic<uint32_t> g_generation{1};  // every configuration change; the pacer re-reads and re-anchors
bool g_trace = false;

int64_t Now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

int Clamp(int index) { return index < 0 ? 0 : index > kLastIndex ? kLastIndex : index; }

const char* RateName(int index) {
    static const char* const kNames[] = {"30 fps", "45 fps", "60 fps", "display maximum"};
    return kNames[Clamp(index)];
}

// The player changed the row (or reset the settings), options.cpp on the GL thread: the pacer re-reads the limit; Java
// (NfsOptions -> NfsFrameRate) saves it and picks the display mode and the ADPF target.
void OnLimitChanged(int index, const char*) {
    g_index.store(Clamp(index), std::memory_order_release);
    g_generation.fetch_add(1, std::memory_order_acq_rel);
}

// ---------------------------------------------------------------------------------------------------------------------
// Vsync phase: one AChoreographer frame callback every half second on a thread of its own, only while the pacer asks.
// The phase is stable, so between samples the pacer extrapolates with the display period (p20b, a callback on every
// vsync: the helper thread had used 1.4 s of CPU over a few minutes of 30 fps on the Honor 9 Lite).
// ---------------------------------------------------------------------------------------------------------------------
namespace vsync {
constexpr int64_t kIdleNs = 2000000000;    // sampling stops two seconds after the pacer's last request
constexpr int64_t kSampleNs = 500000000;   // one frame time per half second
std::atomic<int64_t> g_time{0};            // latest frame time (CLOCK_MONOTONIC, app vsync phase)
std::atomic<int64_t> g_demand{0};
std::atomic<ALooper*> g_looper{nullptr};
bool g_posted = false;                     // helper thread only

// The 64-bit callback on Android 10+ (libapp.so); the API 26 build (libapp_legacy.so) has only the long one, which
// is 64-bit on arm64 as well.
#if __ANDROID_API__ >= 29
void OnVsync(int64_t frame_time, void*) {
#else
void OnVsync(long frame_time, void*) {
#endif
    g_time.store(static_cast<int64_t>(frame_time), std::memory_order_release);
    g_posted = false;
}

void Thread() {
    pthread_setname_np(pthread_self(), "NfsVsync");
    ALooper* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    AChoreographer* choreographer = AChoreographer_getInstance();
    if (!looper || !choreographer) {
        LOGE("vsync: no AChoreographer on the helper thread; pacing uses the frame clock only");
        return;
    }
    g_looper.store(looper, std::memory_order_release);
    for (;;) {
        const int64_t now = Now();
        int timeout = -1;  // nothing wanted, or a callback pending: sleep until woken / called back
        if (now - g_demand.load(std::memory_order_acquire) < kIdleNs && !g_posted) {
            const int64_t due = g_time.load(std::memory_order_acquire) + kSampleNs;
            if (now >= due) {
                g_posted = true;
#if __ANDROID_API__ >= 29
                AChoreographer_postFrameCallback64(choreographer, OnVsync, nullptr);
#else
                AChoreographer_postFrameCallback(choreographer, OnVsync, nullptr);
#endif
            } else {
                timeout = static_cast<int>((due - now) / 1000000) + 1;
            }
        }
        ALooper_pollOnce(timeout, nullptr, nullptr, nullptr);
    }
}

// GL thread: the latest app-phase vsync time (0 until the first sample); keeps the sampling going. The request is
// published before the helper starts, so its first check already sees it (a wake-up sent before the helper has its
// looper would be lost).
int64_t Latest() {
    const int64_t now = Now();
    const int64_t previous = g_demand.exchange(now, std::memory_order_acq_rel);
    static std::once_flag started;
    std::call_once(started, [] { std::thread(Thread).detach(); });
    if (now - previous >= kIdleNs)  // the helper may sleep without a timeout
        if (ALooper* looper = g_looper.load(std::memory_order_acquire)) ALooper_wake(looper);
    return g_time.load(std::memory_order_acquire);
}
}  // namespace vsync

// ---------------------------------------------------------------------------------------------------------------------
// Pacer (GL thread only).
// ---------------------------------------------------------------------------------------------------------------------
struct Pacer {
    uint32_t generation = 0;
    bool active = false, anchored = false, timestamped = false;
    double period = 0, vsync = 0, lead = 0;  // ns
    double ideal = 0;                        // ideal display time of the frame being made
    int64_t target = 0;                      // that time snapped to a hardware vsync
    // statistics (debug.nfsbridge.frametrace 1)
    int64_t window = 0;
    uint32_t frames = 0, waits = 0, late = 0, anchors = 0, phased = 0;
    double waited = 0;
};
Pacer g_pacer;
PFNEGLPRESENTATIONTIMEANDROIDPROC g_present = nullptr;
int g_present_state = -1;  // -1 not looked up yet, 0 unavailable, 1 available

bool SetPresentationTime(int64_t when) {
    if (g_present_state < 0) {
        EGLDisplay display = eglGetCurrentDisplay();
        const char* extensions = display != EGL_NO_DISPLAY ? eglQueryString(display, EGL_EXTENSIONS) : nullptr;
        if (extensions && strstr(extensions, "EGL_ANDROID_presentation_time"))
            g_present = reinterpret_cast<PFNEGLPRESENTATIONTIMEANDROIDPROC>(eglGetProcAddress("eglPresentationTimeANDROID"));
        g_present_state = g_present ? 1 : 0;
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "EGL_ANDROID_presentation_time: %s",
                            g_present ? "on (frames are queued for their vsync)" : "unavailable (pacing by start time only)");
    }
    if (!g_present) return false;
    EGLDisplay display = eglGetCurrentDisplay();
    EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
    return display != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE && g_present(display, surface, when) == EGL_TRUE;
}

// Nearest hardware vsync to t, from the latest Choreographer frame time; t itself while no phase is known.
int64_t Snap(double t, double period, bool* phased) {
    const int64_t latest = vsync::Latest();
    if (latest <= 0 || Now() - latest > 3000000000LL) {
        *phased = false;
        return static_cast<int64_t>(t);
    }
    *phased = true;
    const double hardware = static_cast<double>(latest - g_app_offset.load(std::memory_order_relaxed));
    return static_cast<int64_t>(hardware + std::nearbyint((t - hardware) / period) * period);
}

void SleepUntil(int64_t when) {
    timespec ts{static_cast<time_t>(when / 1000000000LL), static_cast<long>(when % 1000000000LL)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
    }
}

void Configure(Pacer& p) {
    const int index = g_index.load(std::memory_order_acquire);
    const float hz = g_hz.load(std::memory_order_acquire);
    const int limit = kRates[Clamp(index)];
    p.vsync = 1e9 / (hz > 1.0f ? hz : 60.0f);
    p.active = limit > 0 && limit < hz - 0.5f;
    p.anchored = false;
    if (p.active) {
        const double ratio = hz / limit, whole = std::nearbyint(ratio);
        p.period = std::fabs(ratio - whole) < 0.02 ? whole * p.vsync : 1e9 / limit;
        const double deadline = static_cast<double>(g_deadline.load(std::memory_order_acquire));
        p.lead = deadline > p.vsync / 8 && deadline <= p.vsync ? deadline : p.vsync / 2;
    }
    if (p.active)
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                            "pacer: %s on %.2f Hz -> a frame every %.3f ms (%.2f vsyncs), started %.2f ms before its vsync", RateName(index),
                            hz, p.period / 1e6, p.period / p.vsync, (p.lead + p.period) / 1e6);
    else
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "pacer: %s on %.2f Hz -> vsync rate, no waiting", RateName(index), hz);
}

void Trace(Pacer& p, int64_t now) {
    if (!p.window) p.window = now;
    ++p.frames;
    if (now - p.window < 10000000000LL) return;
    const double span = (now - p.window) / 1e9;
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
        "pacer %.1f s: %u frames (%.2f/s), waited %u times (avg %.2f ms), re-anchored %u (late %u), vsync phase %u of %u frames, "
        "presentation time %s", span, p.frames, p.frames / span, p.waits, p.waits ? p.waited / p.waits / 1e6 : 0.0, p.anchors, p.late,
        p.phased, p.frames, g_present_state == 1 ? "on" : "off");
    p.window = now;
    p.frames = p.waits = p.late = p.anchors = p.phased = 0;
    p.waited = 0;
}

void BeforeFrame() {
    Pacer& p = g_pacer;
    const uint32_t generation = g_generation.load(std::memory_order_acquire);
    if (generation != p.generation) {
        p.generation = generation;
        Configure(p);
    }
    if (!p.active) return;
    const int64_t now = Now();
    bool phased = false;
    bool restart = !p.anchored;
    if (!restart) {
        p.target = Snap(p.ideal, p.vsync, &phased);
        const int64_t start = p.target - static_cast<int64_t>(p.lead + p.period);
        if (now > start + static_cast<int64_t>(p.vsync)) {  // more than a vsync late (slow frame, load, pause)
            restart = true;
            ++p.late;
        } else if (start > now) {
            SleepUntil(start);
            ++p.waits;
            p.waited += static_cast<double>(start - now);
        }
    }
    if (restart) {  // first frame or a late one: it starts now, without waiting, and the timeline restarts with it
        p.ideal = static_cast<double>(now) + p.lead + p.period;
        p.target = Snap(p.ideal, p.vsync, &phased);
        p.anchored = true;
        ++p.anchors;
    }
    p.phased += phased;
}

void AfterFrame() {
    Pacer& p = g_pacer;
    if (!p.active) {
        if (p.timestamped) {  // back to automatic timestamps (NATIVE_WINDOW_TIMESTAMP_AUTO)
            SetPresentationTime(INT64_MIN);
            p.timestamped = false;
        }
        return;
    }
    p.timestamped = SetPresentationTime(p.target - static_cast<int64_t>(p.vsync / 2)) || p.timestamped;
    p.ideal += p.period;
    if (g_trace) Trace(p, Now());
}

// ---------------------------------------------------------------------------------------------------------------------
// JNI: com.ea.ironmonkey.port.NfsFrameRate
// ---------------------------------------------------------------------------------------------------------------------
void N_Configure(JNIEnv*, jclass, jint index, jfloat hz, jlong app_offset, jlong deadline) {
    g_index.store(Clamp(index), std::memory_order_release);
    g_hz.store(hz > 1.0f ? hz : 60.0f, std::memory_order_release);
    g_app_offset.store(app_offset, std::memory_order_release);
    g_deadline.store(deadline, std::memory_order_release);
    portopts::Publish(portopts::kFrameRateLimit, Clamp(index));
    g_generation.fetch_add(1, std::memory_order_acq_rel);
}
void N_BeforeFrame(JNIEnv*, jclass) { BeforeFrame(); }
void N_AfterFrame(JNIEnv*, jclass) { AfterFrame(); }

}  // namespace

bool Register(JNIEnv* env) {
    jclass cls = env->FindClass(kJavaClass);
    if (!cls) {
        env->ExceptionClear();
        LOGE("frame rate limit: %s not found", kJavaClass);
        return false;
    }
    const JNINativeMethod methods[] = {
        {"nativeConfigure", "(IFJJ)V", reinterpret_cast<void*>(N_Configure)},
        {"nativeBeforeFrame", "()V", reinterpret_cast<void*>(N_BeforeFrame)},
        {"nativeAfterFrame", "()V", reinterpret_cast<void*>(N_AfterFrame)},
    };
    if (env->RegisterNatives(cls, methods, static_cast<jint>(std::size(methods))) != JNI_OK) {
        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
        LOGE("RegisterNatives failed for %s", kJavaClass);
        env->DeleteLocalRef(cls);
        return false;
    }
    env->DeleteLocalRef(cls);
    portopts::SetListener(portopts::kFrameRateLimit, OnLimitChanged);
    char value[PROP_VALUE_MAX]{};
    g_trace = __system_property_get("debug.nfsbridge.frametrace", value) > 0 && value[0] == '1';
    return true;
}

}  // namespace framerate
