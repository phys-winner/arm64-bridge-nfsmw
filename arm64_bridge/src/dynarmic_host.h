#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
#include <string>
#include <optional>
#include <mutex>
#include <array>
#include <bitset>
#include <atomic>
#include <unordered_map>
#include <map>
#include <deque>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

struct GuestModuleImage {
    std::string name;
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t base = 0;
    uint32_t limit = 0;
};
class ElfLoader;
class ThunkManager;

class DynarmicHost : public Dynarmic::A32::UserCallbacks {
public:
    static constexpr size_t GUEST_ADDRESS_SPACE = 0x100000000ULL;

    DynarmicHost();
    ~DynarmicHost() override;
    // exact_jit: no unsafe JIT optimizations (the FPSR flags tracked, exclusives through the global monitor), as up
    // to b27; the bridge asks for it when this arm64 process runs translated on another CPU (PC emulators).
    bool Initialize(bool exact_jit = false);
    bool LoadGame(const std::string& path);
    bool LoadGameFromMemory(const uint8_t* data, size_t size);
    bool LoadLibraries(const std::string& fmodex, const std::string& fmodevent, const std::string& game);
    bool LoadLibrariesFromMemory(const uint8_t* fmodex, size_t fmodex_size,
                                const uint8_t* fmodevent, size_t fmodevent_size,
                                const uint8_t* game, size_t game_size);
    // Input order is constructor order; all images are relocated before any constructor runs.
    bool LoadModulesFromMemory(const std::vector<GuestModuleImage>& modules);
    ElfLoader& GetElfLoader();
    ElfLoader* GetFmodExLoader();
    ElfLoader* GetFmodEventLoader();
    ElfLoader* FindModule(const std::string& name);
    // Loaded guest modules in load (= constructor) order; valid once loading succeeded.
    size_t ModuleCount() const;
    const ElfLoader* ModuleAt(size_t index, std::string* name = nullptr) const;
    const ElfLoader* ModuleForAddress(uint32_t address, std::string* name = nullptr) const;
    // Usable guest stack [bottom, top) of the calling guest thread.
    void CurrentGuestStack(uint32_t& bottom, uint32_t& top);
    std::recursive_mutex& GetMutex() { return m_compatibility_mutex; }
    // Retained for adapters using the previous API; nested calls now return synchronously.
    void DrainDeferred() {}
    bool RunSelfChecks();
    uint32_t GetResultHigh() const;
    bool RunLibraryConstructors(const std::string& name);
    uint32_t FindLibrarySymbol(const std::string& library, const std::string& symbol) const;
    uint32_t FindSymbol(const std::string& name);

    Dynarmic::A32::Jit& CurrentJit();
    // Sampling profiler (prof.cpp): which guest code a host pc of the calling thread's JITs belongs to. 0 = not JIT
    // code, 1 = translated guest block (guest_pc = its first instruction, bit 0 set for Thumb), 2 = Dynarmic prelude.
    // Only outside guest execution on that thread.
    int ReverseLookupHostPc(const void* host_pc, uint32_t* guest_pc);
    // The host code of the translated block a host pc belongs to (profiling dumps), same thread rules.
    bool HostBlockCode(const void* host_pc, const void** code, size_t* size);
    uint32_t ReadStackWord(Dynarmic::A32::Jit& jit, uint32_t spilled_word_index);
    uint32_t RunFunction(uint32_t target_pc, const std::vector<uint32_t>& args = {});
    uint32_t CallGuestNested(uint32_t target_pc, const std::vector<uint32_t>& args = {});
    void ReleaseCurrentThread();
    void ExitCurrentGuestThread(uint32_t result);
    bool IsCurrentGuestThreadExiting() const;
    void BeginThreadCleanup();
    uint32_t GuestErrnoAddress();
    uint32_t GuestTlsAddress();
    uint32_t GuestTlsWritable();
    void SetGuestTlsAddress(uint32_t address);
    void SetGuestTlsWritable(uint32_t value);
    bool IsJitRunning() const;
    bool HasFailed() const { return m_failed.load(); }
    void Fail(const char* reason);
    // Notes a guest code fault with its pc and lr (module+offset) as the process's first failure (abort message).
    void NoteGuestFault(const char* what, uint32_t pc);

    uint8_t* GetGuestMemoryBase() const { return m_memory; }
    size_t GetGuestMemorySize() const { return m_memory_size; }
    ThunkManager& GetThunkManager() { return *m_thunk_manager; }
    void InvalidateGuestCode(uint32_t addr, size_t size);
    uint32_t GuestAlloc(size_t size, size_t alignment = 8);
    void GuestFree(uint32_t addr);
    uint32_t GuestAllocSize(uint32_t addr);
    uint32_t GuestAllocMapping(size_t size, size_t alignment);
    // Returns an anonymous guest mapping range to the mapping allocator (munmap).
    void GuestFreeMapping(uint32_t address, size_t size);
    // Guest address space: images below kHeapBase, main heap, stacks (kStackBase), large-block arena, mmap region.
    // Large blocks (stage 14): requests of kLargeBlock bytes or more come from their own arena, as Bionic gives them
    // mappings of their own. The game reads whole files into single buffers (the 4x UI texture packs: menu.sba
    // 67 MiB, map.sba 58 MiB, garage.sba 38 MiB, ...). In the one shared heap of b28 (736 MiB) the holes those
    // buffers left behind were taken by smaller long-lived blocks, so a later load found no hole of its size and
    // took it from the bump pointer: one return from a race to the car-unlock screen raised the heap high water
    // from 390 to 458 MiB with about 90 MiB live (Redmi Note 8 Pro).
    static constexpr uint32_t kHeapBase = 0x50000000;
    static constexpr uint32_t kHeapLimit = 0x80000000;
    static constexpr uint32_t kLargeBase = 0xA0000000;
    static constexpr uint32_t kLargeLimit = 0xE0000000;
    static constexpr uint32_t kLargeBlock = 1u << 20;
    static constexpr uint32_t kMapBase = 0xE0000000;
    static constexpr uint32_t kMapLimit = 0xFF000000;
    struct HeapStats {
        uint32_t heap_high_water, heap_free_small, heap_free_huge, huge_free_blocks, huge_dirty;
        uint32_t large_high_water, large_free, large_free_blocks, large_dirty;
        uint32_t map_high_water, map_free;
        uint32_t alloc_failures, last_failed_size;
    };
    HeapStats GetHeapStats();
    void AddExecutableRange(uint32_t base, uint32_t size);
    bool IsExecutableAddress(uint32_t address) const;
    bool FinalizeReadOnlyMemory();

    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t vaddr) override;
    std::uint8_t MemoryRead8(std::uint32_t vaddr) override;
    std::uint16_t MemoryRead16(std::uint32_t vaddr) override;
    std::uint32_t MemoryRead32(std::uint32_t vaddr) override;
    std::uint64_t MemoryRead64(std::uint32_t vaddr) override;
    bool IsReadOnlyMemory(std::uint32_t vaddr) override;
    void MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) override;
    void MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) override;
    void MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) override;
    void MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) override;
    bool MemoryWriteExclusive8(std::uint32_t vaddr, std::uint8_t value, std::uint8_t expected) override;
    bool MemoryWriteExclusive16(std::uint32_t vaddr, std::uint16_t value, std::uint16_t expected) override;
    bool MemoryWriteExclusive32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t expected) override;
    bool MemoryWriteExclusive64(std::uint32_t vaddr, std::uint64_t value, std::uint64_t expected) override;
    void InterpreterFallback(std::uint32_t pc, std::size_t num_instructions) override;
    void CallSVC(std::uint32_t swi) override;
    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) override;
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return UINT64_MAX; }

private:
    // Lazy, bounded JIT pools: one CPU per thread and synchronous callback depth.
    static constexpr size_t kMaxThreads = 64;
    static constexpr size_t kMaxDepth = 8;
    static constexpr size_t kCodeCacheOuter = 64u << 20;   // depth-0 JIT of each guest thread
    static constexpr size_t kCodeCacheNested = 16u << 20;  // nested (callback) JITs
    static constexpr size_t kMaxJits = kMaxThreads * kMaxDepth;
    // Exclusive-monitor processor ids are handed out compactly (lowest free first) instead of
    // slot*kMaxDepth+depth: every guest STREX scans the monitor's reservations, and 512 mostly unused
    // slots cost ~10 % of the game's GL thread (simpleperf, stage 8). The scan covers [0, high water).
    static constexpr size_t kMonitorProcessors = 128;
    static constexpr uint32_t kNoProcessor = UINT32_MAX;
    static constexpr uint32_t kStackBase = 0x80000000;
    static constexpr uint32_t kStackStride = 0x00800000;
    static constexpr uint32_t kStackSize = 0x00400000;
    struct ThreadState;
    static thread_local std::unique_ptr<ThreadState> t_thread;
    ThreadState& EnsureThread();
    Dynarmic::A32::Jit& EnsureJit(ThreadState& thread, size_t depth);
    void RetireThread(ThreadState& thread);
    bool RuntimeSelfCheck();

    uint8_t* m_memory = nullptr;
    size_t m_memory_size = 0;
    size_t m_page_size = 0;
    std::unique_ptr<Dynarmic::ExclusiveMonitor> m_monitor;
    Dynarmic::A32::UserConfig m_config;
    mutable std::mutex m_jits_mutex;
    std::array<Dynarmic::A32::Jit*, kMonitorProcessors> m_live_jits{};
    std::bitset<kMonitorProcessors> m_processor_ids;
    size_t m_processor_high_water = 0;
    std::bitset<kMaxThreads> m_stack_slots;
    std::unique_ptr<ThunkManager> m_thunk_manager;
    std::recursive_mutex m_compatibility_mutex;
    std::atomic<bool> m_failed{false};
    struct Library {
        std::string name;
        std::unique_ptr<ElfLoader> elf;
        bool constructors_ran = false;
    };
    std::vector<Library> m_libraries;
    std::atomic<bool> m_libraries_ready{false};
    std::atomic<bool> m_read_only_memory_finalized{false};
    mutable std::mutex m_code_mutex;
    std::vector<std::pair<uint32_t, uint32_t>> m_code_ranges;

    static constexpr int kMinClassShift = 5;
    static constexpr uint32_t kMaxClassSize = 1u << 16;
    static constexpr int kNumSizeClasses = 16 - kMinClassShift + 1;
    bool HeapSelfCheck();
    bool ValidAllocation(uint32_t address) const; // caller holds m_heap_mutex
    std::mutex m_heap_mutex;
    std::vector<uint32_t> m_free_small[kNumSizeClasses];  // size classes live in the main heap only
    struct DirtyRange { uint32_t base, size; };
    // A bump-allocated range of blocks above kMaxClassSize. Free blocks are address-ordered for coalescing and
    // size-ordered for best fit. Like Bionic's allocators (decay purging), freed bytes stay in place for a while;
    // pages go back to the OS (MADV_DONTNEED) once more than dirty_limit bytes of freed blocks wait, oldest first.
    struct BlockArena {
        const char* name;
        uint32_t base, limit, current;
        uint64_t dirty_limit;
        uint32_t granule;  // block sizes are multiples of it (16 bytes; a page in the large arena)
        std::map<uint32_t, uint32_t> by_address;
        std::multimap<uint32_t, uint32_t> by_size;
        uint64_t free_bytes = 0;
        std::deque<DirtyRange> dirty;
        uint64_t dirty_bytes = 0;
    };
    BlockArena m_main{"main", kHeapBase, kHeapLimit, kHeapBase, 64ull << 20, 16};
    BlockArena m_large{"large", kLargeBase, kLargeLimit, kLargeBase, 32ull << 20, 16};  // granule = page (Initialize)
    // All callers hold m_heap_mutex.
    BlockArena* ArenaOf(uint32_t address);
    const BlockArena* ArenaOf(uint32_t address) const;
    uint32_t AllocFromArena(BlockArena& arena, uint64_t need, uint32_t& capacity, bool& reused);
    void PurgeDirtyHuge(BlockArena& arena);
    void InsertFreeHuge(BlockArena& arena, uint32_t base, uint32_t capacity, bool freed);
    void EraseFreeHuge(BlockArena& arena, std::map<uint32_t, uint32_t>::iterator block);
    uint32_t m_alloc_failures = 0, m_last_failed_size = 0;
    size_t m_alloc_log_bytes = 0;  // debug.nfsbridge.bigalloc <MiB>: log guest allocations of at least that size
    // debug.nfsbridge.jitcache <MiB> (4..64, diagnostics): smaller code caches, so that whole-cache clears inside a
    // run happen within minutes instead of hours of play
    size_t m_code_cache_outer = kCodeCacheOuter, m_code_cache_nested = kCodeCacheNested;
    uint32_t m_map_current = kMapBase;
    std::map<uint32_t, uint32_t> m_map_free;  // unmapped anonymous ranges: base -> size
};
