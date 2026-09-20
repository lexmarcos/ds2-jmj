# A phantom across a map border: what the game does, and what travel can borrow

Research made on 18/09 in Ghidra (`-readOnly`, copy `proj-a1`) and `objdump`,
against `DarkSoulsII.exe` 1.03 / Calibrations 2.02. Nothing was run in either
game. Addresses are Ghidra's, at image base `0x140000000`; offsets for code
are `address - 0x140000000`. Globals written as absolute addresses
(`0x1416148f0`, `0x141616cf8`, `0x1416751f8`) are image-relative too
(`+0x16148f0`, …) and must be resolved from the module base in a hook.

Each finding is marked **[read]** (it is in the decompiled code or the bytes,
with the line quoted) or **[inferred]** (a conclusion drawn from reads, not
measured).

## The answer first

1. **The premise does not hold in the vanilla game.** A live session does not
   let host and phantom walk out of the session's area: the repository's own
   measurement (`docs/DS2_FOG_GATES.md`, "What the boundary is not") says the
   fog that appears when a phantom joins "walls off the area the session may
   use, and stops host and guest alike". The code agrees with that barrier
   being load-bearing: the object sync binds to **one** map at the join and
   nothing rebinds it on a map change (section 3). So the game does not have a
   "unload a map with a phantom present" path to copy. Its design answer is:
   **the session's map is never unloaded while the session lives.**
2. **Inside the map owner, a natural unload and a forced release are the same
   code.** The force byte only overrides the streamer's wish. There is no
   notification, deferred free, unregister or "map left" message that one runs
   and the other skips (section 1). Dropping the force byte already *is* the
   natural unload.
3. **What the mod can borrow is the rebind, not the unload.** The object sync
   has a native unbind (`FUN_140517080`), a native re-arm (`FUN_140517040`)
   and a native guest gate (`FUN_140516370`); after those, the sync's own
   state machine binds again — the host to the map it stands on, the guest to
   the join map stored in its join ctrl (`+0x19c`), which the mod has to
   repoint for the bind frame. The proposal
   (section 6) moves the session's anchor to the destination on both machines
   before the origin goes, instead of keeping the sync closed for the rest of
   the session. This fixes the one dangling binding the code shows. It does
   **not** by itself explain the remaining `MapModelComponent` crash; section 5
   lists the other join-time bindings to the session map that are candidates.
   On the way, a latent write in today's fix turned up: after a release the
   host's legal session end runs the native unbind over the freed blocks
   (section 6).

## 1. What unloads a map, on either path

### The owner's state machine

`FUN_1403cc3f0` (owner per-frame update, called for every owner by the
streamer update `FUN_1403dc3e0` at `+0x3dc7eb`) calls the state machine
`FUN_1403cc450`. Its first lines **[read]**:

```c
if (*(char *)(owner + 0x1e9) == '\0')              // force byte
    *(owner + 0x1eb) = *(owner + 0x1ea);           // the streamer's wish
else
    *(owner + 0x1eb) = 1;
cVar5 = *(char *)(owner + 0x1eb);
switch (*(owner + 0x1e8)) { ... }                  // state
```

`+0x1ea` is written only by the streamer's mask pass `FUN_1403dc930`
(`+0x3dc9ef`, `+0x3dd004`, `+0x3dd0cb`, `+0x3dd0d4` — a scan of every
instruction touching `+0x1ea` found no other writer outside unrelated
functions) **[read]**. In the normal branch it is 1 when the owner's parts
mask computed from the player's cell graph is non-empty, 0 otherwise.

So after the mod drops the force byte, `+0x1eb` takes exactly the value it
would have had in vanilla, and the same `switch` runs **[read]**. There is no
second path.

The states **[read]**, with `cVar5` = wanted:

| state | wanted | what runs | next |
| --- | --- | --- | --- |
| 0 | 1 | load gate (see section 4), `*(+0x1f0)+0x11 |= 1` | 1 |
| 1 | 1 | when the resource's `+0x10 == 9`: `FUN_1403cbed0` builds the parts holder `+0x148` (0x370 bytes, `FUN_1403bae30`) and `+0x1f8` | 2 |
| 2 | 1 | when `*(+0x1f8)+0x10 == 4` | 3 |
| 3 | 1 | listener `*(+0x200)` slot 0 may veto | 4 |
| 4 | 1 | `FUN_1403cbdc0` activates (event flags `FUN_140452240`) | 5 |
| 4/5 | 0 | `FUN_1403cc3a0` (the teardown), then listener slot 1 | 6 |
| 6 | – | waits `*(+0x1f8)+0x10 == 0`, then `FUN_1403cc1d0` frees `+0x1f8` and `+0x148` | 7 |
| 7 | – | waits the resource's `+0x10 == 0` | 0 |

The listener at `+0x200` is zeroed by the owner's constructor
`FUN_1403cb9a0` (`param_1[0x40] = 0`) and a scan of stores to `[reg+0x200]`
in the map code range found no other writer for this class **[read]**, so it
is probably always null **[inferred]**.

### The teardown is synchronous

`FUN_1403cc3a0` **[read]**:

```
1403cc3b0  mov  %rbx,%rcx
1403cc3b3  call 0x1403cb1a0
1403cc3b8  test %al,%al
1403cc3ba  je   0x1403cc3b0          ; loop until it says done
1403cc3bc  movq $0x0,0x150(%rbx)
```

and `FUN_1403cb1a0` returns 1 only when the counter `owner+0x1e0` is `<= 0`
(`1403cb1b5 movzbl 0x1e0(%rcx),%edx ; test %dl,%dl ; jg ... ; mov $0x1,%al`).
Each call does one step and decrements the counter. So **the whole teardown
runs inside one frame, inside one call of the owner update** **[read]**. The
steps, from the top of the counter down **[read]** (`uVar2 = owner+8`, the map
id; `owner+0xc`, the map index):

| step | what it does |
| --- | --- |
| `0xe` | `FUN_14044e9b0(*(ctx+0x70), map)` — EventManager |
| `0xd` | `FUN_1401c5dd0(*(owner+0x90)+0x208, map)`; frees `+0x1a0`; `FUN_1403c88f0(*(mm+0x1b0), idx)`, `FUN_1403e41b0(*(mm+0x1a8), idx)`, `FUN_14040b7b0(*(mm+0x1c0), idx)`; **`FUN_140416ac0(*(ctx+0x40), owner+0x148)`** (the world tables, below); `FUN_1403c3160(*(owner+0x90)+0x198, +0x148)`; frees `+0x188`; `FUN_140407b40(*(mm+0x1b8), idx)`; frees `+0x180`, `+0x178` |
| `0xc`…`4` | at `0xc` `FUN_1401f3a80(MapStateActManager, map)`; frees `+0x160`; `FUN_1401f3b40(MapStateActManager, map)`; jumps the counter to 4 |
| `3` | frees `+0x158` |
| `2` | frees `+0x168` (the table `FUN_1405177c0` reads) |
| `1` | `FUN_140210ad0(*(ctx+0x90), map)`, `FUN_1403fff20(...)`, frees `+0x190`, `+0x198`, `+0x170`, nav (`FUN_140badbd0`/`FUN_140bafff0`), `+0x1d8`, `+0x1d0`…`+0x1a8`; `FUN_14044fb60(EventManager, map)`; `FUN_1401e7700(*(mm+0x200), map)` |

(`ctx = *0x1416148f0`, `mm = *(ctx+0x38)`, the map manager.)

### What happens to the network object table

Step `0xd` calls `FUN_140416ac0(world = *(ctx+0x40), holder)` **[read]**:

- walks nine lists at `world+0x370`…`+0x3b0` (counts at `+0x3b8`) and, for
  every entry whose map index `*(*(entry+0x50)+0x2c)` (`FUN_1404125e0`) is
  this map, detaches its character (`FUN_14040f480`) and marks it for
  deletion (`FUN_140316010`: `chr+0x55 |= 1`, then
  `FUN_140359890(*(ctx+0x18), chr)`);
- then `FUN_14041a900(world, idx)`, which for the table
  `world+0x20+idx*8` (the table `FUN_140419a70` returns) calls
  `FUN_14040e380`, `FUN_14040db30`, and the table's destructor, and zeroes
  the slot.

`FUN_14040db30` **[read]**:

```c
if (*(ctx+0x22f0) != 0 && FUN_1405135f0() == 0 && ... *(mm+0x200) != 0) {
    ... FUN_1401e7870(*(mm+0x200), ...);   // save object state
}
... FUN_14040f190(block) for every 0xa0 block ...
free(array); *(table+0x18) = 0; *(table+0x20) = 0;
```

`FUN_1405135f0` is `GameManagerImp` slot `+0x58`, `ctx+0x24b1 >> 6 & 1`, "in
someone else's world". So **on a guest the object state is not saved and the
0xa0 block array is freed; on a host it is saved and then freed** **[read]**.
That is the only host/guest difference in the unload itself.

### What happens to the remote player copies

`FUN_140416ac0` removes characters from the world's nine lists by map index
**[read]**. Whether the remote `PlayerCtrl` copies (vftable `+0x10e4bb8`,
`+0x54 == 2`) are in those lists was **not** established; they are created by
the network player manager (`*(0x141616cf8+0x20)`, five 0xd0-byte slots from
`+0x1e8`, `FUN_14051b3a0`) and that is where the session keeps them
**[read]**. Nothing in the teardown steps above refers to that manager
**[read]**, so the copies probably survive any map unload **[inferred]** —
consistent with the phantom surviving the mod's travel.

## 2. What happens when the player crosses a map border

The part under the local player's feet is compared with the streamer's
current part every frame in `FUN_1403be060` **[read]**:

```c
lVar8  = *(streamer + 0x20);                     // the previous part
lVar10 = FUN_140312ba0(ctx[0x1a]);               // the part under the player (kind 2)
FUN_1403dc3d0(streamer, lVar10);                 // streamer+0x20 = lVar10 if non-null
FUN_1403dc8e0(streamer, pos, cell, lVar10);      // the call the mod hooks
if (fVar1 != -NAN && fVar1 != mapOf(lVar8))
    (*ctx->vt[0x70])(ctx, mapOf(lVar8), mapOf(lVar10));   // map changed
if (lVar10 != lVar8)
    (*ctx->vt[0x78])(ctx, lVar8, lVar10);                  // part changed
```

`GameManagerImp` (vftable `0x1410c4c68`) slot `+0x70` is `FUN_1401c2030 →
FUN_14039ad90(ctx, old, new)`, which **[read]**:

- writes the new map into `*(*(0x1416751f8+0x368)+0x34)`;
- calls the local character's `*(chr+0x480)` slot `+0x220(new)`;
- `FUN_140210ab0(*(ctx+0x90), old)`;
- `FUN_1403bd3d0(mm, old, new)`: old owner slot `+0x18` (`FUN_140406db0`),
  new owner slot `+0x10` (`FUN_140406d80`) — flags on the owner's `+0x180`
  list — and `FUN_1401e75d0(*(mm+0x200), new)`, the three-map list of the
  save, **which returns at once in someone else's world** (`FUN_1405135f0`);
- `FUN_14044f760(EventManager, old)`, `FUN_1404921e0(*(ctx+0x20), new)`;
- **`FUN_1405136d0(*(ctx+0x22f0), old)` → `FUN_140290360(*(0x141616cf8+0x30))`**,
  the only step that reaches the session manager: it sets two dirty bytes, `+0x64` of `[+0x88]`
  and `+0x4d` of `[+0x90]`. Their consumers, `FUN_140265bb0` and
  `FUN_14026b8b0`, re-request server lists (ids `0x12` and `0x18` in
  `FUN_140275e80`) for the new map id `*(*(*(0x141616cf8+0x20)+0x5b8)+0xc)`.
  That is matchmaking (messages, signs), not the P2P session.

One call was **not** read: `(*(chr+0x480))` slot `+0x220(new)`. `chr+0x480`
is set by the `PlayerCtrl` init `FUN_14037f3d0` from its parameter's `+0x30`
(`*(param_1 + 0x480) = *(param_2 + 0x30)`) **[read]**; its class was not
identified. With that exception, nothing reached from `FUN_14039ad90` touches
a session state machine, sends to the peer or ends anything **[read]**.

Slot `+0x78` (`FUN_1401c2040`) calls `FUN_1401c3fe0` and `FUN_14039aec0 →
FUN_1403bd490`, which calls the **new** owner's slot `+0x20`
(`FUN_1403cb180`, the part-entered handler) **[read]**.

This fires from the real contact, not from the part the mod's streamer hook
substitutes: `lVar10` comes from `FUN_140312ba0(ctx[0x1a])` before
`FUN_1403dc8e0` is called **[read]**. So the mod's teleport produces the same
notifications as walking, as soon as the character has contact on the
destination **[inferred]**, which matches the measurement that the current
map follows the old transport (`DS2_NATIVE_TRAVEL_PLAN.md` §10).

## 3. The object sync is bound to one map for the life of the session

Object `*(0x141616cf8+0x28)`, state at `+8`, record count `+0xc`, records
(0x18 bytes each) at `+0x10`, bound map id `+0x18`, armed byte `+0x74`,
guest gate `+0x198`.

**The bind** (`FUN_1405170e0`, state 0, called every frame from
`FUN_140514020`) **[read]**:

```c
if (state == 0) {
    if (sync+0x74 && DAT_14157c3b0[*(cf8[3]+0x68)*8] && *(cf8[0]+0xb4) > 1) {
        if (*(cf8[0]+0xa4) == 2) { FUN_140517bf0(sync); state = 1; }        // host
        else if (ctx->vt[0x58]() && sync+0x198) { FUN_140517880(sync); state = 2; }  // guest
    }
} else if (state == 1 || state == 2) { FUN_140518230(...); FUN_1405190a0(...); }
```

(`cf8[n]` = `*(0x141616cf8 + n*8)`.)

- The host's bind `FUN_140517bf0` takes the map from
  `FUN_1403bcd90(mm)` = `*(*(mm+8)+0x20)`, the streamer's current part, i.e.
  **the map the host stands on at bind time** **[read]**.
- The guest's bind `FUN_140517880` takes it from `FUN_1402c6de0(cf8[3])`:
  `*(cf8[3]+0x40)` slot `+0xf0` if that object exists, else
  `*(*(cf8[4]+0x5b8)+0xc)` **[read]**. That object is the join ctrl. For a
  summon it is `NetSummonJoinMultiplayCtrl` (vftable `0x1410d7bd8`, the class
  whose slot `+0x50` is the state-4 import `FUN_1402c2fa0`), and its slot
  `+0xf0` is `FUN_1402c1db0`: `*param_2 = *(param_1 + 0x19c)` **[read]**.
  `+0x19c` is written by the state-2 handler `FUN_1402c2a80` from the join
  payload (`*(param_1 + 0x19c) = *param_2;` at `+0x2c2bb2`) and read by the
  import `FUN_1402c2fa0` as the map to import into **[read]**. For a duel,
  `NetDuelJoinMultiplayCtrl` (vftable `0x1410d7678`) slot `+0xf0` is
  `FUN_1402b8cd0`, returning `+0x194` **[read]**. So **a guest's sync always
  binds to the join map, never to the map the guest stands on**, for as long
  as the join ctrl lives **[read]**. That is why §11 of the plan saw the
  state-0 rebuild bind "all 56 against the freed table".
- Both then walk `FUN_140419a70(world, map)` = `world+0x20+idx*8`, and store
  **raw pointers to the 0xa0 blocks** of `*(table+0x18)` into the records
  (`FUN_140515d50` / `FUN_140515de0`: `*(record+0x10) = block`) **[read]**.
  Those are exactly the blocks `FUN_14040db30` frees in section 1.

**Nothing rebinds on a map change.** The unbind `FUN_140517080` (state → 0,
`+0x74 = 0`, `+0x198 = 0`, plus `FUN_140517e70` on a host or `FUN_140517a80`
on a guest) has four callers, all session lifecycle **[read]**:

| caller | when |
| --- | --- |
| `FUN_1402c9540` at `+0x2c9702` | join-ctrl event of type 1 (it also calls the ctrl's slot `+0x30(0xf)`; read as the session's end **[inferred]**) |
| `FUN_1402c9bd0` at `+0x2c9ccb` | member list processing inside the same update |
| `FUN_1402ba8d0` at `+0x2ba9a0` | a going-home warp (`ctx->vt[0x40]`, state 9) |
| `FUN_1402c3900` at `+0x2c3c40` | the other going-home warp (state 9) |

The arm `FUN_140517040` (`+0x74 = 1`, gate 0) is called only by
`FUN_14051f5a0` when the session object becomes host (`+0xa4 = 2`) or guest
(`+0xa4 = 4`); the gate opener `FUN_140516370` only by the two state-4
imports, `FUN_1402c2fa0` and `FUN_1402b9ad0` **[read]**. The map-change
notification of section 2 reaches none of them. The four sync sub-messages
(`FUN_140516380`: `0x14` positions, `0x15` ownership request, `0x16`
ownership transfer, `0x17` release, `0x18` restore) all address records by
index and none carries a map id or a rebind order **[read]**.

**And the sync has no loaded-map check.** The host's sender
`FUN_140518230` reads `*(record+0x10)` and resolves the block's handle; when
the object is gone `FUN_140515c60` returns 1 and `FUN_1405174b0` **writes
`block+0x3c`** and sends `0x17` **[read]**. The guest's writer
`FUN_140518920` looks up the owner by the bound map id and reads
`*(owner+0x168)+0x18` with no null test (`FUN_1405177c0`), which is the crash
`DS2_NetSyncGuardHook` patches **[read]**. `FUN_14039ab60` in the writer's
validation is a world-bounds test on the position, not a map test **[read]**.

So **in vanilla, unloading the bound map during a session is a use after
free on both machines** **[inferred from the reads above]**. The game can
afford that only because the session is fenced into its area (point 1 of the
summary). This is also why the mod's crash "follows the map where the
session was formed" (`DS2_NATIVE_TRAVEL_PLAN.md` §10): that map is the one
every join-time binding points at.

## 4. The two-map limit

State 0 of `FUN_1403cc450` refuses to start a load **[read]**:

```c
if (*(longlong *)(DAT_1416751f8 + 0x80) != 0) {
    if (FUN_140b04ca0() != 0) goto refuse;                 // a busy flag
    mm = *(ctx + 0x38);
    if (1 < FUN_1403bcf20(mm) - FUN_1403bcda0(mm)) goto refuse;
}
```

`FUN_1403bcda0(mm)` is `*(short*)(*(mm+8)+0x1b4)`, and the streamer update
`FUN_1403dc3e0` recomputes `+0x1b4` every frame as **the number of owners in
state 0** (and `+0x1b2` as the number in state 5) **[read]**.
`FUN_1403bcf20(mm)` is `*(*(mm+0x10)+0x18)` **[read]**, which reads as the
number of owners **[inferred, not verified]**. So: while two or more owners
are anywhere in states 1–7 — loaded **or still unloading** — no third owner
leaves state 0 **[inferred]**. That is the measured "third map stays at
state 0" (§10 of the plan). It also means an origin in states 6/7 still
counts: the third load starts only when the origin is back at 0.

The limit only applies while `*(0x1416751f8+0x80) != 0` **[read]**; what that
object is was not established.

## 5. What else the join binds to the session map

The state-4 import on the guest (`FUN_1402b9ad0`, and `FUN_1402c2fa0` as
documented in `DS2_WORLD_STATE.md`) writes the host's world into objects of
**one** map, the one at `ctrl+0x194` **[read]**:

| call | target |
| --- | --- |
| `FUN_1401f69e0(*(world+0x10), blob+0x4018)` | EnemyGeneratorDeadCounter |
| `FUN_14040e1d0(FUN_140419a70(world, map), param_6)` | **the same per-map object table the sync binds to** |
| `FUN_140474590`, `FUN_14047a350`, `FUN_14017ea40` | event flags, values, bonfires, for `map` |
| `FUN_140452fb0(EventTaskManager, map)` → `FUN_140195600` | event tasks of `map` |
| `FUN_1401f3000`, `FUN_1401f30e0(MapStateActManager, map, ...)` | map object state |
| `FUN_140228e70(map, ...)`, `FUN_1401e6b60`, `FUN_1404434c0(*(ctx+0x60), ...)` | not named |
| then `ctrl+0xf0 = 5`, `FUN_140516370(sync)` | open the sync gate |

On a guest, section 1 showed that the unload does not save the object state
(`FUN_14040db30` skips the save in someone else's world), so after the origin
goes and comes back the guest rebuilds that map from its **own** save, not
from the host's import **[inferred]**. That is a world-state divergence (M7),
not by itself a crash. It is listed here because these are the structures the
game assumes stay alive for the session, and the remaining crash
(`MapModelComponent+0xc8`, §11 of the plan) happens exactly when the other
player's copy arrives back in that map. None of the calls above was shown to
keep a pointer the way the sync does; that is the next thing to read.

## 6. Proposal: move the session's anchor instead of unloading under it

The game never unloads the session's map, so there is no game code for that
to borrow. What it does have is a clean unbind and rebind of the one
component known to point into the map's memory. The proposal uses that at
every leg, so that when the origin is released **no session structure is
bound to it any more**.

### A latent write in today's fix, found on the way

`DS2_BonfireInSession_ForgetSyncedMap` drops the bound map's records by
writing `sync+0xc = 0` and closes the guest gate, on whichever machine
releases that map — the host included. It leaves each record's block pointer
`*(record+0x10)` in place. The host's native unbind `FUN_140517e70` does not
walk by count: it walks all 255 record slots (`while (lVar6 < 0x17e8)`, step
0x18) and, for every non-null `*(record+0x10)`, **writes `block+0x3c`**
(`*(lVar4 + 0x3c) = ... | 0x1000000000000`) and calls into the object through
the block's handle **[read]**. So on a host, any later `FUN_140517080` —
join-ctrl event 1 in `FUN_1402c9540` (read as the legal session end) or a
going-home warp (`FUN_1402ba8d0`, `FUN_1402c3900`) — writes into the freed block array of the
released map **[inferred from the reads]**. The guest's `FUN_140517a80` only
clears the records and touches no block **[read]**, so the guest is safe
from this one. Whatever the proposal below becomes, the host needs step 1
(the native unbind, while the map is still loaded) **instead of** the count
write, from the first leg on.

### Sequence for one leg

Assume the current transport up to the point where both players stand on the
destination, the destination owner is in state 5 on both machines, and the
origin is still held by its force byte. `sync = *(0x141616cf8 + 0x28)`.

1. **Both machines, before the map the sync is bound to is released:** call
   `FUN_140517080(sync)`. This is the game's own unbind: state → 0,
   `+0x74 = 0`, `+0x198 = 0`, and `FUN_140517e70` (host) or `FUN_140517a80`
   (guest) clears the records and the three trees. **It has to run while that
   map is still loaded**: the host's `FUN_140517e70` writes `block+0x3c` and
   calls into each object through its handle **[read]**. It replaces
   `ForgetSyncedMap`. The call is `void(sync)`, on the game thread (the owner
   update hook is a good place), and takes the sync's own lock through
   `*(sync+0x78)` slots `+0x10`/`+0x20` **[read]**.
2. **Point the guest's bind at the destination.** The guest binds to the map
   its join ctrl returns from slot `+0xf0` — for a summon,
   `*(NetSummonJoinMultiplayCtrl + 0x19c)`, the join map (section 3). Left
   alone, the rebind goes straight back to the join map, which is exactly the
   failure §11 of the plan measured. So on the guest, around the bind frame:
   save `ctrl+0x19c` (`ctrl = *(*(0x141616cf8+0x18)+0x40)`, after checking its
   vftable is `+0x10d7bd8`), write the destination map id, let the bind run,
   and write the saved value back. `FUN_140517880` reads it once, at bind
   **[read]**. The field is also read by the state-4 import `FUN_1402c2fa0`,
   by `FUN_1402c03e0` (`lea r8,[rdi+0x19c]` at `+0x2c058a`) and written by
   `FUN_1402c1620`; none of those should run in state 5+ **[inferred, not
   verified]**, which is why the write is kept to the bind frame.
3. **Both machines, once steps 1–2 are in place on both:**
   `FUN_140517040(sync)` — `+0x74 = 1`, gate 0. On the **guest only**, then
   `FUN_140516370(sync)` — gate 1. Next frame `FUN_1405170e0` binds: the host
   to the map it stands on (`FUN_140517bf0`), the guest to the value of step 2
   (`FUN_140517880`). The bind also needs
   `DAT_14157c3b0[*(cf8[3]+0x68)*8] != 0` and `*(cf8[0]+0xb4) > 1`
   **[read]**, both true in a live session **[inferred]**.
4. **Check the bind before going on:** on both, `sync+8` is 1 (host) / 2
   (guest), `sync+0x18` is the destination map id, and `sync+0xc` is the same
   number on both (records pair up by index, and both walk the same map's
   table in the same order **[inferred]**). If either side bound to anything
   else, **call `FUN_140517080(sync)` again on it and leave `+0x198` at 0**:
   closing the gate alone would leave state 2 with records pointing into the
   wrong table.
5. **Only then** drop the origin's force byte (the existing two-stage release
   is fine; it runs the same teardown the game would).
6. Order between machines does not matter for correctness: the writer
   rejects indices `>= sync+0xc` **[read]**, so `0x14` packets that arrive
   before the guest has records are dropped, not misapplied.

### What it fixes

- The object sync keeps working after the first travel, instead of staying
  closed for the rest of the session (today's fix, §11 of the plan).
- The rebind against freed memory that §11 measured ("the state-0 write at
  the next travel end rebuilt all 56 against the freed table") cannot happen:
  the rebind is done while the table it binds to is loaded, and the old
  records are gone before the old table is.
- It keeps the invariant the game relies on — the anchor map is loaded on
  both machines — true at every moment, instead of true only until the first
  release.

### What it does not fix

- The `MapModelComponent+0xc8` fault. Nothing read here ties it to the sync
  (the host's sync count was 0 when it happened). Section 5 lists the other
  bindings to check.
- The two-map limit. Section 4 gives the exact gate; the proposal does not
  hold a third map, so it does not collide with it, but the origin must reach
  state 0 before the next leg can load anything.
- The guest's world state of the origin after it unloads (section 5).

### Why not `FUN_1403bcc60`

`FUN_1403bcc60(mm, mapId, uint32 mask[4])` puts the streamer in single-map
mode: `streamer+0x1f0 = index`, `+0x1f6 = 1`, masks into `+0x1d0` and
`+0x1e0` **[read]**. In `FUN_1403dc930`, `+0x1f0 < 0x2a` makes every owner's
`+0x1ea = (i == +0x1f0)` **[read]** — every other map, the origin included,
is unloaded by the same state 5 path. It looks like a native "go to this
map", but:

- its only caller is `FUN_1404834e0` (reached from `FUN_140481900`), an
  event-side request **[read]**;
- no code was found that puts `+0x1f0` back to `>= 0x2a`: the only other store
  is the constructor `FUN_1403dbc60` writing `-1` **[read]** (a scan of
  `dword [reg+0x1f0]` stores in `+0x3d0000..+0x3e0000`; other encodings were
  not searched). Setting it without knowing how the game leaves the mode
  could pin the streamer to one map;
- it drops the origin in the same frame, which is the opposite of what the
  transport needs while the other player may still stand there.

So it is not proposed. It stays a lead if the mode's exit is found.

## 7. Unknowns

1. **Whether vanilla really fences the session.** The code only shows that
   the sync assumes it; the fence itself is the repository's measurement in
   Heide. The premise that phantoms walk between maps (Majula → Forest,
   Heide → Wharf) should be checked in vanilla before anything is built on
   either reading.
2. **Whether the join ctrl's `+0x19c` is safe to repoint for one frame**
   (step 2 of the proposal): who else reads it after the join, in state 5 and
   later. Resolved statically is only that the guest's bind reads it.
3. **Whether the remote `PlayerCtrl` copies are in the nine world lists**
   `FUN_140416ac0` walks. If they are, a map unload marks the other player's
   copy for deletion.
4. What `*(0x1416751f8+0x80)` and `*(*(mm+0x10)+0x18)` are (section 4).
5. Whether any of the state-4 import targets in section 5 keeps a pointer
   into the map, as the sync does.

## 8. Measurements that would confirm this on the live game

All of these observe; none needs a patch. Write every request file to
**both** installations.

1. **The vanilla fence (unknown 1).** With `DS2RemovePhantomFog` off, form a
   co-op session in Heide and walk the host towards No-man's Wharf. Expected:
   both players stopped at the boundary. If the host *can* cross, trace
   `+0x3cb1a0` (teardown step) and `+0x517080` (sync unbind) on both machines
   and record the order — that would be the natural path this study could not
   find.
2. **The rebind target.** On a guest in a live session, read the join ctrl
   (`*(*(0x141616cf8+0x18)+0x40)`, expect vftable `+0x10d7bd8`) and its
   `+0x19c`; expect the join map, before and after a leg. Trace `+0x517880`
   and read `[sync+0x18]` after it returns. This confirms section 3 and is the
   baseline for step 2.
3. **The proposal, one leg, no release.** Run steps 1–4 on the destination
   and stop before step 5. Pass: both `sync+8` back to 1/2, `sync+0x18` =
   destination on both, equal `sync+0xc`, and an object pushed by one player
   moving on the other's screen. Then run step 5 and a return to the meeting
   map: no `excecao`, no `FALHA APARADA`, `p2pSessionVerified` true.
4. **The copies and the world lists (unknown 3).** Read the nine list heads at
   `*(ctx+0x40)+0x370` and walk each through `+0x48`; look for the remote
   `PlayerCtrl` pointer (from `*(0x141616cf8+0x20)+0x1e8`).
5. **The host's latent write.** On a host after one leg with today's build,
   read `sync+0xc` (0) and the first record's `+0x10` (non-null expected),
   then end the session legally with `session end`: a fault in
   `FUN_140517e70` (`+0x517e70`..`+0x517f00`) confirms the finding at the top
   of section 6.
6. **The load gate (section 4).** While a third map sits in state 0, read
   `*(0x1416751f8+0x80)`, `*(*(mm+0x10)+0x18)`, `streamer+0x1b2` and
   `streamer+0x1b4`. Expected: the difference is `>= 2`, and the third load
   starts in the frame the origin reaches state 0.
