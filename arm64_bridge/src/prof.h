#pragma once

#include <cstdint>

class DynarmicHost;

// Sampling profiler of the GL thread (diagnosis only; off unless `adb shell setprop debug.nfsbridge.prof 1` before a
// cold start). See prof.cpp.
namespace prof {
// GL thread, after every RunLoop tick (outside guest execution).
void OnGlTick(DynarmicHost& host, uint64_t now_ns);
}  // namespace prof
