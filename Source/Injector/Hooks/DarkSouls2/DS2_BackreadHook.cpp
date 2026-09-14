/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_BackreadHook.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include "ThirdParty/detours/src/detours.h"
#endif

namespace
{
#ifdef _WIN32

    // Version 1.03 Calibrations 2.02, verified before anything is written.
    //
    // FUN_1403cc3f0, `void(owner, arg)`: one MapAreaCtrlOwner's per-frame
    // update, which the streamer calls for every owner right after writing the
    // masks. Its state machine, FUN_1403cc450, opens by reading the force byte.
    constexpr size_t kOwnerUpdateOffset = 0x3cc3f0;
    constexpr uint8_t kOwnerUpdateBytes[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9 };
    constexpr size_t kOwnerStatesOffset = 0x3cc450;
    constexpr uint8_t kOwnerStatesBytes[] = { 0x40, 0x57, 0x48, 0x83, 0xec, 0x20, 0x80, 0xb9, 0xe9, 0x01, 0x00, 0x00, 0x00 };

    constexpr size_t kOwnerVftable = 0x10e87f0;        // MapAreaCtrlOwner
    constexpr size_t kOwnerMap = 0x08;
    constexpr size_t kOwnerMask = 0x10;                // 4 x uint32, the parts the streamer asks for
    // Every 128-bit parts mask the owner carries, one after another from
    // +0x10. The streamer writes +0x10 and +0x40; FUN_1403dc930 writes +0x50
    // (what the world says is visible), +0x60 (the backread of the visible
    // parts) and +0x70 (the parts the player is in), and the parts controller
    // +0x20 and +0x30. Measured 14/09: with only +0x10 and +0x40 forced, a map
    // came in with no ground. +0x70 is left to the game.
    constexpr size_t kOwnerMasks[] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 };
    constexpr size_t kOwnerState = 0x1e8;              // byte, 5 loaded
    constexpr size_t kOwnerForced = 0x1e9;             // byte

    // FUN_1403dc8e0, `void(streamer, vec4* position, int cell, MapEntity* part,
    // bool)`: once a frame, from FUN_1403be060, the player's position, the nav
    // cell it stands in and the part under its feet. The parts to load are a
    // graph search from that cell (FUN_1403dadd0, FUN_1403da960), so a player
    // in the air - no cell - never brings in the ground of the place it was
    // moved to. Measured 14/09: forced and teleported to, a map with every
    // mask set still let the player fall, and no rigid body was created for
    // it (197 before and after).
    constexpr size_t kStreamerUpdateOffset = 0x3dc8e0;
    constexpr uint8_t kStreamerUpdateBytes[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x0f, 0xb6, 0x44, 0x24, 0x50 };
    // The cell of a position, the way FUN_1403dadd0 finds it: the nav map of
    // the map index (FUN_140badb90, key `(index & 0x3f) << 24 | 0xffffff`),
    // then the nearest cell within 10 units (FUN_140babf90).
    constexpr size_t kNavFindMapOffset = 0xbadb90;
    constexpr uint8_t kNavFindMapBytes[] = { 0x4c, 0x63, 0x51, 0x68, 0x44, 0x8b, 0xca, 0x45, 0x33, 0xc0 };
    constexpr size_t kNavFindCellOffset = 0xbabf90;
    constexpr uint8_t kNavFindCellBytes[] = { 0x48, 0x8b, 0xc4, 0x56, 0x48, 0x83, 0xec, 0x60, 0x0f, 0x29, 0x70, 0xd8 };
    constexpr size_t kContextNav = 0xbc0;              // *(ctx+0xbc0)+0x10
    constexpr size_t kNavManager = 0x10;
    constexpr size_t kOwnerIndex = 0x0c;
    constexpr float kNavSearchRadius = 10.0f;
    constexpr int32_t kNavSearchLimit = 0x40;

    constexpr size_t kContextOffset = 0x16148f0;
    constexpr size_t kMapManager = 0x38;               // ctx+0x38
    constexpr size_t kStreamer = 0x08;
    constexpr size_t kStreamerOwners = 0x38;
    constexpr size_t kStreamerOwnerCount = 0x1b6;      // short
    constexpr int kMaxOwners = 64;

    using OwnerUpdate_p = void(*)(void* Owner, void* Arg);
    OwnerUpdate_p s_original_update = nullptr;
    using StreamerUpdate_p = void(*)(void* Streamer, float* Position, int32_t Cell, void* Part, uint8_t Flag);
    StreamerUpdate_p s_original_streamer = nullptr;
    using NavFindMap_p = uintptr_t(*)(void* Manager, uint32_t Key);
    using NavFindCell_p = int32_t(*)(void* NavMap, const float* Position, float Radius, int32_t Limit, float* Distance);
    NavFindMap_p s_nav_find_map = nullptr;
    NavFindCell_p s_nav_find_cell = nullptr;

    // Where the streamer is told the player is, while a focus is on.
    std::atomic<uint32_t> s_focus_map{ 0 };
    std::atomic<uint32_t> s_focus_generation{ 0 };
    std::mutex s_focus_mutex;
    float s_focus_position[4] = {};
    // Touched only from the game's thread.
    uint32_t s_focus_seen_generation = 0;
    int32_t s_focus_cell = -1;
    uint32_t s_focus_tries = 0;

    uintptr_t s_base = 0;

    std::atomic<uint32_t> s_map{ 0 };
    std::atomic<uint32_t> s_mask[4];
    std::atomic<uint32_t> s_released_map{ 0 };
    std::atomic<uint64_t> s_request_ms{ 0 };

    // Touched only from the game's thread, inside the detour.
    uint8_t s_seen_state = 0xff;
    uint32_t s_seen_map = 0;

    std::atomic<bool> s_running{ false };
    std::thread s_thread;

    std::mutex s_log_mutex;
    std::filesystem::path s_log_path;
    std::filesystem::path s_request_path;

    void Append(const std::string& Text)
    {
        std::scoped_lock Lock(s_log_mutex);
        std::ofstream Stream(s_log_path, std::ios::app);
        if (Stream)
        {
            Stream << Text;
        }
    }

    std::string Clock()
    {
        SYSTEMTIME Now;
        GetLocalTime(&Now);
        return StringFormat("%02u:%02u:%02u.%03u", Now.wHour, Now.wMinute, Now.wSecond, Now.wMilliseconds);
    }

    bool ReadBytes(uintptr_t Address, void* Out, size_t Length)
    {
        __try
        {
            memcpy(Out, (const void*)Address, Length);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool WriteBytes(uintptr_t Address, const void* In, size_t Length)
    {
        __try
        {
            memcpy((void*)Address, In, Length);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadPointer(uintptr_t Address, uintptr_t& Out)
    {
        Out = 0;
        return ReadBytes(Address, &Out, sizeof(Out)) && Out != 0;
    }

    bool BytesMatch(uintptr_t Address, const uint8_t* Expected, size_t Length)
    {
        return memcmp((const void*)Address, Expected, Length) == 0;
    }

    std::string DescribeMask(const uint32_t Mask[4])
    {
        return StringFormat("%08x%08x%08x%08x", Mask[0], Mask[1], Mask[2], Mask[3]);
    }

    void Force(uintptr_t Owner)
    {
        const uint8_t One = 1;
        WriteBytes(Owner + kOwnerForced, &One, 1);

        for (const size_t At : kOwnerMasks)
        {
            uint32_t Mask[4] = {};
            if (ReadBytes(Owner + At, Mask, sizeof(Mask)))
            {
                for (int i = 0; i < 4; ++i)
                {
                    Mask[i] |= s_mask[i].load();
                }
                WriteBytes(Owner + At, Mask, sizeof(Mask));
            }
        }
    }

    void OwnerUpdateHook(void* Owner, void* Arg)
    {
        uint32_t Map = 0;
        const bool HaveMap = Owner != nullptr && ReadBytes((uintptr_t)Owner + kOwnerMap, &Map, sizeof(Map)) && Map != 0;
        if (HaveMap)
        {
            if (Map == s_map.load())
            {
                Force((uintptr_t)Owner);
            }
            else if (Map == s_released_map.load())
            {
                const uint8_t Zero = 0;
                WriteBytes((uintptr_t)Owner + kOwnerForced, &Zero, 1);
                s_released_map.store(0);
                Append(StringFormat("%s  mapa %08x solto\n", Clock().c_str(), Map));
            }
        }

        s_original_update(Owner, Arg);

        if (HaveMap && Map == s_map.load())
        {
            uint8_t State = 0;
            if (ReadBytes((uintptr_t)Owner + kOwnerState, &State, 1) && (State != s_seen_state || Map != s_seen_map))
            {
                const uint64_t Since = GetTickCount64() - s_request_ms.load();
                Append(StringFormat("%s  mapa %08x: estado %u -> %u, %llu ms depois do pedido\n",
                    Clock().c_str(), Map, s_seen_map == Map ? s_seen_state : 0xff, State, (unsigned long long)Since));
                s_seen_state = State;
                s_seen_map = Map;
            }
        }
    }

    // Walks the streamer's owners; any thread, guarded.
    int ReadOwners(uintptr_t Owners[kMaxOwners])
    {
        uintptr_t Context = 0, Manager = 0, Streamer = 0;
        int16_t Count = 0;
        if (!ReadPointer(s_base + kContextOffset, Context) ||
            !ReadPointer(Context + kMapManager, Manager) ||
            !ReadPointer(Manager + kStreamer, Streamer) ||
            !ReadBytes(Streamer + kStreamerOwnerCount, &Count, sizeof(Count)) ||
            Count <= 0 || Count > kMaxOwners)
        {
            return 0;
        }
        int Found = 0;
        for (int i = 0; i < Count; ++i)
        {
            uintptr_t Owner = 0, Vftable = 0;
            if (ReadPointer(Streamer + kStreamerOwners + i * sizeof(uintptr_t), Owner) &&
                ReadPointer(Owner, Vftable) && Vftable == s_base + kOwnerVftable)
            {
                Owners[Found++] = Owner;
            }
        }
        return Found;
    }

    uintptr_t CallNavFindMap(uintptr_t Manager, uint32_t Key)
    {
        __try
        {
            return s_nav_find_map((void*)Manager, Key);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    int32_t CallNavFindCell(uintptr_t NavMap, const float* Position)
    {
        __try
        {
            return s_nav_find_cell((void*)NavMap, Position, kNavSearchRadius, kNavSearchLimit, nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -2;
        }
    }

    // Game's thread: the nav cell of the focus, once its map's nav is in.
    int32_t ResolveFocusCell(uint32_t Map, const float* Position)
    {
        uintptr_t Owners[kMaxOwners] = {};
        const int Count = ReadOwners(Owners);
        int32_t Index = -1;
        for (int i = 0; i < Count; ++i)
        {
            uint32_t Id = 0;
            if (ReadBytes(Owners[i] + kOwnerMap, &Id, sizeof(Id)) && Id == Map)
            {
                ReadBytes(Owners[i] + kOwnerIndex, &Index, sizeof(Index));
                break;
            }
        }
        uintptr_t Context = 0, NavRoot = 0, Manager = 0;
        if (Index < 0 ||
            !ReadPointer(s_base + kContextOffset, Context) ||
            !ReadPointer(Context + kContextNav, NavRoot) ||
            !ReadPointer(NavRoot + kNavManager, Manager))
        {
            return -1;
        }
        const uintptr_t NavMap = CallNavFindMap(Manager, ((uint32_t)Index & 0x3f) << 24 | 0xffffff);
        return NavMap == 0 ? -1 : CallNavFindCell(NavMap, Position);
    }

    void StreamerUpdateHook(void* Streamer, float* Position, int32_t Cell, void* Part, uint8_t Flag)
    {
        const uint32_t Map = s_focus_map.load();
        if (Map != 0)
        {
            float Focus[4] = {};
            const uint32_t Generation = s_focus_generation.load();
            {
                std::scoped_lock Lock(s_focus_mutex);
                memcpy(Focus, s_focus_position, sizeof(Focus));
            }
            if (Generation != s_focus_seen_generation)
            {
                s_focus_seen_generation = Generation;
                s_focus_cell = -1;
                s_focus_tries = 0;
            }
            if (s_focus_cell < 0 && (s_focus_tries++ % 15) == 0)
            {
                s_focus_cell = ResolveFocusCell(Map, Focus);
                if (s_focus_cell >= 0 || s_focus_tries == 1)
                {
                    Append(StringFormat("%s  foco no mapa %08x em (%.3f, %.3f, %.3f): celula %d (tentativa %u)\n",
                        Clock().c_str(), Map, Focus[0], Focus[1], Focus[2], s_focus_cell, s_focus_tries));
                }
            }
            if (s_focus_cell >= 0)
            {
                s_original_streamer(Streamer, Focus, s_focus_cell, Part, Flag);
                return;
            }
        }
        s_original_streamer(Streamer, Position, Cell, Part, Flag);
    }

    void WriteStatus()
    {
        uintptr_t Owners[kMaxOwners] = {};
        const int Count = ReadOwners(Owners);
        uint32_t Asked[4] = {};
        for (int i = 0; i < 4; ++i)
        {
            Asked[i] = s_mask[i].load();
        }
        std::string Text = StringFormat("%s  === backread: %d mapas; pedido %08x mascara %s; foco %08x ===\n",
            Clock().c_str(), Count, s_map.load(), DescribeMask(Asked).c_str(), s_focus_map.load());
        for (int i = 0; i < Count; ++i)
        {
            uint32_t Map = 0, Masks[7][4] = {};
            uint8_t State[4] = {};
            if (ReadBytes(Owners[i] + kOwnerMap, &Map, sizeof(Map)) &&
                ReadBytes(Owners[i] + kOwnerMask, Masks, sizeof(Masks)) &&
                ReadBytes(Owners[i] + kOwnerState, State, sizeof(State)) &&
                (State[0] != 0 || State[1] != 0))
            {
                std::string Parts;
                for (int k = 0; k < 7; ++k)
                {
                    Parts += StringFormat(" +%02x=%s", 0x10 + k * 0x10, DescribeMask(Masks[k]).c_str());
                }
                Text += StringFormat("    [%d] mapa %08x estado %u forcado %u quer %u:%s\n", i, Map, State[0], State[1],
                    State[3], Parts.c_str());
            }
        }
        Append(Text);
    }

    void Apply(const std::string& Line)
    {
        std::istringstream Parts(Line);
        std::string Verb;
        Parts >> Verb;
        if (Verb == "load")
        {
            std::string MapText;
            Parts >> MapText;
            uint32_t Mask[4] = { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff };
            for (int i = 0; i < 4; ++i)
            {
                std::string Word;
                if (!(Parts >> Word))
                {
                    break;
                }
                Mask[i] = (uint32_t)strtoul(Word.c_str(), nullptr, 16);
            }
            const uint32_t Map = (uint32_t)strtoul(MapText.c_str(), nullptr, 16);
            DS2_Backread::Request(Map, Mask);
            Append(StringFormat("%s  === pedido: mapa %08x partes %s ===\n", Clock().c_str(), Map, DescribeMask(Mask).c_str()));
        }
        else if (Verb == "focus")
        {
            std::string MapText;
            float Position[3] = {};
            Parts >> MapText >> Position[0] >> Position[1] >> Position[2];
            const uint32_t Map = (uint32_t)strtoul(MapText.c_str(), nullptr, 16);
            DS2_Backread::Focus(Map, Position);
            Append(StringFormat("%s  === pedido: foco no mapa %08x em (%.3f, %.3f, %.3f) ===\n", Clock().c_str(), Map,
                Position[0], Position[1], Position[2]));
        }
        else if (Verb == "unfocus")
        {
            DS2_Backread::Unfocus();
            Append(StringFormat("%s  === pedido: sem foco ===\n", Clock().c_str()));
        }
        else if (Verb == "clear")
        {
            DS2_Backread::Release();
            Append(StringFormat("%s  === pedido: soltar ===\n", Clock().c_str()));
        }
        else if (Verb == "status")
        {
            WriteStatus();
        }
        else if (!Verb.empty())
        {
            Append(StringFormat("%s  pedido desconhecido: %s\n", Clock().c_str(), Line.c_str()));
        }
    }

    void Run()
    {
        while (s_running.load())
        {
            std::error_code Error;
            if (std::filesystem::exists(s_request_path, Error))
            {
                std::ifstream Stream(s_request_path);
                std::string Line;
                while (std::getline(Stream, Line))
                {
                    Apply(Line);
                }
                Stream.close();
                std::filesystem::remove(s_request_path, Error);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

#endif
}

void DS2_Backread::Request(uint32_t MapId, const uint32_t Mask[4])
{
#ifdef _WIN32
    const uint32_t Previous = s_map.load();
    for (int i = 0; i < 4; ++i)
    {
        s_mask[i].store(Mask[i]);
    }
    s_request_ms.store(GetTickCount64());
    s_map.store(MapId);
    if (Previous != 0 && Previous != MapId)
    {
        s_released_map.store(Previous);
    }
#endif
}

void DS2_Backread::Release()
{
#ifdef _WIN32
    const uint32_t Previous = s_map.exchange(0);
    if (Previous != 0)
    {
        s_released_map.store(Previous);
    }
#endif
}

void DS2_Backread::Focus(uint32_t MapId, const float Position[3])
{
#ifdef _WIN32
    {
        std::scoped_lock Lock(s_focus_mutex);
        s_focus_position[0] = Position[0];
        s_focus_position[1] = Position[1];
        s_focus_position[2] = Position[2];
        s_focus_position[3] = 1.0f;
    }
    s_focus_generation.fetch_add(1);
    s_focus_map.store(MapId);
#endif
}

void DS2_Backread::Unfocus()
{
#ifdef _WIN32
    s_focus_map.store(0);
#endif
}

bool DS2_Backread::Query(uint32_t MapId, uint8_t& State, uint32_t Mask[4])
{
#ifdef _WIN32
    uintptr_t Owners[kMaxOwners] = {};
    const int Count = ReadOwners(Owners);
    for (int i = 0; i < Count; ++i)
    {
        uint32_t Map = 0;
        if (ReadBytes(Owners[i] + kOwnerMap, &Map, sizeof(Map)) && Map == MapId)
        {
            return ReadBytes(Owners[i] + kOwnerState, &State, 1) && ReadBytes(Owners[i] + kOwnerMask, Mask, 4 * sizeof(uint32_t));
        }
    }
#endif
    return false;
}

bool DS2_BackreadHook::Install(Injector& injector)
{
#ifdef _WIN32
    s_base = (uintptr_t)injector.GetBaseAddress();

    if (!BytesMatch(s_base + kOwnerUpdateOffset, kOwnerUpdateBytes, sizeof(kOwnerUpdateBytes)) ||
        !BytesMatch(s_base + kOwnerStatesOffset, kOwnerStatesBytes, sizeof(kOwnerStatesBytes)) ||
        !BytesMatch(s_base + kStreamerUpdateOffset, kStreamerUpdateBytes, sizeof(kStreamerUpdateBytes)) ||
        !BytesMatch(s_base + kNavFindMapOffset, kNavFindMapBytes, sizeof(kNavFindMapBytes)) ||
        !BytesMatch(s_base + kNavFindCellOffset, kNavFindCellBytes, sizeof(kNavFindCellBytes)))
    {
        Error("[DS2_BackreadHook] a atualizacao do dono do mapa nao e a esperada; recusando");
        return false;
    }

    s_log_path = injector.GetDllPath() / "DS2_Backread.log";
    s_request_path = injector.GetDllPath() / "DS2_Backread.req";
    s_original_update = (OwnerUpdate_p)(s_base + kOwnerUpdateOffset);
    s_original_streamer = (StreamerUpdate_p)(s_base + kStreamerUpdateOffset);
    s_nav_find_map = (NavFindMap_p)(s_base + kNavFindMapOffset);
    s_nav_find_cell = (NavFindCell_p)(s_base + kNavFindCellOffset);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original_update, OwnerUpdateHook);
    DetourAttach(&(PVOID&)s_original_streamer, StreamerUpdateHook);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        Error("[DS2_BackreadHook] nao consegui instalar o detour");
        return false;
    }

    s_running.store(true);
    s_thread = std::thread(Run);

    Append(StringFormat("%s  === ds2os backread: pronto ===\n", Clock().c_str()));
    Log("[DS2_BackreadHook] pronto; escreva load <mapa> em DS2_Backread.req");
#endif
    return true;
}

void DS2_BackreadHook::Uninstall()
{
#ifdef _WIN32
    s_running.store(false);
    if (s_thread.joinable())
    {
        s_thread.join();
    }

    if (s_original_update != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original_update, OwnerUpdateHook);
        DetourDetach(&(PVOID&)s_original_streamer, StreamerUpdateHook);
        DetourTransactionCommit();
        s_original_update = nullptr;
    }
#endif
}

const char* DS2_BackreadHook::GetName()
{
    return "DS2 Backread";
}
