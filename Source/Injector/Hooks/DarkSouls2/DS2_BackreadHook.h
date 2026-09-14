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
// default), `focus <map hex> <x> <y> <z>`, `unfocus`, `clear`, `status`.
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

    // The owner of a map as last seen: its load state (+0x1e8, 5 loaded) and
    // parts mask. False when no owner has that map.
    bool Query(uint32_t MapId, uint8_t& State, uint32_t Mask[4]);
}
