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

/// Makes the game believe the player is always inside a multiplay zone.
///
/// Dark Souls II refuses summon signs and invasions wherever the player is not
/// standing in one, which is what stops multiplayer in Majula and other hubs.
/// Measured in a live session, the zone id is the whole check: Majula reports
/// -1 and Heide reports a real zone, while the permissions both carry are
/// identical. This substitutes a real zone whenever the game reports none.
class DS2_ForceMultiPlayZoneHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
