/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Injector/Hooks/Hook.h"

/// What was done together goes into the guest's own save (M7).
///
/// A guest carries the host's event flags while in the host's world and gets
/// its own back at home (docs/DS2_WORLD_STATE.md). The host's progress from
/// before the session arrives in the join snapshot and must not follow the
/// guest home; what changes during the session arrives flag by flag, over P2P
/// packet 0x20. This hook watches that packet on the guest, keeps the
/// **global** flags it applied (the ones the game itself refuses to take from
/// anyone but the host, FUN_14025cdb0), and sets them through the game's own
/// setter once the guest is standing in its own world again. Map flags (doors,
/// levers, the world's objects) stay behind, as the brief says.
///
/// The kept flags are written to `DS2_Carry.pending` as they arrive, so a
/// client that dies before reaching home applies them on its next arrival.
///
/// `DS2_Carry.req` takes one order per line and `DS2_Carry.log` answers:
///
///   flag <id> <0|1>   sets a flag through the game's setter (what an event
///                     does; in a session the host's setter sends it)
///   le <id>           reads a flag
///   status            what is kept and whether it can be applied now
///   limpa             forgets what is kept
class DS2_ProgressCarryHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};

/// Called every frame on the game's thread by DS2_PartyHook.
void DS2_ProgressCarry_Tick();
