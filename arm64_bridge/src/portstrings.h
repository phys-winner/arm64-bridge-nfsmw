#pragma once

#include <cstdint>

class DynarmicHost;

// The port's own UI strings (settings rows of stages 10 and 13) added to the engine's string table at load time, in
// the table's language, without replacing the OBB's string tables. See portstrings.cpp.
namespace portstrings {
// Boot, after the guest images are relocated and before any guest code runs.
void Install(DynarmicHost& host, uint32_t libapp_base);
}  // namespace portstrings
