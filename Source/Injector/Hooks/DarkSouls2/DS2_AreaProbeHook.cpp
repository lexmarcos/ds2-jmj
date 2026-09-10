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
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
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
    // A hardware watchpoint, the same thing Cheat Engine's "find what accesses
    // this address" uses: the processor's debug registers fault on any access
    // to four bytes, without touching the code around them.

    constexpr DWORD64 kDr7EnableDr0 = 1ull;              // L0
    constexpr DWORD64 kDr7ReadWrite = 0b11ull << 16;     // RW0: data read or write
    constexpr DWORD64 kDr7FourBytes = 0b11ull << 18;     // LEN0: four bytes

    struct Access
    {
        uint64_t Count;
        double FirstSeen;
        double LastSeen;
    };

    std::mutex s_access_mutex;
    std::unordered_map<uintptr_t, Access> s_accesses;
    std::atomic_uintptr_t s_watched{0};
    PVOID s_watch_handler = nullptr;
    uintptr_t s_module_base = 0;

    LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* Exception)
    {
        if (Exception->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        if (s_watched.load() == 0)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        CONTEXT* Context = Exception->ContextRecord;

        // Bit 0 of Dr6 says our watchpoint is the one that fired.
        if ((Context->Dr6 & 0x1ull) == 0)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        Context->Dr6 = 0;

        const uintptr_t Rip = (uintptr_t)Context->Rip;
        const double Now = GetSeconds();
        {
            std::scoped_lock lock(s_access_mutex);
            auto& Entry = s_accesses[Rip];
            if (Entry.Count == 0)
            {
                Entry.FirstSeen = Now;
            }
            Entry.Count++;
            Entry.LastSeen = Now;
        }

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    /// Points DR0 at an address on every thread that currently exists.
    int ArmAllThreads(uintptr_t Address)
    {
        HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (Snapshot == INVALID_HANDLE_VALUE)
        {
            return 0;
        }

        const DWORD Process = GetCurrentProcessId();
        const DWORD Self = GetCurrentThreadId();
        int Armed = 0;

        THREADENTRY32 Entry = {};
        Entry.dwSize = sizeof(Entry);
        if (Thread32First(Snapshot, &Entry))
        {
            do
            {
                if (Entry.th32OwnerProcessID != Process || Entry.th32ThreadID == Self)
                {
                    continue;
                }

                HANDLE Thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                                           FALSE,
                                           Entry.th32ThreadID);
                if (Thread == nullptr)
                {
                    continue;
                }

                SuspendThread(Thread);

                CONTEXT Context = {};
                Context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(Thread, &Context))
                {
                    Context.Dr0 = Address;
                    Context.Dr7 = kDr7EnableDr0 | kDr7ReadWrite | kDr7FourBytes;
                    Context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (SetThreadContext(Thread, &Context))
                    {
                        Armed++;
                    }
                }

                ResumeThread(Thread);
                CloseHandle(Thread);
            } while (Thread32Next(Snapshot, &Entry));
        }

        CloseHandle(Snapshot);
        return Armed;
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

    /// One full pass over writable memory, collecting every address that holds
    /// a known area id.
    std::vector<Candidate> ScanEverything()
    {
        std::vector<Candidate> Found;
        MEMORY_BASIC_INFORMATION Info = {};
        uintptr_t Cursor = 0;

        while (VirtualQuery((void*)Cursor, &Info, sizeof(Info)) == sizeof(Info))
        {
            const uintptr_t Base = (uintptr_t)Info.BaseAddress;
            const uintptr_t End = Base + Info.RegionSize;

            if (IsScannable(Info))
            {
                for (uintptr_t At = Base; At + sizeof(uint32_t) <= End; At += sizeof(uint32_t))
                {
                    const uint32_t Value = *(const uint32_t*)At;
                    if (AreaName(Value) != nullptr)
                    {
                        Found.push_back({ At, Value, 0 });
                        if (Found.size() > 200000)
                        {
                            return Found;
                        }
                    }
                }
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

            const uint32_t Value = *(const uint32_t*)Entry.Address;
            if (AreaName(Value) == nullptr)
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

        std::vector<Candidate> Candidates = ScanEverything();
        Append(StringFormat(
            "time=%.3f event=DS2AreaProbe result=first_pass candidates=%zu\n",
            GetSeconds(),
            Candidates.size()));
        Log("[DS2AreaProbe] primeira varredura: %zu candidatos", Candidates.size());

        int Pass = 0;
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
                    ArmAllThreads(s_watched.load());
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
                continue;
            }

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
            const int Armed = ArmAllThreads(Best->Address);
            LastRearm = Now;

            Append(StringFormat(
                "time=%.3f event=DS2AreaWatch result=armed address=0x%016llx threads=%d module_base=0x%016llx\n",
                Now,
                (unsigned long long)Best->Address,
                Armed,
                (unsigned long long)s_module_base));
            Log("[DS2AreaWatch] observando 0x%016llx em %d thread(s); use o item agora",
                (unsigned long long)Best->Address,
                Armed);

            if (Armed == 0)
            {
                Append("    nenhuma thread aceitou o breakpoint de hardware; o Wine pode nao suportar\n");
                Warning("[DS2AreaWatch] nenhuma thread aceitou o breakpoint; o Wine pode nao suportar");
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
