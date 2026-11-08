#pragma once

#include <jni.h>

#include <cstdint>

class DynarmicHost;

// Frame rate limit (stage 10): the game's own settings row "30 / 45 / 60 / MAX" and the frame pacer of the GL thread.
namespace framerate {
// JNI_OnLoad: natives of com.ea.ironmonkey.port.NfsFrameRate and its Java callback. False if the class is missing.
bool Register(JNIEnv* env);
}  // namespace framerate
