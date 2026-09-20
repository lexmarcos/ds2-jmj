/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Injector/Hooks/Hook.h"

#include <cstddef>
#include <cstdint>

/// The owner of the world rests at a bonfire with phantoms in it (M8).
///
/// In a session the game refuses the bonfire in three places, all asking the
/// same question, FUN_14025f690 ("is a multiplayer session up", state 1 or 2):
///
///   1. FUN_1401cb950 (the rest): answers yes -> message 0x453, "Cannot use
///      bonfire", instead of starting the rest;
///   2. FUN_14017ed90 (the rest job, state 2): FUN_14025ea40 nonzero ->
///      cancels the bonfire menu;
///   3. FUN_140199a70 (the menu queue, state 10): answers yes -> cancels the
///      bonfire menu.
///
/// Measured 15/09 by patching the three in memory: the host sat, the menu
/// opened with the phantom beside it, and the session stayed verified.
///
/// 1 and 3 are answered "no" through a detour of FUN_14025f690 that looks at
/// its return address, and only while the local player owns the world, so a
/// phantom still cannot rest in someone else's world. 2 is only reachable once
/// a rest has started, so its branch is patched outright, with expected bytes.
///
/// The rest of a host reaches its guests over DS2_CoopChannel: the world reset
/// the rest runs (FUN_14017fd70) sends WorldReset, and each guest runs the same
/// reset on its copy of the host's world, so enemies the host sees come back
/// come back on the guest too. Nothing is shown on anyone's screen for a rest:
/// a message box in the middle of a fight was worse than no warning (decided
/// 15/09). `DS2_Bonfire.log` says what was sent and received.
///
/// The guest's travel list is the host's world, not the maps it happens to
/// have loaded: the host publishes which bonfires it has lit as a bitmap over
/// the bonfire table's order, and the guest writes them into its own copy of
/// the table (the column the manager's +0x44 selects).
///
/// A white phantom rests too: the event action entries stop dropping the
/// bonfire's prompt for someone in another world (FUN_140453ce0, patched only
/// while the local player is a white phantom), the bonfire's event script is
/// told the player is not a guest (query 130602), the rest heals the guest
/// (measured 15/09: 400 -> 914), and instead of resetting its own copy the
/// guest tells the host, who resets its world and sends the reset to everyone.
///
/// Travel is a vote anyone can start, and when it passes nobody leaves the
/// session: each machine takes its own player to the bonfire the way a respawn
/// at a bonfire of another map does (DS2_DeathIntercept::GoToBonfire, the
/// backread hook bringing the map in beside this one), with no warp. Leaving
/// the session and letting the party put everyone back together took about 75
/// seconds and sent each guest through its own world first, which is not
/// travelling together; it is what is left when the destination's map cannot
/// be brought in.
///
/// The host picking a bonfire, or a guest picking one (a proposal the host
/// refuses if it has not lit that bonfire), opens a Yes/No box naming the
/// bonfire and its area for everyone who did not pick. On a no, or no answer
/// in 30 s, everyone is told. When all say yes the pick is dropped and every
/// machine moves its own player.
///
/// The fallback, kept for a map the backread hook cannot bring in: the guests
/// leave the session the legal way (the end a host's travel causes, which
/// costs no penalty), the host travels with the game's own travel, and the
/// party joins everyone again at the new bonfire about 75 s later. A host that
/// travelled with a phantom still in its world closed twice, and holding the
/// travel any later than the pick froze the host (15/09).
class DS2_BonfireInSessionHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};

/// Called every frame on the game's thread by DS2_PartyHook: a guest acts on
/// the host's rest events.
void DS2_BonfireInSession_Tick();

/// Puts the net layer's character sync back to its idle state, the game's
/// own state 0, which makes it rebuild its list for the map that is loaded.
/// DS2_BackreadHook calls this the instant before it releases a map: the
/// sync still holds records for that map's characters, and the net thread
/// walked them ~170 ms after every release that killed a guest on 17/09.
void DS2_BonfireInSession_IdleNetSync(const char* Why);

/// The backread is about to let this map go. If the object sync is still bound
/// to it - on a guest it stays bound to the map the session began in - its
/// records point into memory about to be freed, and the host's next object
/// packet would be written there. Drops them first.
void DS2_BonfireInSession_ForgetSyncedMap(uint32_t Map);

/// True while a session's enemy sync is bound to this map, which is the map the
/// session began in. The backread never releases it: the game's own join
/// bindings assume it never unloads while the session lives.
bool DS2_BonfireInSession_IsSessionMap(uint32_t Map);

/// The map the session began in, 0 when there is no session. It is the one map
/// that is never released, so its target cost is spent for as long as the
/// session lives, and a destination has to fit beside it.
uint32_t DS2_BonfireInSession_SessionMap();

/// Step 7 of M8 6b: point the guest's join controller at another map.
///
/// `NetSummonJoinMultiplayCtrl + 0x19c` names the map the session began in
/// and is never written again by the game, so it rebinds straight back to the
/// map a release is trying to free. This is the four-byte write that stops
/// that, and it is the guest's alone - a host's holder reads 0.
///
/// `Expected` is the value `+0x19c` must currently hold, or 0 to skip the
/// check; a mismatch refuses rather than writes. The sixteen bytes at
/// `+0x1a0` - the map and the position to return to - are read before and
/// after and restored if they moved, because that block is what sends the
/// guest home and four bytes must not be able to reach it.
bool DS2_BonfireInSession_RepointSessionMap(uint32_t NewMap, uint32_t Expected);

