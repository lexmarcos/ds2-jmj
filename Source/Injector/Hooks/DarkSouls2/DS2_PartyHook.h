/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Injector/Hooks/Hook.h"

/// Entering a session without the ritual (M3): a summon sign placed by the
/// injector, with no soapstone in the inventory and no menu.
///
/// `DS2_Party.req` takes one order per line and `DS2_Party.log` answers:
///
///   placa <tipo>   places this player's own sign, as the soapstone would
///   status         what the sign manager holds
///
/// The order runs on the game's thread, inside a detour of a sign-manager
/// method the game already calls every frame.
class DS2_PartyHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
