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

/// Reports the multiplay zone the player is standing in, and what it permits.
///
/// Dark Souls II decides whether summoning is allowed from the zone rather than
/// the area, so this reads the zone id and its permission byte as the game
/// computes them. It observes only; nothing is changed.
class DS2_MultiPlayZoneProbeHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
