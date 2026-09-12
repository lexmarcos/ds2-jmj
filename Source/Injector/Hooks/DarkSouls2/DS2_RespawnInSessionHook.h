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

// A guest who dies in a host's world comes back **into it**, instead of the
// session being torn down.
//
// Dark Souls II has no way to stand a dead player up where they fell - that is
// Tears of Denial, a Dark Souls III ring, and looking for its equivalent here
// wasted a day. What the game does have is the load it performs for a guest
// *joining* a host, and a session object that survives one: state 2
// (`FUN_1402c2a80`) loads an area with the session alive and comes out the
// other side still in the session. So a death is not answered with a
// resurrection; it is answered with **the same request the join uses**.
//
// The death of a phantom has its own terminal. A selector at `0x14018ffcf`
// reads the role from `record+0xe0`, looks it up in the twenty-entry table at
// `0x1410c0050`, and sends an ordinary death to `FUN_140190920` and a
// phantom's to `FUN_140190950`. That second one already has the shape this
// needs - ask the session layer to act, then mark the record done - and only
// the destination is wrong:
//
//     if (there is a session)  end it, reason 2      <- what this replaces
//     else                     FUN_14044fde0(record) <- respawn at a bonfire
//
// The replacement is the state-2 request, byte for byte: kind 0, motive 4, the
// host's map from `session+0x19c`, the flavour byte the join computes from the
// role, the position, and the flag **1**. The form matters: the warp announces
// itself to the session layer (`FUN_1402c7ec0`), and motive 4 with the flag
// set is the one those listeners are known to tolerate, because it is the one
// a join already sends.
//
// Off unless `DS2_Respawn.req` says otherwise, because if the host's copy of
// the phantom cannot be stood back up by sync, this leaves a corpse walking.
class DS2_RespawnInSessionHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
