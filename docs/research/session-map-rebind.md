# Moving the enemy sync to another map, so the session's map can be released

Research made on 20/09 in Ghidra (`-noanalysis -readOnly`) and `objdump`,
against `DarkSoulsII.exe` 1.03 / Calibrations 2.02. Nothing was run in either
game. Addresses are Ghidra's at image base `0x140000000`; in a hook, resolve
them from the module base.

Each finding is **[read]** (the decompiled line or the bytes are quoted) or
**[inferred]**.

This is M8's item 6b. The map a session began in is pinned for the whole
session, so its target cost is spent the whole time and a heavy pinned map
blocks a heavy destination. Releasing it means moving the **enemy object
sync** — `sync = *(0x141616cf8 + 0x28)` — to the destination first. This
document is the recipe for that, and it corrects four things
`docs/research/phantom-map-border.md` and `object-table-lifecycle.md` had as
inferences.

## The four corrections, first

1. **`*(sync+8) = 1` / `= 2` is written by the caller, with no test of whether
   the bind bound anything** (`0x14051718e`, `0x1405171b9`) **[read]**. So an
   arm made before the destination's table exists leaves the sync in state 1
   or 2 with **zero** records, and there is **no retry and no timeout**. The
   sync is then dead for the rest of the session. That is the "sync stays
   closed" symptom already recorded, with its cause.
2. **The host writes `sync+0x18` only when the streamer has a current part**
   (`0x140517c2f`, guarded) **[read]**, while **the guest writes it
   unconditionally** (`0x1405178c4`) **[read]**. Poking the field is useless
   on the guest and misleading on the host.
3. **Bit 48 of `block+0x3c` means "the enemy object exists", not "I own it"**
   **[read]**. Ownership is `record+0x0a` bit 4.
4. **The guest's destination table is default-initialised from map data**
   (`FUN_14040f0f0` / `FUN_14040f590` / `FUN_14040f5d0`), not imported from
   the host and not from its own save: `FUN_14041a5f0` skips the saved-status
   fetch on a guest **[read]**. The generator *list* matches; the enemy
   *state* need not. So "records pair by index" is not something to assume —
   compare the per-record generator ids at `sync+0x38` instead.

## The sync, field by field

| off | meaning |
| --- | --- |
| `+0x08` | state: 0 idle, 1 bound as host, 2 bound as guest |
| `+0x0c` | record count |
| `+0x10` | record array, 255 × 0x18 |
| `+0x18` | bound map id |
| `+0x20`, `+0x40`, `+0x58` | three trees |
| `+0x38` | u16 generator id per **record index** |
| `+0x74` | armed byte |
| `+0x78` | the lock (vt slot `+0x10` lock, `+0x20` unlock) |
| `+0x198` | guest gate byte |

A record, 0x18 bytes **[read]**: `+0x00` owner player id (`0x7fff` nobody,
`0x7f00` dead), `+0x02` pending owner, `+0x04` f32 timer, `+0x08` generator
id, `+0x0a` flags (**bit 0 = live**, bit 1 dead, bit 4 owned by me), `+0x10`
the raw pointer into the 0xa0 enemy-status block.

## Who may write what

Exhaustive by instruction scan over `0x140513000..0x14051a000` plus every
function known to hold the sync pointer **[read]**; a path reaching the sync
from neither seed would be missed **[inferred]**.

| field | where | by | value |
| --- | --- | --- | --- |
| `+0x08` | `0x14051718e` / `0x1405171b9` / `0x1405170ba` | `FUN_1405170e0` / same / `FUN_140517080` | 1 / 2 / 0 |
| `+0x0c` | `0x140517e50` / `0x140517a61` | `FUN_140517bf0` (host bind) / `FUN_140517880` (guest bind) | records built |
| `+0x0c` | `0x140517f12` / `0x140517abe` | `FUN_140517e70` / `FUN_140517a80` | 0 |
| `+0x18` | `0x140517c2f` **conditional** / `0x1405178c4` **unconditional** | host bind / guest bind | the map |
| `+0x74` | `0x14051705d` / `0x1405170c1` | `FUN_140517040` / `FUN_140517080` | 1 / 0 |
| `+0x198` | `0x140516370` / `0x140517061` / `0x1405170c5` | gate / arm / unbind | 1 / 0 / 0 |

**Only the two bind functions ever raise `+0x0c`**, only from
`FUN_1405170e0`, only in state 0, only under the lock, and only when
`FUN_140419a70(mgr, *(sync+0x18)) != 0`. No packet and no snapshot import
raises it **[read]**.

## The per-frame machine

`FUN_140514020(netCtx, dt)` runs the join controllers, then the player
manager, then `FUN_1405170e0(sync, dt)` **[read]** — so an unbind ordered by
the join-controller update lands in the same frame, before the sync update.

`FUN_1405170e0` binds only when all of these hold **[read]**: state 0;
`+0x74` set; `(&DAT_14157c3b0)[type*8] != 0` where
`type = *(DAT_141616cf8[3]+0x68)` (the table is seven pairs, types 1..5 allow
the sync, 0 and 6 do not — **which type co-op is was not established**);
`*(session+0xb4) > 1`. The host branch adds `*(session+0xa4) == 2`; the guest
branch adds `GameManagerImp` vtable slot `+0x58` ("in someone else's world")
**and** `sync+0x198`.

The unbind, in full **[read]**:

```c
FUN_140517080(sync) {
    lock;
    if (state == 1) FUN_140517e70(sync);   // host clear: walks ALL 255 slots,
    else if (state == 2) FUN_140517a80(sync); //  writes block+0x3c, hands each
    state = 0; +0x74 = 0; +0x198 = 0;      //  enemy back to local AI
    unlock;
}
```

`+0x18` is **not** reset by the unbind; the stale map id survives to the next
bind **[read]**.

## The recipe

### Host

1. **While the origin's table is still allocated** — before the owner's
   teardown reaches `FUN_140416ac0` — on the game thread: `FUN_140517080`.
   Not optional: `FUN_140517e70` writes into the blocks.
2. **Wait until the destination is the map under the host's feet.**
   `FUN_1403bcd90(mm)` = `*(*(mm+8)+0x20)` must be non-null and its owner's
   map id must be the destination. Otherwise the host rebinds to the origin,
   *successfully and silently*, against the map it is about to free — the
   host's characteristic failure **[read]**.
3. Wait for `FUN_140419a70(*(ctx+0x40), destination) != 0`.
4. `FUN_140517040(sync)`. The host needs no gate.
5. Next frame `FUN_1405170e0` binds and the caller sets state 1.
6. **Verify**: state 1, `+0x18 == destination`, `+0x0c > 0` (unless the map
   genuinely has no generators), and the first *n* entries of `sync+0x38`
   equal the guest's. If not: unbind and go back to 3.
7. **Only then** drop the origin's force byte.

### Guest

1. Same unbind. `FUN_140517a80` touches no block, so the timing is less
   sharp.
2. **Repoint the bind source.** `FUN_140517880` takes the map from
   `FUN_1402c6de0` = the live join controller's `+0xf0` =
   `*(NetSummonJoinMultiplayCtrl + 0x19c)`, read once at the top of the bind
   **[read]**. Write the destination there for the bind frame and restore it.
   Poking `sync+0x18` does nothing — it is overwritten at `0x1405178c4`.
3. Wait for `FUN_140419a70(*(ctx+0x40), destination) != 0`.
4. `FUN_140517040` **then** `FUN_140516370` — in that order, because the arm
   clears `+0x198` **[read]**.
5. Verify as on the host, with state 2.

### The window both machines share

Between the two unbinds and the two binds, **both counts must be 0**. The
packet `FUN_140516380` carries **no map id** — a type byte, a payload, a
length and a sender — and every handler addresses records by **index**
**[read]**. A host bound to Majula with 56 records talking to a guest still
bound to Brume with 56 records drives Brume's generator *k* with Majula's.
The failure is a silent mis-apply, not a crash.

## Why `FUN_140419a70(mgr, destination) != 0` is the gate

Both binds do the table lookup and, when it returns 0, **return having done
nothing** (`0x1405178d3`, `0x140517c45`) — while the caller sets the state
anyway. From there `FUN_1405170e0` takes the "already bound" branch forever
and `FUN_140518230` / `FUN_1405190a0` both loop to `+0x0c`, which is 0
**[read]**. Nothing returns the state to 0 but `FUN_140517080`, whose four
callers are all session lifecycle. Dead, silently, for the session.

The table can lag the map because `FUN_1403ca8d0` only *queues* the map index
at `mgr+0x332`, and `FUN_140417810` drains that queue only while
`*(short*)(mgr+0x3c6) == 0` **[read]**. An owner can sit in state 5 for
several frames with `FUN_140419a70` still 0.

Two refinements **[read]**: the manager slot is stored before
`FUN_14040dca0` fills the blocks, but all of it is one call on the game
thread, so a game-thread observer never sees a half-built table; and
`FUN_14040dca0` returns success without allocating when the map has no
generators, so a non-null table with a legitimate `+0x0c == 0` exists.

A sharper reason not to linger bound to a dying map, on the guest: the `0x14`
handler's `FUN_1403bce40` path dereferences `*(owner+0x168)` **with no null
test** **[read]**, and `owner+0x168` is freed at teardown step 2. That is the
fault `DS2_NetSyncGuardHook` patches, and it is reachable only through a
record with bit 0 set.

## Packets in the gap are safe; "armed wrong" is not

`FUN_140516380` takes the same lock as the update and the unbind **[read]**.
A `0x14` arriving between unbind and re-arm is dropped three times over: the
validation loop aborts the whole packet on any index `>= +0x0c`, bit 0 is
clear on every record, and `record+0x10` is 0 **[read]**. `0x15`–`0x18` each
test the index against `+0x0c` and bit 0 before touching anything.

What is dangerous is the other shape:

- **Zeroing `+0x0c` without clearing the records**, which is what the
  injector's `ForgetSyncedMap` does today, leaves all 255 `record+0x10`
  pointing into the table. Safe against packets, **not** against the host's
  own `FUN_140517e70` at the next legal unbind, which walks all 255 slots
  regardless of the count **[read]**.
- **A re-arm that bound to a map that is then freed** — the host's
  streamer-lag case above.

## What this does not establish

1. Which multiplay type co-op is, in the gate table at `0x14157c3b0`.
2. Whether `ctrl+0x19c` is safe to repoint for one frame; statically
   `FUN_140517880` is the only reader in the bind path **[inferred]**.
3. Whether the game thread holds a lock at owner teardown that inverts
   against `sync+0x78`. Calling `FUN_140517080` from inside `FUN_140416ac0`
   has not been shown deadlock-free.
4. Whether host and guest skip the same generators; the `sync+0x38`
   comparison is what settles it at run time.
5. How many frames the table lags in practice.
6. All of it is static. Nothing was run in either game.

---

# The unbind at the map free — verified 20/09

A second reading, of the proposal in
[object-table-lifecycle.md](object-table-lifecycle.md) §5: hook
`FUN_140416ac0` and call `FUN_140517080` there. **The proposal is essentially
correct**, with four corrections and two consequences it did not state — and
**the lock-inversion risk it left open is settled: no inversion is possible.**

## Names, at last

- The sync is a **`NetEnemyManager`** — ctor `FUN_140515860`, vftable
  `0x1410fb580`, RTTI locator `0x1412e2ce8` **[read]**.
- `sync+0x78` is the **same object** as `root+8`: both are written from the
  same constructor argument (`FUN_140515860` and `FUN_14051efc0`) **[read]**.
  It is created by `FUN_140512f30` and its first line writes
  `DLUT::DLLifecycleAdapter<DLKR::DLPlainLightMutex>::vftable` **[read]**.
- So the whole net subsystem shares **one** light mutex, used through vtable
  slot `+0x10` (lock, `0xffffffff` = INFINITE) and `+0x20` (unlock), and it is
  **re-entrant** — proven by the game itself: `FUN_14051f5a0` takes it and
  then calls `FUN_140517040`, which takes it again **[read]**.

## The four corrections

1. **`holder` is `*(void**)(owner+0x148)`**, the pointer stored there, not the
   address. `owner` is a `MapAreaCtrlOwner` **[read]**.
2. **`FUN_140416ac0`'s own first act is a different getter.** It calls
   `FUN_1403bb3d0(holder)` = `*(u32*)(*(holder+8)+0xc)`, a **slot index**, and
   returns early when it is `> 0x29` **[read]**. A hook must mirror that
   early-out or it will unbind on a call the original discards.
   `FUN_1403bb3c0` = `*(u32*)(*(holder+8)+8)` is the map id, used later. The
   function compares both forms against `FUN_1403ba320`, so they are the same
   id space — which is what makes the `sync+0x18 == mapId` test meaningful.
3. **The guard must test the state, not the count.** `FUN_140517080` clears
   records only when `*(u32*)(sync+8)` is 1 or 2; with state 0 it clears
   nothing **[read]**. Use `*(u32*)(sync+8) != 0`.
4. **`FUN_14041a900` is a callee at the tail of `FUN_140416ac0`**, not a
   later sibling step **[read]**. An entry detour does run before it, but for
   that reason.

And one thing the timing note got right for the wrong reason: the teardown is
**not one step per frame**. `FUN_1403cc3a0` and `FUN_1403cbb50` both spin
`do { c = FUN_1403cb1a0(owner); } while (c == 0)` **[read]**, so steps
`0x0e → 0x01` all run inside one call stack, in one frame.

## Host-only, not both

`FUN_140517e70` (host clear) dereferences `*(record+0x10)` as a raw block
pointer — `block+0x3c |= 1<<48`, then resolves the character and hands the
enemy back to local AI **[read]**. `FUN_140517a80` (guest clear) has **no raw
dereference**; it only clears **[read]**. So "it must run while the table is
alive" is a **host-only** constraint.

## The lock question, settled

The game thread holds **zero** locks when it reaches step `0x0d`. Every
ancestor frame was scanned — `FUN_1401bf200`, `FUN_1403bdf30`,
`FUN_1403dc3e0`, `FUN_1403cc3f0`, `FUN_1403cc450`, `FUN_1403cc3a0`,
`FUN_1403cb1a0`, and the map-change path — and none acquires one **[read]**.

Three independent reasons there is no inversion:

1. A thread that acquires one lock while holding none cannot be a node in a
   wait cycle.
2. The net subsystem has **one** mutex for this data, and it is re-entrant.
3. The hook creates no new order. The only order `FUN_140517080` introduces
   is *net mutex → heap*, which `FUN_1402c9540 → FUN_140517080` already does
   every frame in vanilla **[read]**.

Everything that takes the mutex **[read]**: `FUN_140516380` (the packet
dispatch, `NetEnemyManager` vftable slot `+0x08`), `FUN_1405170e0`,
`FUN_140517080`, `FUN_140517040`, `FUN_14051f5a0`, `FUN_140520ed0`, and
`FUN_1402c9540` (which holds `obj3+0xc8` first). None of the packet handlers
or their callees takes a second lock.

## The consequence the proposal did not state

**`FUN_140517080` zeroes `sync+0x74`, and that permanently disarms the sync.**
The only writer of `+0x74 = 1` is `FUN_140517040`, whose only caller is a
session event **[read]**. So **step 1 alone leaves the enemy sync dead for the
rest of the session, on both machines**. The re-arm is not an optional
follow-up; it is part of the same change.

Also: all four vanilla callers of `FUN_140517080` are session-**end** paths,
each paired with `FUN_1405205d0(root)` — which takes the same mutex and resets
a session queue. Vanilla never runs the unbind mid-session. Whether to call
the pair is a decision to take deliberately, not by omission.

## Offsets and expected bytes

| function | offset | first bytes |
| --- | --- | --- |
| `FUN_140416ac0` (detour target) | `+0x416ac0` | `48 89 54 24 10 57 41 55 48 83 ec 48` |
| `FUN_140517080` (unbind) | `+0x517080` | `48 89 5c 24 08 57 48 83 ec 20 48 8b 79 78` |
| `FUN_1403bb3c0` (map id) | `+0x3bb3c0` | `48 8b 41 08 8b 48 08 48 8b c2 89 0a c3` |
| `FUN_1403bb3d0` (slot index) | `+0x3bb3d0` | `48 8b 41 08 8b 40 0c c3` |
| `FUN_140517040` (re-arm) | `+0x517040` | `48 89 5c 24 08 57 48 83 ec 20 48 8b 79 78` |
| `FUN_1403ba320` (map id, other form) | `+0x3ba320` | `48 8b 41 28 8b 48 08 48 8b c2 89 0a c3` |
| `FUN_1403cb1a0` (the caller) | `+0x3cb1a0` | `40 56 48 83 ec 50 48 8b 05 a3 6a 21 01` |
| `FUN_140517bf0` (host bind) | `+0x517bf0` | `48 89 4c 24 08 53 41 54 41 56 48 81 ec 80 00 00 00` |

`FUN_140416ac0`'s first instruction is exactly five bytes, so a rel32 jump is
instruction-aligned; the instruction at `+0x12` is a rel32 call that a
trampoline must relocate (Detours does).

The corrected hook body:

```c
void hook(void* mgr, void* holder)
{
    void* root = *(void**)(s_base + 0x1616cf8);
    if (root != nullptr && holder != nullptr)
    {
        char* sync = *(char**)((char*)root + 0x28);
        if (sync != nullptr && FUN_1403bb3d0(holder) <= 0x29)
        {
            uint32_t Map = 0;
            FUN_1403bb3c0(holder, &Map);
            if (*(uint32_t*)(sync + 0x18) == Map && *(uint32_t*)(sync + 8) != 0)
            {
                FUN_140517080(sync);
            }
        }
    }
    original(mgr, holder);
}
```

## What this reading could not establish

- Which thread runs `FUN_140516380`. It is a vftable slot with no direct
  callers; "the packet is applied on the net thread" stays **[inferred]**. It
  does not change the verdict.
- That the teardown runs on "the game thread" is **[inferred]**:
  `FUN_1401bf200` has no code callers, only a stage vtable slot, and the other
  entry is inside the Arxan-protected region. What is **[read]** is that
  `FUN_1401bf200`'s own body calls the map-manager update *and*
  `FUN_140514020` — which takes the net mutex — in the same body.
- Whether the three case-`0x0d` calls before `FUN_140416ac0` can free a
  sync-referenced block. Each frees one object of its own sub-manager; that
  those are never the blocks is **[inferred]**.
- `FUN_1405205d0`'s semantics beyond "takes the same mutex".
- The scan matches the C idiom for a lock plus the names `Mutex`/`Critical`/
  `Lock`; an inlined spin-lock or one behind an Arxan thunk would not show.
