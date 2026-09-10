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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
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

    struct Candidate
    {
        uintptr_t Address;
        uint32_t Value;
        int Changes;
    };

    std::atomic_bool s_running{false};
    std::thread s_thread;
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

        Append("============================================================\n");
        Append(StringFormat("time=%.3f event=DS2AreaProbe result=scan_started\n", GetSeconds()));

        std::vector<Candidate> Candidates = ScanEverything();
        Append(StringFormat(
            "time=%.3f event=DS2AreaProbe result=first_pass candidates=%zu\n",
            GetSeconds(),
            Candidates.size()));
        Log("[DS2AreaProbe] primeira varredura: %zu candidatos", Candidates.size());

        int Pass = 0;
        while (s_running.load())
        {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            if (!s_running.load())
            {
                break;
            }

            int Changed = 0;
            Refine(Candidates, Changed);
            Pass++;

            if (Changed == 0)
            {
                continue;
            }

            // Only the addresses that have followed the player are worth
            // reporting; everything else is a constant that happens to match.
            Append(StringFormat(
                "time=%.3f event=DS2AreaProbe result=moved pass=%d remaining=%zu changed=%d\n",
                GetSeconds(),
                Pass,
                Candidates.size(),
                Changed));

            int Reported = 0;
            for (const Candidate& Entry : Candidates)
            {
                if (Entry.Changes == 0 || Reported >= 40)
                {
                    continue;
                }
                Append(StringFormat(
                    "    address=0x%016llx value=0x%08x area=%s changes=%d\n",
                    (unsigned long long)Entry.Address,
                    Entry.Value,
                    AreaName(Entry.Value),
                    Entry.Changes));
                Reported++;
            }
            Log("[DS2AreaProbe] %d endereco(s) seguiram o jogador (restam %zu candidatos)",
                Changed,
                Candidates.size());
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
#endif
}

const char* DS2_AreaProbeHook::GetName()
{
    return "DS2 Area Probe";
}
