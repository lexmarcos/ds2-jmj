/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Injector/Hooks/Hook.h"

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
class DS2_BonfireInSessionHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
