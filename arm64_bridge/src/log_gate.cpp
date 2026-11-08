// Logcat gate of the release build (stage 12); see log_gate.h. The decision is made once per process from the system
// property debug.nfsbridge.logs (or at build time with ARM32_BRIDGE_ENABLE_LOGGING); fatal messages always pass, so
// an abort still leaves its reason in the log.

#include "log_gate.h"

#include <android/log.h>
#include <android/set_abort_message.h>
#include <dlfcn.h>
#include <sys/system_properties.h>

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef ARM32_BRIDGE_ENABLE_LOGGING
#define ARM32_BRIDGE_ENABLE_LOGGING 0
#endif

// -Wl,--wrap=<symbol>: references to <symbol> from the bridge's objects resolve to __wrap_<symbol>, the liblog
// function itself stays reachable as __real_<symbol>.
extern "C" {
int __real___android_log_write(int prio, const char* tag, const char* text);
int __real___android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap);
}

namespace logs {

bool Enabled() {
    static const bool on = [] {
        if (ARM32_BRIDGE_ENABLE_LOGGING) return true;
        char value[PROP_VALUE_MAX]{};
        return __system_property_get("debug.nfsbridge.logs", value) > 0 && strcmp(value, "1") == 0;
    }();
    return on;
}

void Install(JNIEnv* env) {
    if (Enabled()) {
        jclass gate = env->FindClass("com/ea/ironmonkey/port/NfsLog");
        jfieldID on = gate ? env->GetStaticFieldID(gate, "on", "Z") : nullptr;
        if (on) env->SetStaticBooleanField(gate, on, JNI_TRUE);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (gate) env->DeleteLocalRef(gate);
        __android_log_print(ANDROID_LOG_INFO, "Arm32Bridge_Log", "logs on (debug.nfsbridge.logs)%s",
                            on ? "" : ", Java gate NfsLog.on not found");
        return;
    }
    // API 30+: the process-wide threshold of liblog also covers android.util.Log of the framework and the game.
    using SetMinimumPriority = int32_t (*)(int32_t);
    if (void* liblog = dlopen("liblog.so", RTLD_NOW | RTLD_NOLOAD)) {
        if (auto set = reinterpret_cast<SetMinimumPriority>(dlsym(liblog, "__android_log_set_minimum_priority"))) {
            set(ANDROID_LOG_FATAL);
        }
        dlclose(liblog);
    }
}

namespace {
char g_first_failure[256];
std::atomic<bool> g_failure_claimed{false};
std::atomic<bool> g_failure_ready{false};
}  // namespace

void NoteFailure(const char* reason) {
    if (!reason || g_failure_claimed.exchange(true)) return;
    snprintf(g_first_failure, sizeof(g_first_failure), "%s", reason);
    g_failure_ready.store(true, std::memory_order_release);
}

const char* FirstFailure() {
    return g_failure_ready.load(std::memory_order_acquire) ? g_first_failure : nullptr;
}

void Abort(const char* tag, const char* format, ...) {
    char text[768];
    va_list ap;
    va_start(ap, format);
    int length = vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    if (length < 0) length = 0;
    const char* first = FirstFailure();
    if (first && size_t(length) < sizeof(text) && !strstr(text, first))
        snprintf(text + length, sizeof(text) - size_t(length), " (first failure: %s)", first);
    __android_log_write(ANDROID_LOG_FATAL, tag, text);
    android_set_abort_message(text);
    abort();
}

}  // namespace logs

namespace {

bool Pass(int prio) {
    return prio >= ANDROID_LOG_FATAL || logs::Enabled();
}

}  // namespace

// A dropped line returns what liblog returns for a priority it does not log.
extern "C" int __wrap___android_log_write(int prio, const char* tag, const char* text) {
    return Pass(prio) ? __real___android_log_write(prio, tag, text) : -EPERM;
}

extern "C" int __wrap___android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap) {
    return Pass(prio) ? __real___android_log_vprint(prio, tag, fmt, ap) : -EPERM;
}

extern "C" int __wrap___android_log_print(int prio, const char* tag, const char* fmt, ...) {
    if (!Pass(prio)) return -EPERM;
    va_list ap;
    va_start(ap, fmt);
    const int written = __real___android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return written;
}
