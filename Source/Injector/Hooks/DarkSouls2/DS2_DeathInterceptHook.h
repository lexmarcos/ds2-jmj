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

// The door a death of the local player goes through, and a hand on it.
//
// Every character, enemies included, owns a `ChrDeadActionCtrl` (vftable
// `0x1410bf308`), and its slot +0x20, `FUN_14013c720`, is ticked once a frame.
// In state 0 it looks at `*(chr+0xb8)+0x759`; when that byte is set it copies
// the death's parameters out of `+0x75c..+0x76d`, clears the byte, and moves
// to state 2, where a timer fires everything a death means: the notification
// to the character manager, `RequestNotifyDeath`, the souls, and in the end
// the warp to the last bonfire.
//
// Measured on 13/09 with the character's HP poked to zero: `FUN_14016a650`
// sets the byte from the per-frame update of the player (HP below 1, bit 15 of
// `+0x4c8` clear, byte not already set), this function consumed it, and six
// seconds later the game warped with motive 1. Slot +0x10, `FUN_14013c3b0`,
// the consumer the first reading pointed at, was never called for the local
// player.
//
// So this is where a death can be refused. `DS2_Death.req`, one per line:
//
//   observe   write down every death of the local player and let it happen
//   cancel    clear the byte before the controller sees it, and give the HP
//             back, so the game never learns there was a death
//   status    write the counters to the log
//
// Cancelling is only half of a respawn. A death by falling
// (`FUN_140372e20`, cause 0x5a) zeroes the HP on every frame the character
// is still in the air, so `cancel` holds it once a frame for as long as the
// fall lasts, and moving the character out of the air stops that but leaves it
// without control and with the camera parked where it fell. To get out of
// that, switch back to `observe` and let a death through.
//
// `observe` is the default. Two more doors are only watched, never held:
// `FUN_14013c500`, the instant death that fires every consequence at once and
// never touches `+0x759`, and `FUN_14013c3b0`, which may be how a remote
// character's death is replayed. See step 3 of the M2 plan in
// docs/DS2_SEAMLESS_COOP_TASKS.md.
class DS2_DeathInterceptHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
