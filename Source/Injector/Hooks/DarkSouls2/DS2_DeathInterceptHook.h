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
//   respawn   cancel, and pay for the death the way the game would, without
//             the reload: the carried souls into a bloodstain where the
//             character died (the old one removed), hollowing, the death
//             counters, a worn protection ring broken, Estus refilled,
//             "YOU DIED", and the character standing at the spawn point of the
//             last bonfire with full HP. Each cost goes through the game's own
//             gate for who pays, so a phantom keeps its souls and its humanity
//   feature <name> on|off
//             switch one part off without a build: almas, hollow, contador,
//             anel, mancha_online (off: sends nothing, see the .cpp), estus,
//             banner, copias
//   status    write the counters to the log
//
// In a session the death is the dying machine's to refuse, but its HP 0 can
// reach the other machine first, and there the copy of the player dies through
// its own controller ("Phantom Chico has been vanquished", measured 13/09).
// With cancel or respawn, a pending death of another player's copy
// (PlayerCtrl) is refused the same way.
//
// A death by falling needs more than the byte. Touching a death volume of the
// map (`FUN_14036fdf0`) sets bit 51 of `*(chr+0xb8)+0x4c0` and asks for
// FallDeadCameraOperator through `CameraManager+0x450`; the fall controller
// then kills (`FUN_140372e20`, cause 0x5a, bit 9) on every frame the character
// is in the air. So when `cancel` refuses a death that carries those marks it
// also moves the character to the spawn point of the bonfire in the respawn
// record (or back to its last position on the ground, if that bonfire is not
// in the loaded map), waits for the fall controller to say it is down, and
// only then clears the bits and the camera byte - the camera manager pops its
// own request. Without the camera, the stick looked dead: movement is relative
// to a camera that was looking up at the character from where it fell.
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
