/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Injector/Hooks/Hook.h"

/// The net thread stops reading a map's character table after the map is gone.
///
/// FUN_1405177c0 answers "what is the state of character <index> of the map
/// this record names". It reads the map id from param_1+0x18, asks the map
/// manager who owns it, and then takes one of two tables out of the owner:
///
///   +0x5177ff  mov rcx,[rax+0x160]   ; test rcx,rcx ; je -> returns false
///   +0x517839  mov rcx,[rax+0x168]   ; NO check
///   +0x517843  mov r8,[rcx+0x10]     <- faults with rcx == 0
///
/// The owner survives its map: the 38 map owners exist for the whole run and
/// only their tables come and go. So when the backread hook releases a map,
/// +0x168 becomes null while the lookup still succeeds, and the very next
/// object packet faults. Measured 17/09: five crashes in a row at +0x517843,
/// each 167-173 ms after the line saying a map was about to be released, all
/// with rcx zero and the same fossil map id in rdx.
///
/// The fix is the check the sibling path already has. Returning false is the
/// game's own answer for "no such record" (its caller at +0x518d64 tests al
/// and branches away), so nothing downstream sees anything new.
///
/// It fits without moving the call that follows, because ebx already holds the
/// zero-extended index from +0x5177d1 and nothing writes bx in between: the two
/// `movzwl %bx` that the compiler emitted are redundant, and dropping them pays
/// for the test and the branch.
///
/// Installed at inject time, never from a tick: the 24 byte write is not
/// atomic and the net thread runs this code as soon as a session exists.
class DS2_NetSyncGuardHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
