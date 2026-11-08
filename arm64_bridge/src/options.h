#pragma once

#include <jni.h>

#include <cstdint>

class DynarmicHost;

// The port's own options in the game's settings pages (stage 10: frame rate limit; stage 13: render scale, graphics
// quality, nitro motion blur; stage 16: iOS colour grading). See options.cpp.
namespace portopts {

enum Id : int {
    kFrameRateLimit = 0,   // 0..3: 30 / 45 / 60 / display maximum (framerate.cpp)
    kRenderScale = 1,      // 0..5: 50 / 60 / 70 / 80 / 90 / 100 % of the screen width for the 3D scene (graphics.cpp)
    kGraphicsQuality = 2,  // 0..4: engine tiers 1-4 (Low, Medium, High, Highest) and MAX = tier 4 + the port's effects
    kNitroBlur = 3,        // 0..1: off / on, stronger motion blur while the nitrous burns (graphics.cpp)
    kColourGrading = 4,    // 0..1: off / on, the iOS colour-ramp pass of the MAX quality (DE-03, graphics.cpp)
    kCount
};

constexpr int kQualityMax = 4;

// Current value (any thread).
int Value(Id id);

// A feature module's reaction when the player changes a row or resets the settings (GL thread, inside the hook).
using Listener = void (*)(int value, const char* why);
void SetListener(Id id, Listener listener);

// Sets a value without telling Java (start-up values from the preferences, NfsFrameRate's configuration).
void Publish(Id id, int value);

// Graphics quality in effect for this process: the value Java passed before the engine started (later changes in the
// settings take effect at the next start). kQualityMax until Java has passed one.
int SessionQuality();
inline bool PortEffects() { return SessionQuality() == kQualityMax; }

// Boot, after the guest images are relocated and before any guest code runs: the option hooks in libapp.
void Install(DynarmicHost& host, uint32_t libapp_base);

// JNI_OnLoad: natives of com.ea.ironmonkey.port.NfsOptions and its Java callback. False if the class is missing.
bool Register(JNIEnv* env);

}  // namespace portopts
