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

// The one door a session leaves by, and a lever that can hold it shut.
//
// A session is a state machine; `FUN_1402c3630` runs it once a frame off the
// state in `session+0xf8`, and state 8 is the teardown that sends
// `RequestNotifyLeaveSession` and warps the guest home. Nothing reaches state
// 8 on its own: something first asks for the end through slot +0x30 of the
// session, `FUN_1402c2f20`, which writes **why** into `session+0x1cc` and
// leaves the machine to act on it.
//
// That makes the request the place to stand. This detours it and writes a line
// for every call - who the session belongs to (`session+0xd8` is the role, the
// same index the twenty-entry table uses), what state it was in, the reason,
// and the return address of whoever asked.
//
// It can also refuse. `DS2_Session.req` takes one command per line:
//
//   block <reason>   refuse the end for this reason
//   allow <reason>   stop refusing it
//   clear            refuse nothing
//   role <n|any>     only refuse for this role
//   status           write the current setting to the log
//
// Nothing is refused by default. Whether a session can survive the refusal is
// the open question of seamless co-op, and the reason this exists: see M1 in
// docs/DS2_SEAMLESS_COOP_TASKS.md.
class DS2_SeamlessSessionHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
