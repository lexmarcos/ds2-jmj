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

// Where the game faults, written down before it dies.
//
// A client that closes leaves nothing behind: Proton reports exit code -1 and
// the Steam log says the process stopped. Measured on 14/09, when a guest's
// game closed a moment after its respawn brought in another map, there was
// no address to start from.
//
// A vectored handler sees every exception first, so this one writes an access
// violation, an illegal instruction or a stack overflow whose instruction is
// in the game's own image to `DS2_Crash.log` - the instruction's offset, the
// address it touched, the registers, and the return addresses into the game
// found on the stack - and lets the exception go on to whoever handles it.
// Faults in the injector's own guarded reads never get here (their instruction
// is not in the game), and at most 32 are written per boot.
class DS2_CrashHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
