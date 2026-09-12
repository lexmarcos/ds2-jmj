/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Injector/Hooks/Hook.h"

/// Creates every fog wall as if the local player owned the world.
///
/// A fog wall is a `MapObjWhiteDoorComponent`, and its init stamps the local
/// player's phantom type into the object:
///
///   *(int*)(this + 0x80) = *(*(*(FeManager + 0x3b8) + 0x10) + 0x68);
///
/// Measured on a running game, that chain reads 0 for the owner of a world and
/// 5 for a red phantom, and the five doors of an area carry 0 when the player
/// is alone and 5 the moment he is a guest in someone else's world — which is
/// also the moment the barrier that pens a phantom into one area appears.
///
/// This zeroes the stamp at the source, by replacing the call that fetches the
/// type with `xor eax,eax`. Writing 0 over the field afterwards does nothing:
/// no method of the class reads it again, so whatever it decides is decided
/// while the door is being built.
///
/// It is an experiment, and an honest one only if it can fail: if the barrier
/// survives this, the stamp is a symptom rather than the cause.
class DS2_PhantomFogHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
