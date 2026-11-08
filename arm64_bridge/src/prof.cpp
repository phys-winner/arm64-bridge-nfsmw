// Sampling profiler of the GL thread (stage 13 performance work; diagnosis only).
//
// Off unless `adb shell setprop debug.nfsbridge.prof 1` before a cold start (a number above 1 is the sampling period
// in microseconds of the thread's CPU time, default 250). A CPU-time timer of the GL thread
// (timer_create(CLOCK_THREAD_CPUTIME_ID) with SIGEV_THREAD_ID back to the same thread) raises SIGPROF; the handler
// stores the interrupted host pc in a ring and returns. The timer only runs while the thread is on a CPU, so blocked
// system calls are not interrupted. After every RunLoop tick the GL thread drains the ring outside guest execution:
// a pc in one of its Dynarmic code caches becomes the guest address of the translated block it belongs to (the
// prelude, Dynarmic's dispatcher and trampolines, is counted on its own), any other pc is attributed to its shared
// object (dl_iterate_phdr ranges) and offset. Every 10 s the share of each part and the hottest guest blocks and host
// locations go to logcat (tag Arm32Bridge_Prof) and the whole window is appended to <external files>/nfs_prof.txt:
//   W <window> <seconds> <samples>
//   G <guest block address> <samples>
//   H <object> <offset> <samples>
// Guest blocks map to guest functions offline (IDA); host offsets of the bridge map to symbols with llvm-symbolizer
// and the unstripped arm64_bridge/build_arm64/libapp.so.
#include "prof.h"

#include <android/log.h>
#include <link.h>
#include <signal.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dynarmic_host.h"

#define LOG_TAG "Arm32Bridge_Prof"
#include "bridge_log.h"

namespace prof {
namespace {

constexpr char kOutPath[] = "/storage/emulated/0/Android/data/com.ea.games.nfs13_row/files/nfs_prof.txt";
constexpr uint32_t kRingSize = 1u << 16;
constexpr uint64_t kWindowNs = 10000000000ull;

std::atomic<uint32_t> g_head{0};
uint64_t g_ring[kRingSize];

void OnSample(int, siginfo_t*, void* context) {
    const auto* uc = static_cast<const ucontext_t*>(context);
    const uint32_t i = g_head.fetch_add(1, std::memory_order_relaxed);
    g_ring[i & (kRingSize - 1)] = uc->uc_mcontext.pc;
}

struct Object {
    uintptr_t begin, end, bias;  // executable segment [begin, end) at load bias `bias`
    std::string name;
};

struct State {
    int mode = -1;  // -1 not checked yet, 0 off, 1 sampling
    uint32_t tail = 0;
    uint64_t window_start = 0;
    uint32_t window = 0;
    uint64_t samples = 0, jit = 0, prelude = 0, unknown = 0, dropped = 0;
    std::unordered_map<uint32_t, uint32_t> guest;                  // guest block -> samples
    std::unordered_map<uint32_t, uint64_t> guest_host;             // guest block -> a host pc inside its code
    std::unordered_map<uint64_t, uint32_t> host;                   // (object << 40) | offset -> samples
    std::unordered_map<uint32_t, uint64_t> per_object;             // object -> samples
    std::vector<Object> objects;
};
State g;  // GL thread only

int Lookup(uintptr_t pc) {
    for (size_t i = 0; i < g.objects.size(); ++i)
        if (pc >= g.objects[i].begin && pc < g.objects[i].end) return static_cast<int>(i);
    return -1;
}

void LoadObjects() {
    g.objects.clear();
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void*) {
        const char* slash = info->dlpi_name ? strrchr(info->dlpi_name, '/') : nullptr;
        std::string name = slash ? slash + 1 : (info->dlpi_name && *info->dlpi_name ? info->dlpi_name : "[main]");
        for (int i = 0; i < info->dlpi_phnum; ++i) {
            const auto& ph = info->dlpi_phdr[i];
            if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_X)) continue;
            // Host locations are reported as ELF virtual addresses (pc - load bias), as llvm-symbolizer expects.
            g.objects.push_back({info->dlpi_addr + ph.p_vaddr, info->dlpi_addr + ph.p_vaddr + ph.p_memsz, info->dlpi_addr, name});
        }
        return 0;
    }, nullptr);
}

bool Start() {
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.nfsbridge.prof", value) <= 0 || value[0] == '0' || value[0] == '\0') return false;
    long period_us = strtol(value, nullptr, 10);
    if (period_us <= 1) period_us = 250;
    struct sigaction action{};
    action.sa_sigaction = OnSample;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPROF, &action, nullptr) != 0) {
        LOGE("profiler: sigaction failed: %s", strerror(errno));
        return false;
    }
    sigevent event{};
    event.sigev_notify = SIGEV_THREAD_ID;
    event.sigev_signo = SIGPROF;
    event.sigev_notify_thread_id = gettid();
    timer_t timer{};
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &event, &timer) != 0) {
        LOGE("profiler: timer_create failed: %s", strerror(errno));
        return false;
    }
    itimerspec spec{};
    spec.it_interval.tv_nsec = period_us * 1000;
    spec.it_value.tv_nsec = period_us * 1000;
    if (timer_settime(timer, 0, &spec, nullptr) != 0) {
        LOGE("profiler: timer_settime failed: %s", strerror(errno));
        return false;
    }
    LoadObjects();
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "profiler on: GL thread %d, a sample every %ld us of its CPU time, %zu code ranges",
                        gettid(), period_us, g.objects.size());
    return true;
}

void Drain(DynarmicHost& host) {
    const uint32_t head = g_head.load(std::memory_order_acquire);
    if (head - g.tail > kRingSize) {
        g.dropped += head - g.tail - kRingSize;
        g.tail = head - kRingSize;
    }
    for (; g.tail != head; ++g.tail) {
        const uint64_t pc = g_ring[g.tail & (kRingSize - 1)];
        ++g.samples;
        uint32_t guest_pc = 0;
        const int kind = host.ReverseLookupHostPc(reinterpret_cast<const void*>(pc), &guest_pc);
        if (kind == 1) {
            ++g.jit;
            ++g.guest[guest_pc];
            g.guest_host[guest_pc] = pc;
            continue;
        }
        if (kind == 2) {
            ++g.prelude;
            continue;
        }
        int object = Lookup(pc);
        if (object < 0) {
            LoadObjects();  // a library loaded since the last look
            object = Lookup(pc);
        }
        if (object < 0) {
            ++g.unknown;
            continue;
        }
        ++g.per_object[object];
        ++g.host[(uint64_t(object) << 40) | (pc - g.objects[object].bias)];
    }
}

template <typename K>
std::vector<std::pair<K, uint64_t>> Top(const std::unordered_map<K, uint32_t>& map, size_t n) {
    std::vector<std::pair<K, uint64_t>> v(map.begin(), map.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (v.size() > n) v.resize(n);
    return v;
}

void Report(DynarmicHost& host, double seconds) {
    if (!g.samples) return;
    const double total = double(g.samples);
    std::string parts;
    char line[256];
    std::vector<std::pair<uint32_t, uint64_t>> objects(g.per_object.begin(), g.per_object.end());
    std::sort(objects.begin(), objects.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < objects.size() && i < 8; ++i) {
        snprintf(line, sizeof(line), ", %s %.1f%%", g.objects[objects[i].first].name.c_str(), 100.0 * objects[i].second / total);
        parts += line;
    }
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "window %u, %.1f s: %llu samples, JIT code %.1f%%, Dynarmic prelude %.1f%%%s, unknown %.1f%%%s",
                        g.window, seconds, (unsigned long long)g.samples, 100.0 * g.jit / total, 100.0 * g.prelude / total,
                        parts.c_str(), 100.0 * g.unknown / total, g.dropped ? " (ring overflow)" : "");
    std::string text;
    for (const auto& [pc, count] : Top(g.guest, 30)) {
        snprintf(line, sizeof(line), " %08x:%.2f", pc, 100.0 * count / total);
        text += line;
    }
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "hottest guest blocks (%%):%s", text.c_str());
    text.clear();
    for (const auto& [key, count] : Top(g.host, 20)) {
        snprintf(line, sizeof(line), " %s+%llx:%.2f", g.objects[key >> 40].name.c_str(), (unsigned long long)(key & ((1ull << 40) - 1)),
                 100.0 * count / total);
        text += line;
    }
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "hottest host locations (%% of the window, ELF address):%s", text.c_str());
    FILE* f = fopen(kOutPath, "a");
    if (!f) return;
    fprintf(f, "W %u %.3f %llu jit=%llu prelude=%llu unknown=%llu\n", g.window, seconds, (unsigned long long)g.samples,
            (unsigned long long)g.jit, (unsigned long long)g.prelude, (unsigned long long)g.unknown);
    for (const auto& [pc, count] : g.guest) fprintf(f, "G %08x %u\n", pc, count);
    for (const auto& [key, count] : g.host)
        fprintf(f, "H %s %llx %u\n", g.objects[key >> 40].name.c_str(), (unsigned long long)(key & ((1ull << 40) - 1)), count);
    // Host code of the hottest guest blocks (disassembled offline): C <guest block> <host entry> <hex bytes>
    for (const auto& [pc, count] : Top(g.guest, 12)) {
        const void* code = nullptr;
        size_t size = 0;
        if (!host.HostBlockCode(reinterpret_cast<const void*>(g.guest_host[pc]), &code, &size) || size > 65536) continue;
        fprintf(f, "C %08x %llx ", pc, (unsigned long long)reinterpret_cast<uintptr_t>(code));
        for (size_t i = 0; i < size; ++i) fprintf(f, "%02x", static_cast<const uint8_t*>(code)[i]);
        fputc('\n', f);
    }
    fclose(f);
}

}  // namespace

void OnGlTick(DynarmicHost& host, uint64_t now) {
    if (g.mode < 0) {
        g.mode = Start() ? 1 : 0;
        g.window_start = now;
        g.tail = g_head.load(std::memory_order_acquire);
    }
    if (g.mode != 1) return;
    Drain(host);
    if (now - g.window_start < kWindowNs) return;
    Report(host, (now - g.window_start) / 1e9);
    ++g.window;
    g.window_start = now;
    g.samples = g.jit = g.prelude = g.unknown = g.dropped = 0;
    g.guest.clear();
    g.guest_host.clear();
    g.host.clear();
    g.per_object.clear();
}

}  // namespace prof
