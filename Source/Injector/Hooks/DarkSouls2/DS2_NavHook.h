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

// Where the player is standing, written down often enough to steer by.
//
// Every test that needs two characters in the same place has been driven by
// hand so far - a screenshot, a guess at which way is forward, a nudge, another
// screenshot. That costs more than the tests do. This publishes the local
// player's position so the harness can walk there on its own.
//
// The chain is the one the game uses itself when a guest joins a host's world
// and has to say where it is standing (`FUN_1402c2a80`):
//
//     player  = *(*(*(0x1416148f0) + 0xa8) + 0xc0)
//     position = player + 0xa8    three floats, x y z, the live one
//     facing   = player + 0xbc and + 0xc4, a normalised 2D direction
//
// There are two more position triples just before `+0xa8`; they do not follow
// the character, and only this one does. Measured by walking and re-reading.
//
// Writes `DS2_Nav.txt` beside the DLL, replaced whole every tick, one line:
//
//     <x> <y> <z> <facing x> <facing z> <player pointer> <tick>
//
// The tick counts samples, and it is there because a reader cannot otherwise
// tell a character standing still from a file that stopped being written. That
// difference is a walk that arrived against a walk that is stuck, and getting
// it wrong once already cost a wrong diagnosis.
//
// The pointer is **not** a way to tell the two instances apart: with no ASLR,
// two copies of the game land on the same heap address, and both publish it.
//
// It reads and never writes game memory, so it is always on for Dark Souls II
// and there is nothing to verify before installing.
class DS2_NavHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
