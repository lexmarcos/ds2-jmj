/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Injector/Hooks/Hook.h"

/// Keeps every fog wall in the state it has when nobody is visiting.
///
/// `FUN_1401d24a0` decides what a fog wall should be, from the door's kind and
/// the field at `this + 0x80`, and every branch that matters turns on that
/// field being non-zero. Measured on a running game it reads 0 while the player
/// is alone in a world and 5 from the moment a phantom is in it — on the host's
/// client as much as the guest's, which is what makes it the session and not
/// the player.
///
/// Two instructions write it, one in the door's init and one in its per-frame
/// update, and both become `xor eax,eax`. An earlier attempt patched only the
/// init: the update wrote the value back on the next frame, which is exactly
/// the kind of failure that looks like a wrong hypothesis and is not.
///
/// The mode byte at `this + 0x85` also moves with a session. Pinning it changed
/// nothing, so it is left alone.
class DS2_PhantomFogHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
