# Every place the session and net subsystems remember a map id

Research made on 20/09 in Ghidra (`-noanalysis -readOnly`), sweeping the whole
net module (3,976 functions, `0x140248000–0x1402d8000` and
`0x140510000–0x140525000`) for every producer of a map id and every consumer
that turns one into a pointer into a loaded map. Nothing was run in either
game. **[read]** / **[inferred]**.

The completeness criterion: **a stored id that no consumer ever feeds to the
map system cannot dereference freed memory.** So the sweep is of consumers,
not of fields.

## Two corrections first

1. **`*(ctx+0x22f0)` and `*0x141616cf8` are different objects.** The first is
   0x3d8 bytes, the network subsystem's lifecycle root — call it **NetMan**.
   The second is a separate 0x70-byte object. They looked like one because
   the `FUN_140513xxx` accessors are *called* with `ctx+0x22f0` in `rcx` and
   then **ignore `rcx` and read the global** **[read]**.
2. **`docs/DS2_FOG_GATES.md` calls `ctx+0x22f0` "the FeManager".** It is not;
   it is the net lifecycle root, and RTTI gives it no name because the
   derived class overrides nothing.

## The tree, named

`FUN_140513be0(NetMan)` builds all of it in one function **[read]**:

| slot | size | class |
| --- | --- | --- |
| `cf8[0]` `+0x00` | 0x2d0 | **NetSessionManager** |
| `cf8[2]` `+0x10` | 0x1b0 | **NetSyncDataManager** |
| `cf8[3]` `+0x18` | 0x100 | multiplay-ctrl holder; `+0x40` is the live join ctrl |
| `cf8[4]` `+0x20` | 0x2500 | network player manager; `+0x5b8` is the watcher pointer |
| `cf8[5]` `+0x28` | 0x1a0 | **NetEnemyManager** — the object sync |
| `cf8[6]` `+0x30` | 0xf0 | **NetSvrManager** |
| `cf8[7]` `+0x38` | 0xa8 | **NetSvrStateChartManager** |
| `cf8[8]` `+0x40` | 0x50 | **NetParamContainer** |
| `NetMan+0x3b0` | 0x2a0 | **NetNpcPhantomManager** |
| `NetMan+0x3c0` | 0x100 | **NetPlayerWatcher** |

`*(cf8[4]+0x5b8)` is `NetMan+0x3c0`, so the chain the whole net layer reads —
`*(*(cf8[4]+0x5b8)+0xc)` — **is `NetPlayerWatcher+0x0c`** **[read]**.

## The ranking

### Rank 1 — would crash a live session

**`NetEnemyManager+0x18`** (the bound map) and **the record block pointers at
`+0x10`**. If the bound map's owner reaches state 0 and its table slot is
zeroed, `FUN_1403bce40` still returns the owner struct — it is a slot, not
freed — and `FUN_1405177c0` does
`*(int *)(*(longlong *)(owner + 0x168) + 0x18)` **with no null test**
**[read]**. That is exactly what `DS2_NetSyncGuardHook` patches. The host's
unbind additionally walks all 255 records and writes into the freed array.

> **A nuance that narrows the problem.** The stale `+0x18` an unbind leaves
> behind is **not reachable afterwards**: the packet dispatch gates only on
> the array pointer, but `FUN_140518920` validates every record index against
> `+0x0c`, which the unbind zeroes **[read]**. So after an unbind every `0x14`
> is rejected and `FUN_1405177c0` is never called.
>
> **The danger window is "bound while the map goes", not "unbound with a
> stale id".**

### Rank 2 — would misbehave, probably not crash

**`NetSummonJoinMultiplayCtrl+0x19c`** (duel: `+0x194`). Read at *every*
guest rebind, so left alone the guest rebinds straight back to the join map.
`FUN_140517880` does check the table lookup, so a **zeroed** slot gives a
silent empty bind and a slot not yet zeroed gives a use-after-free — which
one depends on the release path **[read + inferred]**.

### Rank 3 — new, and not the one anybody was guarding

`FUN_1403bce00(mm, mapIndex)` turns a record's map-relative coordinates into
world coordinates by adding the owner's origin at `+0x2f0`. It has five
callers in the net band and **three do not null-test the owner** **[read]**:

| site | in |
| --- | --- |
| `0x14026d9cc` | `FUN_14026d5f0` ← `FUN_14026b200` |
| `0x140267af8` | `FUN_140267610` ← `FUN_1402654d0` |
| `0x1402a6607` | `FUN_1402a6560` ← `FUN_1402a6720` |

The map id comes from a **server record** — signs, bloodstains, ghosts,
messages. In vanilla this never fires, because the client only ever holds
records for the area it is standing in. **If a released map's records are
still cached and one of those handlers runs, the lookup returns 0 and the
code faults near null** **[read for the code, inferred for the scenario]**.

That is a hazard specific to releasing the meeting map, and it is **not** the
one `DS2_NetSyncGuardHook` covers.

### Harmless, and named so nobody hunts them again

- The guest's **return-home record**, `SummonCtrl+0x1a0 … +0x1c4` (duel:
  `+0x198 …`): consumed only as a warp target, and a warp loads its
  destination **[read]**.
- The hard-coded fallback at `SummonCtrl+0x1b8`, which is
  `FUN_1403bf510("m10_02_00_00")` **[read]**.
- **`NetPlayerWatcher+0x0c`**: rewritten every frame from the locally loaded
  map, and its ~13 readers all copy it into a local **[read]**. It cannot go
  stale.
- `NetPlayerWatcher+0x10/+0x14/+0x18/+0x20/+0x24`: **not map ids** — region
  index, last valid region, region param id, the region's multiplay byte.
- The P2P coordinate codec: carries a map index per packet, null-tests the
  owner both ways **[read]**.
- `NetNpcPhantomManager+0x248…+0x288`: **not map ids**, param-row fields.
- `NetSummonAcceptMultiplayCtrl+0x19c…+0x1a8`: **not a map id**; its `+0x1b8`
  is a bitfield that collides numerically with the summon ctrl's map field
  and has caused confusion before.

## What this could not pin down

- **`NetMan+0x3c8` (u64) and `NetMan+0x3d0` (u32)**, both initialised to
  `-1`. No reader or writer found in the surrounding band. The sentinel makes
  them *look* like cached ids. **Unresolved.**
- `NetSessionManager`, `NetSyncDataManager`, the rest of `cf8[3]` and
  `cf8[4]`'s player records: **no map-id field found**, and the positive
  evidence is that no function in their bands calls any map consumer. A map
  id stored there but never consumed would be harmless by construction — but
  it is **not positively excluded**.
- `FUN_1402c03e0`, which earlier docs call the host's `0xf` handler and
  attribute a `+0x19c` read to: it is **not** in
  `NetSummonJoinMultiplayCtrl`'s vftable, and its usage matches the accept
  ctrl. **That attribution should be treated as unverified.**
- Which record type each of the three unguarded handlers serves.
