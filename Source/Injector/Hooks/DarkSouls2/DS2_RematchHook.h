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

// Summons the same red sign again, from the host's side, without the player
// walking back to it.
//
// The server cannot do this: every push it has was measured and none of them
// forms a session on its own (docs/DS2_REMATCH_AFTER_DEATH.md). The session is
// born when the host's client touches the sign, so that is what this repeats.
//
// Two detours, both on the game's own thread:
//
//   NetSvrSummonSignManager::<summon>  remembers the manager pointer, which
//                                      the summon needs and which nothing else
//                                      hands out
//   SummonSignSetCtrl::<add sign>      fires when a sign arrives in the
//                                      client's own registry, with the new
//                                      SignHandle in its out parameter — the
//                                      one moment when the handle is known and
//                                      the sign is fully built
//
// Summoning once turns the rematch on, and it stays on. That works because of
// an interlock the game already has: a phantom cannot place a sign while it is
// in someone else's world, so a sign arriving is proof the last duel ended.
// Writing "0" to DS2_Rematch.req turns it off; anything else turns it on.
class DS2_RematchHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
