/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_AreaProbeHook.h"
#include "Injector/Config/RuntimeConfig.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"
#include "Shared/Platform/Platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
#if defined(_WIN32) && defined(_M_X64)

    struct KnownArea
    {
        uint32_t Id;
        const char* Name;
    };

    // The ids the server knows about. An address that holds one of these, and
    // swaps to another when the player travels, is following the player.
    constexpr KnownArea kAreas[] = {
    { 0x0098e4a0u, "Things Betwixt" },
    { 0x009932c0u, "Majula" },
    { 0x009a1d20u, "Forest of Fallen Giants" },
    { 0x009d5170u, "Heide's Tower of Flame" },
    { 0x009d2a60u, "Path to No-man's Wharf" },
    { 0x009b55a0u, "No-man's Wharf" },
    { 0x009b0780u, "The Lost Bastille" },
    { 0x009b0780u, "Sinner's Rise" },
    { 0x009c18f0u, "Huntsman's Copse" },
    { 0x009b2e90u, "Harvest Valley" },
    { 0x009b7cb0u, "Iron Keep" },
    { 0x009d0350u, "Shaded Woods" },
    { 0x009d7880u, "Ruined Fork Road" },
    { 0x009d9f90u, "Doors of Pharros" },
    { 0x009ab960u, "Brightstone Cove Tseldora" },
    { 0x009dc6a0u, "Grave of Saints" },
    { 0x009c6710u, "The Gutter" },
    { 0x01346150u, "Drangleic Castle" },
    { 0x0132dab0u, "Shrine of Amana" },
    { 0x0134d680u, "Undead Crypt" },
    { 0x009ae070u, "Aldia's Keep" },
    { 0x009cb530u, "Dragon Aerie" },
    { 0x030047b0u, "Shulva, Sanctum City" },
    { 0x03006ec0u, "Brume Tower" },
    { 0x030095d0u, "Frozen Eleum Loyce" },
    };

    const char* AreaName(uint32_t Value)
    {
        for (const KnownArea& Area : kAreas)
        {
            if (Area.Id == Value)
            {
                return Area.Name;
            }
        }
        return nullptr;
    }

    // Enough to scan efficiently, small enough that the allocation is never
    // something the game notices.
    constexpr size_t kChunkBytes = 1u << 20;

    struct Candidate
    {
        uintptr_t Address;
        uint32_t Value;
        int Changes;
    };

    std::mutex s_log_mutex;

    void Append(const std::string& Text)
    {
        std::scoped_lock lock(s_log_mutex);
        std::filesystem::path Path = Injector::Instance().GetDllPath() / "DS2_AreaProbe.log";
        std::ofstream Stream(Path, std::ios::app);
        if (Stream.is_open())
        {
            Stream << Text;
        }
    }

    // ---- phase two: who reads the address we found --------------------------
    //
    // A guard page: the page holding the address is marked PAGE_GUARD, so any
    // access to it faults once and reports the exact address touched. Wine
    // accepts debug registers and never fires them, but implements this.


    struct Access
    {
        uint64_t Count;
        double FirstSeen;
        double LastSeen;
    };

    std::mutex s_access_mutex;
    std::unordered_map<uintptr_t, Access> s_accesses;

    // Counters so silence can be told apart from a handler that never runs.
    std::atomic_uint64_t s_exceptions_seen{0};
    std::atomic_uint64_t s_single_steps{0};
    std::atomic_uint64_t s_guard_hits{0};
    std::atomic_bool s_rearm_pending{false};
    std::atomic_uintptr_t s_watched{0};
    PVOID s_watch_handler = nullptr;
    uintptr_t s_module_base = 0;

    constexpr DWORD64 kTrapFlag = 0x100;

    /// Re-applies PAGE_GUARD to the page holding the watched address.
    ///
    /// The guard is one-shot: the processor clears it as it delivers the fault,
    /// so it has to be put back after every hit.
    bool ArmGuardPage(uintptr_t Address)
    {
        MEMORY_BASIC_INFORMATION Info = {};
        if (VirtualQuery((void*)Address, &Info, sizeof(Info)) != sizeof(Info))
        {
            return false;
        }
        if (Info.State != MEM_COMMIT || (Info.Protect & PAGE_GUARD) != 0)
        {
            return false;
        }

        DWORD Previous = 0;
        return VirtualProtect(Info.BaseAddress, Info.RegionSize, Info.Protect | PAGE_GUARD, &Previous) != 0;
    }

    LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* Exception)
    {
        s_exceptions_seen++;

        const uintptr_t Watched = s_watched.load();
        if (Watched == 0)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        CONTEXT* Context = Exception->ContextRecord;
        const DWORD Code = Exception->ExceptionRecord->ExceptionCode;

        if (Code == STATUS_GUARD_PAGE_VIOLATION)
        {
            s_guard_hits++;

            // The record carries the address the instruction actually touched,
            // which is what separates our four bytes from the rest of the page.
            const uintptr_t Touched =
                Exception->ExceptionRecord->NumberParameters >= 2
                    ? (uintptr_t)Exception->ExceptionRecord->ExceptionInformation[1]
                    : 0;

            if (Touched >= Watched && Touched < Watched + sizeof(uint32_t))
            {
                const uintptr_t Rip = (uintptr_t)Context->Rip;
                const double Now = GetSeconds();
                std::scoped_lock lock(s_access_mutex);
                auto& Entry = s_accesses[Rip];
                if (Entry.Count == 0)
                {
                    Entry.FirstSeen = Now;
                }
                Entry.Count++;
                Entry.LastSeen = Now;
            }

            // The guard is gone now. Step one instruction, then put it back.
            Context->EFlags |= (DWORD)kTrapFlag;
            s_rearm_pending.store(true);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (Code == EXCEPTION_SINGLE_STEP && s_rearm_pending.load())
        {
            s_single_steps++;
            s_rearm_pending.store(false);
            Context->EFlags &= ~(DWORD)kTrapFlag;
            ArmGuardPage(Watched);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

    /// Writes the accesses seen so far, rarest first.
    ///
    /// Something read every frame piles up thousands of hits; a check that only
    /// runs when an item is used shows up with a handful. Sorting that way puts
    /// the interesting one at the top.
    void ReportAccesses()
    {
        std::vector<std::pair<uintptr_t, Access>> Sorted;
        {
            std::scoped_lock lock(s_access_mutex);
            Sorted.assign(s_accesses.begin(), s_accesses.end());
        }
        // Always report, even with nothing to show: an empty report says the
        // loop is alive, which silence does not.
        Append(StringFormat(
            "time=%.3f event=DS2AreaWatch result=counters exceptions=%llu single_steps=%llu "
            "guard_hits=%llu readers=%zu\n",
            GetSeconds(),
            (unsigned long long)s_exceptions_seen.load(),
            (unsigned long long)s_single_steps.load(),
            (unsigned long long)s_guard_hits.load(),
            Sorted.size()));

        if (Sorted.empty())
        {
            return;
        }

        std::sort(Sorted.begin(), Sorted.end(), [](const auto& a, const auto& b) {
            return a.second.Count < b.second.Count;
        });

        Append(StringFormat(
            "time=%.3f event=DS2AreaWatch result=report readers=%zu\n",
            GetSeconds(),
            Sorted.size()));

        int Reported = 0;
        for (const auto& [Rip, Info] : Sorted)
        {
            if (Reported >= 25)
            {
                break;
            }
            Append(StringFormat(
                "    rip=0x%016llx offset=%s+0x%llx hits=%llu first=%.3f last=%.3f\n",
                (unsigned long long)Rip,
                s_module_base != 0 && Rip > s_module_base ? "DarkSoulsII.exe" : "?",
                (unsigned long long)(s_module_base != 0 && Rip > s_module_base ? Rip - s_module_base : Rip),
                (unsigned long long)Info.Count,
                Info.FirstSeen,
                Info.LastSeen));
            Reported++;
        }
    }

    std::atomic_bool s_running{false};
    std::thread s_thread;
    bool IsScannable(const MEMORY_BASIC_INFORMATION& Info)
    {
        if (Info.State != MEM_COMMIT)
        {
            return false;
        }
        // The area id is a variable the game writes as the player travels, so
        // read-only pages cannot hold it.
        const DWORD Writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((Info.Protect & Writable) == 0)
        {
            return false;
        }
        return (Info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
    }

    /// Copies a region into a buffer, surviving the case where another thread
    /// frees it between the protection check and the read.
    ///
    /// Without this the probe eventually faults inside the game and kills it,
    /// which it did: a debug tool has no business crashing what it observes.
    bool TryCopy(uintptr_t Address, size_t Size, void* Destination)
    {
        __try
        {
            memcpy(Destination, (const void*)Address, Size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool TryReadU32(uintptr_t Address, uint32_t& Out)
    {
        return TryCopy(Address, sizeof(uint32_t), &Out);
    }

    /// One full pass over writable memory, collecting every address that holds
    /// a known area id.
    std::vector<Candidate> ScanEverything()
    {
        std::vector<Candidate> Found;
        std::vector<uint8_t> Buffer(kChunkBytes);
        MEMORY_BASIC_INFORMATION Info = {};
        uintptr_t Cursor = 0;

        while (VirtualQuery((void*)Cursor, &Info, sizeof(Info)) == sizeof(Info))
        {
            const uintptr_t Base = (uintptr_t)Info.BaseAddress;
            const uintptr_t End = Base + Info.RegionSize;

            if (IsScannable(Info))
            {
                // Copy in fixed chunks rather than whole regions. A region can
                // be gigabytes, and allocating that much to scan it starved the
                // game badly enough to hang it.
                for (uintptr_t Chunk = Base; Chunk < End; Chunk += kChunkBytes)
                {
                    const size_t Size = (size_t)std::min<uintptr_t>(kChunkBytes, End - Chunk);
                    if (!TryCopy(Chunk, Size, Buffer.data()))
                    {
                        continue;
                    }

                    const size_t Count = Size / sizeof(uint32_t);
                    const uint32_t* Values = (const uint32_t*)Buffer.data();
                    for (size_t Index = 0; Index < Count; Index++)
                    {
                        if (AreaName(Values[Index]) != nullptr)
                        {
                            Found.push_back({ Chunk + Index * sizeof(uint32_t), Values[Index], 0 });
                            if (Found.size() > 200000)
                            {
                                return Found;
                            }
                        }
                    }
                }

                // Scanning is background work; leave the game some air.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            if (End <= Cursor)
            {
                break;
            }
            Cursor = End;
        }

        return Found;
    }

    /// Keeps only the candidates that still hold a known area id, and counts
    /// the ones that changed to a different area since the last pass.
    void Refine(std::vector<Candidate>& Candidates, int& OutChangedThisPass)
    {
        std::vector<Candidate> Kept;
        Kept.reserve(Candidates.size());
        OutChangedThisPass = 0;

        for (Candidate& Entry : Candidates)
        {
            MEMORY_BASIC_INFORMATION Info = {};
            if (VirtualQuery((void*)Entry.Address, &Info, sizeof(Info)) != sizeof(Info) || !IsScannable(Info))
            {
                continue;
            }

            uint32_t Value = 0;
            if (!TryReadU32(Entry.Address, Value) || AreaName(Value) == nullptr)
            {
                continue;
            }

            if (Value != Entry.Value)
            {
                Entry.Value = Value;
                Entry.Changes++;
                OutChangedThisPass++;
            }
            Kept.push_back(Entry);
        }

        Candidates.swap(Kept);
    }

    /// Arms the watch on an address the caller already knows, if it currently
    /// holds a known area id.
    ///
    /// This is retried rather than checked once: twenty seconds after injection
    /// the player is usually still at the menu and the value is zero, which
    /// says nothing about whether the address is right.
    bool TryArmFromHint(const std::string& Hint)
    {
        if (Hint.empty())
        {
            return false;
        }

        const uintptr_t Address = (uintptr_t)strtoull(Hint.c_str(), nullptr, 0);
        if (Address == 0 || (Address & 0x3) != 0)
        {
            return false;
        }

        uint32_t Value = 0;
        if (!TryReadU32(Address, Value) || AreaName(Value) == nullptr)
        {
            return false;
        }

        s_watch_handler = AddVectoredExceptionHandler(1, WatchHandler);
        s_watched.store(Address);
        const bool Guarded = ArmGuardPage(Address);

        Append(StringFormat(
            "time=%.3f event=DS2AreaWatch result=armed_from_hint address=0x%016llx "
            "value=0x%08x area=%s guard=%d module_base=0x%016llx\n",
            GetSeconds(),
            (unsigned long long)Address,
            Value,
            AreaName(Value),
            Guarded ? 1 : 0,
            (unsigned long long)s_module_base));
        Log("[DS2AreaWatch] usando o endereco informado 0x%016llx (%s)",
            (unsigned long long)Address,
            AreaName(Value));
        return true;
    }

    void ProbeThread()
    {
        // Let the game finish loading before walking its address space.
        std::this_thread::sleep_for(std::chrono::seconds(20));

        const bool Watch = Injector::Instance().GetConfig().DS2WatchAreaReads;
        s_module_base = (uintptr_t)Injector::Instance().GetBaseAddress();

        Append("============================================================\n");
        Append(StringFormat("time=%.3f event=DS2AreaProbe result=scan_started watch=%d\n",
                            GetSeconds(),
                            Watch ? 1 : 0));

        const std::string Hint =
            Watch ? Injector::Instance().GetConfig().DS2AreaAddress : std::string();

        // Give the hint a while to become valid before paying for a scan.
        for (int Attempt = 0; Attempt < 60 && !Hint.empty() && s_watched.load() == 0; Attempt++)
        {
            if (TryArmFromHint(Hint))
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }

        if (s_watched.load() != 0)
        {
            // Armed from the hint; the loop below only reports from here on.
            double LastReport = 0.0;
            double LastRearm = 0.0;
            while (s_running.load())
            {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                const double Now = GetSeconds();
                if (Now - LastRearm >= 15.0)
                {
                    ArmGuardPage(s_watched.load());
                    LastRearm = Now;
                }
                if (Now - LastReport >= 15.0)
                {
                    ReportAccesses();
                    LastReport = Now;
                }
            }
            ReportAccesses();
            return;
        }

        std::vector<Candidate> Candidates = ScanEverything();
        Append(StringFormat(
            "time=%.3f event=DS2AreaProbe result=first_pass candidates=%zu\n",
            GetSeconds(),
            Candidates.size()));
        Log("[DS2AreaProbe] primeira varredura: %zu candidatos", Candidates.size());

        int Pass = 0;
        int PassesWithoutMovement = 0;
        double LastReport = 0.0;
        double LastRearm = 0.0;

        while (s_running.load())
        {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            if (!s_running.load())
            {
                break;
            }

            const double Now = GetSeconds();

            // Once the watchpoint is set, the scan has done its job and the
            // thread's only remaining work is reporting.
            if (s_watched.load() != 0)
            {
                // Threads created after arming carry no debug registers, so
                // the watchpoint has to be reapplied as the game spawns them.
                if (Now - LastRearm >= 15.0)
                {
                    ArmGuardPage(s_watched.load());
                    LastRearm = Now;
                }
                if (Now - LastReport >= 15.0)
                {
                    ReportAccesses();
                    LastReport = Now;
                }
                continue;
            }

            int Changed = 0;
            Refine(Candidates, Changed);
            Pass++;

            if (Changed == 0)
            {
                PassesWithoutMovement++;

                // The structure holding the area id is allocated when the
                // player loads into the world. Scanning before that happens
                // finds only constants, and no amount of travelling will move
                // them, so start over rather than wait forever.
                if (PassesWithoutMovement >= 60)
                {
                    PassesWithoutMovement = 0;
                    Candidates = ScanEverything();
                    Append(StringFormat(
                        "time=%.3f event=DS2AreaProbe result=rescan candidates=%zu\n",
                        GetSeconds(),
                        Candidates.size()));
                    Log("[DS2AreaProbe] nada se moveu; varri de novo: %zu candidatos",
                        Candidates.size());
                }
                continue;
            }

            PassesWithoutMovement = 0;

            Append(StringFormat(
                "time=%.3f event=DS2AreaProbe result=moved pass=%d remaining=%zu changed=%d\n",
                Now,
                Pass,
                Candidates.size(),
                Changed));

            const Candidate* Best = nullptr;
            int Reported = 0;
            for (const Candidate& Entry : Candidates)
            {
                if (Entry.Changes == 0)
                {
                    continue;
                }
                if (Best == nullptr || Entry.Changes > Best->Changes)
                {
                    Best = &Entry;
                }
                if (Reported < 40)
                {
                    Append(StringFormat(
                        "    address=0x%016llx value=0x%08x area=%s changes=%d\n",
                        (unsigned long long)Entry.Address,
                        Entry.Value,
                        AreaName(Entry.Value),
                        Entry.Changes));
                    Reported++;
                }
            }
            Log("[DS2AreaProbe] %d endereco(s) seguiram o jogador (restam %zu candidatos)",
                Changed,
                Candidates.size());

            if (!Watch || Best == nullptr)
            {
                continue;
            }

            // A watchpoint covers four bytes and the address has to be aligned
            // to that, which the game's own variable will be.
            if ((Best->Address & 0x3) != 0)
            {
                Append(StringFormat(
                    "time=%.3f event=DS2AreaWatch result=unaligned address=0x%016llx\n",
                    Now,
                    (unsigned long long)Best->Address));
                continue;
            }

            s_watch_handler = AddVectoredExceptionHandler(1, WatchHandler);
            if (s_watch_handler == nullptr)
            {
                Append(StringFormat("time=%.3f event=DS2AreaWatch result=no_handler\n", Now));
                continue;
            }

            s_watched.store(Best->Address);
            const int Armed = ArmGuardPage(Best->Address) ? 1 : 0;
            LastRearm = Now;

            Append(StringFormat(
                "time=%.3f event=DS2AreaWatch result=armed address=0x%016llx guard=%d module_base=0x%016llx\n",
                Now,
                (unsigned long long)Best->Address,
                Armed,
                (unsigned long long)s_module_base));
            Log("[DS2AreaWatch] observando 0x%016llx (guard=%d); use o item agora",
                (unsigned long long)Best->Address,
                Armed);

            if (Armed == 0)
            {
                Append("    nao consegui marcar a pagina com PAGE_GUARD\n");
                Warning("[DS2AreaWatch] nao consegui marcar a pagina com PAGE_GUARD");
            }
        }

        if (s_watched.load() != 0)
        {
            ReportAccesses();
        }
    }

#endif
}

bool DS2_AreaProbeHook::Install(Injector& injector)
{
#if defined(_WIN32) && defined(_M_X64)
    if (!injector.GetConfig().DS2ProbeArea)
    {
        return true;
    }

    Log("[DS2AreaProbe] ligado; ande entre areas para os candidatos se separarem");
    s_running.store(true);
    s_thread = std::thread(ProbeThread);
    return true;
#else
    Warning("[DS2AreaProbe] requer Windows x64; nao instalado.");
    return true;
#endif
}

void DS2_AreaProbeHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    s_running.store(false);
    if (s_thread.joinable())
    {
        s_thread.join();
    }
    if (s_watch_handler != nullptr)
    {
        RemoveVectoredExceptionHandler(s_watch_handler);
        s_watch_handler = nullptr;
    }
#endif
}

const char* DS2_AreaProbeHook::GetName()
{
    return "DS2 Area Probe";
}
