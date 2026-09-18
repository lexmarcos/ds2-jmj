# World state in a session (M4)

M4 asks that doors, levers, elevators, shortcuts, illusory walls and Pharros
opened by the host appear open to whoever joined, and that this does **not**
carry into the guest's world. Before building anything, the question is what
the game already does. This file is what was measured on 15/09.

## Status: paused on 15/09 — how to resume

**Why it stopped.** Everything that can be measured without playing has been
measured. What is left needs a mechanism neither character has triggered, and
in Heide there is none: flags and object state are the same in both worlds.
Finding and triggering one takes manual testing, and that was not available.

**What is already done** (details in the sections below):

1. The guest receives **all** of the host's event flags on entry, in place of
   his own, and gets his own back at home — measured with bits set on one side
   only.
2. The entry is a **snapshot** that the host exports and the guest imports in
   the warp (`FUN_1402bf8f0` → `FUN_1402c2fa0`, one hit each, measured), and
   it also carries `EventValueManager`, `EventBonfireManager`,
   `MapStateActManager` and `EnemyGeneratorDeadCounter` (read).
3. In a session the host propagates every flag that changes through the P2P
   packet `0x20` (`FUN_140474a60` → `FUN_14051e6b0`); the guest can only
   change map flags (`FUN_14025cdb0`) (read; no flag changed in the sessions
   measured).
4. The guest's save records the object state of his own world, not the host's
   (`SaveDataObj`, read).
5. Tools: `ds2os-dev flags` (reads and compares both accounts' flags) and
   `up --party --party-host 2` (Chico hosts).

**The next step, exactly.** With a mechanism that has not been triggered in
the host's world (lever, door, elevator, illusory wall):

1. `up --seamless --keep-fog --party [--party-host N]`, `death --instance
   <host> mode respawn` if there is a fight;
2. a trace in both installations: `bp 474a60` (flag in `rdx`, value in `r8`),
   `bp 25ce10 deref rdx 8`, `bp 25cec0 deref rdx 8` and `bp 240270` (a
   `StateActCtrl` changing state, new state in `rdx`);
3. `ds2os-dev flags` and the object state (below) on both, before;
4. **(a)** the host triggers it in session → hits and differences on the
   guest; **(c)** `session end` → the guest at home goes back to his own
   state; **(b)** `retoma` → a new entry → the guest sees it triggered with
   no `0x20`.

Mechanisms are single-use in the save; the order (a), (c), (b) reuses the same
one.

## Map object state, from memory

The objects with a state machine are `MapObjStateActComponent` (vftable
`0x1410c6d78`, with `0x1410c6dc8` at `+0x30`): `scan 1410c6d78 8 400` finds
about 120 in Heide. In each component, `+0x08` is the `MapEntity` (position at
`+0x70`, `float` x, y, z; `*(+0x28)+8` the map id) and `+0x48` the
`StateActCtrl` (vftable `0x1410cf668`), where `+0x1c` is the current state,
`+0x1d` the index and `+0x1f` a mark. Bonfires are objects of this kind: 1.1 m
from each one's spawn point, state `30` when lit.

**A misreading, so as not to repeat it.** The bonfire `0x7bac`, 170 m from
Heide's Ruin, read `10` on one client and `30` on the other, and looked like a
mechanism only one of them had triggered. It was not: whoever **came back from
a session** reads `10` on distant objects (with `+0x1f = 2`) until they load
again, and whoever loaded the map from scratch reads the saved state. Both
characters have Heide's three bonfires lit. Only compare objects with
`+0x1f = 0`, or after a clean load on both.

The `StateActCtrl`'s `SetState` is slot `+0xe8` (`FUN_14023ff60` →
`FUN_1402410d0` in the manager `*(*0x1416148f0+0xa0)+0x280`); the current state
comes out of slot `+0x50`. The snapshot import applies `(índice, estado)`
pairs through that slot (`FUN_1401f30e0`).

**Careful with teleports.** A `MapEntity`'s origin is not ground: a teleport
to the grid of objects in the water west of Heide's Ruin killed Samuel (hollow
3 → 4, billed by `respawn` mode). Use `teleport --to-bonfire` or `goto`.

## The game already syncs event flags over P2P

The P2P packets are registered in `FUN_14051e3d0`, and type `0x20` is
`NetP2pPacketEventFlag` (vftable `0x1410fb688`, 7 slots). Its neighbours:
`0x2a` AiFlag, `0x2b` AiData, `0x2c` AiGroup, `0x31` SpEffect.

| function | role |
| --- | --- |
| `FUN_140474a60(mgr, flag, valor)` | **the game's setter**. In a session, a client that is not the host (`FUN_1405135f0`) only writes the flags `FUN_14025cdb0` accepts; if the flag changed and there is a session, `FUN_14051e6b0` sends it |
| `FUN_14051e6b0` | sends 8 bytes (`uint32 flag`, `byte valor`) through the packet's slot `+0x18`, `FUN_14025cec0` → `NetSvr` slot `+0x78`, type `0x20` |
| `FUN_14025ce10` | receives: 8 bytes; if the sender is not the host, it goes through the same filter; writes with `FUN_1404750b0` |
| `FUN_1404750b0(mgr, flag, valor)` | writes the bit and calls the listeners (`FUN_140184ff0`) when it changes |
| `FUN_14025cdb0(flag)` | the filter: refuses `flag < 1e9` (and the `1e9..` block) whose `(flag/10000) % 100 > 2`, that is, the **globals**; accepts the map ones (`1310xxxxx` in Heide) |

So in a session **the host sends any flag**, and the guest can only change map
flags.

## Where the flags live

`mgr = *(*(*0x1416148f0 + 0x70) + 0x20)` (`EventFlagManager`, vftable
`0x1410eff58`). At `mgr + 0x20` there is a table of 31 buckets indexed by
`((flag / 10000) * 0x89) % 0x1f`; each node has `+0x00` a pointer to the
bytes, `+0x08` the size, `+0x0c` the category (`flag / 10000`) and `+0x10` the
next one. The bit is `flag % 10000`, from the highest to the lowest within
each byte.

Through the harness: `chain a 16148f0 70,20 312` reads the manager; the nodes
and the bytes come out with `abs`. In Heide only five categories are loaded:
`10` and `20` (1250 bytes each, the globals) and `13100`–`13102` (25 bytes
each, the map). Samuel and Chico had exactly the same bits (34 in `10`, 18 in
`20`, 2 in `13100`), so comparing the two says nothing on its own.

`FUN_1404744b0` empties the table (a map change) and writes into `mgr + 0x118`
whether the client is in a session as a non-host.

## What was measured

With both in Heide, `up --seamless --party`, a trace on `25ce10` and `25cec0`
(`deref rdx 8`) in both installations:

1. **Entry: no flag packet.** Neither did the host send nor the guest receive
   a `0x20` when the session formed.
2. **The guest receives the host's map flags on entry.** With the session torn
   down, the bit `131000199` (no known use) was set **only in the host's
   memory**. Once the session formed, the guest had the bit. Since no `0x20`
   crossed, the copy on entry comes from another path, not found yet.
3. **The guest does not take the flag home.** After `session end`, in his own
   world, the bit was off on Chico and on for Samuel (and was switched back
   off in Samuel's memory afterwards).
4. **The globals come along too.** The same test with the last bit of category
   `10` (`109999`): set only on the host, present on the guest in the session,
   absent on the guest at home, switched back off on the host.

5. **It is a swap, not a merge.** The reverse: `109999` set only on the guest,
   at home. In the session the guest had the bit **off**, like the host; back
   at home, on again (and switched off in Chico's memory afterwards).

The guest **carries the host's flags** on entry, in place of his own, and gets
his own back on returning.

That is the behaviour the design asks for with doors and levers, **if** they
are map flags: the guest sees the host's world, and his own world does not
change.

## Where the copy on entry comes from

It is not the `0x20`: it is a **world snapshot** that the host serialises and
the guest imports in the entry warp.

The `EventFlagBuffer` (`*(mgr + 0x18)`, vftable `0x1410c2fa8`) holds **two**
copies, cleared separately by `FUN_140185320(buf, 0|1)`:

| copy | globals | maps (3 × 25 bytes, with the map id at `+0xe7c` / `+0x1930`) |
| --- | --- | --- |
| 0, his own world | `+0x008` and `+0x4ea` (1250 bytes each, categories `10` and `20`) | `+0xde6`… |
| 1, someone else's world | `+0xe88` and `+0x136a` | `+0x184c`… |

`FUN_1404744b0`, when it empties the manager on a map change, writes into
`mgr + 0x118` whether the client is a guest in a session, and `FUN_1404746b0`
uses that byte to say in which copy the departing map is discarded
(`FUN_140186480`).

- **Exports** (host): `FUN_140185ac0` copies copy 0 into a blob, via
  `FUN_140474570`, called from `FUN_1402b6880` and `FUN_1402bf8f0`.
- **Imports** (guest): `FUN_140185da0` copies the blob into copy 1 and
  `FUN_140184f70` notifies the listeners, via `FUN_140474590`, called from
  `FUN_1402b9ad0` and `FUN_1402c2fa0` — virtual methods neighbouring the entry
  warp's constructor (`FUN_1402c2a80`).

Measured on one entry (15/09, 08:36, a trace on `474570` and `474590`): the
host went through `FUN_140474570` coming from `+0x2bfbfc` (`FUN_1402bf8f0`)
and the guest through `FUN_140474590` coming from `+0x2c3080`
(`FUN_1402c2fa0`), both with `r8 = 0a1f0000`, Heide's map. One hit each, and
no `0x20`.

`FUN_1402c2fa0`, when `+0xf8 == 4`, imports from the same blob everything the
design calls the host's world. The destinations, named by the RTTI of the
vftable read in memory (`ctx = *0x1416148f0`):

| blob | destination | class |
| --- | --- | --- |
| `+0x3108` | `*(ctx+0x70)+0x20` | `EventFlagManager` — the flags, measured above |
| `+0x3b18` | `*(ctx+0x70)+0x28` | `EventValueManager` (`FUN_14047a350`); `FUN_14047a430` fills it with the flags `10010000`–`10019999` |
| `+0x59ec`, 16 bytes | `*(ctx+0x70)+0x58` | `EventBonfireManager` (`FUN_14017ea40`) |
| `+0x3000` / `+0x3004` | `*(ctx+0x38)+0x1f8` | `MapStateActManager` (`FUN_1401f3000`, `FUN_1401f30e0`) — map object state |
| `+0x4018` | `*(ctx+0x40)+0x10` | `EnemyGeneratorDeadCounter` (`FUN_1401f69e0`) — enemy despawn |

`*(ctx+0x70)` is the `EventManager`; `+0x10` inside it is the
`EventTaskManager`.

So the original game's entry is already **an authoritative snapshot of the
host**: flags, event values, bonfires, object state and enemy kills. What M4
needs to measure is how much of that reaches the object on screen and what
changes **after** the entry, which only the `0x20` (flags) covers as far as
has been seen.

## Object state in the guest's save

The save keeps map object state in a `SaveDataObj` (vftable `0x1410da378`): up
to three maps, 0x6008 bytes each, version `0x69`.

- **Writes** (`FUN_1402e5a10`, slot `+0x10`): for each map kept in
  `*(*(ctx+0x38)+0x200)+0x18`, if the client is **not** a guest in a session
  and the map is loaded, it serialises the live objects (`FUN_1401f22c0`); if
  it is a guest, it writes the stored copy (`FUN_1401e7450`) — his world, not
  the host's. Then `FUN_1401f2ea0` on the `MapStateActManager`.
- **Reads** (`FUN_1402e5890`, slot `+0x18`): up to 3 blocks into
  `FUN_1401e7a10`, then `FUN_1401f2ce0`.

Read, not measured: this is the game making sure a guest does not save the
host's doors and levers into his own save, which is what the design asks for.

## What is still unknown

- **Whether doors, levers, elevators and illusory walls are map flags.**
  `MapObjStateActComponent` (vftable `0x1410c6d78`) may keep per-object state
  outside the `EventFlagManager`. No real mechanism has been triggered yet.
- ~~**A flag change made during the session.**~~ Measured in M7 (15/09): the
  host's setter (`DS2_Carry.req`, `flag <id> 1`) reached the guest in 20 ms,
  global and map alike. What is missing is an **object** changing state live.
- **What the guest sees of a lever he pulls himself** in the host's world: a
  map flag gets through the filter, but the object may have another lock.
- **Changes after entry other than flags.** `MapStateActManager`,
  `EnemyGeneratorDeadCounter` and `EventBonfireManager` arrive in the
  snapshot; whether they have a packet of their own during the session is
  unknown.
- **The opposite side of the design.** "What you do together is saved for
  everyone" (a boss killed in the session stays dead in the guest's world) is
  exactly what the game does **not** do: the guest's world comes back as it
  was. That is work for a later milestone, not M4.
