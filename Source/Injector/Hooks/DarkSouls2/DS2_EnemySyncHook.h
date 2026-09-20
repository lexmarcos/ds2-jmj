/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Injector/Hooks/Hook.h"

#include <cstdint>
#include <string>

/// The enemy replication table, and the one thing that must happen before its
/// map goes away.
///
/// `NetEnemyManager` is `*(0x141616cf8 + 0x28)`, vftable `0x1410fb580`. Its
/// `+0x08` is the state (0 unbound, 1 host, 2 guest), `+0x0c` the record
/// count, `+0x10` the fixed 255-slot table, `+0x18` the map the records belong
/// to, `+0x74` the armed byte and `+0x198` the guest's gate.
///
/// Each in-use slot holds a **raw pointer** into a per-map array of 0xa0-byte
/// records. Measured 20/09 with a live session: 149 records, 0xa0 apart, host
/// in state 1 and guest in state 2 on the same bound map. That array is freed
/// by the map path; the pointers are nulled only by a session-state
/// transition, and there is no call from one to the other. So releasing the
/// bound map with the sync still bound leaves up to 255 records writing into
/// freed memory, every tick, on both roles.
///
/// That is the mechanism `docs/research/risk-4-six-readings.md` ends on, and
/// it is dormant today only because the bound map **is** the session's map,
/// which `DS2_BackreadHook` refuses to release. M8 item 6b is the act of
/// lifting that refusal, so this is the piece that has to exist first.
///
/// The hook sits at the entry of `FUN_140416ac0(chrMgr, map)`, the per-map
/// character release the teardown reaches at its case `0x0d` — the last place
/// the table is still whole. If the map being torn down is the one the sync is
/// bound to, the sync is unbound there, before anything of that map is freed.
///
/// The three primitives, read from the binary on 20/09:
///
///   * `FUN_140517080(mgr)` unbinds: it takes the object at `+0x78` (the
///     shared lock), clears through it, and branches on the state at `+0x08`;
///   * `FUN_140517040(mgr)` arms: the same opening, then `+0x74 = 1` and
///     `+0x198 = 0`;
///   * `FUN_140516370(mgr)` is two instructions, `+0x198 = 1` and `ret` — the
///     guest's gate, and nothing else.
class DS2_EnemySyncHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};

namespace DS2_EnemySync
{
    /// The map the records belong to, or 0 when nothing is bound. Believed
    /// only with the manager's vftable.
    uint32_t BoundMap();

    /// 0 unbound, 1 host, 2 guest.
    uint32_t State();

    /// How many records are bound.
    uint32_t Count();

    /// Unbind now, on the game's thread. False when there was nothing bound.
    /// `Why` goes in the log beside it.
    bool Unbind(const char* Why);

    /// Arm the sync again. The guest also needs `OpenGuestGate()` **after**
    /// this, because arming clears the gate.
    bool Arm(const char* Why);

    /// The guest's gate byte at `+0x198`.
    bool OpenGuestGate();

    /// Step 8 of M8 6b: has this map's enemy generator table been **built**,
    /// not merely allocated?
    ///
    ///     table = *(mgr + 0x20 + index*8),  mgr = *(*0x1416148f0 + 0x40)
    ///     ready = table != 0 && *(int32_t*)(table + 0x24) == mapId
    ///
    /// `+0x24` is the only field that separates the two: the ctor writes
    /// `0xffffffff`, the build writes the real map id as its first act, and
    /// the free writes `0xffffffff` back. **Do not test the block count** — a
    /// map with no generators legitimately gets a valid table with none.
    ///
    /// The index is the backread owner's own `+0x0c`, which the game's lookup
    /// reaches through an Arxan trampoline that cannot be read statically.
    /// That the two are the same index space is measured, not assumed:
    /// on 20/09 every loaded map on both instances resolved through
    /// `DS2_Backread::IndexOf` to a table whose `+0x24` was its own map id.
    ///
    /// Measured the same day: the table is ready with the owner still at
    /// **state 4**, so state 5 is a late trigger, not an early one. Poll this
    /// with a hard timeout and treat the timeout as failure — there is no
    /// retry anywhere in that path.
    bool TableReady(uint32_t Map);

    /// The count of generator list 7, `mgr+0x3c6`: "something is still dying
    /// from the last map change". The create queue only drains while it is 0.
    uint32_t DyingCount();

    /// Step 9 of M8 6b: arm the sync again and, on a guest, open its gate -
    /// in that order, because arming writes `+0x198 = 0` and the gate is what
    /// puts it back. `Guest` is the caller's answer to "is this machine a
    /// guest in a session"; the sync's own state cannot be asked, because
    /// after an unbind it reads 0 on both roles.
    ///
    /// What arming does **not** do is bind: `FUN_140517040` writes `+0x74 = 1`
    /// and clears the gate, and nothing else. Which map the records come back
    /// for is decided elsewhere, and that is measured rather than assumed.
    bool Rearm(bool Guest, const char* Why);

    /// Everything worth reading at once, for the log: state, bound map, count
    /// and the first few record pointers.
    std::string Describe();

    /// The four-slot create queue at `mgr+0x332`; 0xff is an empty slot. A
    /// fifth request while all four are taken is dropped with no retry.
    void CreateQueue(uint8_t Out[4]);
}
