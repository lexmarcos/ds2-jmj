# The enemy generator table: how it is built, how it is freed, and every race

Research made on 20/09 in Ghidra (`-noanalysis -readOnly`) and `objdump`,
against `DarkSoulsII.exe` 1.03 / Calibrations 2.02. Nothing was run in either
game. `ctx = *0x1416148f0`, `mgr = *(ctx+0x40)`. **[read]** / **[inferred]**.

For M8 item 6b: the table is the object a mid-session map release most easily
corrupts, and the one the destination needs before the enemy sync can rebind.

## The readiness test, and the one that looks right and is wrong

```c
table = FUN_140419a70(mgr, mapId);                 /* = *(mgr + 0x20 + idx*8) */
ready = table != 0 && *(int32_t*)(table + 0x24) == mapId;
```

`+0x24` is the only field that separates "allocated" from "built": the ctor
writes `0xffffffff`, `FUN_14040dca0` writes the real map id as its first act,
and the free writes `0xffffffff` back **[read]**.

> **Do not test `blocks != 0`.** A map with **zero generators** legitimately
> gets a valid table with `+0x18 == 0` and `+0x20 == 0` **[read]** — exactly
> the `blocks 0-0` row in the 19/09 crash dump.

Before touching a block, repeat the engine's own guard:
`blocks != 0 && idx < count`, then `block = blocks + idx*0xa0`.

## The gate that decides *when* — and why travel makes it worse

Creation is queued, not immediate. `FUN_140416a00(mgr, mapHandle)` — called
from the owner's **setup step 0xd** — writes one byte, the map index, into a
**four-slot queue at `mgr+0x332`** **[read]**. `FUN_140417810`, the manager's
per-frame update, drains it — **but only while `mgr+0x3c6 == 0`** **[read]**.

**`mgr+0x3c6` is the element count of list 7**, one of nine generator lists,
and list 7 means *"generators whose character has been asked to go away and
has not gone yet"* **[read]**. Nodes enter it in `FUN_140419d50`, which runs
only when the local player's current map changes; they leave it in
`FUN_14041c340`, once the character's handle no longer resolves.

So the gate is not a timer. It is **"nothing is still dying from the last map
change"** — and **every teleport the mod does raises it**. If the mod keeps
the source map loaded with its enemies alive, or one character refuses to be
released, **the create queue never drains and the destination silently never
gets a table** **[read for the mechanism, inferred for the scenario]**.

**Never key off the owner's state 5.** The enqueue happens at setup step 13,
so the request is already in the queue before state 5 is observable; the
table then appears at the first update with `mgr+0x3c6 == 0` — the next
frame when quiet, unbounded after a teleport. Poll the readiness test every
frame with a hard timeout, and treat the timeout as failure.

## Four ways a create is lost, none of them loud

1. **Silently dropped when the queue is full.** `FUN_140416a00` writes
   nothing when all four slots hold other indices, and **never retries**
   **[read]**. Stock play never queues four; a mod holding three maps and
   streaming neighbours plausibly can.
2. **Starved by `mgr+0x3c6`**, above.
3. **Failure is terminal.** Both failure branches of `FUN_14041a5f0` free
   both objects, zero both slots and **never re-queue** **[read]**.
4. **A release outside the step machine leaves the create queued.** The only
   thing that cancels it is `FUN_140416ac0`, reached only from
   `FUN_1403cb1a0` case `0xd`, whose last act is an unconditional sweep of
   the queue for that index **[read]**:

```
140416d29: call   0x14041a900        ; the synchronous free
140416d2e: add    $0x332,%rdi        ; then cancel the pending create
140416d48: movb   $0xff,(%rdi)
```

So with the stock release path a queued create **is** cancelled. With a
hand-rolled release — poking the state byte, freeing the owner's sub-objects
by hand — it is not.

And a stale create **fails quietly, not loudly**: `FUN_1403bce60` returns an
owner for **every** map of the world, at a fixed index, for the whole session,
loaded or not **[read]** — the streamer builds that list once. So the check
only rejects an out-of-range index; a stale create proceeds and builds either
an empty-but-valid table for an unloaded map or a table over stale data
**[inferred]**. Neither is a null dereference, which makes it harder to
diagnose, not easier.

## After a stock release, nothing harmful is left in the manager

`FUN_14041a900` clears both queues for the index, frees the blocks, destroys
the Ctrl and the status and zeroes both slots; `FUN_140416ac0` before it
sweeps **all nine lists**, unlinks every node of that map, decrements each
count, despawns the characters and erases the nodes from the secondary index
**[read]**.

The one genuine leftover is the **kill-cooldown vector at `mgr+0x3f0`** —
eight `{generator id, age}` entries, keyed by generator, untouched by the
release. After release and reload, a generator killed just before the release
stays suppressed for the rest of its cooldown **[read]**. Harmless, arguably
correct.

Two corrections to earlier notes:

- **`FUN_140416220`'s leak is not on this path.** It is reached from one site
  in the ctx-level reset, zeroes all 42 slots of both arrays **without
  freeing**, and clears both queues at once. It cannot double-free; it leaks.
  **Its trigger was not established** — the only xref is an unanalysed chunk
  in the `0x141b` region whose body reads like the warp/load state machine,
  so "world reset only" is **not proven**, and it may run on every warp.
- **The free queue at `mgr+0x336` is dead code.** An instruction scan found
  no producer anywhere: every free in this build is synchronous, and the
  drain's second loop is unreachable **[read]**. Do not plan a "queue the
  free" mod around it.

## The only consumer that caches — and a memory test for it

`FUN_140419a70` has exactly eight callers; **seven re-look-up through the
manager inside the call and never keep the pointer** **[read]**: the `'N'`/`'O'`
packet handler, the two snapshot builders, the two snapshot appliers, and the
per-map fan-out.

The eighth and ninth are the enemy object sync's two builders,
`FUN_140517880` and `FUN_140517bf0`, which cache **raw block pointers** into
the record array — `record+0x10 = blocks + i*0xa0`, verbatim **[read]**. The
array is built **once**, for **one** map, and read every frame while the sync
is live. `FUN_14041a900` frees the blocks out from under it. **That is the
only use-after-free path found.**

The test the mod can run entirely from memory:

```
sync     = *(void**)(0x141616cf8 + 0x28);
builtFor = *(int32_t*)(sync + 0x18);   /* the map its records point into */
state    = *(int32_t*)(sync + 0x08);   /* 0 not built, 1 or 2 live */

releasing map M is use-after-free-safe iff (state == 0 || M != builtFor)
```

## What this could not establish

- The intra-frame order of the manager update versus the owner state
  machine's driver. Worth one frame; it does not change the polling rule.
- What resets `sync+0x08` to 0, i.e. when the record array is rebuilt. That
  is the missing half of the test above.
- What triggers `FUN_140416220`, hence whether its leak is title-screen only
  or happens on every warp.
- Whether `FUN_14040eb20`'s failure path is reachable in practice.
