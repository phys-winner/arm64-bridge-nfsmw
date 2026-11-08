#pragma once

#include <jni.h>

// Logcat gate of the release build (stage 12). The bridge is linked with -Wl,--wrap for __android_log_print,
// __android_log_write and __android_log_vprint (CMakeLists.txt), so every log call of its own objects, of the
// libraries linked into it and of the ARM32 guest (whose liblog imports are thunks onto these functions) passes
// log_gate.cpp, which drops everything below ANDROID_LOG_FATAL unless logging is switched on:
//   adb shell setprop debug.nfsbridge.logs 1      (then a cold start of the game; a build made with
//                                                 build_arm64.ps1 -EnableLogging logs without the property)
namespace logs {

bool Enabled();

// JNI_OnLoad: with logging on, sets com.ea.ironmonkey.port.NfsLog.on (the Java side of the gate); with logging off,
// lowers the whole process to fatal messages on API 30+ (__android_log_set_minimum_priority), which also silences
// the Java framework's logs of this process.
void Install(JNIEnv* env);

// The first failure reason of the process (DynarmicHost::Fail, FakeJniEnv::Fail, guest heap exhaustion); later ones
// are ignored. Abort() appends it to its own message.
void NoteFailure(const char* reason);
const char* FirstFailure();

// Stage 14: ends the process with the reason as the abort message, which debuggerd prints in the tombstone
// ("Abort message: ...", also in `adb logcat -b crash`) even in the release build without logs, and logs it at
// ANDROID_LOG_FATAL, which the gate always passes.
[[noreturn]] void Abort(const char* tag, const char* format, ...) __attribute__((format(printf, 2, 3)));

}  // namespace logs
