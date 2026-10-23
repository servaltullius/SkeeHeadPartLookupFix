// SkeeHeadPartLookupFix
//
// RaceMenu (skee64.dll) applies sculpt data to every face geometry of an actor whose face is rebuilt
// (TESNPC::UpdateMorphs hook -> FaceMorphInterface::ApplyMorphs -> per-geometry lambda; skee64+0xBDD50 in
// 0.4.20.0 Steam). On AE builds that lambda has GetHeadPartByName() inlined:
//
//     for (i = 0; i < dataHandler->headParts.count; i++) {
//         BSFixedString partName(name.c_str());          // string-pool insert, every iteration
//         if (headParts[i]->partName == partName) { partName.Release(); ...apply sculpt...; break; }
//         partName.Release();                            // string-pool release, every iteration
//     }
//
// With tens of thousands of head parts this is two global string-pool operations per head part per
// face geometry: ~440,000 create/release pairs per face rebuild with 36,775 head parts and 15 sculpted
// face geometries.
//
// The fix: just before the loop (the alignment NOPs in front of the loop head) call a stub that sets
// the loop index (EBX) to the matching slot from a case-insensitive name -> index map built once at
// kDataLoaded. The loop then matches on its first iteration and the original code applies the sculpt
// exactly as before. No match -> index = count, the loop exits like a full scan with no match would.
// Anything the map cannot answer exactly (not built yet, head part array changed, slot name differs,
// non-ASCII name) -> index 0, i.e. the original full scan.
//
// Every build also has GetHeadPartByName as a function of its own (all callers on SE 0.4.16, one other
// caller on AE); it is replaced as a whole (same first-match result, from the map).
//
// Supported builds (kBuilds): RaceMenu 0.4.20.0 Steam and GOG, 0.4.19.16 (AE 1.6.1170): sculpt loop + the
// function; 0.4.16 (SE 1.5.97): the function (the sculpt lambda calls it there). A build is recognised by
// its PE timestamp and image size, then every instruction the patches rely on is checked; anything else is
// left alone. Three more inlined copies on AE (preset paths, not hot in profiles) keep the original scan.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
    // ------------------------------------------------------------------ code-writing helpers
    // Copies `code` into a fresh RW page `mem`, then makes it RX. Frees the page and returns nullptr on failure.
    [[nodiscard]] std::uint8_t* FinishExec(std::uint8_t* mem, std::span<const std::uint8_t> code) noexcept
    {
        if (!mem) {
            return nullptr;
        }
        std::memcpy(mem, code.data(), code.size());
        DWORD old = 0;
        if (!::VirtualProtect(mem, 0x1000, PAGE_EXECUTE_READ, &old)) {
            ::VirtualFree(mem, 0, MEM_RELEASE);
            return nullptr;
        }
        ::FlushInstructionCache(::GetCurrentProcess(), mem, code.size());
        return mem;
    }

    void WriteCode(std::uintptr_t dst, std::span<const std::uint8_t> bytes)
    {
        REL::safe_write(dst, bytes.data(), bytes.size());
        ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(dst), bytes.size());
    }

    void PutU64(std::uint8_t* dst, std::uint64_t value) noexcept
    {
        std::memcpy(dst, &value, sizeof(value));
    }

    // rel32 from the end of a 5-byte call/jmp at `site` to `target`; false when out of range.
    [[nodiscard]] bool Rel32(std::uintptr_t site, std::uintptr_t target, std::int32_t& out) noexcept
    {
        const auto delta = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(site + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) {
            return false;
        }
        out = static_cast<std::int32_t>(delta);
        return true;
    }

    // Executable copy of `code`, never freed (patched code jumps into it). It is placed within rel32 reach of
    // every address in [lo, hi), the patched module, so a patch site needs only a 5-byte call/jmp. Free regions
    // are searched outward from the module.
    [[nodiscard]] std::uint8_t* AllocExecNear(std::span<const std::uint8_t> code, std::uintptr_t lo, std::uintptr_t hi) noexcept
    {
        SYSTEM_INFO si{};
        ::GetSystemInfo(&si);
        const std::uintptr_t gran = si.dwAllocationGranularity;
        constexpr std::uintptr_t kReach = 0x7FF00000;  // a little under 2 GB, minus the code size below
        const std::uintptr_t minAddr = hi > kReach ? hi - kReach : gran;
        const std::uintptr_t maxAddr = lo + kReach - 0x1000;

        const auto tryAt = [&](std::uintptr_t addr) -> std::uint8_t* {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!::VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) || mbi.State != MEM_FREE) {
                return nullptr;
            }
            return static_cast<std::uint8_t*>(::VirtualAlloc(reinterpret_cast<void*>(addr), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        };

        std::uint8_t* mem = nullptr;
        for (std::uintptr_t addr = (lo & ~(gran - 1)) - gran; !mem && addr >= minAddr && addr < lo; addr -= gran) {
            mem = tryAt(addr);
        }
        for (std::uintptr_t addr = (hi + gran - 1) & ~(gran - 1); !mem && addr <= maxAddr; addr += gran) {
            mem = tryAt(addr);
        }
        return FinishExec(mem, code);
    }

    // ------------------------------------------------------------------ supported skee64 builds
    constexpr std::uint32_t kHeadPartArrayOffset = 0x130;  // DataHandler::headParts.entries (SE and AE)
    constexpr std::uint32_t kHeadPartCountOffset = 0x140;  // DataHandler::headParts.count
    constexpr std::uint32_t kPartNameOffset = 0x118;       // BGSHeadPart BSFixedString compared by the loop

    struct ExpectedBytes
    {
        std::uintptr_t rva;
        std::vector<std::uint8_t> bytes;
    };

    enum class PatchKind
    {
        kInlineLoop,    // NOPs in front of the sculpt lambda's inlined loop -> call stub that sets EBX
        kWholeFunction  // GetHeadPartByName(const std::string&) entry -> jmp SHLF_GetHeadPartByName
    };

    struct PatchSite
    {
        PatchKind kind;
        std::uintptr_t rva;     // patch site
        std::uint32_t len;      // bytes replaced (NOP run, or the whole first instruction)
        std::uint32_t nameRsp;  // kInlineLoop: [rsp+nameRsp] at the site = the std::string being looked up
    };

    struct SkeeBuild
    {
        const char* name;
        std::uint32_t timeDateStamp;  // PE header: tells same-version Steam and GOG builds apart
        std::uint32_t sizeOfImage;
        std::vector<PatchSite> sites;
        std::vector<ExpectedBytes> expected;  // every instruction the patches rely on
    };

    // GetHeadPartByName compiles to the same 0xD1-byte function in every supported build (only the game
    // addresses it adds differ), so its checks are offsets from the function start.
    [[nodiscard]] std::vector<ExpectedBytes> GetHeadPartByNameChecks(std::uintptr_t fn)
    {
        return {
            { fn + 0x00, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20,
                           0x57, 0x48, 0x83, 0xEC, 0x20 } },                                  // prologue (patch site = first 5 bytes)
            { fn + 0x1B, { 0x48, 0x8B, 0xF1, 0x48, 0x8B, 0x38 } },                            // mov rsi,rcx (std::string*) / mov rdi,[rax]
            { fn + 0x26, { 0x33, 0xDB, 0x39, 0x9F, 0x40, 0x01, 0x00, 0x00 } },                // xor ebx,ebx / cmp [rdi+140h],ebx
            { fn + 0x38, { 0x48, 0x83, 0x7E, 0x18, 0x10 } },                                  // cmp [rsi+18h],10h (std::string capacity)
            { fn + 0x40, { 0x48, 0x8B, 0x87, 0x30, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x2C, 0xD8 } },  // mov rax,[rdi+130h] / mov rbp,[rax+rbx*8]
            { fn + 0x75, { 0x48, 0x39, 0x85, 0x18, 0x01, 0x00, 0x00 } },                      // cmp [rbp+118h], rax
            { fn + 0x94, { 0xFF, 0xC3, 0x3B, 0x9F, 0x40, 0x01, 0x00, 0x00 } },                // inc ebx / cmp ebx,[rdi+140h]
            { fn + 0x9E, { 0x33, 0xC0 } },                                                    // not found: return nullptr
            { fn + 0xCC, { 0x48, 0x8B, 0xC5 } },                                              // found: return the head part
        };
    }

    [[nodiscard]] std::vector<ExpectedBytes> Concat(std::vector<ExpectedBytes> a, const std::vector<ExpectedBytes>& b)
    {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    }

    // For kInlineLoop: EBX is the loop index (0 at the site), RDI = DataHandler, the count/array/name offsets,
    // the std::string local (its capacity compare at nameRsp+18h), and the loop head after the site.
    const std::array<SkeeBuild, 4> kBuilds{ {
        { "RaceMenu 0.4.20.0 (AE, Steam)", 0x69E40A71, 0x274000,
            { { PatchKind::kInlineLoop, 0xBDDF4, 12, 0x78 }, { PatchKind::kWholeFunction, 0x582C0, 5, 0 } },
            Concat({
                { 0xBDD9C, { 0x33, 0xDB } },                                                  // xor ebx, ebx
                { 0xBDDC9, { 0x48, 0x8D, 0x4C, 0x24, 0x78 } },                                // lea rcx,[rsp+78h] (std::string name)
                { 0xBDDE7, { 0x83, 0xBF, 0x40, 0x01, 0x00, 0x00, 0x00 } },                    // cmp dword [rdi+140h],0
                { 0xBDDF4, { 0x0F, 0x1F, 0x40, 0x00, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 } },  // patch site (NOPs)
                { 0xBDE00, { 0x8B, 0xCB, 0x8B, 0x87, 0x40, 0x01, 0x00, 0x00, 0x48, 0x3B, 0xC8 } },        // mov ecx,ebx / mov eax,[rdi+140h] / cmp rcx,rax
                { 0xBDE0D, { 0x48, 0x8B, 0x87, 0x30, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x2C, 0xC8 } },        // mov rax,[rdi+130h] / mov rbp,[rax+rcx*8]
                { 0xBDE1D, { 0x48, 0x83, 0xBC, 0x24, 0x90, 0x00, 0x00, 0x00, 0x10 } },        // cmp [rsp+90h],10h (name capacity)
                { 0xBDE51, { 0x48, 0x39, 0x85, 0x18, 0x01, 0x00, 0x00 } },                    // cmp [rbp+118h], rax
                { 0xBDE70, { 0xFF, 0xC3, 0x3B, 0x9F, 0x40, 0x01, 0x00, 0x00 } },              // inc ebx / cmp ebx,[rdi+140h]
            }, GetHeadPartByNameChecks(0x582C0)) },
        { "RaceMenu 0.4.20.0 (AE, GOG)", 0x69E40B69, 0x274000,
            { { PatchKind::kInlineLoop, 0xBE0B9, 7, 0x68 }, { PatchKind::kWholeFunction, 0x58290, 5, 0 } },
            Concat({
                { 0xBE06C, { 0x33, 0xDB } },                                                  // xor ebx, ebx
                { 0xBE08E, { 0x48, 0x8D, 0x4C, 0x24, 0x68 } },                                // lea rcx,[rsp+68h] (std::string name)
                { 0xBE0AC, { 0x83, 0xBF, 0x40, 0x01, 0x00, 0x00, 0x00 } },                    // cmp dword [rdi+140h],0
                { 0xBE0B9, { 0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00 } },                    // patch site (NOPs)
                { 0xBE0C0, { 0x8B, 0xCB, 0x8B, 0x87, 0x40, 0x01, 0x00, 0x00, 0x48, 0x3B, 0xC8 } },
                { 0xBE0CD, { 0x48, 0x8B, 0x87, 0x30, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x34, 0xC8 } },        // mov rsi,[rax+rcx*8]
                { 0xBE0DD, { 0x48, 0x83, 0xBC, 0x24, 0x80, 0x00, 0x00, 0x00, 0x10 } },        // cmp [rsp+80h],10h
                { 0xBE111, { 0x48, 0x39, 0x86, 0x18, 0x01, 0x00, 0x00 } },                    // cmp [rsi+118h], rax
                { 0xBE130, { 0xFF, 0xC3, 0x3B, 0x9F, 0x40, 0x01, 0x00, 0x00 } },
            }, GetHeadPartByNameChecks(0x58290)) },
        { "RaceMenu 0.4.19.16 (AE)", 0x65B4092C, 0x26C000,
            { { PatchKind::kInlineLoop, 0xBA165, 11, 0x68 }, { PatchKind::kWholeFunction, 0x57F00, 5, 0 } },
            Concat({
                { 0xBA112, { 0x33, 0xF6 } },                                                  // xor esi, esi
                { 0xBA139, { 0x48, 0x8D, 0x4C, 0x24, 0x68 } },                                // lea rcx,[rsp+68h] (std::string name)
                { 0xBA157, { 0x8B, 0xDE, 0x39, 0x9F, 0x40, 0x01, 0x00, 0x00 } },              // mov ebx,esi / cmp [rdi+140h],ebx
                { 0xBA165, { 0x66, 0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 } },  // patch site (NOPs)
                { 0xBA170, { 0x8B, 0xCB, 0x8B, 0x87, 0x40, 0x01, 0x00, 0x00, 0x48, 0x3B, 0xC8 } },
                { 0xBA17D, { 0x48, 0x8B, 0x87, 0x30, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x2C, 0xC8 } },
                { 0xBA18D, { 0x48, 0x83, 0xBC, 0x24, 0x80, 0x00, 0x00, 0x00, 0x10 } },        // cmp [rsp+80h],10h
                { 0xBA1C1, { 0x48, 0x39, 0x85, 0x18, 0x01, 0x00, 0x00 } },
                { 0xBA1E0, { 0xFF, 0xC3, 0x3B, 0x9F, 0x40, 0x01, 0x00, 0x00 } },
            }, GetHeadPartByNameChecks(0x57F00)) },
        { "RaceMenu 0.4.16 (SE 1.5.97)", 0x5F799CB1, 0x1E4000,
            { { PatchKind::kWholeFunction, 0x34BE0, 5, 0 } },
            GetHeadPartByNameChecks(0x34BE0) },
    } };

    // ------------------------------------------------------------------ name -> index map
    [[nodiscard]] inline char AsciiLower(char c) noexcept
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    // The engine's case-insensitive string pool may fold more than ASCII; such names keep the original scan.
    [[nodiscard]] bool HasNonAscii(std::string_view s) noexcept
    {
        return std::any_of(s.begin(), s.end(), [](char c) { return static_cast<std::uint8_t>(c) >= 0x80; });
    }

    struct CiHash
    {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view s) const noexcept
        {
            std::uint64_t h = 1469598103934665603ull;  // FNV-1a over the lower-cased bytes
            for (const char c : s) {
                h ^= static_cast<std::uint8_t>(AsciiLower(c));
                h *= 1099511628211ull;
            }
            return static_cast<std::size_t>(h);
        }
    };

    struct CiEqual
    {
        using is_transparent = void;
        [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const noexcept
        {
            if (a.size() != b.size()) {
                return false;
            }
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (AsciiLower(a[i]) != AsciiLower(b[i])) {
                    return false;
                }
            }
            return true;
        }
    };

    struct HeadPartIndex
    {
        const std::byte* dataHandler{ nullptr };
        const std::byte* const* entries{ nullptr };  // the array the map was built from
        std::uint32_t count{ 0 };
        std::unordered_map<std::string, std::uint32_t, CiHash, CiEqual> byName;
    };

    std::atomic<const HeadPartIndex*> g_index{ nullptr };
    std::atomic<std::uint64_t> g_hits{ 0 };
    std::atomic<std::uint64_t> g_misses{ 0 };
    std::atomic<std::uint64_t> g_fallbacks{ 0 };
    std::atomic<std::uint64_t> g_avoided{ 0 };  // string-pool create/release pairs the original scan would have made

    [[nodiscard]] const std::byte* const* HeadPartEntries(const std::byte* dataHandler) noexcept
    {
        return *reinterpret_cast<const std::byte* const* const*>(dataHandler + kHeadPartArrayOffset);
    }

    [[nodiscard]] std::uint32_t HeadPartCount(const std::byte* dataHandler) noexcept
    {
        return *reinterpret_cast<const std::uint32_t*>(dataHandler + kHeadPartCountOffset);
    }

    [[nodiscard]] const char* PartName(const std::byte* headPart) noexcept
    {
        return headPart ? *reinterpret_cast<const char* const*>(headPart + kPartNameOffset) : nullptr;
    }

    constexpr std::uint32_t kScan = 0xFFFFFFFF;

    // The map's answer for `key`: the first matching slot, the head part count (no match), or kScan when the
    // map cannot answer exactly and the original full scan must run. Allocation-free and lock-free: it runs
    // on engine job threads.
    [[nodiscard]] std::uint32_t LookupSlot(std::string_view key, const std::byte* dataHandler) noexcept
    {
        const HeadPartIndex* index = g_index.load(std::memory_order_acquire);
        if (!index || index->dataHandler != dataHandler || index->count != HeadPartCount(dataHandler) ||
            index->entries != HeadPartEntries(dataHandler) || HasNonAscii(key)) {
            g_fallbacks.fetch_add(1, std::memory_order_relaxed);
            return kScan;
        }

        const auto it = index->byName.find(key);
        if (it == index->byName.end()) {
            g_misses.fetch_add(1, std::memory_order_relaxed);
            g_avoided.fetch_add(index->count, std::memory_order_relaxed);
            return index->count;  // same result as a full scan without a match
        }

        const char* slotName = PartName(index->entries[it->second]);
        if (!slotName || !CiEqual{}(slotName, key)) {
            g_fallbacks.fetch_add(1, std::memory_order_relaxed);
            return kScan;  // slot changed under us
        }

        g_hits.fetch_add(1, std::memory_order_relaxed);
        g_avoided.fetch_add(it->second + 1, std::memory_order_relaxed);
        return it->second;
    }

    // kInlineLoop: called from the stub with skee's own std::string (MSVC layout, same toolset family) and
    // DataHandler. Returns the loop start index for EBX (0 = the original full scan).
    extern "C" std::uint32_t SHLF_FindStartIndex(const std::string* name, const std::byte* dataHandler) noexcept
    {
        if (!name || !dataHandler) {
            g_fallbacks.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        const auto slot = LookupSlot(*name, dataHandler);
        return slot == kScan ? 0 : slot;
    }

    // kWholeFunction: replaces skee's GetHeadPartByName(const std::string&) -> BGSHeadPart* (or nullptr).
    // When the map cannot answer, it scans like the original: the name interned once, compared by pool
    // pointer against every head part's partName, first match wins.
    extern "C" const std::byte* SHLF_GetHeadPartByName(const std::string* name) noexcept
    {
        const auto* dataHandler = reinterpret_cast<const std::byte*>(RE::TESDataHandler::GetSingleton());
        if (!name || !dataHandler) {
            return nullptr;
        }
        const std::uint32_t count = HeadPartCount(dataHandler);
        const auto* const* entries = HeadPartEntries(dataHandler);
        const auto slot = LookupSlot(*name, dataHandler);
        if (slot != kScan) {
            return slot < count ? entries[slot] : nullptr;
        }
        const RE::BSFixedString key(name->c_str());
        // The raw pool pointer, as the original compares it (data() would turn null into "").
        const char* const keyPtr = *reinterpret_cast<const char* const*>(&key);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (entries[i] && PartName(entries[i]) == keyPtr) {
                return entries[i];
            }
        }
        return nullptr;
    }

    void BuildIndex()
    {
        auto* dataHandler = reinterpret_cast<const std::byte*>(RE::TESDataHandler::GetSingleton());
        if (!dataHandler) {
            SKSE::log::error("TESDataHandler not available; lookup cache not built (original scan stays in use)");
            return;
        }

        auto* index = new HeadPartIndex();
        index->dataHandler = dataHandler;
        index->entries = HeadPartEntries(dataHandler);
        index->count = HeadPartCount(dataHandler);
        index->byName.reserve(index->count);

        std::uint32_t named = 0;
        std::uint32_t duplicates = 0;
        std::uint32_t nonAscii = 0;
        for (std::uint32_t i = 0; i < index->count; ++i) {
            const char* name = PartName(index->entries[i]);
            if (!name) {
                continue;  // left to the original scan (the slot check fails for it)
            }
            ++named;
            if (HasNonAscii(name)) {
                ++nonAscii;  // never served from the map (lookups with such names fall back)
            }
            // emplace keeps the first index: the original loop returns the first match in array order.
            if (!index->byName.emplace(name, i).second) {
                ++duplicates;
            }
        }

        g_index.store(index, std::memory_order_release);  // never freed: read lock-free by job threads
        SKSE::log::info("Lookup cache built: {} head parts, {} named, {} unique names ({} duplicate names keep "
                        "their first slot, {} non-ASCII names use the original scan)",
            index->count, named, index->byName.size(), duplicates, nonAscii);
    }

    // The counters cover every patched lookup (the sculpt lambda's loop on AE, GetHeadPartByName on all builds)
    // and are reset when a save starts loading, so each report is one load's figures.
    std::atomic<std::uint32_t> g_loadGeneration{ 0 };

    void ResetStats() noexcept
    {
        g_hits.store(0, std::memory_order_relaxed);
        g_misses.store(0, std::memory_order_relaxed);
        g_fallbacks.store(0, std::memory_order_relaxed);
        g_avoided.store(0, std::memory_order_relaxed);
        g_loadGeneration.fetch_add(1, std::memory_order_relaxed);
    }

    // One line, 30 s after a load: the post-load face rebuilds are done by then. Skipped when another load
    // started meanwhile (its own report follows).
    void LogLoadStatsLater()
    {
        const auto generation = g_loadGeneration.load(std::memory_order_relaxed);
        std::thread([generation] {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            if (g_loadGeneration.load(std::memory_order_relaxed) != generation) {
                return;
            }
            SKSE::log::info("[this load, first 30 s] head part lookups: {} from the cache, {} not found, {} used the "
                            "original scan; ~{} string-pool create/release pairs avoided",
                g_hits.load(std::memory_order_relaxed),
                g_misses.load(std::memory_order_relaxed),
                g_fallbacks.load(std::memory_order_relaxed),
                g_avoided.load(std::memory_order_relaxed));
        }).detach();
    }

    // ------------------------------------------------------------------ patch
    [[nodiscard]] bool ReadFileVersion(const wchar_t* path, std::array<std::uint16_t, 4>& out)
    {
        DWORD handle = 0;
        const DWORD size = ::GetFileVersionInfoSizeW(path, &handle);
        if (size == 0) {
            return false;
        }
        std::vector<std::byte> data(size);
        if (!::GetFileVersionInfoW(path, 0, size, data.data())) {
            return false;
        }
        VS_FIXEDFILEINFO* info = nullptr;
        UINT len = 0;
        if (!::VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) {
            return false;
        }
        out = { HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS) };
        return true;
    }

    // kInlineLoop stub, reached by `call rel32` from the site:
    //   sub rsp,28h
    //   lea rcx,[rsp+nameRsp+30h]  ; the caller's [rsp+nameRsp] (return address + 28h of our frame in between)
    //   mov rdx,rdi                ; DataHandler
    //   mov rax,imm64              ; SHLF_FindStartIndex
    //   call rax
    //   mov ebx,eax                ; loop index
    //   add rsp,28h
    //   ret
    [[nodiscard]] std::array<std::uint8_t, 35> InlineLoopStub(std::uint32_t nameRsp)
    {
        std::array<std::uint8_t, 35> code{
            0x48, 0x83, 0xEC, 0x28,
            0x48, 0x8D, 0x8C, 0x24, 0, 0, 0, 0,
            0x48, 0x8B, 0xD7,
            0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
            0xFF, 0xD0,
            0x8B, 0xD8,
            0x48, 0x83, 0xC4, 0x28,
            0xC3,
            0xCC
        };
        const std::uint32_t disp = nameRsp + 0x30;
        std::memcpy(code.data() + 8, &disp, sizeof(disp));
        PutU64(code.data() + 17, reinterpret_cast<std::uint64_t>(&SHLF_FindStartIndex));
        return code;
    }

    // kWholeFunction thunk, reached by `jmp rel32` from the function entry: jmp [rip+0] -> SHLF_GetHeadPartByName
    [[nodiscard]] std::array<std::uint8_t, 14> WholeFunctionThunk()
    {
        std::array<std::uint8_t, 14> code{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
        PutU64(code.data() + 6, reinterpret_cast<std::uint64_t>(&SHLF_GetHeadPartByName));
        return code;
    }

    // Every byte a patch overwrites must be one of the checked bytes, or the table entry is unusable.
    [[nodiscard]] bool SitesAreChecked(const SkeeBuild& build) noexcept
    {
        return std::all_of(build.sites.begin(), build.sites.end(), [&](const PatchSite& s) {
            return std::any_of(build.expected.begin(), build.expected.end(), [&](const ExpectedBytes& e) {
                return e.rva <= s.rva && s.rva + s.len <= e.rva + e.bytes.size();
            });
        });
    }

    [[nodiscard]] bool CodeMatches(std::uintptr_t base, const SkeeBuild& build, std::uintptr_t& firstDiff) noexcept
    {
        for (const auto& e : build.expected) {
            if (e.rva + e.bytes.size() > build.sizeOfImage ||
                std::memcmp(reinterpret_cast<const void*>(base + e.rva), e.bytes.data(), e.bytes.size()) != 0) {
                firstDiff = e.rva;
                return false;
            }
        }
        return true;
    }

    // Returns the patched build, or nullptr when nothing was patched.
    [[nodiscard]] const SkeeBuild* InstallPatch()
    {
        HMODULE skee = ::GetModuleHandleW(L"skee64.dll");
        if (!skee) {
            SKSE::log::warn("skee64.dll (RaceMenu) is not loaded; nothing to patch");
            return nullptr;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(skee);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const std::uint32_t stamp = nt->FileHeader.TimeDateStamp;
        const std::uint32_t size = nt->OptionalHeader.SizeOfImage;

        wchar_t path[MAX_PATH]{};
        ::GetModuleFileNameW(skee, path, MAX_PATH);
        std::array<std::uint16_t, 4> version{};
        const bool hasVersion = ReadFileVersion(path, version);
        SKSE::log::info("skee64.dll file version {}, PE timestamp {:08X}, image size {:X}, at {:X}",
            hasVersion ? std::format("{}.{}.{}.{}", version[0], version[1], version[2], version[3]) : std::string("?"),
            stamp, size, base);

        const auto it = std::find_if(kBuilds.begin(), kBuilds.end(),
            [&](const SkeeBuild& b) { return b.timeDateStamp == stamp && b.sizeOfImage == size; });
        if (it == kBuilds.end()) {
            SKSE::log::warn("This skee64.dll build is not supported (supported: RaceMenu 0.4.20.0 Steam/GOG, 0.4.19.16, "
                            "0.4.16 SE); not patching");
            return nullptr;
        }
        const SkeeBuild& build = *it;
        if (!SitesAreChecked(build)) {
            SKSE::log::error("{}: a patch site is not covered by the byte checks (table error); not patching", build.name);
            return nullptr;
        }
        std::uintptr_t diff = 0;
        if (!CodeMatches(base, build, diff)) {
            SKSE::log::warn("{}: code at skee64+{:X} differs from the expected build (edited skee64.dll, or another "
                            "plugin patched it first); not patching", build.name, diff);
            return nullptr;
        }

        // All targets first, so a failure leaves skee64 untouched (all sites or none).
        std::vector<std::pair<std::uint8_t*, std::int32_t>> targets;
        for (const auto& s : build.sites) {
            std::uint8_t* mem = s.kind == PatchKind::kInlineLoop ?
                                    AllocExecNear(InlineLoopStub(s.nameRsp), base, base + size) :
                                    AllocExecNear(WholeFunctionThunk(), base, base + size);
            std::int32_t rel = 0;
            if (!mem || !Rel32(base + s.rva, reinterpret_cast<std::uintptr_t>(mem), rel)) {
                if (mem) {
                    ::VirtualFree(mem, 0, MEM_RELEASE);
                }
                for (const auto& t : targets) {
                    ::VirtualFree(t.first, 0, MEM_RELEASE);
                }
                SKSE::log::error("{}: could not allocate code within reach of skee64.dll; not patching", build.name);
                return nullptr;
            }
            targets.emplace_back(mem, rel);
        }

        // kInlineLoop: call stub + NOPs over the alignment NOPs (RAX/RCX/RDX are reloaded at the loop head).
        // kWholeFunction: jmp thunk over the first instruction (the function is never re-entered).
        for (std::size_t i = 0; i < build.sites.size(); ++i) {
            const auto& s = build.sites[i];
            std::vector<std::uint8_t> patch(s.len, 0x90);
            patch[0] = s.kind == PatchKind::kInlineLoop ? 0xE8 : 0xE9;
            std::memcpy(patch.data() + 1, &targets[i].second, sizeof(std::int32_t));
            WriteCode(base + s.rva, patch);
            SKSE::log::info("{}: patched skee64+{:X} ({}) -> {:X}", build.name, s.rva,
                s.kind == PatchKind::kInlineLoop ? "sculpt head part lookup" : "GetHeadPartByName",
                reinterpret_cast<std::uintptr_t>(targets[i].first));
        }
        return &build;
    }

    const SkeeBuild* g_patchedBuild = nullptr;  // set at kPostLoad; nullptr = nothing patched

    void InitializeLogging()
    {
        auto logDirectory = SKSE::log::log_directory();
        if (!logDirectory) {
            return;
        }
        std::error_code error;
        std::filesystem::create_directories(*logDirectory, error);
        try {
            auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                (*logDirectory / "SkeeHeadPartLookupFix.log").string(), true);
            auto logger = std::make_shared<spdlog::logger>("skee_headpart_lookup_fix", std::move(sink));
            spdlog::set_default_logger(std::move(logger));
            spdlog::set_level(spdlog::level::info);
            spdlog::flush_on(spdlog::level::info);
        } catch (const std::exception&) {
        }
    }

    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        switch (message->type) {
        case SKSE::MessagingInterface::kPostLoad:
            // every plugin (skee64 included) is loaded by now; no face is built yet
            g_patchedBuild = InstallPatch();
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            if (g_patchedBuild) {
                BuildIndex();  // only read through: the DataHandler offsets are verified for the patched builds only
            } else {
                SKSE::log::info("Nothing patched; lookup cache not built");
            }
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            ResetStats();
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            if (g_patchedBuild) {
                LogLoadStatsLater();
            }
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    InitializeLogging();
    if (const auto* plugin = SKSE::PluginDeclaration::GetSingleton()) {
        SKSE::log::info("{} {} loading", plugin->GetName(), plugin->GetVersion().string("."));  // version from CMakeLists
    }

    if (auto* messaging = SKSE::GetMessagingInterface(); !messaging || !messaging->RegisterListener(OnMessage)) {
        SKSE::log::error("Could not register the SKSE message listener");
        return false;
    }
    return true;
}
