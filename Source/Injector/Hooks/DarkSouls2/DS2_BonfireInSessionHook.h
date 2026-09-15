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
/// The rest of a host reaches its guests over DS2_CoopChannel: sitting down
/// sends RestStarted, and each guest shows "A player is resting at a bonfire.";
/// the world reset the rest runs (FUN_14017fd70) sends WorldReset, and each
/// guest runs the same reset on its copy of the host's world, so enemies the
/// host sees come back come back on the guest too. `DS2_Bonfire.log` says what
/// was sent and received.
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

/// From the warp hook, on the game's thread: a travel the owner of the world
/// asked for while guests are in it. True when it is held: every guest gets a
/// Yes/No box, and the travel goes ahead when all say yes (the guests then
/// follow to the host's new bonfire, DS2_DeathIntercept_FollowHost), or is
/// canceled with a message on a no or after 30 s.
bool DS2_BonfireInSession_HoldTravel(void* Context, const uint8_t* Request, size_t Size, uint8_t Flag);
