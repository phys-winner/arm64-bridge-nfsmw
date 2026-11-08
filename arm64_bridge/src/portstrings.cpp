// The port's own UI strings (stage 13). Until stage 12 the settings rows' titles and labels were added to the
// eleven string tables of the OBB (overlay files rebuilt from the original OBB), which also replaced every string of a
// modified cache. Now the tables stay as the OBB has them and the bridge adds its strings to the engine's loaded table:
//   TextManager 0x40A188 -> StringTable ctor 0x4056E4(table, path) -> loader 0x4058F4 fills the table's hash map
//   (FNV-1 of the string id; insert-or-find 0x406364(&result, table, &id) returns {node, bucket, inserted}, node =
//   {id, text, next}, both pointers into the SBIN data).
//   0x4056E8 (ADD R11, SP, #0x18 after the PUSH) records the path of the table being loaded;
//   0x405790 (ADD R0, R4, #0x28, right after the loader returns, R4 = table) inserts every port string of the
//   table's language whose id the table does not have yet, with guest copies of id and text that live as long as
//   the process.
// The language: the engine mounts /published/stringdata/<folder> as /published/strings (0x782F4: the locale object
// *0xADB69C, string {begin, end, capacity} at +32 appended to "/published/stringdata" at 0x78408) and loads
// /published/strings/nfsmw_android.sb, so the folder (e.g. RUS_RU) comes from the locale object; a path that names
// "stringdata/<folder>" itself wins; English for an unknown folder.
// The strings come from assets/port_strings.txt (UTF-8 lines "LANGUAGE<TAB>ID<TAB>TEXT", written by
// work/tools/settings_ui.py). A language switch in the settings loads a new table and gets its strings the same way.
#include "portstrings.h"

#include <android/asset_manager.h>
#include <android/log.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "dynarmic_host.h"
#include "thunk_manager.h"

#define LOG_TAG "Arm32Bridge_Strings"
#include "bridge_log.h"

AAssetManager* BridgeAssetManager();  // jni_bridge.cpp

namespace portstrings {
namespace {

// libapp 1.3.128 offsets (cfg::kModules base 0x40000000)
constexpr uint32_t kCtorPush = 0x4056E4, kCtorPushInsn = 0xE92D4DF0;  // PUSH {R4-R8, R10, R11, LR}
constexpr uint32_t kCtorHook = 0x4056E8, kCtorHookInsn = 0xE28DB018;  // ADD R11, SP, #0x18
constexpr uint32_t kLoadedHook = 0x405790, kLoadedInsn = 0xE2840028;  // ADD R0, R4, #0x28
constexpr uint32_t kLoaderCall = 0x40578C, kLoaderCallInsn = 0xEB000058;  // BL 0x4058F4
constexpr uint32_t kInsert = 0x406364;
constexpr uint32_t kLocale = 0xADB69C, kLocaleFolder = 32;
constexpr char kAsset[] = "port_strings.txt";

uint32_t g_base = 0;
std::mutex g_mutex;
bool g_loaded = false;
std::map<std::string, std::vector<std::pair<std::string, std::string>>> g_strings;  // language -> (id, text)
std::map<std::string, uint32_t> g_guest_text;  // guest copies (id or language + id + text), never freed
std::string g_path;                            // path of the table being loaded

void Load() {
    if (g_loaded) return;
    g_loaded = true;
    AAssetManager* assets = BridgeAssetManager();
    AAsset* asset = assets ? AAssetManager_open(assets, kAsset, AASSET_MODE_BUFFER) : nullptr;
    if (!asset) {
        LOGE("port strings: assets/%s missing", kAsset);
        return;
    }
    const char* data = static_cast<const char*>(AAsset_getBuffer(asset));
    const size_t size = static_cast<size_t>(AAsset_getLength(asset));
    size_t count = 0;
    for (size_t pos = 0; data && pos < size;) {
        size_t end = pos;
        while (end < size && data[end] != '\n') ++end;
        std::string line(data + pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        g_strings[line.substr(0, a)].emplace_back(line.substr(a + 1, b - a - 1), line.substr(b + 1));
        ++count;
    }
    AAsset_close(asset);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "port strings: %zu entries in %zu languages", count, g_strings.size());
}

uint32_t GuestText(DynarmicHost& h, const std::string& cache_key, const std::string& text) {
    auto it = g_guest_text.find(cache_key);
    if (it != g_guest_text.end()) return it->second;
    const uint32_t address = h.GuestAlloc(text.size() + 1, 4);
    if (!address) return 0;
    for (size_t i = 0; i < text.size(); ++i) h.MemoryWrite8(address + uint32_t(i), static_cast<uint8_t>(text[i]));
    h.MemoryWrite8(address + uint32_t(text.size()), 0);
    g_guest_text.emplace(cache_key, address);
    return address;
}

std::string ReadCString(DynarmicHost& h, uint32_t address, size_t limit) {
    std::string out;
    for (size_t i = 0; address && i < limit; ++i) {
        const uint8_t c = h.MemoryRead8(address + uint32_t(i));
        if (!c) return out;
        if (c < 0x20 || c > 0x7E) return {};
        out.push_back(static_cast<char>(c));
    }
    return {};
}

// The constructor's path argument: a C string, or a string object {begin, end, ...}.
std::string PathArgument(DynarmicHost& h, uint32_t arg) {
    std::string path = ReadCString(h, arg, 512);
    if (path.find('/') != std::string::npos) return path;
    const uint32_t begin = h.MemoryRead32(arg), end = h.MemoryRead32(arg + 4);
    if (begin && end > begin && end - begin < 512) {
        path = ReadCString(h, begin, end - begin + 1);
        if (path.find('/') != std::string::npos) return path;
    }
    return {};
}

std::string LanguageOf(const std::string& path) {
    const size_t at = path.find("stringdata/");
    if (at == std::string::npos) return {};
    const size_t start = at + 11, end = path.find('/', start);
    return path.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string LocaleFolder(DynarmicHost& h) {
    const uint32_t locale = h.MemoryRead32(g_base + kLocale);
    if (!locale) return {};
    const uint32_t begin = h.MemoryRead32(locale + kLocaleFolder), end = h.MemoryRead32(locale + kLocaleFolder + 4);
    if (!begin || end <= begin || end - begin > 64) return {};
    std::string folder = ReadCString(h, begin, end - begin + 1);
    while (!folder.empty() && folder.front() == '/') folder.erase(folder.begin());
    while (!folder.empty() && folder.back() == '/') folder.pop_back();
    return folder;
}

void Insert(DynarmicHost& h, uint32_t table) {
    std::lock_guard lock(g_mutex);
    Load();
    std::string language = LanguageOf(g_path);
    if (language.empty()) language = LocaleFolder(h);
    auto strings = g_strings.find(language);
    if (strings == g_strings.end()) strings = g_strings.find("ENG_US");
    if (strings == g_strings.end()) return;
    const uint32_t scratch = GuestText(h, "\x01scratch", std::string(15, '\0'));  // {node, bucket, inserted} + id slot
    if (!scratch) return;
    size_t added = 0, present = 0;
    for (const auto& [id, text] : strings->second) {
        const uint32_t id_address = GuestText(h, "\x02" + id, id);
        const uint32_t text_address = GuestText(h, strings->first + '\t' + id + '\t' + text, text);
        if (!id_address || !text_address) break;
        h.MemoryWrite32(scratch + 12, id_address);
        h.CallGuestNested(g_base + kInsert, {scratch, table, scratch + 12});
        const uint32_t node = h.MemoryRead32(scratch);
        if (!node) continue;
        if (h.MemoryRead8(scratch + 8)) {
            h.MemoryWrite32(node + 4, text_address);
            ++added;
        } else {
            ++present;  // the OBB's own table already has it (an older overlay, or a modified cache)
        }
    }
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "string table %s (%s, language folder %s): %zu port strings added, %zu already present",
                        strings->first.c_str(), g_path.empty() ? "path unknown" : g_path.c_str(),
                        language.empty() ? "unknown" : language.c_str(), added, present);
}

}  // namespace

void Install(DynarmicHost& host, uint32_t libapp_base) {
    if (host.MemoryRead32(libapp_base + kCtorPush) != kCtorPushInsn || host.MemoryRead32(libapp_base + kCtorHook) != kCtorHookInsn ||
        host.MemoryRead32(libapp_base + kLoaderCall) != kLoaderCallInsn || host.MemoryRead32(libapp_base + kLoadedHook) != kLoadedInsn) {
        LOGE("port strings: StringTable code differs from 1.3.128, the port's rows keep their string ids");
        return;
    }
    g_base = libapp_base;
    struct Hook { uint32_t offset; const char* name; ThunkHandler handler; };
    const Hook hooks[] = {
        {kCtorHook, "nfs_strings_table_path", [](Dynarmic::A32::Jit& jit, DynarmicHost& h) {
             jit.Regs()[11] = jit.Regs()[13] + 0x18;  // the replaced ADD R11, SP, #0x18
             std::lock_guard lock(g_mutex);
             g_path = PathArgument(h, jit.Regs()[1]);
         }},
        {kLoadedHook, "nfs_strings_table_loaded", [](Dynarmic::A32::Jit& jit, DynarmicHost& h) {
             jit.Regs()[0] = jit.Regs()[4] + 0x28;  // the replaced ADD R0, R4, #0x28
             Insert(h, jit.Regs()[4]);
         }},
    };
    for (const auto& hook : hooks) {
        const uint32_t trampoline = host.GetThunkManager().RegisterThunk(hook.name, hook.handler);
        host.MemoryWrite32(libapp_base + hook.offset, host.MemoryRead32(trampoline));
        host.InvalidateGuestCode(libapp_base + hook.offset, 4);
    }
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "string table hooks installed");
}

}  // namespace portstrings
