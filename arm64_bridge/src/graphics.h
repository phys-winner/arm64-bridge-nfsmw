#pragma once

#include <cstdint>

class DynarmicHost;

// Stage 13 graphics options: render scale and nitro motion blur (live), graphics quality (per session); stage 16 the
// iOS colour grading switch (live). See graphics.cpp.
namespace graphics {

// Boot, after the guest images are relocated and before any guest code runs: the render scale, motion blur and colour
// grading hooks.
void Install(DynarmicHost& host, uint32_t libapp_base);

// Java (NfsOptions.init), before the engine starts: the graphics quality of this session (portopts::kGraphicsQuality);
// at tier 24 on Adreno also the level-3 car shader (stage 16, b33).
void ApplySessionQuality(DynarmicHost& host, uint32_t libapp_base, int quality);

}  // namespace graphics
