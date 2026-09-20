/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Injector/Hooks/Hook.h"

/// The part activation list stops calling through objects that are gone.
///
/// FUN_1403f3ae0 is the whole of it:
///
///   for (n = *(obj + 0x38); n; n = n[1]) (*(code**)(*n + 8))(n, obj, a, b);
///
/// a singly linked list of listeners, each called through slot 1 of its own
/// vftable, walked every frame from the world update (FUN_1403be060 ->
/// FUN_1403f32f0). Releasing a map leaves listeners of that map's parts linked
/// here, and the pointer stays harmless until something reuses the memory.
///
/// Measured 17/09: Brume Tower -> Majula killed the guest three times out of
/// three, about half a second after arriving, while Brume -> Heide, Brume ->
/// Iron Keep and Heide -> Majula were clean. Brume Tower is the largest map in
/// the rotation, so it is the one whose load reuses what Majula left behind:
/// once at +0x3f3b20 with the vftable reading `bded80c840bde337`, once one
/// frame deeper inside a listener that was still callable.
///
/// This walks the same list, and the moment a node's vftable is not inside the
/// game's own image it cuts the list there and stops. Everything past a rotten
/// node is only reachable through it, so nothing readable is lost, and the cut
/// is kept: the list does not grow a second head of rubbish next frame.
class DS2_PartNotifyGuardHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
