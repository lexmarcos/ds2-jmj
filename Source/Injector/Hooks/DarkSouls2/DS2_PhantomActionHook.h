/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Injector/Hooks/Hook.h"

/// Lets a white phantom pull the levers and open the doors of the world it is
/// in.
///
/// Measured 19/09 in Heide: the host standing at a lever read `A: Pull` and
/// the phantom beside him, 0.6 m from the same object, read nothing. It is not
/// the object's state and not a flag — every `MapObjStateActComponent` within
/// 20 m read the same state on both machines, and the map's three flag
/// categories were byte for byte equal.
///
/// The prompt system is `EventKeyGuideCtrl` (vftable `0x1410ef228`). When a
/// character walks into an action volume, `FUN_140454310` asks
/// `FUN_140453760(chr, ctrl+0xaa)` whether that character may act, and on a
/// no it sets the character's bit in the exclusion mask at `ctrl+0xa0`, which
/// is what `FUN_140453ce0` refuses on every frame afterwards. Inside
/// `FUN_140453760` the role byte `*(*(chr+0xb0)+0x3c)` picks a bit of the
/// 24-bit mask: role 1, the white phantom, needs bit 1.
///
/// That mask is **data, not code**. For every object with a state machine —
/// levers, doors and bonfires alike — `FUN_140453b30` builds it from the
/// object's own row: `1 | (row[0x1a] << 1) | (row[0x1b] << 9) | ((row[0x1c] &
/// 0xf) << 17)`. So `row[0x1a]` bit 0 is literally "a white phantom may use
/// this action", and it is set per object by the game's own data. Read live on
/// both machines: every bonfire (`122a`, `7ba2`, `7ba7`, `7bac`) reads `0xff`,
/// which is why a guest can rest; the lever reads `0x00`.
///
/// So this is the unmodded game's rule, not something the mod broke — and the
/// seamless brief wants it gone. The patch is the one immediate that builds
/// that bit, `or $0x1,%al` -> `or $0x3,%al` at `+0x453b47`, which sets mask
/// bit 1 for every map-object action. By the switch in `FUN_140453760` that
/// admits roles 1 and 3, the white phantoms, and nothing else; the press path
/// (`FUN_1404562a0`) never reads the role, so the action runs once the prompt
/// is offered.
///
/// **What it does not do.** Whatever the phantom then changes still has to
/// reach the host. A mechanism whose result is a map flag does travel — a
/// guest may write map flags (`FUN_14025cdb0`) and the change goes out on the
/// game's `0x20` packet — but per-object state does not, and that half is
/// untested. See docs/DS2_WORLD_STATE.md.
class DS2_PhantomActionHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
