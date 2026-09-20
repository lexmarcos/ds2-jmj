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

// Every warp the game performs, written down - and a lever on the one that
// sends a player home when a session ends.
//
// Who died decides what the lever is worth, and both halves are measured:
// a red invader is already sent to his last bonfire by the game, so nothing
// changes; a co-op phantom is sent back to the spot he stood on when he was
// summoned, and that is the one this replaces. What a death still costs a
// co-op run is the session itself, which nothing here touches.
//
// Every warp in the game passes through one virtual call, slot +0x40 of the
// global context (`0x1416148f0`), and the request it carries says why:
//
//   reason 1   you died in your own world - go to the last bonfire
//   reason 4   the session is over - go back to your own world
//
// Motive 4 alone is not "go home": the same motive carries a guest *into* a
// host's world. The game separates the two on the third argument, and so does
// this hook - redirecting the wrong one cancels the summon, which is how it was
// found. See docs/DS2_SEAMLESS_COOP.md.
//
// The replacement is not hand built. The game's own "respawn where you last
// rested" call is reachable from the same context the warp arrives with
// (`*(ctx + 0x70)` is the respawn record), so the hook calls it and gets the
// vanilla request, bonfire and map included.
//
// The hook also writes a line for every warp it sees, whether or not it
// changes one. That log is how the table of reasons above was built, and it is
// the only way to tell which path a given piece of the game takes.
//
// Writing "0" to DS2_Seamless.req leaves the logging on and stops the
// redirection; anything else turns it back on.
class DS2_SeamlessCoopHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
