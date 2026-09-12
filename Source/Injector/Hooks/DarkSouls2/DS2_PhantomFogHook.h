/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Injector/Hooks/Hook.h"

/// Keeps every fog wall in the mode it has when nobody is visiting.
///
/// A fog wall is a `MapObjWhiteDoorComponent` and carries a mode byte at
/// `this + 0x85`. Measured on a running game: every door reads `0x14` while
/// the player is alone in a world and `0x0a` from the moment a phantom is in
/// it — on the host's client as well as the guest's — and `0x0a` is the branch
/// that runs the door's timer instead of zeroing it.
///
/// Two instructions write that byte, one in the door's init and one in its
/// per-frame update, and both become `mov al,0x14`. Patching only the first
/// achieves nothing: the update rewrites it on the next frame.
///
/// An earlier attempt patched a different field, `this + 0x80`, which carries
/// the local player's phantom type. It changed nothing, and a reading already
/// in hand said why: the host owns his world, his doors carry 0 there in every
/// state, and the barrier stops him too.
///
/// This is still an experiment. If a fog wall pinned to `0x14` leaves the
/// barrier standing, the mode is not the lever either.
class DS2_PhantomFogHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
