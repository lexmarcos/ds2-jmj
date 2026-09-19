/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_BackreadHook.h"
#include "Injector/Hooks/DarkSouls2/DS2_TravelWatchHook.h"
#include "Injector/Hooks/DarkSouls2/DS2_BonfireInSessionHook.h"
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
#include <map>
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
    //
    // **+0x50 and +0x60 are not written any more.** They are the visibility
    // pair, and the part activation state machine (FUN_1403f32f0) owns them:
    // it carries a count in the top eleven bits of `[*(obj+0x40)+0x10]`, moves
    // it by 0x20 at a time, and guards each move with a bit at +0x48. Setting
    // visibility bits behind it makes that count go the wrong way -
    // `((n >> 5) - 1) * 0x20` on a zero wraps - and objects are then freed or
    // kept against what still points at them. That is the shape of every
    // remaining death on 17/09: a pointer whose low half is zeroed and whose
    // high half holds something else, on the world update, on the guest, after
    // a travel, in a different function each time.
    constexpr size_t kOwnerMasks[] = { 0x10, 0x20, 0x30, 0x40 };
    // The streaming cap. In FUN_1403cc450's state 0, the only way out:
    //
    //   +0x3cc4f0  2b d8        sub  %eax,%ebx      ; owners - owners in state 0
    //   +0x3cc4f2  83 fb 01     cmp  $0x1,%ebx      ; <- the cap
    //   +0x3cc4fa  7f 1d        jg   refuse
    //
    // so at most two maps are ever loading, loaded or unloading, and a third
    // forced owner sits in state 0 for good (docs/research/streaming-budget.md).
    // The travel needs the session's map held for the whole session (see
    // DS2_BonfireInSession_IsSessionMap), the map it stands on and the
    // destination: three. Measured 18/09 with the byte poked: Iron Keep, which
    // never left state 0 with Majula held, loaded at once, and the map heap
    // read 57-60% of its 13.5 MiB. Neighbours the game streams in by itself
    // would make a fourth; they are evicted while a travel waits.
    constexpr size_t kStreamCapOffset = 0x3cc4f0;
    constexpr uint8_t kStreamCapExpected[] = { 0x2b, 0xd8, 0x83, 0xfb, 0x01, 0x48, 0x8b, 0x5c, 0x24, 0x30, 0x7f, 0x1d };
    constexpr size_t kStreamCapByte = 4;
    // Three maps, not four. Measured 18/09 on the real Brume Tower: the fourth
    // map (Majula held, Iron Keep, Iron Keep's neighbour, Brume arriving) hit a
    // null allocation at +0x1bee1c4 inside the load - a fixed global pool, not
    // the 13.5 MiB map heap - the trap caught it and the host died a frame
    // later at +0x1d8a02. The same null write came with four maps on the Heide
    // leg of the 32-leg run. Three always fit; the travel makes room for its
    // destination by evicting the neighbours the game streamed in by itself
    // (StreamerMasksHook).
    constexpr uint8_t kStreamCap = 0x02;
    bool s_cap_raised = false;

    // FUN_1403dc930, `void(streamer, arg)`, once a frame from FUN_1403dc3e0
    // before the owners update: turns the streamer's reach into each owner's
    // masks and "wanted" byte (+0x1ea). Every mask source checks the per-map
    // "allowed" byte at streamer+0x188[i], which FUN_1403dc3e0 rebuilds at the
    // top of every frame; a zero there makes map i look exactly like one the
    // search does not reach, and the game takes it down on its own path
    // (5 -> 6 -> 7 -> 0), the one it uses when the player walks away
    // (docs/research/streaming-budget.md, 2.2 and 2.3).
    constexpr size_t kStreamerMasksOffset = 0x3dc930;
    constexpr uint8_t kStreamerMasksBytes[] = { 0x40, 0x55, 0x53, 0x57, 0x48, 0x8d, 0xac, 0x24, 0x20, 0xfb, 0xff, 0xff };
    constexpr size_t kStreamerAllowed = 0x188;         // 42 bytes, one per owner
    constexpr size_t kStreamerPlayerMap = 0x30;        // int, the owner index under the player
    constexpr ULONGLONG kEvictWindowMs = 15000;

    // FUN_1403cc3a0, `bool(owner)`: the whole teardown of one map, run inside
    // one call (it loops FUN_1403cb1a0 until the step counter is spent).
    //
    // The teardown stops the effects the map's entities hold (component
    // finalize FUN_1403f3fc0 -> FUN_1404c83f0: a soft stop, then the handles
    // are unlinked) but the stopped trees keep ticking with nobody holding
    // them, reading what the map left behind. Leaving Brume Tower that way
    // killed the host in FUN_140fd8570 about 0.4 s later, one exit in four,
    // on 18/09; in the unmodded game Brume Tower is only left through a
    // loading screen, which clears every effect first.
    //
    // Clearing every effect the same way (SfxFxManagerBase slot +0x50) held
    // for 60+ legs but took the arrival map's own effects for good: a census
    // on 19/09 at Heide counted 71 live trees before the clear and 14 after,
    // the bonfire flames (ids 3020/13020, one per bonfire) and the torches
    // among the missing, because their spawners never spawn them again.
    //
    // So now only what this teardown orphaned goes: the live top nodes with
    // no handle left (node+0xf8) after the teardown that had one before it,
    // killed the way FUN_140a09d50 kills a tree (each child through
    // FUN_140a09cf0 and FUN_140a09860, then the top node through
    // FUN_140a09860). Every effect of the other maps is still held by its
    // own entity (every tree of the census was) and is not touched. Only DLC
    // maps (0x32xxxxxx) have crashed, and only their teardown does this.
    constexpr size_t kTeardownOffset = 0x3cc3a0;
    constexpr uint8_t kTeardownBytes[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9 };
    constexpr size_t kOwnerMapId = 0x08;
    constexpr size_t kContextSfxSystem = 0xbc8;
    constexpr size_t kSfxManagerBase = 0x10;
    constexpr size_t kFxRoots = 0x10;          // FXManager: first root; root +8 next, +0x10 top node
    constexpr size_t kFxRootId = 0x34;
    constexpr size_t kFxNodeFlags = 0x58;      // bit 30 alive
    constexpr size_t kFxNodeChild = 0x90;
    constexpr size_t kFxNodeSibling = 0x88;
    constexpr size_t kFxNodeHandles = 0xf8;
    constexpr size_t kFxKillSubtreeOffset = 0xa09cf0;
    constexpr uint8_t kFxKillSubtreeBytes[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x56, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x9a, 0x90, 0x00, 0x00, 0x00 };
    constexpr size_t kFxKillNodeOffset = 0xa09860;
    constexpr uint8_t kFxKillNodeBytes[] = { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0x0b, 0x02, 0x00, 0x00, 0x55, 0x56, 0x48, 0x83, 0xec, 0x28 };
    constexpr size_t kMaxOrphans = 512;
    using Teardown_p = bool(*)(void* Owner);
    using FxKill_p = void(*)(uintptr_t Manager, uintptr_t Node);
    Teardown_p s_original_teardown = nullptr;
    FxKill_p s_fx_kill_subtree = nullptr;
    FxKill_p s_fx_kill_node = nullptr;

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
    constexpr size_t kOwnerIndexField = 0x0c;
    // A MapEntity: its kind byte and the owner of the map it belongs to.
    constexpr size_t kEntityKind = 0xa2;
    constexpr uint8_t kEntityPart = 2;
    constexpr size_t kPartOwner = 0x28;
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
    alignas(16) float s_focus_position[4] = {};
    // Touched only from the game's thread.
    uint32_t s_focus_seen_generation = 0;
    int32_t s_focus_cell = -1;
    uint32_t s_focus_tries = 0;

    uintptr_t s_base = 0;

    std::atomic<uint32_t> s_map{ 0 };
    std::atomic<uint32_t> s_mask[4];

    // Maps kept for a while, by map index: the maps where other players stand,
    // with the parts around them. Measured 14/09: a guest's respawn in another
    // map unloaded the map the host was standing in on the guest's machine,
    // and the guest's game closed a moment later.
    constexpr int kMaxKept = 8;
    struct Kept
    {
        int32_t Index = -1;
        ULONGLONG Until = 0;
        bool Forced = false;
        uint32_t Mask[4] = {};
    };
    std::mutex s_keep_mutex;
    Kept s_kept[kMaxKept];
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
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
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
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
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

    // The whole life of a map runs through one call: FUN_1403cc3f0, the
    // owner's per-frame update, whose state machine loads the map, streams its
    // parts and - at the end - takes it all apart. Every crash of 15 and 16/09
    // that was not caught elsewhere came from inside that teardown, at a
    // different address each time (+0x3d8782, +0x3c1bf8, +0x40d2c7, +0x3f4230,
    // +0x3ece30): chasing them one by one only moved the address. One guard
    // over the whole call covers all of them, and the cost of a fault caught
    // here is a map that is half taken apart - memory this session will not get
    // back - instead of a game that closes and costs the guest ten
    // illegal-disconnect points.
    //
    // No C++ objects live in here: __try cannot sit in a function that unwinds.
    bool GuardedOwnerUpdate(OwnerUpdate_p Fn, void* Owner, void* Arg)
    {
        __try
        {
            Fn(Owner, Arg);
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    std::atomic<uint64_t> s_caught_update{ 0 };
    std::atomic<uint64_t> s_not_owner{ 0 };

    // Every part bit this hook has ever turned on, per map index, and when a
    // release of that map began.
    //
    // The bits were only ever OR'd and never taken back, so a map was let go
    // with all of its parts still asked for. The owner's state machine then
    // tore it down without ever deactivating them, and whatever the parts had
    // registered themselves in - the notify list at +0x38, the component list
    // at +0x18, the array at +0x10 of the world update - kept pointing at
    // blocks the allocator had already handed to somebody else. That is every
    // remaining death of 17 and 18/09, in a different container each time.
    //
    // So a release is two steps now: take our bits back first, let the game
    // see the smaller mask for a few frames and deactivate what it no longer
    // needs, and only then drop the force byte.
    struct Added
    {
        uint32_t Bits[4] = {};
        ULONGLONG ReleasingAt = 0;
        bool Releasing = false;
    };
    std::mutex s_added_mutex;
    Added s_added[0x40];
    constexpr ULONGLONG kLetGoMs = 700;
    std::atomic<bool> s_session_map_held{ false };
    // The session's map this hook refused to let go, and since when it has
    // not been one. Measured 18/09: after a legal session end the host kept
    // Majula forced for good - a map slot and its memory held for nothing.
    // It goes once it has not been the session's map for five seconds; the
    // delay rides over the moment a warp inside a session puts the enemy sync
    // back to state 0 before it binds again.
    std::atomic<uint32_t> s_held_session_map{ 0 };
    ULONGLONG s_held_not_session_since = 0;
    constexpr ULONGLONG kHeldGraceMs = 5000;

    void RememberAdded(int32_t Index, const uint32_t Bits[4])
    {
        if (Index < 0 || Index > 0x3f)
        {
            return;
        }
        std::scoped_lock Lock(s_added_mutex);
        for (int i = 0; i < 4; ++i)
        {
            s_added[Index].Bits[i] |= Bits[i];
        }
    }

    // Step one of a release: take back every part bit this hook turned on and
    // start the clock. True the first time, so the caller says it once.
    bool BeginLetGo(uintptr_t Owner, uint32_t Map, const char* Why)
    {
        // The game's own rule, kept: in vanilla the fog that rises when a
        // phantom joins fences the session's area, so the map the session
        // began in never unloads while the session lives, and everything the
        // join binds - the enemy sync's records, the enemy generator table -
        // counts on it (docs/research/phantom-map-border.md,
        // object-table-lifecycle.md). Releasing it is what every guest crash
        // of 17 and 18/09 came from. It stays forced; the raised cap leaves
        // room for the travel around it.
        if (DS2_BonfireInSession_IsSessionMap(Map))
        {
            s_held_session_map.store(Map);
            s_held_not_session_since = 0;
            if (!s_session_map_held.exchange(true))
            {
                Append(StringFormat("%s  mapa %08x %s, but it is the session's map; kept loaded for the session\n",
                    Clock().c_str(), Map, Why));
            }
            return false;
        }

        int32_t Index = -1;
        if (!ReadBytes(Owner + kOwnerIndexField, &Index, sizeof(Index)) || Index < 0 || Index > 0x3f)
        {
            // No index to remember bits against: the old behaviour, at once.
            DS2_BonfireInSession_ForgetSyncedMap(Map);
            const uint8_t Zero = 0;
            WriteBytes(Owner + kOwnerForced, &Zero, 1);
            Append(StringFormat("%s  mapa %08x %s; solto sem indice\n", Clock().c_str(), Map, Why));
            return true;
        }

        // Before anything of this map goes: the object sync may still be bound
        // to it, and would keep writing into its memory.
        DS2_BonfireInSession_ForgetSyncedMap(Map);

        uint32_t Bits[4] = {};
        {
            std::scoped_lock Lock(s_added_mutex);
            if (s_added[Index].Releasing)
            {
                return false;
            }
            memcpy(Bits, s_added[Index].Bits, sizeof(Bits));
            s_added[Index].Releasing = true;
            s_added[Index].ReleasingAt = GetTickCount64();
            memset(s_added[Index].Bits, 0, sizeof(s_added[Index].Bits));
        }

        uint32_t Cleared = 0;
        if ((Bits[0] | Bits[1] | Bits[2] | Bits[3]) != 0)
        {
            for (const size_t At : kOwnerMasks)
            {
                uint32_t Mask[4] = {};
                if (!ReadBytes(Owner + At, Mask, sizeof(Mask)))
                {
                    continue;
                }
                bool Any = false;
                for (int i = 0; i < 4; ++i)
                {
                    const uint32_t Next = Mask[i] & ~Bits[i];
                    if (Next != Mask[i])
                    {
                        Mask[i] = Next;
                        Any = true;
                    }
                }
                if (Any)
                {
                    WriteBytes(Owner + At, Mask, sizeof(Mask));
                    ++Cleared;
                }
            }
        }
        Append(StringFormat("%s  mapa %08x %s; devolvi as partes que eu tinha pedido (%u bloco(s)) e solto em %llu ms\n",
            Clock().c_str(), Map, Why, Cleared, (unsigned long long)kLetGoMs));
        return true;
    }

    // Step two, a few frames later: the force byte goes.
    void FinishLetGo(uintptr_t Owner, uint32_t Map)
    {
        int32_t Index = -1;
        if (!ReadBytes(Owner + kOwnerIndexField, &Index, sizeof(Index)) || Index < 0 || Index > 0x3f)
        {
            return;
        }
        {
            std::scoped_lock Lock(s_added_mutex);
            if (!s_added[Index].Releasing || GetTickCount64() - s_added[Index].ReleasingAt < kLetGoMs)
            {
                return;
            }
            s_added[Index].Releasing = false;
        }
        const uint8_t Zero = 0;
        WriteBytes(Owner + kOwnerForced, &Zero, 1);
        Append(StringFormat("%s  mapa %08x solto\n", Clock().c_str(), Map));
    }

    void Force(uintptr_t Owner)
    {
        const uint8_t One = 1;
        WriteBytes(Owner + kOwnerForced, &One, 1);

        {
            uint32_t Mine[4] = {};
            int32_t Index = -1;
            for (int i = 0; i < 4; ++i)
            {
                Mine[i] = s_mask[i].load();
            }
            if (ReadBytes(Owner + kOwnerIndexField, &Index, sizeof(Index)))
            {
                RememberAdded(Index, Mine);
            }
        }

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

    // Game's thread: is this owner's map kept, with which parts, and should its
    // force byte go?
    enum class KeepVerdict { None, Keep, Drop };
    KeepVerdict CheckKept(uintptr_t Owner, uint32_t Mask[4])
    {
        int32_t Index = -1;
        if (!ReadBytes(Owner + kOwnerIndexField, &Index, sizeof(Index)) || Index < 0)
        {
            return KeepVerdict::None;
        }
        const ULONGLONG Now = GetTickCount64();
        std::scoped_lock Lock(s_keep_mutex);
        for (Kept& Entry : s_kept)
        {
            if (Entry.Index != Index)
            {
                continue;
            }
            if (Now < Entry.Until)
            {
                Entry.Forced = true;
                memcpy(Mask, Entry.Mask, sizeof(Entry.Mask));
                return KeepVerdict::Keep;
            }
            const bool WasForced = Entry.Forced;
            Entry = Kept();
            return WasForced ? KeepVerdict::Drop : KeepVerdict::None;
        }
        return KeepVerdict::None;
    }

    // Two fixed tables every loaded map draws from, and the game ends the
    // process ("out of memory." in DLFixedVector.inl, the trap at
    // +0x1bee1c4) when either is full. Measured and read 19/09 after the host
    // died with Majula held, Frozen Eleum Loyce loaded and 0a170000 being
    // built: the TargetManager (*(ctx+0x48), 2048 entries, count at +0x8018)
    // takes one entry per enemy generator, character and targetable map
    // object of every loaded map; the chameleon areas (*(mapmgr+0x208), 3
    // entries, count at +0x78) one per map with chameleon data. Written down
    // on every owner state change and once the count settles, so each map's
    // cost and whether a release gives it back can be read from the log.
    constexpr size_t kTargetManager = 0x48;
    constexpr size_t kTargetCount = 0x8018;
    constexpr size_t kTargetVftable = 0x10ed868;
    constexpr uint64_t kTargetCapacity = 0x800;
    constexpr size_t kChameleon = 0x208;
    constexpr size_t kChameleonCount = 0x78;
    constexpr uint64_t kChameleonCapacity = 3;
    constexpr ULONGLONG kTargetSettleMs = 1500;
    // Keep this much free for what spawns later (characters, summons).
    constexpr uint64_t kTargetMargin = 150;
    // A map never measured is assumed as heavy as the heaviest one seen,
    // Frozen Eleum Loyce (1392).
    constexpr uint32_t kUnknownCost = 1400;
    // Measured 19/09 (docs/DS2_NATIVE_TRAVEL_PLAN.md, section 15).
    const std::pair<uint32_t, uint32_t> kSeedCosts[] = {
        { 0x0a040000, 313 }, { 0x0a1f0000, 281 }, { 0x0a130000, 464 }, { 0x0a110000, 875 },
        { 0x32240000, 1126 }, { 0x0a220000, 232 }, { 0x0a170000, 794 }, { 0x140b0000, 446 },
        { 0x32250000, 1392 },
    };
    std::mutex s_cost_mutex;
    std::map<uint32_t, uint32_t> s_costs;
    std::filesystem::path s_costs_path;
    uint64_t s_cost_base[64] = {};
    uint64_t s_cost_pending = 0;
    std::atomic<int32_t> s_unload_index{ -1 };
    std::atomic<uint64_t> s_unload_since{ 0 };
    ULONGLONG s_unload_logged = 0;
    int32_t s_unload_let_go = -1;
    constexpr ULONGLONG kUnloadWindowMs = 32000;

    void LoadCosts()
    {
        std::scoped_lock Lock(s_cost_mutex);
        for (const auto& Seed : kSeedCosts)
        {
            s_costs[Seed.first] = Seed.second;
        }
        std::ifstream Stream(s_costs_path);
        std::string Line;
        while (std::getline(Stream, Line))
        {
            unsigned Map = 0, Cost = 0;
            if (sscanf_s(Line.c_str(), "%x %u", &Map, &Cost) == 2 && Map != 0 && Cost > 0 && Cost < 0x800)
            {
                s_costs[Map] = Cost;
            }
        }
    }

    void LearnCost(uint32_t Map, uint32_t Cost)
    {
        {
            std::scoped_lock Lock(s_cost_mutex);
            s_costs[Map] = Cost;
        }
        std::ofstream Stream(s_costs_path, std::ios::app);
        Stream << StringFormat("%08x %u\n", Map, Cost);
    }

    uint8_t s_owner_states[64] = {};
    bool s_owner_states_known[64] = {};
    uint64_t s_targets_logged = ~0ull;
    uint64_t s_targets_last = ~0ull;
    ULONGLONG s_targets_changed_at = 0;
    ULONGLONG s_targets_sampled_at = 0;

    // False when either table cannot be read.
    bool ReadBudget(uint64_t& Targets, uint64_t& Chameleon)
    {
        uintptr_t Context = 0, Manager = 0, Vftable = 0, Maps = 0, Areas = 0;
        Targets = 0;
        Chameleon = 0;
        return ReadPointer(s_base + kContextOffset, Context) && Context != 0 &&
            ReadPointer(Context + kTargetManager, Manager) && Manager != 0 &&
            ReadPointer(Manager, Vftable) && Vftable == s_base + kTargetVftable &&
            ReadBytes(Manager + kTargetCount, &Targets, sizeof(Targets)) &&
            ReadPointer(Context + kMapManager, Maps) && Maps != 0 &&
            ReadPointer(Maps + kChameleon, Areas) && Areas != 0 &&
            ReadBytes(Areas + kChameleonCount, &Chameleon, sizeof(Chameleon));
    }

    std::string DescribeBudget()
    {
        uint64_t Targets = 0, Chameleon = 0;
        if (!ReadBudget(Targets, Chameleon))
        {
            return "targets ?, chameleon ?";
        }
        return StringFormat("targets %llu/%llu, chameleon %llu/%llu", (unsigned long long)Targets,
            (unsigned long long)kTargetCapacity, (unsigned long long)Chameleon, (unsigned long long)kChameleonCapacity);
    }

    // Every owner's state change, with the tables beside it.
    void WatchOwnerState(uintptr_t Owner, uint32_t Map)
    {
        int32_t Index = -1;
        uint8_t State = 0;
        if (!ReadBytes(Owner + kOwnerIndexField, &Index, sizeof(Index)) || Index < 0 || Index > 63 ||
            !ReadBytes(Owner + kOwnerState, &State, 1))
        {
            return;
        }
        if (s_owner_states_known[Index] && s_owner_states[Index] == State)
        {
            return;
        }
        const uint8_t Before = s_owner_states_known[Index] ? s_owner_states[Index] : 0xff;
        s_owner_states[Index] = State;
        s_owner_states_known[Index] = true;
        // A map's cost: the targets it adds between starting to build (4) and
        // the count settling with it loaded.
        uint64_t Now = 0, Chameleon = 0;
        if (State == 4 && ReadBudget(Now, Chameleon))
        {
            s_cost_base[Index] = Now;
            s_cost_pending |= 1ull << Index;
        }
        else if (State == 0)
        {
            s_cost_pending &= ~(1ull << Index);
        }
        Append(StringFormat("%s  budget: map %08x [%d] state %u -> %u; %s\n", Clock().c_str(), Map, Index,
            (unsigned)Before, (unsigned)State, DescribeBudget().c_str()));
    }

    int ReadOwners(uintptr_t Owners[kMaxOwners]);

    // The target count once it has stopped moving, with the maps it serves.
    void WatchTargets()
    {
        const ULONGLONG Now = GetTickCount64();
        if (Now - s_targets_sampled_at < 100)
        {
            return;
        }
        s_targets_sampled_at = Now;
        uint64_t Targets = 0, Chameleon = 0;
        if (!ReadBudget(Targets, Chameleon))
        {
            return;
        }
        if (Targets != s_targets_last)
        {
            s_targets_last = Targets;
            s_targets_changed_at = Now;
            return;
        }
        if (Targets == s_targets_logged || Now - s_targets_changed_at < kTargetSettleMs)
        {
            return;
        }
        std::string Loaded;
        uintptr_t Owners[kMaxOwners] = {};
        const int Count = ReadOwners(Owners);
        for (int i = 0; i < Count; ++i)
        {
            uint32_t Map = 0;
            uint8_t State = 0;
            if (ReadBytes(Owners[i] + kOwnerMap, &Map, 4) && ReadBytes(Owners[i] + kOwnerState, &State, 1) && State != 0)
            {
                Loaded += StringFormat(" %08x:%u", Map, (unsigned)State);
            }
        }
        const long long Delta = s_targets_logged == ~0ull ? 0 : (long long)Targets - (long long)s_targets_logged;
        s_targets_logged = Targets;
        // Learned only when exactly one map was building since the last time.
        if (s_cost_pending != 0 && (s_cost_pending & (s_cost_pending - 1)) == 0)
        {
            int Index = 0;
            while (((s_cost_pending >> Index) & 1) == 0)
            {
                ++Index;
            }
            const uint32_t Map = DS2_Backread::MapAt(Index);
            if (Map != 0 && s_owner_states[Index] == 5 && Targets > s_cost_base[Index] &&
                Targets - s_cost_base[Index] < kTargetCapacity)
            {
                const uint32_t Cost = (uint32_t)(Targets - s_cost_base[Index]);
                LearnCost(Map, Cost);
                Append(StringFormat("%s  budget: map %08x costs %u targets\n", Clock().c_str(), Map, Cost));
            }
        }
        s_cost_pending = 0;
        Append(StringFormat("%s  budget: targets settled at %llu (%+lld), chameleon %llu; maps in:%s\n", Clock().c_str(),
            (unsigned long long)Targets, Delta, (unsigned long long)Chameleon, Loaded.empty() ? " none" : Loaded.c_str()));
    }

    void OwnerUpdateHook(void* Owner, void* Arg)
    {
        // Proof, not assumption, before anything is written. Everything below
        // writes into this object at fixed offsets: the force byte at +0x1e9
        // and **six blocks of sixteen bytes** of parts mask from +0x10 to
        // +0x60. If it is ever not a MapAreaCtrlOwner, that is 96 bytes of
        // part bits OR'd through the middle of something else - and a mask
        // OR'd over the top half of a live pointer is precisely the corruption
        // the guest and the host have been dying of: `00000001410e86d8`
        // becoming `00b54001410e86d8` is `0x00000001 | 0x00b54000`, exactly a
        // mask landing in the second half of the block at +0x50. The loop that
        // reads these owners from the streamer has always checked the vftable;
        // this one, which writes, never did (fixed 16/09).
        uintptr_t Vftable = 0;
        const bool IsOwner = Owner != nullptr && ReadPointer((uintptr_t)Owner, Vftable) &&
            Vftable == s_base + kOwnerVftable;
        if (!IsOwner)
        {
            if (s_not_owner.fetch_add(1) < 20)
            {
                Append(StringFormat("%s  ATENCAO: o ciclo do mapa foi chamado com %p, que nao e um MapAreaCtrlOwner (vftable +0x%zx); nao escrevo nada nele\n",
                    Clock().c_str(), Owner, Vftable >= s_base ? (size_t)(Vftable - s_base) : (size_t)0));
            }
            GuardedOwnerUpdate(s_original_update, Owner, Arg);
            return;
        }

        uint32_t Map = 0;
        const bool HaveMap = Owner != nullptr && ReadBytes((uintptr_t)Owner + kOwnerMap, &Map, sizeof(Map)) && Map != 0;
        uint32_t KeptMask[4] = {};
        const KeepVerdict Verdict = HaveMap ? CheckKept((uintptr_t)Owner, KeptMask) : KeepVerdict::None;
        if (HaveMap)
        {
            // A release that began a few frames ago finishes here.
            FinishLetGo((uintptr_t)Owner, Map);

            // The session's map, held while the session lived, goes once the
            // session is over.
            if (Map == s_held_session_map.load())
            {
                if (DS2_BonfireInSession_IsSessionMap(Map))
                {
                    s_held_not_session_since = 0;
                }
                else if (s_held_not_session_since == 0)
                {
                    s_held_not_session_since = GetTickCount64();
                }
                else if (GetTickCount64() - s_held_not_session_since >= kHeldGraceMs)
                {
                    s_held_session_map.store(0);
                    s_held_not_session_since = 0;
                    s_session_map_held.store(false);
                    Append(StringFormat("%s  mapa %08x is no longer the session's map; letting it go\n", Clock().c_str(), Map));
                    BeginLetGo((uintptr_t)Owner, Map, "the session ended");
                }
            }

            if (Verdict == KeepVerdict::Keep)
            {
                // The force byte and the parts this keep asks for. Both are
                // needed: a forced owner with an empty mask never leaves state
                // 0, which is how a version of 16/09 left the guest unable to
                // travel at all - the host went first, the host's copy forced
                // the destination on the guest's machine with no parts, and
                // the guest's own request then found an owner that would never
                // load.
                const uint8_t One = 1;
                WriteBytes((uintptr_t)Owner + kOwnerForced, &One, 1);
                // Nothing to add means nothing written: a keep with no parts
                // of its own holds the map exactly as the game loaded it.
                const bool Any = (KeptMask[0] | KeptMask[1] | KeptMask[2] | KeptMask[3]) != 0;
                if (Any)
                {
                    int32_t Index = -1;
                    if (ReadBytes((uintptr_t)Owner + kOwnerIndexField, &Index, sizeof(Index)))
                    {
                        RememberAdded(Index, KeptMask);
                    }
                    for (const size_t At : kOwnerMasks)
                    {
                        uint32_t Mask[4] = {};
                        if (ReadBytes((uintptr_t)Owner + At, Mask, sizeof(Mask)))
                        {
                            for (int i = 0; i < 4; ++i)
                            {
                                Mask[i] |= KeptMask[i];
                            }
                            WriteBytes((uintptr_t)Owner + At, Mask, sizeof(Mask));
                        }
                    }
                }
            }
            else if (Verdict == KeepVerdict::Drop && Map != s_map.load())
            {
                // The character sync goes idle **before** the map goes: every
                // guest death at +0x517843 on 17/09 came 167-173 ms after one
                // of these two lines, four of four, on the net thread, with
                // the sync object in r13 and this map's id in rdx. Idling it
                // at the end of the travel (the earlier fix) was too early -
                // this release comes thirty seconds later, and by then the
                // sync had been rebuilt and filled again.
                // The sync is not put back to state 0 here any more: that
                // rebuilds it against freed memory. BeginLetGo drops its
                // records instead (DS2_BonfireInSession_ForgetSyncedMap).
                if (BeginLetGo((uintptr_t)Owner, Map, "nao e mais de ninguem"))
                {
                    DS2_TravelWatch::Open(15000, "mapa solto: nao e mais de ninguem");
                }
            }

            if (Map == s_map.load())
            {
                Force((uintptr_t)Owner);
            }
            else if (Map == s_released_map.load())
            {
                // Not from under another player: its keep holds the byte.
                const bool StillKept = Verdict == KeepVerdict::Keep;
                if (!StillKept)
                {
                    BeginLetGo((uintptr_t)Owner, Map, "a pedido");
                }
                s_released_map.store(0);
                if (StillKept)
                {
                    Append(StringFormat("%s  mapa %08x solto, mas segue mantido por outro jogador\n", Clock().c_str(), Map));
                }
            }
        }

        if (!GuardedOwnerUpdate(s_original_update, Owner, Arg))
        {
            const uint64_t Count = s_caught_update.fetch_add(1);
            if (Count < 60)
            {
                Append(StringFormat("%s  FALHA APARADA no ciclo do mapa %08x (dono %p); o mapa fica pela metade em vez de o jogo fechar\n",
                    Clock().c_str(), Map, Owner));
            }
        }

        if (HaveMap)
        {
            WatchOwnerState((uintptr_t)Owner, Map);
        }
        WatchTargets();

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
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
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
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
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
                ReadBytes(Owners[i] + kOwnerIndexField, &Index, sizeof(Index));
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

    // Does this entity belong to that map? Everything is proven before it is
    // believed: the kind byte, the owner's vftable, then the owner's map id.
    bool PartOfMap(void* Part, uint32_t Map)
    {
        uint8_t Kind = 0;
        uintptr_t Owner = 0, Vftable = 0;
        uint32_t Mine = 0;
        return Part != nullptr && Map != 0 &&
            ReadBytes((uintptr_t)Part + kEntityKind, &Kind, 1) && Kind == kEntityPart &&
            ReadPointer((uintptr_t)Part + kPartOwner, Owner) && Owner != 0 &&
            ReadPointer(Owner, Vftable) && Vftable == s_base + kOwnerVftable &&
            ReadBytes(Owner + kOwnerMap, &Mine, sizeof(Mine)) && Mine == Map;
    }

    using StreamerMasks_p = void(*)(void*, void*);
    StreamerMasks_p s_original_masks = nullptr;
    uint64_t s_evict_request_seen = 0;
    uint64_t s_evict_logged = 0;           // bit per owner index, per request

    // While a travel waits for its destination, the maps the game streamed in
    // by itself - loaded or loading, not forced, not under the player, not the
    // destination - are not allowed this frame. The held session's map, the
    // map kept for the other player and the origin kept by the travel are all
    // forced, so they are never touched.
    void EvictNeighbours(uintptr_t Streamer)
    {
        const uint32_t Destination = s_map.load();
        const uint64_t Requested = s_request_ms.load();
        if (Destination == 0 || Requested == 0 || GetTickCount64() - Requested > kEvictWindowMs)
        {
            return;
        }
        if (Requested != s_evict_request_seen)
        {
            s_evict_request_seen = Requested;
            s_evict_logged = 0;
        }
        int16_t Count = 0;
        int32_t PlayerIndex = -1;
        if (!ReadBytes(Streamer + kStreamerOwnerCount, &Count, sizeof(Count)) || Count <= 0 || Count > kMaxOwners)
        {
            return;
        }
        ReadBytes(Streamer + kStreamerPlayerMap, &PlayerIndex, sizeof(PlayerIndex));

        // Nothing to make room for once the destination is in.
        for (int i = 0; i < Count; ++i)
        {
            uintptr_t Owner = 0, Vftable = 0;
            uint32_t Map = 0;
            uint8_t State = 0;
            if (ReadPointer(Streamer + kStreamerOwners + i * sizeof(uintptr_t), Owner) && ReadPointer(Owner, Vftable) &&
                Vftable == s_base + kOwnerVftable && ReadBytes(Owner + kOwnerMap, &Map, sizeof(Map)) &&
                Map == Destination && ReadBytes(Owner + kOwnerState, &State, 1) && State == 5)
            {
                return;
            }
        }

        for (int i = 0; i < Count && i < 64; ++i)
        {
            uintptr_t Owner = 0, Vftable = 0;
            uint32_t Map = 0;
            uint8_t State = 0, Forced = 0;
            if (!ReadPointer(Streamer + kStreamerOwners + i * sizeof(uintptr_t), Owner) || !ReadPointer(Owner, Vftable) ||
                Vftable != s_base + kOwnerVftable || !ReadBytes(Owner + kOwnerMap, &Map, sizeof(Map)) ||
                !ReadBytes(Owner + kOwnerState, &State, 1) || !ReadBytes(Owner + kOwnerForced, &Forced, 1))
            {
                continue;
            }
            if (State == 0 || Forced != 0 || i == PlayerIndex || Map == Destination)
            {
                continue;
            }
            const uint8_t Zero = 0;
            WriteBytes(Streamer + kStreamerAllowed + i, &Zero, 1);
            if ((s_evict_logged & (1ull << i)) == 0)
            {
                s_evict_logged |= 1ull << i;
                Append(StringFormat("%s  mapa %08x (state %u) was streamed in by the game; not allowed until %08x is in\n",
                    Clock().c_str(), Map, (unsigned)State, Destination));
            }
        }
    }

    void DumpEffects(const char* Why);

    uintptr_t EffectsManager()
    {
        uintptr_t Context = 0, Sfx = 0, Manager = 0, Fx = 0;
        if (!ReadBytes(s_base + kContextOffset, &Context, 8) || Context == 0 ||
            !ReadBytes(Context + kContextSfxSystem, &Sfx, 8) || Sfx == 0 ||
            !ReadBytes(Sfx + kSfxManagerBase, &Manager, 8) || Manager == 0 ||
            !ReadBytes(Manager + 8, &Fx, 8))
        {
            return 0;
        }
        return Fx;
    }

    // The live top nodes nobody holds a handle to.
    size_t Orphans(uintptr_t Fx, uintptr_t* Out, size_t Room)
    {
        size_t Count = 0;
        uintptr_t Root = 0;
        if (!ReadBytes(Fx + kFxRoots, &Root, 8))
        {
            return 0;
        }
        for (int Guard = 0; Root != 0 && Guard < 4000 && Count < Room; ++Guard)
        {
            uintptr_t Top = 0, Handles = 1;
            uint32_t Flags = 0;
            if (ReadBytes(Root + 0x10, &Top, 8) && Top != 0 &&
                ReadBytes(Top + kFxNodeFlags, &Flags, 4) && (Flags & (1u << 30)) != 0 &&
                ReadBytes(Top + kFxNodeHandles, &Handles, 8) && Handles == 0)
            {
                Out[Count++] = Top;
            }
            if (!ReadBytes(Root + 8, &Root, 8))
            {
                break;
            }
        }
        return Count;
    }

    uintptr_t s_orphans_before[kMaxOrphans];
    uintptr_t s_orphans_after[kMaxOrphans];

    // The effect nodes the dying map's own entities hold, read before the
    // teardown: owner+0x160 is the entity container (MapEntity* array at
    // +0x10, int16 count at +0x2c); each entity's components are a list at
    // +0x18 (next +0x10); a slot component has a MapSfxSlotCtrl at +0x50,
    // whose list A (+0x20, entries next at +0x78) holds two FX handles
    // inline at +0x08 and +0x38 and list B (+0x30, next at +0xc8) two at
    // +0x58 and +0x88; a handle's node is at its +0x10.
    //
    // Only these may be killed after the teardown. Measured 19/09: leaving
    // Eleum Loyce with the players waiting in Majula, the teardown also left
    // ids 218 and 251 without a holder - common effects, not the map's -
    // and three teardowns of three that killed them took the host down 90 ms
    // later with a corrupted heap; the ones that killed only the map's own
    // 8519 were clean.
    constexpr size_t kOwnerEntities = 0x160;
    constexpr size_t kSlotCtrlVftableOffset = 0x10e86d8;
    constexpr size_t kMaxMapNodes = 4096;
    uintptr_t s_map_nodes[kMaxMapNodes];
    size_t s_map_node_count = 0;

    void AddMapNode(uintptr_t Handle)
    {
        uintptr_t Node = 0;
        if (ReadBytes(Handle + 0x10, &Node, 8) && Node != 0 && s_map_node_count < kMaxMapNodes)
        {
            s_map_nodes[s_map_node_count++] = Node;
        }
    }

    void CollectMapNodes(uintptr_t Owner)
    {
        s_map_node_count = 0;
        uintptr_t Container = 0, Array = 0;
        int16_t Count = 0;
        if (!ReadBytes(Owner + kOwnerEntities, &Container, 8) || Container == 0 ||
            !ReadBytes(Container + 0x10, &Array, 8) || Array == 0 ||
            !ReadBytes(Container + 0x2c, &Count, 2) || Count <= 0)
        {
            return;
        }
        for (int e = 0; e < Count && e < 8192; ++e)
        {
            uintptr_t Entity = 0, Component = 0;
            if (!ReadBytes(Array + e * 8, &Entity, 8) || Entity == 0 || !ReadBytes(Entity + 0x18, &Component, 8))
            {
                continue;
            }
            for (int c = 0; Component != 0 && c < 64; ++c)
            {
                uintptr_t Vftable = 0;
                if (ReadBytes(Component + 0x50, &Vftable, 8) && Vftable == s_base + kSlotCtrlVftableOffset)
                {
                    const uintptr_t Ctrl = Component + 0x50;
                    uintptr_t Entry = 0;
                    ReadBytes(Ctrl + 0x20, &Entry, 8);
                    for (int k = 0; Entry != 0 && k < 256; ++k)
                    {
                        AddMapNode(Entry + 0x08);
                        AddMapNode(Entry + 0x38);
                        if (!ReadBytes(Entry + 0x78, &Entry, 8))
                        {
                            break;
                        }
                    }
                    Entry = 0;
                    ReadBytes(Ctrl + 0x30, &Entry, 8);
                    for (int k = 0; Entry != 0 && k < 256; ++k)
                    {
                        AddMapNode(Entry + 0x58);
                        AddMapNode(Entry + 0x88);
                        if (!ReadBytes(Entry + 0xc8, &Entry, 8))
                        {
                            break;
                        }
                    }
                }
                if (!ReadBytes(Component + 0x10, &Component, 8))
                {
                    break;
                }
            }
        }
    }

    bool IsMapNode(uintptr_t Node)
    {
        for (size_t i = 0; i < s_map_node_count; ++i)
        {
            if (s_map_nodes[i] == Node)
            {
                return true;
            }
        }
        return false;
    }

    bool TeardownHook(void* Owner)
    {
        uint32_t Map = 0;
        if (Owner == nullptr || !ReadBytes((uintptr_t)Owner + kOwnerMapId, &Map, sizeof(Map)) ||
            (Map & 0xff000000u) != 0x32000000u || s_fx_kill_subtree == nullptr || s_fx_kill_node == nullptr)
        {
            return s_original_teardown(Owner);
        }
        const uintptr_t Fx = EffectsManager();
        if (Fx == 0)
        {
            Append(StringFormat("%s  mapa %08x teardown: no effects manager; effects left alone\n", Clock().c_str(), Map));
            return s_original_teardown(Owner);
        }
        DumpEffects("before the teardown");
        CollectMapNodes((uintptr_t)Owner);
        const size_t Before = Orphans(Fx, s_orphans_before, kMaxOrphans);
        const bool Result = s_original_teardown(Owner);
        const size_t After = Orphans(Fx, s_orphans_after, kMaxOrphans);

        unsigned Killed = 0, Spared = 0;
        std::string Ids, SparedIds;
        for (size_t i = 0; i < After; ++i)
        {
            const uintptr_t Top = s_orphans_after[i];
            bool Old = false;
            for (size_t j = 0; j < Before && !Old; ++j)
            {
                Old = s_orphans_before[j] == Top;
            }
            uint32_t Flags = 0;
            if (Old || !ReadBytes(Top + kFxNodeFlags, &Flags, 4) || (Flags & (1u << 30)) == 0)
            {
                continue;
            }
            uintptr_t RootPtr = 0;
            uint32_t Id = 0;
            if (ReadBytes(Top + 0xc8, &RootPtr, 8) && RootPtr != 0)
            {
                ReadBytes(RootPtr + kFxRootId, &Id, 4);
            }
            if (!IsMapNode(Top))
            {
                ++Spared;
                if (Spared <= 16)
                {
                    SparedIds += StringFormat(" %u", Id);
                }
                continue;
            }
            uintptr_t Child = 0;
            ReadBytes(Top + kFxNodeChild, &Child, 8);
            for (int Guard = 0; Child != 0 && Guard < 256; ++Guard)
            {
                uintptr_t Next = 0;
                ReadBytes(Child + kFxNodeSibling, &Next, 8);
                s_fx_kill_subtree(Fx, Child);
                s_fx_kill_node(Fx, Child);
                Child = Next;
            }
            s_fx_kill_node(Fx, Top);
            ++Killed;
            if (Killed <= 16)
            {
                Ids += StringFormat(" %u", Id);
            }
        }
        Append(StringFormat("%s  mapa %08x teardown: %u effect tree(s) of its own entities left with no holder killed (ids%s); %u not the map's spared (ids%s); %zu of its nodes read; %zu orphan(s) from before left alone\n",
            Clock().c_str(), Map, Killed, Ids.empty() ? " none" : Ids.c_str(), Spared, SparedIds.empty() ? " none" : SparedIds.c_str(),
            s_map_node_count, Before));
        return Result;
    }

    // A map being taken down for a travel that would not fit beside it.
    void UnloadAsked(uintptr_t Streamer)
    {
        const int32_t Wanted = s_unload_index.load();
        if (Wanted < 0)
        {
            return;
        }
        int16_t Count = 0;
        if (!ReadBytes(Streamer + kStreamerOwnerCount, &Count, sizeof(Count)) || Count <= 0 || Count > kMaxOwners)
        {
            return;
        }
        for (int i = 0; i < Count && i < 64; ++i)
        {
            uintptr_t Owner = 0, Vftable = 0;
            int32_t Index = -1;
            uint32_t Map = 0;
            uint8_t State = 0;
            if (!ReadPointer(Streamer + kStreamerOwners + i * sizeof(uintptr_t), Owner) || !ReadPointer(Owner, Vftable) ||
                Vftable != s_base + kOwnerVftable || !ReadBytes(Owner + kOwnerIndexField, &Index, sizeof(Index)) ||
                Index != Wanted || !ReadBytes(Owner + kOwnerMap, &Map, sizeof(Map)) || !ReadBytes(Owner + kOwnerState, &State, 1))
            {
                continue;
            }
            if (State == 0 || GetTickCount64() - s_unload_since.load() > kUnloadWindowMs || DS2_BonfireInSession_IsSessionMap(Map))
            {
                Append(StringFormat("%s  budget: map %08x [%d] %s\n", Clock().c_str(), Map, Index,
                    State == 0 ? "is down" : "did not come down in time; left as it is"));
                s_unload_index.store(-1);
                return;
            }
            const uint8_t Zero = 0;
            WriteBytes(Streamer + kStreamerAllowed + i, &Zero, 1);
            // The parts this backread asked for stay in the owner's masks
            // until they are given back, and with them the map stays (19/09:
            // unforced, unwanted and off the player's map, Eleum Loyce sat at
            // state 5 for 15 s). The normal release gives them back and drops
            // the force byte 700 ms later.
            if (s_unload_let_go != Wanted)
            {
                s_unload_let_go = Wanted;
                BeginLetGo(Owner, Map, "the travel budget needs it gone");
            }
            // Once a second, what keeps it: the streamer's player map, and the
            // owner's wanted byte (+0x1ea, FUN_1403dc930).
            const ULONGLONG Now = GetTickCount64();
            if (Now - s_unload_logged >= 1000)
            {
                s_unload_logged = Now;
                int32_t PlayerIndex = -1;
                uint8_t Wanted = 0, Forced = 0;
                ReadBytes(Streamer + kStreamerPlayerMap, &PlayerIndex, sizeof(PlayerIndex));
                ReadBytes(Owner + 0x1ea, &Wanted, 1);
                ReadBytes(Owner + kOwnerForced, &Forced, 1);
                bool Kept = false;
                {
                    std::scoped_lock Lock(s_keep_mutex);
                    for (const auto& Entry : s_kept)
                    {
                        Kept = Kept || (Entry.Index == Index && Now < Entry.Until);
                    }
                }
                Append(StringFormat("%s  budget: map %08x [%d] still state %u; wanted %u, forced %u, kept %u, player map [%d]\n",
                    Clock().c_str(), Map, Index, (unsigned)State, (unsigned)Wanted, (unsigned)Forced, (unsigned)Kept, PlayerIndex));
            }
            return;
        }
        s_unload_index.store(-1);
    }

    void StreamerMasksHook(void* Streamer, void* Arg)
    {
        UnloadAsked((uintptr_t)Streamer);
        EvictNeighbours((uintptr_t)Streamer);
        s_original_masks(Streamer, Arg);
    }

    void StreamerUpdateHook(void* Streamer, float* Position, int32_t Cell, void* Part, uint8_t Flag)
    {
        const uint32_t Map = s_focus_map.load();
        if (Map != 0)
        {
            // The nav search reads the position with aligned SSE: measured
            // 14/09, an unaligned vec4 faulted inside FUN_140babf90.
            alignas(16) float Focus[4] = {};
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
                // The part goes with the cell or it does not go at all. The
                // fourth argument is the entity under the local player's feet,
                // which belongs to the map the player is really in, and the
                // cell here names another one. Handing the streamer both
                // writes a part of one map into the bookkeeping of another,
                // and the pointer dies with whichever map goes first: on 17/09
                // a teardown of Majula walked a part whose vftable read
                // 00003817410eaf80, the low half a real address in the game's
                // image and the top half somebody else's.
                //
                // Saying nothing at all is worse: with no part the streamer
                // never builds the ground, the player never lands, and the
                // travel fails outright (measured the same day). So the part
                // is passed exactly while it is this map's, which after the
                // teleport it is.
                s_original_streamer(Streamer, Focus, s_focus_cell, PartOfMap(Part, Map) ? Part : nullptr, Flag);
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

    // The live effects, one line per root effect id: how many trees, how
    // many of their top nodes someone holds a handle to (node+0xf8), and how
    // many carry the default parameter block. A measuring tool: which effect
    // is a bonfire's flame, and who holds it, is not read anywhere yet.
    // Guarded reads from whichever thread asks; the lists can move under it.
    void DumpEffects(const char* Why)
    {
        uintptr_t Context = 0, Sfx = 0, Manager = 0, Fx = 0, Root = 0;
        if (!ReadBytes(s_base + kContextOffset, &Context, 8) || Context == 0 ||
            !ReadBytes(Context + kContextSfxSystem, &Sfx, 8) || Sfx == 0 ||
            !ReadBytes(Sfx + kSfxManagerBase, &Manager, 8) || Manager == 0 ||
            !ReadBytes(Manager + 8, &Fx, 8) || Fx == 0 || !ReadBytes(Fx + 0x10, &Root, 8))
        {
            Append(StringFormat("%s  effects (%s): no effects manager\n", Clock().c_str(), Why));
            return;
        }
        struct Row { uint32_t Id; uint32_t Def; unsigned Trees; unsigned Held; unsigned Default; };
        Row Rows[96] = {};
        size_t Count = 0;
        unsigned Total = 0;
        for (int Guard = 0; Root != 0 && Guard < 2000; ++Guard)
        {
            uint32_t Id = 0, Def = 0;
            uintptr_t Node = 0, DefPtr = 0, Handles = 0, Params = 0;
            ReadBytes(Root + 0x34, &Id, 4);
            if (ReadBytes(Root + 0x10, &Node, 8) && Node != 0)
            {
                if (ReadBytes(Node + 0x98, &DefPtr, 8) && DefPtr != 0)
                {
                    ReadBytes(DefPtr + 8, &Def, 4);
                }
                ReadBytes(Node + 0xf8, &Handles, 8);
                ReadBytes(Node + 0x50, &Params, 8);
            }
            ++Total;
            size_t i = 0;
            while (i < Count && !(Rows[i].Id == Id && Rows[i].Def == Def))
            {
                ++i;
            }
            if (i == Count && Count < 96)
            {
                Rows[Count++] = { Id, Def, 0, 0, 0 };
            }
            if (i < Count)
            {
                ++Rows[i].Trees;
                Rows[i].Held += Handles != 0 ? 1 : 0;
                Rows[i].Default += Params == s_base + 0x1673048 ? 1 : 0;
            }
            if (!ReadBytes(Root + 8, &Root, 8))
            {
                break;
            }
        }
        std::string Text = StringFormat("%s  effects (%s): %u tree(s)\n", Clock().c_str(), Why, Total);
        for (size_t i = 0; i < Count; ++i)
        {
            Text += StringFormat("    root id %u, top def %u: %u tree(s), %u held, %u default params\n",
                Rows[i].Id, Rows[i].Def, Rows[i].Trees, Rows[i].Held, Rows[i].Default);
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
        else if (Verb == "keep")
        {
            // What another player standing in that map does, without a session.
            int32_t Index = -1;
            uint32_t Milliseconds = 0;
            Parts >> Index >> Milliseconds;
            uint32_t Mask[4] = {};
            int Words = 0;
            for (; Words < 4; ++Words)
            {
                std::string Word;
                if (!(Parts >> Word))
                {
                    break;
                }
                Mask[Words] = (uint32_t)strtoul(Word.c_str(), nullptr, 16);
            }
            DS2_Backread::KeepIndex(Index, Milliseconds, Words == 4 ? Mask : nullptr);
            Append(StringFormat("%s  === pedido: manter o mapa de indice %d por %u ms, partes %s ===\n", Clock().c_str(), Index,
                Milliseconds, Words == 4 ? DescribeMask(Mask).c_str() : "todas"));
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
        else if (Verb == "fx")
        {
            DumpEffects("asked");
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

void DS2_Backread::KeepIndex(int32_t Index, uint32_t Milliseconds, const uint32_t* Mask)
{
#ifdef _WIN32
    if (Index < 0 || Index > 0x3f)
    {
        return;
    }
    const uint32_t Nothing[4] = {};
    const ULONGLONG Until = GetTickCount64() + Milliseconds;
    bool Added = false, Changed = false;
    uint32_t Now[4] = {};
    {
        std::scoped_lock Lock(s_keep_mutex);
        Kept* Entry = nullptr;
        Kept* Free = nullptr;
        for (Kept& Candidate : s_kept)
        {
            if (Candidate.Index == Index)
            {
                Entry = &Candidate;
                break;
            }
            if (Candidate.Index < 0 && Free == nullptr)
            {
                Free = &Candidate;
            }
        }
        if (Entry == nullptr && Free != nullptr)
        {
            Entry = Free;
            Entry->Index = Index;
            Entry->Forced = false;
            // With nothing to say which parts, no parts at all: the force
            // byte alone holds a map that is already in, and that is every
            // case this keep serves - the other player is standing in it.
            //
            // Asking for every part used to be what happened here, and it is
            // a request for parts the map does not have: 128 bits per block
            // over six blocks, against maps whose part tables are far
            // shorter. The teardown walks the set bits, and on 17/09 the
            // guest's release of Brume Tower faulted twice inside it
            // (+0x3f6476 and +0x3ba0be, both on a part pointer built out of
            // rubbish), the trap left the map half gone, and the next map to
            // load died on the wreckage. A keep that carries real parts still
            // ORs them; only the invented ones are gone.
            memcpy(Entry->Mask, Mask != nullptr ? Mask : Nothing, sizeof(Entry->Mask));
            Added = true;
        }
        else if (Entry != nullptr && Mask != nullptr)
        {
            // Masks add up and never shrink: a travel keeping the map whole
            // must not be cut down to the parts under the other player's copy
            // by the keep that copy renews every frame, and the reverse. A
            // map kept in two shapes at once unloaded in two phases (parts on
            // arrival, the rest when the keep ended), which the solo control
            // never does, and that is where the guest's game died (16/09).
            for (int i = 0; i < 4; ++i)
            {
                if ((Entry->Mask[i] | Mask[i]) != Entry->Mask[i])
                {
                    Entry->Mask[i] |= Mask[i];
                    Changed = true;
                }
            }
        }
        if (Entry != nullptr)
        {
            // The longest deadline wins: the keep a travel asks for (tens of
            // seconds) must not be cut short by the keep the other player's
            // copy renews every frame (a few seconds). Measured 15/09: a
            // 30 s hold on the map a travel left became 5 s and the map went.
            if (Until > Entry->Until)
            {
                Entry->Until = Until;
            }
            memcpy(Now, Entry->Mask, sizeof(Now));
        }
    }
    if (Added || Changed)
    {
        const bool AnyPart = (Now[0] | Now[1] | Now[2] | Now[3]) != 0;
        Append(StringFormat("%s  mapa de indice %d mantido: um jogador esta nele, %s\n", Clock().c_str(), Index,
            AnyPart ? StringFormat("partes %s", DescribeMask(Now).c_str()).c_str()
                    : "sem pedir parte nenhuma; seguro o mapa como esta"));
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

bool DS2_Backread::Targets(uint64_t& Count)
{
#ifdef _WIN32
    uint64_t Chameleon = 0;
    return ReadBudget(Count, Chameleon);
#else
    return false;
#endif
}

uint64_t DS2_Backread::TargetLimit()
{
#ifdef _WIN32
    return kTargetCapacity - kTargetMargin;
#else
    return 0;
#endif
}

uint32_t DS2_Backread::TargetCost(uint32_t MapId)
{
#ifdef _WIN32
    std::scoped_lock Lock(s_cost_mutex);
    const auto Found = s_costs.find(MapId);
    return Found == s_costs.end() ? kUnknownCost : Found->second;
#else
    return 0;
#endif
}

uint32_t DS2_Backread::MapAt(int32_t Index)
{
#ifdef _WIN32
    uintptr_t Owners[kMaxOwners] = {};
    const int Count = ReadOwners(Owners);
    for (int i = 0; i < Count; ++i)
    {
        uint32_t Map = 0;
        int32_t At = -1;
        if (ReadBytes(Owners[i] + kOwnerIndexField, &At, sizeof(At)) && At == Index &&
            ReadBytes(Owners[i] + kOwnerMap, &Map, sizeof(Map)))
        {
            return Map;
        }
    }
#endif
    return 0;
}

void DS2_Backread::DropKeeps()
{
#ifdef _WIN32
    std::scoped_lock Lock(s_keep_mutex);
    for (Kept& Entry : s_kept)
    {
        if (Entry.Index >= 0)
        {
            Entry.Until = 0;
        }
    }
#endif
}

void DS2_Backread::Unload(int32_t Index)
{
#ifdef _WIN32
    if (Index < 0 || Index > 0x3f)
    {
        return;
    }
    {
        std::scoped_lock Lock(s_keep_mutex);
        for (Kept& Entry : s_kept)
        {
            if (Entry.Index == Index)
            {
                Entry = Kept();
            }
        }
    }
    s_unload_since.store(GetTickCount64());
    s_unload_let_go = -1;
    s_unload_index.store(Index);
#endif
}

int32_t DS2_Backread::Unloading()
{
#ifdef _WIN32
    return s_unload_index.load();
#else
    return -1;
#endif
}

bool DS2_Backread::Unloaded(int32_t Index)
{
#ifdef _WIN32
    uintptr_t Owners[kMaxOwners] = {};
    const int Count = ReadOwners(Owners);
    for (int i = 0; i < Count; ++i)
    {
        int32_t At = -1;
        uint8_t State = 0;
        if (ReadBytes(Owners[i] + kOwnerIndexField, &At, sizeof(At)) && At == Index &&
            ReadBytes(Owners[i] + kOwnerState, &State, 1))
        {
            return State == 0;
        }
    }
#endif
    return true;
}

int32_t DS2_Backread::IndexOf(uint32_t MapId)
{
#ifdef _WIN32
    uintptr_t Owners[kMaxOwners] = {};
    const int Count = ReadOwners(Owners);
    for (int i = 0; i < Count; ++i)
    {
        uint32_t Map = 0;
        int32_t Index = -1;
        if (ReadBytes(Owners[i] + kOwnerMap, &Map, sizeof(Map)) && Map == MapId &&
            ReadBytes(Owners[i] + kOwnerIndexField, &Index, sizeof(Index)))
        {
            return Index;
        }
    }
#endif
    return -1;
}

bool DS2_BackreadHook::Install(Injector& injector)
{
#ifdef _WIN32
    s_base = (uintptr_t)injector.GetBaseAddress();

    if (!BytesMatch(s_base + kOwnerUpdateOffset, kOwnerUpdateBytes, sizeof(kOwnerUpdateBytes)) ||
        !BytesMatch(s_base + kOwnerStatesOffset, kOwnerStatesBytes, sizeof(kOwnerStatesBytes)) ||
        !BytesMatch(s_base + kStreamerUpdateOffset, kStreamerUpdateBytes, sizeof(kStreamerUpdateBytes)) ||
        !BytesMatch(s_base + kNavFindMapOffset, kNavFindMapBytes, sizeof(kNavFindMapBytes)) ||
        !BytesMatch(s_base + kNavFindCellOffset, kNavFindCellBytes, sizeof(kNavFindCellBytes)) ||
        !BytesMatch(s_base + kStreamerMasksOffset, kStreamerMasksBytes, sizeof(kStreamerMasksBytes)) ||
        !BytesMatch(s_base + kTeardownOffset, kTeardownBytes, sizeof(kTeardownBytes)))
    {
        Error("[DS2_BackreadHook] a atualizacao do dono do mapa nao e a esperada; recusando");
        return false;
    }

    s_log_path = injector.GetDllPath() / "DS2_Backread.log";
    s_costs_path = injector.GetDllPath() / "DS2_TargetCosts.txt";
    LoadCosts();

    // The cap goes up only if the whole compare is the one it was read from.
    if (BytesMatch(s_base + kStreamCapOffset, kStreamCapExpected, sizeof(kStreamCapExpected)))
    {
        DWORD Previous = 0;
        const uintptr_t At = s_base + kStreamCapOffset + kStreamCapByte;
        if (VirtualProtect((void*)At, 1, PAGE_EXECUTE_READWRITE, &Previous))
        {
            *(uint8_t*)At = kStreamCap;
            FlushInstructionCache(GetCurrentProcess(), (void*)At, 1);
            DWORD Ignored = 0;
            VirtualProtect((void*)At, 1, Previous, &Ignored);
            s_cap_raised = true;
        }
    }
    else
    {
        Error("[DS2_BackreadHook] the streaming cap is not the expected compare; left at two maps");
    }
    s_request_path = injector.GetDllPath() / "DS2_Backread.req";
    s_original_update = (OwnerUpdate_p)(s_base + kOwnerUpdateOffset);
    s_original_streamer = (StreamerUpdate_p)(s_base + kStreamerUpdateOffset);
    s_original_masks = (StreamerMasks_p)(s_base + kStreamerMasksOffset);
    s_original_teardown = (Teardown_p)(s_base + kTeardownOffset);
    if (BytesMatch(s_base + kFxKillSubtreeOffset, kFxKillSubtreeBytes, sizeof(kFxKillSubtreeBytes)) &&
        BytesMatch(s_base + kFxKillNodeOffset, kFxKillNodeBytes, sizeof(kFxKillNodeBytes)))
    {
        s_fx_kill_subtree = (FxKill_p)(s_base + kFxKillSubtreeOffset);
        s_fx_kill_node = (FxKill_p)(s_base + kFxKillNodeOffset);
    }
    else
    {
        Error("[DS2_BackreadHook] the effects kill is not the expected code; a DLC map's orphaned effects are left alone");
    }
    s_nav_find_map = (NavFindMap_p)(s_base + kNavFindMapOffset);
    s_nav_find_cell = (NavFindCell_p)(s_base + kNavFindCellOffset);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original_update, OwnerUpdateHook);
    DetourAttach(&(PVOID&)s_original_streamer, StreamerUpdateHook);
    DetourAttach(&(PVOID&)s_original_masks, StreamerMasksHook);
    DetourAttach(&(PVOID&)s_original_teardown, TeardownHook);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        Error("[DS2_BackreadHook] nao consegui instalar o detour");
        return false;
    }

    s_running.store(true);
    s_thread = std::thread(Run);

    Append(StringFormat("%s  === ds2os backread: pronto ===\n", Clock().c_str()));
    Append(StringFormat("%s  streaming cap: %s\n", Clock().c_str(),
        s_cap_raised ? "raised to three maps" : "left at two maps"));
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
        DetourDetach(&(PVOID&)s_original_masks, StreamerMasksHook);
        DetourDetach(&(PVOID&)s_original_teardown, TeardownHook);
        DetourTransactionCommit();
        s_original_update = nullptr;
    }
#endif
}

const char* DS2_BackreadHook::GetName()
{
    return "DS2 Backread";
}
