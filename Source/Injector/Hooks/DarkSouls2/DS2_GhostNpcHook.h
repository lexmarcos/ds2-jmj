/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Injector/Hooks/Hook.h"

/// Gives a guest the world's NPCs solid instead of as white ghosts.
///
/// Measured 20/09 with both characters standing in Majula: the two machines
/// hold the **same ten characters**, at the same positions and with the same
/// HP, and the NPCs read role 0 on the guest as well. Nothing is missing and
/// nothing is out of sync — the guest's client draws them that way on purpose.
///
/// The character factory (`FUN_1403560a0`) asks `FUN_140513440` — networked
/// **and** in someone else's world — and adds one to the character's kind when
/// it is true: 7 becomes 8 and 10 becomes 11, for the "NPC" class only. That
/// is why the talkable NPCs ghost and ordinary enemies, kind 14, do not. The
/// kind lands at `chr+0x54`, and a five-byte-stride table at `0x1410bfff0`
/// gives each kind a `+2` byte meaning "this character is simulated here": 1
/// for the host's kinds, 0 for the guest's. The game's own name for the state
/// is in the binary — on the same test it picks a
/// `GhostChrMorphemeTimeActEventHandler`.
///
/// `FUN_140312dd0`, the character's Initialize, then branches on that byte
/// twice:
///
///   * `+0x31337c` forces the phantom draw type, `*(*(chr+0xb0)+0x48) = 15`,
///     where the host's path takes it from `NpcParam+0x20`. Read live on the
///     same NPC: **0 on the host, 15 on the guest**, with `+0x38` zero on both
///     so `+0x48` is what `FUN_14016f6d0` hands to the draw object each frame.
///   * `+0x3141cd` forces the collision filter to drop bit 1, which is what
///     makes the ghost something you walk through.
///
/// Both patches turn the branch that selects the guest-only path into an
/// unconditional jump onto the target the host's kinds already fall through
/// to, so a guest-created NPC takes literally the host's path and nothing
/// else changes. The kind itself is left alone: the guest still does not
/// simulate the NPC, and footsteps, ambient sound and the damage predicate
/// stay as the game left them.
///
/// **What this is not.** These are the guest's own local copies — with the
/// two players in different maps the host had a different set loaded
/// entirely. Making them solid gives each player their own NPC; it does not
/// share one between them.
class DS2_GhostNpcHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
