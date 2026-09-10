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

/// Says which of a set of functions actually ran.
///
/// Comparing memory between two areas has answered what the game holds, not
/// what it does. This answers the other question: arm a list of addresses,
/// press the button, and read back which of them were reached. Running it once
/// where an item works and once where it is refused shows where the two paths
/// part, which is the thing no amount of diffing has found.
///
/// Breakpoints are one-shot. The first hit records the address and puts the
/// original byte back for good, so nothing is single-stepped and a hot
/// function costs one exception rather than thousands - the mistake that took
/// the game down when a guard page was tried for the same purpose.
class DS2_TraceHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
