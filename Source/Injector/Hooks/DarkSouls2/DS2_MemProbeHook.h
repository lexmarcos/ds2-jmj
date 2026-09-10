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

/// Reads the game's own memory on request, from inside the process.
///
/// The game's address space can be read from Linux through /proc/<pid>/mem,
/// but only by an ancestor of the game process, and Steam reparents it away.
/// This does the same job from inside, where no such rule applies: it answers
/// requests dropped in a file next to the injector, and dumps the map block
/// and the multiplay zone state by itself whenever the player changes area,
/// which is what a Heide-against-Majula comparison needs.
class DS2_MemProbeHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
