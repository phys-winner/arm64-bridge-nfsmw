#include "dynarmic_host.h"
#include "elf_loader.h"
#include "thunk_manager.h"
#include "guest_cp15.h"
#include "jni_fake_env.h"
#include "thunks_opensles.h"
#include "log_gate.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <stdexcept>
#include <fstream>
#include <iterator>
#include <android/log.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#define LOG_TAG "Arm32Bridge_Host"
#include "bridge_log.h"

namespace {
constexpr uint32_t kExitArm = 4;
constexpr uint32_t kExitThumb = 8;
constexpr uint32_t kBlockMagic = 0x41524D31;
constexpr uint32_t kFreedMagic = 0x41524D46;
constexpr uint32_t kHeaderSize = 16;
bool PowerOfTwo(size_t value) { return value && !(value & (value - 1)); }


}

void GuestSyncForget(uint32_t addr, uint32_t size);
void GuestLibcThreadExit(DynarmicHost& host);

struct DynarmicHost::ThreadState {
    DynarmicHost* owner = nullptr;
    uint32_t slot = kMaxThreads;
    uint32_t stack_bottom = 0;
    uint32_t stack_top = 0;
    uint32_t errno_addr = 0;
    uint32_t tls_allocation = 0;
    uint32_t tls_address = 0;
    uint32_t tls_writable = 0;
    uint32_t fpscr = 0;
    uint32_t result_hi = 0;
    size_t depth = 0;
    Dynarmic::A32::Jit* active = nullptr;
    bool exiting = false;
    uint32_t exit_value = 0;
    std::array<std::unique_ptr<Dynarmic::A32::Jit>, kMaxDepth> jits;
    std::array<uint32_t, kMaxDepth> processor_ids = MakeNoProcessors();
    ~ThreadState() { if (owner) owner->RetireThread(*this); }
    static std::array<uint32_t, kMaxDepth> MakeNoProcessors() {
        std::array<uint32_t, kMaxDepth> ids{};
        ids.fill(kNoProcessor);
        return ids;
    }
};
thread_local std::unique_ptr<DynarmicHost::ThreadState> DynarmicHost::t_thread;

DynarmicHost::DynarmicHost() = default;
DynarmicHost::~DynarmicHost() {
    StopAudioThread();
    FakeJniEnv::Instance().Shutdown(*this);
    ReleaseCurrentThread();
    {
        std::lock_guard<std::mutex> lock(m_jits_mutex);
        if (m_stack_slots.any()) {
            logs::Abort(LOG_TAG, "Host destroyed while guest threads remain");
        }
    }
    if (m_memory) munmap(m_memory, m_memory_size);
}

bool DynarmicHost::Initialize(bool exact_jit) {
    if (m_memory) return false;
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || !PowerOfTwo(page_size) || size_t(page_size) >= kStackSize) return false;
    m_page_size = size_t(page_size);
    m_large.granule = uint32_t(m_page_size);  // large blocks start and end on pages: a free one goes back whole
    {
        char value[PROP_VALUE_MAX]{};
        if (__system_property_get("debug.nfsbridge.bigalloc", value) > 0 && atol(value) > 0)
            m_alloc_log_bytes = size_t(atol(value)) << 20;
        value[0] = '\0';
        if (__system_property_get("debug.nfsbridge.jitcache", value) > 0 && atol(value) >= 4 && atol(value) < 64) {
            m_code_cache_outer = size_t(atol(value)) << 20;
            m_code_cache_nested = std::min(m_code_cache_outer, kCodeCacheNested);
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "code cache %zu MiB per thread, %zu MiB nested (debug.nfsbridge.jitcache)",
                                m_code_cache_outer >> 20, m_code_cache_nested >> 20);
        }
    }
    m_memory_size = GUEST_ADDRESS_SPACE;
    void* memory = mmap(nullptr, m_memory_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (memory == MAP_FAILED) { m_memory_size = 0; LOGE("Cannot reserve guest 4-GiB address space"); return false; }
    m_memory = static_cast<uint8_t*>(memory);
    m_monitor = std::make_unique<Dynarmic::ExclusiveMonitor>(kMonitorProcessors);
    m_monitor->SetScanLimit(0);  // no JIT yet; grows with the processor ids handed out (EnsureJit)
    m_config.callbacks = this;
    m_config.global_monitor = m_monitor.get();
    m_config.arch_version = Dynarmic::A32::ArchVersion::v7;
    m_config.enable_cycle_counting = false;
    m_config.always_little_endian = true;
    m_config.define_unpredictable_behaviour = true;
    m_config.fastmem_pointer = reinterpret_cast<uintptr_t>(m_memory);
    m_config.code_cache_size = kCodeCacheNested;
    // The game never reads the cumulative floating-point exception flags of FPSCR (no VMRS of IOC/DZC/OFC/UFC/IXC in
    // libapp, FMOD or libc++), so the JIT does not track them: without this every floating-point instruction next to a
    // memory access paid an MSR/MRS FPSR pair (stage 13 profile: 40 such pairs in the engine's 4x4 matrix multiply).
    // setprop debug.nfsbridge.jitfpsr exact restores the exact behaviour for comparison.
    // Exclusive loads and stores (LDREX/STREX, the engine's reference counts and locks) run inline as a
    // compare-and-swap instead of calling into the global monitor (patches/dynarmic_bridge.patch);
    // setprop debug.nfsbridge.jitexcl monitor restores the monitor. A translated process (exact_jit, stage 16) keeps
    // both exact, as the JIT was up to b27, the last build reported to run on PC emulators.
    {
        char mode[PROP_VALUE_MAX]{};
        auto optimizations = Dynarmic::all_safe_optimizations;
        __system_property_get("debug.nfsbridge.jitfpsr", mode);
        if (!exact_jit && strcmp(mode, "exact") != 0) optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreFPSRAccumulation;
        mode[0] = '\0';
        __system_property_get("debug.nfsbridge.jitexcl", mode);
        if (!exact_jit && strcmp(mode, "monitor") != 0) optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        m_config.unsafe_optimizations = optimizations != Dynarmic::all_safe_optimizations;
        m_config.optimizations = optimizations;
        if (exact_jit)
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "translated process: exact JIT (FPSR flags tracked, exclusive monitor)");
    }
    m_config.coprocessors[15] = std::make_shared<GuestCp15>(*this);
    MemoryWrite32(kExitArm, 0xEF000000);
    MemoryWrite16(kExitThumb, 0xDF00);
    AddExecutableRange(kExitArm, 4);
    AddExecutableRange(kExitThumb, 2);
    m_thunk_manager = std::make_unique<ThunkManager>(*this);
    m_thunk_manager->RegisterAllThunks();
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Dynarmic Host initialized: AArch64 host, A32 guest, %zu bytes, page=%zu, per-thread JIT", m_memory_size, m_page_size);
    return true;
}

namespace {
std::vector<uint8_t> ReadImage(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}

bool DynarmicHost::LoadGame(const std::string& path) {
    const auto image = ReadImage(path);
    return LoadGameFromMemory(image.data(), image.size());
}

bool DynarmicHost::LoadGameFromMemory(const uint8_t* data, size_t size) {
    return LoadModulesFromMemory({{"game", data, size, 0x40000000, 0x50000000}});
}

bool DynarmicHost::LoadLibraries(const std::string& fmodex, const std::string& fmodevent, const std::string& game) {
    const auto a = ReadImage(fmodex), b = ReadImage(fmodevent), c = ReadImage(game);
    return LoadLibrariesFromMemory(a.data(), a.size(), b.data(), b.size(), c.data(), c.size());
}

bool DynarmicHost::LoadLibrariesFromMemory(const uint8_t* fmodex, size_t fmodex_size,
                                         const uint8_t* fmodevent, size_t fmodevent_size,
                                         const uint8_t* game, size_t game_size) {
    // Compatibility layout. Larger games should supply explicit disjoint reservations.
    return LoadModulesFromMemory({
        {"libfmodex.so", fmodex, fmodex_size, 0x40400000, 0x40600000},
        {"libfmodevent.so", fmodevent, fmodevent_size, 0x40600000, 0x50000000},
        {"game", game, game_size, 0x40000000, 0x40400000},
    });
}

bool DynarmicHost::LoadModulesFromMemory(const std::vector<GuestModuleImage>& inputs) {
    if (!m_memory || !m_libraries.empty() || inputs.empty() || HasFailed()) return false;
    for (size_t i = 0; i < inputs.size(); ++i) {
        const auto& in = inputs[i];
        if (in.name.empty() || !in.data || !in.size || in.base < 0x00100000 ||
            in.base >= in.limit || in.limit > kHeapBase) return false;
        for (size_t j = 0; j < i; ++j)
            if (in.name == inputs[j].name || (in.base < inputs[j].limit && inputs[j].base < in.limit)) return false;
    }
    for (const auto& in : inputs) {
        auto elf = std::make_unique<ElfLoader>(m_memory, m_memory_size, in.base, in.limit);
        if (!elf->LoadFromMemory(in.data, in.size)) { Fail("Guest ELF load failed"); return false; }
        m_libraries.push_back({in.name, std::move(elf), false});
    }
    auto resolver = [this](const std::string& name, uint8_t type) {
        for (auto it = m_libraries.rbegin(); it != m_libraries.rend(); ++it)
            if (uint32_t address = it->elf->GetSymbolAddress(name, type)) return address;
        return m_thunk_manager->ResolveSymbol(name, type);
    };
    for (const auto& library : m_libraries)
        if (!library.elf->Relocate(resolver)) { Fail("Guest ELF relocation failed"); return false; }
    m_libraries_ready.store(true, std::memory_order_release);
    for (const auto& library : m_libraries)
        if (!RunLibraryConstructors(library.name)) return false;
    return !HasFailed();
}

bool DynarmicHost::RunLibraryConstructors(const std::string& name) {
    for (auto& library : m_libraries) {
        if (library.name != name) continue;
        if (library.constructors_ran) return !HasFailed();
        library.constructors_ran = true;
        for (uint32_t function : library.elf->GetInitArray()) {
            RunFunction(function);
            if (HasFailed()) return false;
        }
        return true;
    }
    return false;
}

ElfLoader* DynarmicHost::FindModule(const std::string& name) {
    for (const auto& library : m_libraries) if (library.name == name) return library.elf.get();
    return nullptr;
}
size_t DynarmicHost::ModuleCount() const {
    return m_libraries_ready.load(std::memory_order_acquire) ? m_libraries.size() : 0;
}
const ElfLoader* DynarmicHost::ModuleAt(size_t index, std::string* name) const {
    if (index >= ModuleCount()) return nullptr;
    if (name) *name = m_libraries[index].name;
    return m_libraries[index].elf.get();
}
const ElfLoader* DynarmicHost::ModuleForAddress(uint32_t address, std::string* name) const {
    for (size_t i = 0; i < ModuleCount(); ++i)
        if (m_libraries[i].elf->ContainsAddress(address)) {
            if (name) *name = m_libraries[i].name;
            return m_libraries[i].elf.get();
        }
    return nullptr;
}
void DynarmicHost::CurrentGuestStack(uint32_t& bottom, uint32_t& top) {
    auto& thread = EnsureThread();
    bottom = thread.stack_bottom;
    top = thread.stack_top;
}
ElfLoader& DynarmicHost::GetElfLoader() {
    if (auto* game = FindModule("game")) return *game;
    if (m_libraries.empty()) throw std::logic_error("Guest ELF is not loaded");
    return *m_libraries.back().elf;
}
ElfLoader* DynarmicHost::GetFmodExLoader() { return FindModule("libfmodex.so"); }
ElfLoader* DynarmicHost::GetFmodEventLoader() { return FindModule("libfmodevent.so"); }
uint32_t DynarmicHost::FindLibrarySymbol(const std::string& library, const std::string& symbol) const {
    if (!m_libraries_ready.load(std::memory_order_acquire)) return 0;
    for (const auto& item : m_libraries) if (item.name == library) return item.elf->GetSymbolAddress(symbol);
    return 0;
}
uint32_t DynarmicHost::FindSymbol(const std::string& name) {
    if (m_libraries_ready.load(std::memory_order_acquire))
        for (auto it = m_libraries.rbegin(); it != m_libraries.rend(); ++it)
            if (uint32_t address = it->elf->GetSymbolAddress(name)) return address;
    return m_thunk_manager ? m_thunk_manager->FindRegisteredSymbol(name) : 0;
}

DynarmicHost::ThreadState& DynarmicHost::EnsureThread() {
    if (t_thread) {
        if (t_thread->owner != this) logs::Abort(LOG_TAG, "Multiple guest hosts on one thread");
        return *t_thread;
    }
    auto thread = std::make_unique<ThreadState>();
    thread->owner = this;
    {
        std::lock_guard<std::mutex> lock(m_jits_mutex);
        for (size_t slot = 0; slot < kMaxThreads; ++slot) {
            if (!m_stack_slots[slot]) { m_stack_slots.set(slot); thread->slot = slot; break; }
        }
    }
    if (thread->slot == kMaxThreads) logs::Abort(LOG_TAG, "Guest thread pool exhausted (%zu threads)", kMaxThreads);
    const uint32_t bottom = kStackBase + thread->slot * kStackStride;
    thread->stack_bottom = bottom + m_page_size;
    thread->stack_top = bottom + kStackSize;
    if (mprotect(m_memory + bottom, m_page_size, PROT_NONE) != 0 ||
        mprotect(m_memory + thread->stack_top, m_page_size, PROT_NONE) != 0) {
        logs::Abort(LOG_TAG, "Could not protect guest stack guards");
    }
    thread->tls_allocation = GuestAlloc(256);
    thread->tls_address = thread->tls_allocation;
    thread->errno_addr = thread->tls_allocation ? thread->tls_allocation + 8 : 0;
    if (!thread->errno_addr) logs::Abort(LOG_TAG, "Could not allocate thread errno");
    MemoryWrite32(thread->tls_address, thread->tls_address);
    MemoryWrite32(thread->tls_address + 4, thread->slot + 1);
    t_thread = std::move(thread);
    return *t_thread;
}

Dynarmic::A32::Jit& DynarmicHost::EnsureJit(ThreadState& thread, size_t depth) {
    if (depth >= kMaxDepth) logs::Abort(LOG_TAG, "Guest nested JIT depth exhausted (%zu)", kMaxDepth);
    if (!thread.jits[depth]) {
        auto config = m_config;
        size_t id = kMonitorProcessors;
        {
            std::lock_guard<std::mutex> lock(m_jits_mutex);
            for (size_t i = 0; i < kMonitorProcessors; ++i) {
                if (!m_processor_ids.test(i)) { id = i; break; }
            }
            if (id == kMonitorProcessors) logs::Abort(LOG_TAG, "Guest JIT processor pool exhausted (%zu)", kMonitorProcessors);
            m_processor_ids.set(id);
            if (id + 1 > m_processor_high_water) {
                m_processor_high_water = id + 1;
                m_monitor->SetScanLimit(m_processor_high_water);
            }
        }
        thread.processor_ids[depth] = static_cast<uint32_t>(id);
        config.processor_id = id;
        // A thread's outermost JIT runs all of its guest code; the game's GL thread translates ~125k blocks
        // (~16 MiB of host code) over boot, menus and one race, and a full cache is cleared as a whole (a
        // 1.3 s retranslation stall on bridge_target). Nested JITs only run callbacks. Reservations are
        // virtual; only translated code becomes resident.
        config.code_cache_size = depth == 0 ? m_code_cache_outer : m_code_cache_nested;
        auto jit = std::make_unique<Dynarmic::A32::Jit>(config);
        {
            std::lock_guard<std::mutex> lock(m_jits_mutex);
            m_monitor->ClearProcessor(id);
            m_live_jits[id] = jit.get();
            thread.jits[depth] = std::move(jit);
        }
    }
    return *thread.jits[depth];
}

Dynarmic::A32::Jit& DynarmicHost::CurrentJit() {
    auto& thread = EnsureThread();
    return thread.active ? *thread.active : EnsureJit(thread, 0);
}

int DynarmicHost::ReverseLookupHostPc(const void* host_pc, uint32_t* guest_pc) {
    if (!t_thread || t_thread->owner != this || t_thread->depth) return 0;
    for (auto& jit : t_thread->jits) {
        if (!jit) continue;
        bool thumb = false;
        const int kind = jit->ReverseLookup(host_pc, guest_pc, &thumb);
        if (kind == 1 && thumb) *guest_pc |= 1;
        if (kind) return kind;
    }
    return 0;
}

bool DynarmicHost::HostBlockCode(const void* host_pc, const void** code, size_t* size) {
    if (!t_thread || t_thread->owner != this || t_thread->depth) return false;
    for (auto& jit : t_thread->jits) {
        uint64_t descriptor = 0;
        if (jit && jit->ReverseLookupDescriptor(host_pc, &descriptor)) return jit->BlockCode(descriptor, code, size);
    }
    return false;
}

uint32_t DynarmicHost::ReadStackWord(Dynarmic::A32::Jit& jit, uint32_t spilled_word_index) {
    if (!t_thread || t_thread->owner != this || t_thread->active != &jit || !t_thread->depth)
        throw std::out_of_range("Stack read requires this thread's active guest JIT");
    const uint64_t sp = jit.Regs()[13];
    const uint64_t address = sp + uint64_t(spilled_word_index) * sizeof(uint32_t);
    if ((sp & 3) || sp < t_thread->stack_bottom || sp > t_thread->stack_top ||
        address < t_thread->stack_bottom || address + sizeof(uint32_t) > t_thread->stack_top ||
        address + sizeof(uint32_t) > m_memory_size)
        throw std::out_of_range("Guest argument lies outside the active thread's usable stack");
    uint32_t value;
    memcpy(&value, m_memory + address, sizeof(value));
    return value;
}

bool DynarmicHost::IsJitRunning() const {
    return t_thread && t_thread->owner == this && t_thread->depth != 0;
}

uint32_t DynarmicHost::GuestErrnoAddress() { return EnsureThread().errno_addr; }
uint32_t DynarmicHost::GuestTlsAddress() { return EnsureThread().tls_address; }
uint32_t DynarmicHost::GuestTlsWritable() { return EnsureThread().tls_writable; }
void DynarmicHost::SetGuestTlsWritable(uint32_t value) { EnsureThread().tls_writable=value; }
void DynarmicHost::SetGuestTlsAddress(uint32_t address) {
    if (!address || uint64_t(address)+256>m_memory_size) { Fail("Invalid guest TLS address");return; }
    auto& thread=EnsureThread();thread.tls_address=address;thread.errno_addr=address+8;
}

void DynarmicHost::RetireThread(ThreadState& thread) {
    if (thread.depth) logs::Abort(LOG_TAG, "Attempted to free executing guest thread");
    if (thread.slot == kMaxThreads) return;
    {
        std::lock_guard<std::mutex> lock(m_jits_mutex);
        for (size_t depth = 0; depth < kMaxDepth; ++depth) {
            const uint32_t id = thread.processor_ids[depth];
            if (id == kNoProcessor) continue;
            m_live_jits[id] = nullptr;
            m_monitor->ClearProcessor(id);
        }
    }
    for (auto& jit : thread.jits) jit.reset();
    {
        // Ids return to the pool only after their JITs are gone.
        std::lock_guard<std::mutex> lock(m_jits_mutex);
        for (auto& id : thread.processor_ids) {
            if (id != kNoProcessor) m_processor_ids.reset(id);
            id = kNoProcessor;
        }
    }
    GuestFree(thread.tls_allocation);
    const uint32_t bottom = kStackBase + thread.slot * kStackStride;
    if (madvise(m_memory + bottom, kStackStride, MADV_DONTNEED) != 0) Fail("Guest stack discard failed");
    std::lock_guard<std::mutex> lock(m_jits_mutex);
    m_stack_slots.reset(thread.slot);
}

void DynarmicHost::ReleaseCurrentThread() {
    if (!t_thread || t_thread->owner != this) return;
    if (t_thread->depth) { Fail("Guest thread release while executing"); return; }
    BeginThreadCleanup();
    GuestLibcThreadExit(*this);
    t_thread.reset();
}

bool DynarmicHost::IsCurrentGuestThreadExiting() const {
    return t_thread && t_thread->owner == this && t_thread->exiting;
}

void DynarmicHost::BeginThreadCleanup() {
    if (!t_thread || t_thread->owner != this) return;
    if (t_thread->depth) { Fail("Thread cleanup attempted inside guest execution"); return; }
    t_thread->exiting = false;
    for (auto& jit : t_thread->jits)
        if (jit) jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
}

void DynarmicHost::ExitCurrentGuestThread(uint32_t result) {
    auto& thread = EnsureThread();
    thread.exiting = true;
    thread.exit_value = result;
    for (auto& jit : thread.jits)
        if (jit) jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
}

void DynarmicHost::Fail(const char* reason) {
    if (!m_failed.exchange(true)) {
        LOGE("Bridge FAILED: %s", reason);
        logs::NoteFailure(reason);
    }
    std::lock_guard<std::mutex> lock(m_jits_mutex);
    for (auto* jit : m_live_jits)
        if (jit) jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
}

uint32_t DynarmicHost::RunFunction(uint32_t target, const std::vector<uint32_t>& args) {
    if (HasFailed() || !m_memory) return 0;
    if (!IsExecutableAddress(target)) { Fail("Guest entry is not executable"); return 0; }
    auto& thread = EnsureThread();
    if (thread.exiting) return thread.exit_value;
    const size_t depth = thread.depth;
    auto& jit = EnsureJit(thread, depth);
    auto* outer = thread.active;
    const uint32_t old_sp = outer ? outer->Regs()[13] : thread.stack_top;
    if (old_sp < thread.stack_bottom || old_sp > thread.stack_top || (depth && old_sp - thread.stack_bottom < 256)) {
        Fail("Invalid guest stack at call boundary"); return 0;
    }
    const uint32_t top = depth ? old_sp - 256 : thread.stack_top;
    const uint64_t extra = args.size() > 4 ? uint64_t(args.size() - 4) * 4 : 0;
    if (extra > uint64_t(top) - thread.stack_bottom) { Fail("Guest arguments exceed stack"); return 0; }
    const uint32_t sp = (top - uint32_t(extra)) & ~7u;
    if (sp < thread.stack_bottom) { Fail("Guest argument alignment crosses stack guard"); return 0; }
    jit.Regs().fill(0);
    jit.ExtRegs().fill(0);
    jit.SetCpsr(0x10 | ((target & 1) ? 0x20 : 0));
    jit.SetFpscr(outer ? outer->Fpscr() : thread.fpscr);
    jit.ClearExclusiveState();
    m_monitor->ClearProcessor(thread.processor_ids[depth]);
    for (size_t i = 0; i < std::min(args.size(), size_t(4)); ++i) jit.Regs()[i] = args[i];
    for (size_t i = 4; i < args.size(); ++i) MemoryWrite32(sp + uint32_t(i - 4) * 4, args[i]);
    jit.Regs()[13] = sp;
    jit.Regs()[14] = (target & 1) ? (kExitThumb | 1) : kExitArm;
    jit.Regs()[15] = target & ~1u;
    thread.active = &jit;
    ++thread.depth;
    // No host registry/allocator/JIT lock is held over Run or a blocking/reentrant thunk.
    for (;;) {
        const auto reason = jit.Run();
        if (thread.exiting || HasFailed()) break;
        const uint32_t bits = static_cast<uint32_t>(reason);
        const uint32_t cache = static_cast<uint32_t>(Dynarmic::HaltReason::CacheInvalidation);
        if (bits && !(bits & ~cache)) continue;
        const uint32_t pc = jit.Regs()[15];
        const uint32_t returned = static_cast<uint32_t>(Dynarmic::HaltReason::UserDefined1);
        if ((bits & returned) && !(bits & ~(cache | returned)) &&
            (pc == kExitArm || pc == kExitArm + 4 || pc == kExitThumb || pc == kExitThumb + 2)) break;
        LOGE("Unexpected guest halt: entry=0x%08x PC=0x%08x LR=0x%08x reason=0x%x", target, pc, jit.Regs()[14], bits);
        Fail("Unexpected guest halt"); break;
    }
    thread.fpscr = jit.Fpscr();
    if (outer) outer->SetFpscr(thread.fpscr);
    thread.result_hi = jit.Regs()[1];
    const uint32_t result = thread.exiting ? thread.exit_value : jit.Regs()[0];
    --thread.depth;
    thread.active = outer;
    return result;
}

uint32_t DynarmicHost::CallGuestNested(uint32_t target, const std::vector<uint32_t>& args) {
    return RunFunction(target, args);
}

void DynarmicHost::InvalidateGuestCode(uint32_t address, size_t size) {
    std::lock_guard<std::mutex> lock(m_jits_mutex);
    for (auto* jit : m_live_jits)
        if (jit) jit->InvalidateCacheRange(address, size);
}

uint32_t DynarmicHost::GuestAlloc(size_t size, size_t alignment) {
    if (!PowerOfTwo(alignment) || size > kLargeLimit - kLargeBase || alignment > kLargeLimit - kLargeBase) return 0;
    if (!size) size = 1;
    alignment = std::max(alignment, size_t(8));
    const uint64_t need = uint64_t(size) + alignment + kHeaderSize;
    if (need > kLargeLimit - kLargeBase) return 0;
    uint32_t base = 0, capacity = 0;
    bool reused = false;
    HeapStats failed{};
    {
        std::lock_guard<std::mutex> lock(m_heap_mutex);
        if (need <= kMaxClassSize) {
            int cls = 0;
            while ((1u << (cls + kMinClassShift)) < need) ++cls;
            capacity = 1u << (cls + kMinClassShift);
            auto& list = m_free_small[cls];
            if (!list.empty()) {
                base = list.back(); list.pop_back(); reused = true;
            } else if (uint64_t(m_main.current) + capacity <= m_main.limit) {
                base = m_main.current; m_main.current += capacity;
            }
        } else {
            // Large requests prefer the large arena, the rest the main heap; each falls back to the other one.
            BlockArena& first = need >= kLargeBlock ? m_large : m_main;
            BlockArena& second = &first == &m_large ? m_main : m_large;
            base = AllocFromArena(first, need, capacity, reused);
            if (!base) base = AllocFromArena(second, need, capacity, reused);
        }
        if (!base) {
            ++m_alloc_failures;
            m_last_failed_size = uint32_t(std::min<size_t>(size, UINT32_MAX));
        }
    }
    if (!base) {
        failed = GetHeapStats();
        char reason[200];
        snprintf(reason, sizeof(reason), "guest heap exhausted: %zu bytes; main hw %u KiB (%u KiB free), large hw %u KiB "
                 "(%u KiB free)", size, failed.heap_high_water / 1024, failed.heap_free_huge / 1024,
                 failed.large_high_water / 1024, failed.large_free / 1024);
        LOGE("%s; main %u free blocks + %u KiB small, large %u free blocks", reason, failed.huge_free_blocks,
             failed.heap_free_small / 1024, failed.large_free_blocks);
        logs::NoteFailure(reason);
        return 0;
    }
    const uint32_t payload = uint32_t((uint64_t(base) + kHeaderSize + alignment - 1) & ~(uint64_t(alignment) - 1));
    if (reused) memset(m_memory + payload, 0, size);
    MemoryWrite32(payload - 16, kBlockMagic);
    MemoryWrite32(payload - 12, base);
    MemoryWrite32(payload - 8, capacity);
    MemoryWrite32(payload - 4, uint32_t(size));
    if (m_alloc_log_bytes && size >= m_alloc_log_bytes) {
        const uint32_t lr = t_thread && t_thread->active ? t_thread->active->Regs()[14] : 0;
        std::string module;
        const ElfLoader* owner = ModuleForAddress(lr & ~1u, &module);
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "guest alloc %zu bytes -> 0x%08x (%s arena) lr=0x%08x (%s+0x%x)",
                            size, payload, base >= kLargeBase ? "large" : "main", lr, owner ? module.c_str() : "?",
                            owner ? (lr & ~1u) - owner->GetBaseAddress() : 0u);
    }
    return payload;
}

uint32_t DynarmicHost::AllocFromArena(BlockArena& arena, uint64_t need, uint32_t& capacity, bool& reused) {
    const uint64_t rounded = (need + arena.granule - 1) & ~uint64_t(arena.granule - 1);
    if (rounded > arena.limit - arena.base) return 0;
    capacity = uint32_t(rounded);
    // Best fit among the free blocks; split when the rest is itself a huge block.
    uint32_t source = 0, source_end = 0;
    auto fit = arena.by_size.lower_bound(capacity);
    if (fit != arena.by_size.end()) {
        source = fit->second; source_end = fit->second + fit->first;
        EraseFreeHuge(arena, arena.by_address.find(source));
    } else if (!arena.by_address.empty()) {
        // The topmost free block borders the bump pointer: grow it instead of leaving it behind.
        auto top = std::prev(arena.by_address.end());
        if (top->first + top->second == arena.current && uint64_t(top->first) + capacity <= arena.limit) {
            source = top->first; source_end = top->first + top->second;
            EraseFreeHuge(arena, top);
            arena.current = source + capacity;
            source_end = std::min(source_end, arena.current);
        }
    }
    if (source) {
        reused = true;
        const uint32_t rest = source_end > source + capacity ? source_end - (source + capacity) : 0;
        if (rest > kMaxClassSize) InsertFreeHuge(arena, source + capacity, rest, false);
        else capacity = std::max(capacity, source_end - source);
        return source;
    }
    if (uint64_t(arena.current) + capacity > arena.limit) return 0;
    reused = false;
    const uint32_t base = arena.current;
    arena.current += capacity;
    return base;
}

DynarmicHost::BlockArena* DynarmicHost::ArenaOf(uint32_t address) {
    if (address >= m_main.base && address < m_main.current) return &m_main;
    if (address >= m_large.base && address < m_large.current) return &m_large;
    return nullptr;
}

const DynarmicHost::BlockArena* DynarmicHost::ArenaOf(uint32_t address) const {
    return const_cast<DynarmicHost*>(this)->ArenaOf(address);
}

bool DynarmicHost::ValidAllocation(uint32_t address) const {
    if (address < kHeaderSize) return false;
    const BlockArena* arena = ArenaOf(address - kHeaderSize);
    if (!arena) return false;
    uint32_t header[4]; memcpy(header, m_memory + address - kHeaderSize, sizeof(header));
    const uint32_t base = header[1], capacity = header[2], size = header[3];
    return header[0] == kBlockMagic && base >= arena->base && (base & 15) == 0 &&
           capacity >= (1u << kMinClassShift) && !(capacity & 15) &&
           (capacity > kMaxClassSize || (PowerOfTwo(capacity) && arena == &m_main)) &&
           uint64_t(base) + capacity <= arena->current && address >= base + kHeaderSize &&
           size && uint64_t(address) + size <= uint64_t(base) + capacity;
}

void DynarmicHost::GuestFree(uint32_t address) {
    uint32_t base, capacity, size;
    {
        std::lock_guard<std::mutex> lock(m_heap_mutex);
        if (!ValidAllocation(address)) return;
        base = MemoryRead32(address - 12); capacity = MemoryRead32(address - 8); size = MemoryRead32(address - 4);
        MemoryWrite32(address - 16, kFreedMagic);
    }
    // Retire address-keyed synchronization before another allocator can recycle this block.
    GuestSyncForget(address, size);
    std::lock_guard<std::mutex> lock(m_heap_mutex);
    if (capacity <= kMaxClassSize) {
        int cls = 0;
        while ((1u << (cls + kMinClassShift)) < capacity) ++cls;
        m_free_small[cls].push_back(base);
    } else InsertFreeHuge(base >= kLargeBase ? m_large : m_main, base, capacity, true);
}

void DynarmicHost::EraseFreeHuge(BlockArena& arena, std::map<uint32_t, uint32_t>::iterator block) {
    auto range = arena.by_size.equal_range(block->second);
    for (auto it = range.first; it != range.second; ++it)
        if (it->second == block->first) { arena.by_size.erase(it); break; }
    arena.free_bytes -= block->second;
    arena.by_address.erase(block);
}

void DynarmicHost::InsertFreeHuge(BlockArena& arena, uint32_t base, uint32_t capacity, bool freed) {
    const DirtyRange range{base, capacity};
    uint32_t end = base + capacity;
    auto next = arena.by_address.lower_bound(base);
    if (next != arena.by_address.end() && next->first == end) {
        end += next->second;
        EraseFreeHuge(arena, next);
    }
    auto after = arena.by_address.lower_bound(base);
    if (after != arena.by_address.begin()) {
        auto previous = std::prev(after);
        if (previous->first + previous->second == base) {
            base = previous->first;
            EraseFreeHuge(arena, previous);
        }
    }
    arena.by_address.emplace(base, end - base);
    arena.by_size.emplace(end - base, base);
    arena.free_bytes += end - base;
    if (!freed) return;
    arena.dirty.push_back(range);
    arena.dirty_bytes += range.size;
    PurgeDirtyHuge(arena);
}

void DynarmicHost::PurgeDirtyHuge(BlockArena& arena) {
    const uint64_t page = m_page_size;
    while (arena.dirty_bytes > arena.dirty_limit && !arena.dirty.empty()) {
        const DirtyRange range = arena.dirty.front();
        arena.dirty.pop_front();
        arena.dirty_bytes -= range.size;
        // Return only the parts that are still free; reallocated parts are live again.
        const uint64_t range_end = uint64_t(range.base) + range.size;
        auto it = arena.by_address.upper_bound(range.base);
        if (it != arena.by_address.begin()) --it;
        for (; it != arena.by_address.end() && it->first < range_end; ++it) {
            const uint64_t low = std::max<uint64_t>(it->first, range.base);
            const uint64_t high = std::min<uint64_t>(uint64_t(it->first) + it->second, range_end);
            const uint64_t first = (low + page - 1) & ~(page - 1), last = high & ~(page - 1);
            if (last > first) madvise(m_memory + first, size_t(last - first), MADV_DONTNEED);
        }
    }
}

DynarmicHost::HeapStats DynarmicHost::GetHeapStats() {
    std::lock_guard<std::mutex> lock(m_heap_mutex);
    HeapStats stats{};
    stats.heap_high_water = m_main.current - kHeapBase;
    for (int cls = 0; cls < kNumSizeClasses; ++cls)
        stats.heap_free_small += uint32_t(m_free_small[cls].size()) << (cls + kMinClassShift);
    stats.heap_free_huge = uint32_t(m_main.free_bytes);
    stats.huge_free_blocks = uint32_t(m_main.by_address.size());
    stats.huge_dirty = uint32_t(m_main.dirty_bytes);
    stats.large_high_water = m_large.current - kLargeBase;
    stats.large_free = uint32_t(m_large.free_bytes);
    stats.large_free_blocks = uint32_t(m_large.by_address.size());
    stats.large_dirty = uint32_t(m_large.dirty_bytes);
    stats.map_high_water = m_map_current - kMapBase;
    for (const auto& [base, size] : m_map_free) stats.map_free += size;
    stats.alloc_failures = m_alloc_failures;
    stats.last_failed_size = m_last_failed_size;
    return stats;
}

uint32_t DynarmicHost::GuestAllocSize(uint32_t address) {
    std::lock_guard<std::mutex> lock(m_heap_mutex);
    return ValidAllocation(address) ? MemoryRead32(address - 4) : 0;
}

uint32_t DynarmicHost::GuestAllocMapping(size_t size, size_t alignment) {
    if (!size || !PowerOfTwo(alignment) || size > kMapLimit - kMapBase) return 0;
    std::lock_guard<std::mutex> lock(m_heap_mutex);
    // First fit among unmapped ranges (their pages were replaced by fresh zero pages).
    for (auto it = m_map_free.begin(); it != m_map_free.end(); ++it) {
        const uint64_t aligned = (uint64_t(it->first) + alignment - 1) & ~(uint64_t(alignment) - 1);
        const uint64_t end = uint64_t(it->first) + it->second;
        if (aligned + size > end) continue;
        const uint32_t base = it->first;
        m_map_free.erase(it);
        if (aligned > base) m_map_free.emplace(base, uint32_t(aligned - base));
        if (aligned + size < end) m_map_free.emplace(uint32_t(aligned + size), uint32_t(end - aligned - size));
        return uint32_t(aligned);
    }
    const uint64_t aligned = (uint64_t(m_map_current) + alignment - 1) & ~(uint64_t(alignment) - 1);
    if (aligned >= kMapLimit || size > kMapLimit - aligned) return 0;
    m_map_current = uint32_t(aligned + size);
    return uint32_t(aligned);
}

void DynarmicHost::GuestFreeMapping(uint32_t address, size_t size) {
    if (!size || address < kMapBase) return;
    std::lock_guard<std::mutex> lock(m_heap_mutex);
    uint64_t begin = address, end = std::min<uint64_t>(uint64_t(address) + size, m_map_current);
    if (begin >= end) return;
    // Merge with overlapping or adjacent free ranges; a repeated munmap is idempotent.
    auto it = m_map_free.upper_bound(uint32_t(begin));
    if (it != m_map_free.begin()) {
        auto previous = std::prev(it);
        if (uint64_t(previous->first) + previous->second >= begin) {
            begin = previous->first;
            end = std::max<uint64_t>(end, uint64_t(previous->first) + previous->second);
            m_map_free.erase(previous);
        }
    }
    for (it = m_map_free.lower_bound(uint32_t(begin)); it != m_map_free.end() && it->first <= end;) {
        end = std::max<uint64_t>(end, uint64_t(it->first) + it->second);
        it = m_map_free.erase(it);
    }
    if (end == m_map_current) { m_map_current = uint32_t(begin); return; }
    m_map_free.emplace(uint32_t(begin), uint32_t(end - begin));
}

bool DynarmicHost::HeapSelfCheck() {
    bool ok = true;
#define CHECK_HEAP(test) do { if (!(test)) { LOGE("HeapSelfCheck FAILED: %s", #test); ok = false; } } while (0)
    const uint32_t a = GuestAlloc(100), b = GuestAlloc(100), aligned = GuestAlloc(64, 0x1000);
    CHECK_HEAP(a && b && a != b && !(a & 7) && !(b & 7));
    CHECK_HEAP(a + 100 <= b - kHeaderSize || b + 100 <= a - kHeaderSize);
    CHECK_HEAP(aligned && !(aligned & 0xFFF));
    CHECK_HEAP(GuestAllocSize(a) == 100);
    MemoryWrite32(a, 0xDEADBEEF); GuestFree(a); GuestFree(a);
    CHECK_HEAP(GuestAllocSize(a) == 0);
    const uint32_t reused = GuestAlloc(100);
    CHECK_HEAP(reused == a && MemoryRead32(reused) == 0);
    GuestFree(0x40000000); GuestFree(kHeapBase + 4); GuestFree(b + 8);
    CHECK_HEAP(GuestAllocSize(b) == 100);
    const uint32_t big = GuestAlloc(8 * 1024 * 1024, 0x10000);
    CHECK_HEAP(big && !(big & 0xFFFF) && big >= kLargeBase && big < kLargeLimit);
    GuestFree(big);
    const uint32_t big2 = GuestAlloc(8 * 1024 * 1024, 0x10000);
    CHECK_HEAP(big2 == big);
    const uint32_t medium = GuestAlloc(256 * 1024);
    CHECK_HEAP(medium >= kHeapBase && medium < kHeapLimit && GuestAllocSize(medium) == 256 * 1024);
    CHECK_HEAP(GuestAlloc(SIZE_MAX) == 0 && GuestAlloc(8, 3) == 0);
    GuestFree(big2); GuestFree(medium); GuestFree(b); GuestFree(reused); GuestFree(aligned);
    CHECK_HEAP(GuestAllocSize(big2) == 0 && GuestAllocSize(medium) == 0);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "HeapSelfCheck %s", ok ? "OK" : "FAILED");
#undef CHECK_HEAP
    return ok;
}

bool DynarmicHost::RuntimeSelfCheck() {
    const uint32_t sum = 0x2000, align = 0x2100, thumb = 0x2200;
    std::vector<uint32_t> code = {0xE0800001, 0xE0800002, 0xE0800003};
    for (uint32_t offset = 0; offset < 20; offset += 4) { code.push_back(0xE59D1000 | offset); code.push_back(0xE0800001); }
    code.push_back(0xE12FFF1E);
    for (size_t i = 0; i < code.size(); ++i) MemoryWrite32(sum + i * 4, code[i]);
    MemoryWrite32(align, 0xE1A0000D); MemoryWrite32(align + 4, 0xE2000007); MemoryWrite32(align + 8, 0xE12FFF1E);
    MemoryWrite32(thumb, 0x4770202A);
    AddExecutableRange(sum, code.size() * 4); AddExecutableRange(align, 12); AddExecutableRange(thumb, 4);
    const std::vector<uint32_t> arguments{1, 2, 3, 4, 5, 6, 7, 8, 9};
    bool ok = RunFunction(sum, arguments) == 45 && RunFunction(align, arguments) == 0 && RunFunction(thumb | 1) == 42;
    const uint32_t nested = m_thunk_manager->RegisterThunk("__bridge_selfcheck_nested", [sum, arguments](auto& jit, auto& host) {
        const uint32_t sp = jit.Regs()[13], lr = jit.Regs()[14];
        const uint32_t result = host.CallGuestNested(sum, arguments);
        if (&host.CurrentJit() != &jit || jit.Regs()[13] != sp || jit.Regs()[14] != lr) host.Fail("Nested JIT corrupted caller");
        jit.Regs()[0] = result;
    });
    ok &= RunFunction(nested) == 45;
    const uint32_t checked_stack = m_thunk_manager->RegisterThunk("__bridge_selfcheck_stack", [](auto& jit, auto& host) {
        uint32_t total = 0;
        for (uint32_t word = 0; word < 5; ++word) total += host.ReadStackWord(jit, word);
        bool overflow_rejected = false;
        try { host.ReadStackWord(jit, UINT32_MAX); }
        catch (const std::out_of_range&) { overflow_rejected = true; }
        const uint32_t saved_sp = jit.Regs()[13];
        const auto& thread = host.EnsureThread();
        bool guards_rejected = true;
        for (uint32_t bad_sp : {thread.stack_bottom - 4, thread.stack_top, UINT32_MAX - 3}) {
            jit.Regs()[13] = bad_sp;
            bool rejected = false;
            try { host.ReadStackWord(jit, 0); }
            catch (const std::out_of_range&) { rejected = true; }
            guards_rejected &= rejected;
        }
        jit.Regs()[13] = saved_sp;
        if (!overflow_rejected || !guards_rejected) host.Fail("Guest stack reader failed bounds self-check");
        jit.Regs()[0] = total;
    });
    ok &= RunFunction(checked_stack, arguments) == 35;
    const uint32_t parent_errno = GuestErrnoAddress();
    MemoryWrite32(parent_errno, 0x1234);
    const uint32_t concurrent = m_thunk_manager->RegisterThunk("__bridge_selfcheck_thread", [sum, arguments, parent_errno](auto& jit, auto& host) {
        uint32_t result = 0;
        std::thread worker([&] {
            const uint32_t child_errno = host.GuestErrnoAddress();
            if (child_errno == parent_errno) host.Fail("Thread errno alias");
            host.MemoryWrite32(child_errno, 0x5678);
            result = host.RunFunction(sum, arguments);
            host.ReleaseCurrentThread();
        });
        worker.join();
        jit.Regs()[0] = result;
    });
    // Join while the caller is inside a thunk proves no common JIT/registry lock spans execution.
    ok &= RunFunction(concurrent) == 45 && RunFunction(concurrent) == 45 && MemoryRead32(parent_errno) == 0x1234;
    MemoryWrite32(parent_errno, 0);
    ok &= !HasFailed();
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "BridgeAbiThreadSelfCheck %s (ARM/Thumb, 9-word spill, nested calls, thread/errno recycling)", ok ? "OK" : "FAILED");
    return ok;
}

void DynarmicHost::AddExecutableRange(uint32_t base, uint32_t size) {
    if (!size || uint64_t(base) + size > m_memory_size || uint64_t(base) + size > UINT32_MAX) {
        Fail("Invalid executable range"); return;
    }
    std::lock_guard<std::mutex> lock(m_code_mutex);
    m_code_ranges.emplace_back(base, base + size);
}

bool DynarmicHost::IsExecutableAddress(uint32_t address) const {
    const uint32_t pc = address & ~1u;
    if (m_libraries_ready.load())
        for (const auto& library : m_libraries)
            if (library.elf->IsExecutable(address)) return true;
    std::lock_guard<std::mutex> lock(m_code_mutex);
    for (const auto& [begin, end] : m_code_ranges)
        if (pc >= begin && uint64_t(pc) + 2 <= end) return true;
    return false;
}

std::optional<std::uint32_t> DynarmicHost::MemoryReadCode(std::uint32_t address) {
    if (!IsExecutableAddress(address)) {
        LOGE("[RUNAWAY] code fetch 0x%08x", address);
        // One-shot failure context: the caller's return address and argument registers.
        if (t_thread && t_thread->active) {
            const auto& r = t_thread->active->Regs();
            std::string module;
            const ElfLoader* owner = ModuleForAddress(r[14] & ~1u, &module);
            LOGE("[RUNAWAY] lr=0x%08x (%s+0x%x) sp=0x%08x r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x r4=0x%08x r5=0x%08x r12=0x%08x",
                 r[14], owner ? module.c_str() : "?", owner ? (r[14] & ~1u) - owner->GetBaseAddress() : 0u,
                 r[13], r[0], r[1], r[2], r[3], r[4], r[5], r[12]);
            const uint32_t sp = r[13];
            if (uint64_t(sp) + 32 <= m_memory_size)
                LOGE("[RUNAWAY] stack %08x %08x %08x %08x %08x %08x %08x %08x", MemoryRead32(sp), MemoryRead32(sp + 4),
                     MemoryRead32(sp + 8), MemoryRead32(sp + 12), MemoryRead32(sp + 16), MemoryRead32(sp + 20),
                     MemoryRead32(sp + 24), MemoryRead32(sp + 28));
        }
        NoteGuestFault("invalid guest code fetch", address);
        Fail("Invalid guest code fetch");
        return 0xE7FEDEFFu;
    }
    if (uint64_t(address) + 4 > m_memory_size) return std::nullopt;
    return MemoryRead32(address);
}

#define MEMORY_READ(bits, type) type DynarmicHost::MemoryRead##bits(uint32_t address) { \
    type value = 0; \
    if (uint64_t(address) + sizeof(value) <= m_memory_size) memcpy(&value, m_memory + address, sizeof(value)); \
    else Fail("Guest memory read out of bounds"); \
    return value; \
}
MEMORY_READ(8, uint8_t)
MEMORY_READ(16, uint16_t)
MEMORY_READ(32, uint32_t)
MEMORY_READ(64, uint64_t)
#undef MEMORY_READ

bool DynarmicHost::IsReadOnlyMemory(uint32_t address) {
    if (!m_read_only_memory_finalized.load(std::memory_order_acquire)) return false;

    // PT_LOAD segments without PF_W had immutable mappings in the original
    // process.  Keeping the largest folded read wholly inside such a segment
    // lets Dynarmic safely constant-fold ARM literal and rodata loads.
    constexpr uint64_t kMaximumFoldedRead = sizeof(uint64_t);
    for (const auto& library : m_libraries) {
        const uint64_t image_base = library.elf->GetBaseAddress();
        for (const auto& segment : library.elf->GetSegments()) {
            if (!(segment.flags & PF_R) || (segment.flags & PF_W)) continue;
            const uint64_t begin = image_base + segment.vaddr;
            const uint64_t end = begin + segment.memsz;
            if (address >= begin && uint64_t(address) + kMaximumFoldedRead <= end)
                return true;
        }
    }
    return false;
}

bool DynarmicHost::FinalizeReadOnlyMemory() {
    if (!m_libraries_ready.load(std::memory_order_acquire) || HasFailed() ||
        m_read_only_memory_finalized.exchange(true, std::memory_order_acq_rel)) {
        Fail("Invalid guest read-only memory finalization");
        return false;
    }

    uint64_t immutable_bytes = 0;
    for (const auto& library : m_libraries)
        for (const auto& segment : library.elf->GetSegments())
            if ((segment.flags & PF_R) && !(segment.flags & PF_W))
                immutable_bytes += segment.memsz;
    LOGI("Guest read-only memory finalized: %" PRIu64 " bytes available to JIT constant folding",
         immutable_bytes);
    return true;
}

#define MEMORY_WRITE(bits, type) void DynarmicHost::MemoryWrite##bits(uint32_t address, type value) { \
    if (uint64_t(address) + sizeof(value) <= m_memory_size) memcpy(m_memory + address, &value, sizeof(value)); \
    else Fail("Guest memory write out of bounds"); \
}
MEMORY_WRITE(8, uint8_t)
MEMORY_WRITE(16, uint16_t)
MEMORY_WRITE(32, uint32_t)
MEMORY_WRITE(64, uint64_t)
#undef MEMORY_WRITE

void DynarmicHost::InterpreterFallback(uint32_t pc, size_t) {
    LOGE("Interpreter fallback at 0x%08x", pc);
    NoteGuestFault("unsupported guest instruction", pc);
    Fail("Unsupported guest instruction");
}
void DynarmicHost::CallSVC(uint32_t swi) {
    auto& jit = CurrentJit();
    if (!swi) {
        const uint32_t pc = jit.Regs()[15];
        if (pc == kExitArm || pc == kExitArm + 4 || pc == kExitThumb || pc == kExitThumb + 2)
            jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
        else {
            // EABI Linux syscall: r7 is the number, r0-r5 carry six words.
            // The libc adapter takes number in r0, then applies ARM argument ABI.
            const auto registers = jit.Regs();
            const uint32_t entry = m_thunk_manager->FindRegisteredSymbol("syscall");
            uint32_t result = RunFunction(entry,{registers[7],registers[0],registers[1],
                registers[2],registers[3],registers[4],registers[5]});
            if(result==UINT32_MAX)result=0u-MemoryRead32(GuestErrnoAddress());
            jit.Regs()[0]=result;
        }
        return;
    }
    m_thunk_manager->DispatchThunk(swi, jit);
}
void DynarmicHost::ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception exception) {
    LOGE("Guest exception PC=0x%08x type=%d", pc, int(exception));
    char what[48];
    snprintf(what, sizeof(what), "guest instruction exception %d", int(exception));
    NoteGuestFault(what, pc);
    Fail("Guest instruction exception");
}

void DynarmicHost::NoteGuestFault(const char* what, uint32_t pc) {
    std::string module;
    const ElfLoader* owner = ModuleForAddress(pc & ~1u, &module);
    const uint32_t lr = t_thread && t_thread->active ? t_thread->active->Regs()[14] : 0;
    std::string lr_module;
    const ElfLoader* lr_owner = ModuleForAddress(lr & ~1u, &lr_module);
    char note[200];
    snprintf(note, sizeof(note), "%s at 0x%08x (%s+0x%x), lr 0x%08x (%s+0x%x)", what, pc, owner ? module.c_str() : "?",
             owner ? (pc & ~1u) - owner->GetBaseAddress() : 0u, lr, lr_owner ? lr_module.c_str() : "?",
             lr_owner ? (lr & ~1u) - lr_owner->GetBaseAddress() : 0u);
    logs::NoteFailure(note);
}

template<class T> static bool CompareAndSwap(uint8_t* memory, size_t size, uint32_t address, T value, T expected) {
    if (uint64_t(address) + sizeof(T) > size || address % alignof(T)) return false;
    return __atomic_compare_exchange_n(reinterpret_cast<T*>(memory + address), &expected, value, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
#define EXCLUSIVE(bits, type) bool DynarmicHost::MemoryWriteExclusive##bits(uint32_t address, type value, type expected) { \
    return CompareAndSwap(m_memory, m_memory_size, address, value, expected); \
}
EXCLUSIVE(8, uint8_t)
EXCLUSIVE(16, uint16_t)
EXCLUSIVE(32, uint32_t)
EXCLUSIVE(64, uint64_t)
#undef EXCLUSIVE

uint32_t DynarmicHost::GetResultHigh() const {
    return t_thread && t_thread->owner == this ? t_thread->result_hi : 0;
}

bool DynarmicHost::RunSelfChecks() {
    return HeapSelfCheck() && ElfLoader::SelfCheck(m_memory, m_memory_size) && RuntimeSelfCheck();
}
