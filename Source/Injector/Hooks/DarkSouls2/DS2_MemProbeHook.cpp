/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_MemProbeHook.h"
#include "Injector/Config/RuntimeConfig.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"
#include "Shared/Platform/Platform.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
#ifdef _WIN32

    // Two module-relative globals reach everything this hook reports on.
    // Chains were read out of the disassembly around DarkSoulsII.exe+0x250dc0.
    constexpr size_t kZoneCtrlRoot = 0x1616cf8;   // -> [+0x20] -> [+0x5b8] = zone control
    constexpr size_t kMapRoot      = 0x16148f0;   // -> [+0x38] -> [+0x08] -> [+0x20] -> [+0x30] -> [+0x70] = map block

    constexpr size_t kBlockMapIdOffset = 0x1c;

    std::atomic<bool> s_running{ false };
    std::thread s_thread;
    uintptr_t s_base = 0;
    std::filesystem::path s_log_path;
    std::filesystem::path s_request_path;

    // Every read goes through here. The chains walk live game structures that
    // other threads are free to tear down, so a fault has to be an answer
    // rather than a crash. Kept free of anything that needs unwinding, which
    // MSVC will not allow beside __try.
    bool TryRead(const void* From, void* Into, size_t Length)
    {
        __try
        {
            memcpy(Into, From, Length);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadPointer(uintptr_t At, uintptr_t& Out)
    {
        uintptr_t Value = 0;
        if (!TryRead((const void*)At, &Value, sizeof(Value)))
        {
            return false;
        }
        Out = Value;
        return true;
    }

    // Walks base + Start, then dereferences each offset in turn. Returns the
    // address the last dereference landed on, without reading through it.
    bool Walk(uintptr_t Start, const std::vector<size_t>& Offsets, uintptr_t& Out)
    {
        uintptr_t At = Start;
        for (size_t Offset : Offsets)
        {
            if (At == 0)
            {
                return false;
            }
            if (!ReadPointer(At + Offset, At))
            {
                return false;
            }
        }
        Out = At;
        return At != 0;
    }

    void Append(const std::string& Text)
    {
        std::ofstream Stream(s_log_path, std::ios::app);
        if (Stream.is_open())
        {
            Stream << Text;
        }
    }

    std::string HexDump(const std::vector<uint8_t>& Bytes, uintptr_t Address)
    {
        std::string Out;
        char Line[160];
        for (size_t i = 0; i < Bytes.size(); i += 16)
        {
            int Written = snprintf(Line, sizeof(Line), "    %016llx +0x%03zx ",
                (unsigned long long)(Address + i), i);
            Out.append(Line, (size_t)Written);
            for (size_t j = 0; j < 16 && i + j < Bytes.size(); ++j)
            {
                Written = snprintf(Line, sizeof(Line), "%02x", Bytes[i + j]);
                Out.append(Line, (size_t)Written);
                if (j % 4 == 3)
                {
                    Out.push_back(' ');
                }
            }
            Out.push_back('\n');
        }
        return Out;
    }

    void DumpRegion(const char* Label, uintptr_t Address, size_t Length)
    {
        if (Address == 0)
        {
            Append(StringFormat("  %s: nao resolvido\n", Label));
            return;
        }

        std::vector<uint8_t> Bytes(Length, 0);
        if (!TryRead((const void*)Address, Bytes.data(), Length))
        {
            Append(StringFormat("  %s: 0x%016llx ilegivel\n", Label, (unsigned long long)Address));
            return;
        }

        Append(StringFormat("  %s: 0x%016llx (%zu bytes)\n", Label, (unsigned long long)Address, Length));
        Append(HexDump(Bytes, Address));
    }

    bool ResolveBlock(uintptr_t& Out)
    {
        return Walk(s_base + kMapRoot, { 0, 0x38, 0x08, 0x20, 0x30, 0x70 }, Out);
    }

    bool ResolveZoneCtrl(uintptr_t& Out)
    {
        return Walk(s_base + kZoneCtrlRoot, { 0, 0x20, 0x5b8 }, Out);
    }

    // The standard picture: everything a Heide-against-Majula comparison needs,
    // written whenever the area changes so both halves land in one log.
    void DumpStandard(const char* Reason, int32_t MapId)
    {
        Append(StringFormat("\n=== %s map=%d ===\n", Reason, (int)MapId));

        uintptr_t Block = 0;
        if (ResolveBlock(Block))
        {
            DumpRegion("block", Block, 0x100);
        }
        else
        {
            Append("  block: cadeia nao resolveu\n");
        }

        uintptr_t ZoneCtrl = 0;
        if (ResolveZoneCtrl(ZoneCtrl))
        {
            DumpRegion("zonectrl", ZoneCtrl, 0x80);
        }
        else
        {
            Append("  zonectrl: cadeia nao resolveu\n");
        }
    }

    bool CurrentMapId(int32_t& Out)
    {
        uintptr_t Block = 0;
        if (!ResolveBlock(Block))
        {
            return false;
        }
        int32_t Value = 0;
        if (!TryRead((const void*)(Block + kBlockMapIdOffset), &Value, sizeof(Value)))
        {
            return false;
        }
        Out = Value;
        return true;
    }

    // A request file holds one dump per line, in one of three shapes:
    //   abs   <label> <hex address> <length>
    //   mod   <label> <hex offset from the module base> <length>
    //   chain <label> <hex offset from the module base> <off,off,...> <length>
    void ServeRequests()
    {
        std::error_code Error;
        if (!std::filesystem::exists(s_request_path, Error))
        {
            return;
        }

        std::vector<std::string> Lines;
        {
            std::ifstream Stream(s_request_path);
            std::string Line;
            while (std::getline(Stream, Line))
            {
                Lines.push_back(Line);
            }
        }
        std::filesystem::remove(s_request_path, Error);

        for (const std::string& Line : Lines)
        {
            if (Line.empty() || Line[0] == '#')
            {
                continue;
            }

            std::istringstream Parts(Line);
            std::string Kind;
            std::string Label;
            std::string Where;
            Parts >> Kind >> Label >> Where;
            if (Kind.empty() || Label.empty() || Where.empty())
            {
                continue;
            }

            uintptr_t Address = 0;
            size_t Length = 0;

            if (Kind == "abs" || Kind == "mod")
            {
                Parts >> std::dec >> Length;
                Address = (uintptr_t)strtoull(Where.c_str(), nullptr, 16);
                if (Kind == "mod")
                {
                    Address += s_base;
                }
            }
            else if (Kind == "chain")
            {
                std::string OffsetList;
                Parts >> OffsetList >> std::dec >> Length;

                std::vector<size_t> Offsets{ 0 };
                size_t At = 0;
                while (At <= OffsetList.size())
                {
                    size_t Comma = OffsetList.find(',', At);
                    std::string One = OffsetList.substr(At, Comma == std::string::npos ? std::string::npos : Comma - At);
                    if (!One.empty())
                    {
                        Offsets.push_back((size_t)strtoull(One.c_str(), nullptr, 16));
                    }
                    if (Comma == std::string::npos)
                    {
                        break;
                    }
                    At = Comma + 1;
                }

                uintptr_t Start = s_base + (uintptr_t)strtoull(Where.c_str(), nullptr, 16);
                if (!Walk(Start, Offsets, Address))
                {
                    Append(StringFormat("\n=== pedido %s: cadeia nao resolveu ===\n", Label.c_str()));
                    continue;
                }
            }
            else
            {
                continue;
            }

            if (Length == 0 || Length > 0x4000)
            {
                Length = 0x100;
            }

            Append(StringFormat("\n=== pedido %s ===\n", Label.c_str()));
            DumpRegion(Label.c_str(), Address, Length);
        }
    }

    void Run()
    {
        int32_t LastMap = -1;
        bool HaveMap = false;

        while (s_running.load())
        {
            int32_t MapId = 0;
            if (CurrentMapId(MapId))
            {
                if (!HaveMap || MapId != LastMap)
                {
                    DumpStandard(HaveMap ? "area mudou" : "primeira area", MapId);
                    LastMap = MapId;
                    HaveMap = true;
                }
            }

            ServeRequests();

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

#endif
}

bool DS2_MemProbeHook::Install(Injector& injector)
{
#ifdef _WIN32
    if (injector.GetConfig().GameType != GameType::DarkSouls2)
    {
        return true;
    }

    s_base = (uintptr_t)injector.GetBaseAddress();
    s_log_path = injector.GetDllPath() / "DS2_MemProbe.log";
    s_request_path = injector.GetDllPath() / "DS2_MemProbe.req";

    {
        std::ofstream Stream(s_log_path, std::ios::app);
        if (Stream.is_open())
        {
            Stream << StringFormat("\n============ sonda de memoria ligada base=0x%016llx ============\n",
                (unsigned long long)s_base);
        }
    }

    s_running.store(true);
    s_thread = std::thread(Run);

    Log("[DS2MemProbe] sonda no ar, pedidos em %s", s_request_path.string().c_str());
#endif
    return true;
}

void DS2_MemProbeHook::Uninstall()
{
#ifdef _WIN32
    s_running.store(false);
    if (s_thread.joinable())
    {
        s_thread.join();
    }
#endif
}

const char* DS2_MemProbeHook::GetName()
{
    return "DS2 Memory Probe";
}
