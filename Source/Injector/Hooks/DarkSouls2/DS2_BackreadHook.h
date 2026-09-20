/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Injector/Hooks/Hook.h"

#include <cstdint>

// A map brought in beside the one the player stands in, without a warp.
//
// Every warp tears down the map, the local player and the other players'
// characters, so a respawn at a bonfire in a map that is not loaded cannot use
// one without losing the session. The game also streams: at every load it
// creates a `MapAreaCtrlOwner` for each of its 38 maps (`FUN_1403dc0a0`), and
// the streamer (`*(*(ctx+0x38)+8)`) recomputes every frame which parts of
// which map to keep, from the collision the player stands on
// (`FUN_1403dc3e0`, `FUN_1401c8cd0`), into a 128-bit mask at `owner+0x10`
// (copied to `+0x40`). Each owner then runs its load state machine
// (`FUN_1403cc450`, state at `+0x1e8`, 0 unloaded to 5 loaded) on
// `+0x1e9` (forced) or `+0x1ea` (wanted).
//
// Measured on 14/09, solo in Heide:
//   - `+0x1e9` set by hand on Majula's owner brought Majula in beside Heide:
//     state 5, its characters created, and its bonfire (`0x122a`) in the
//     bonfire list with world coordinates 226.7 m away. Clearing it unloaded
//     Majula again;
//   - but its parts mask stayed 0: no ground. Teleported to the bonfire, the
//     player fell;
//   - the streamer's own override (`+0x1f0` index, `+0x1d0` mask) forces one
//     map and zeroes every other one. Forcing Heide with every part was fine;
//     forcing Majula unloaded Heide under the player and closed the game.
//
// So this hook takes one owner by the hand: before its update
// (`FUN_1403cc3f0`), after the streamer has written the natural masks, it sets
// the force byte and ORs a parts mask in, and lets go when asked.
//
// A forced map also needs its ground: the parts come from a graph search
// over nav cells from the cell the player stands in, and a player in the air
// has none. A focus tells the streamer (`FUN_1403dc8e0`) to search from the
// cell of a position in the forced map instead.
//
// `DS2_Backread.req`: `load <map hex> [<mask hex> x4]` (every part by
// default), `focus <map hex> <x> <y> <z>`, `unfocus`, `clear`, `status`, and
// `keep <map index> <ms> [<mask hex> x4]`, the keep a remote player gets, for
// testing solo.
class DS2_BackreadHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};

namespace DS2_Backread
{

    // Keep a map loaded with these parts, beside whatever the player needs,
    // until Release. One map at a time; a new request replaces the old one.
    void Request(uint32_t MapId, const uint32_t Mask[4]);
    void Release();

    // Tell the streamer the player stands at this position of that map, so
    // the parts around it load the way they would if the player had walked
    // there. Needs the map's nav in (Request first). Until Unfocus.
    void Focus(uint32_t MapId, const float Position[3]);
    void Unfocus();

    // Keep the map with this index forced, with these parts beside the ones
    // the game wants, for the next few milliseconds; called again to keep it
    // longer. For the maps other players stand in, which must not unload under
    // them. Without a mask, only the force byte: the map stays exactly as the
    // game loaded it.
    // ForOtherPlayer marks the hold as "the other player's copy is standing
    // there"; Heaviest() refuses to make room by taking such a map down.
    void KeepIndex(int32_t Index, uint32_t Milliseconds, const uint32_t* Mask = nullptr,
        bool ForOtherPlayer = false);

    // The owner of a map as last seen: its load state (+0x1e8, 5 loaded) and
    // parts mask. False when no owner has that map.
    bool Query(uint32_t MapId, uint8_t& State, uint32_t Mask[4]);

    // The owner index of a map (the one collision handles carry in bits 4..9),
    // or -1 when no owner has that map.
    int32_t IndexOf(uint32_t MapId);

    // The map budget (see the TargetManager in DS2_BackreadHook.cpp): the
    // entries in use now, the capacity with a margin kept free, and what a
    // map costs, measured on each load and remembered across boots
    // (DS2_TargetCosts.txt), with a conservative guess for a map never seen.
    bool Targets(uint64_t& Count);
    uint64_t TargetLimit();
    uint32_t TargetCost(uint32_t MapId);

    // The map id of the owner with this index, 0 when none.
    uint32_t MapAt(int32_t Index);

    // Ends every KeepIndex hold now; their maps go through the normal release.
    // Ends the holds this machine took for itself. A hold marking the map
    // another player stands in survives, and lapses on its own once his copy
    // stops being seen there - pass true only when the indices themselves
    // stopped meaning anything, as a reload makes them.
    void DropKeeps(bool IncludingOtherPlayers = false);

    // Takes the map with this index down even if the player stands in it, the
    // way a loading screen would: its keep is dropped, its force byte cleared
    // and the streamer is told it cannot reach it, until it is gone or 15 s
    // pass. For a travel that would not fit beside the map it leaves.
    void Unload(int32_t Index);
    bool Unloaded(int32_t Index);
    // The index being taken down, -1 when none.
    int32_t Unloading();

    /// The index of a map going down through BeginLetGo, or -1. Unloading()
    /// only ever names the budget's victim, so a map let go for any other
    /// reason - a keep expiring, an explicit request, the session ending -
    /// was invisible to anything watching Unloading(), and the lighting
    /// sweep is one of those.
    int32_t Releasing();
    void CancelUnload();

    // The loaded map with the highest target cost that is none of these and
    // not the session's, with its owner index; 0 when there is none.
    uint32_t Heaviest(uint32_t NotA, uint32_t NotB, uint32_t NotC, int32_t& Index);
    // The lighting entries (MapCubeEnvLightMapBank at owner+0x1a0: {id,
    // entry*} pairs at +0x10, count at +0x18) of the map with this index.
    size_t LightingEntries(int32_t Index, uintptr_t* Out, size_t Room);
}
