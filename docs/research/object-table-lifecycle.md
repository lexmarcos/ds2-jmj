# The per-map "network object table": what it is, who owns it, who points into it

Research made on 18/09 in Ghidra (`-readOnly`, copy `proj-a4`) and `objdump`,
against `DarkSoulsII.exe` 1.03 / Calibrations 2.02. Nothing was run in either
game. Addresses are Ghidra's at image base `0x140000000`; in a hook, resolve
them from the module base (`s_base + (addr - 0x140000000)`).

Each finding is marked **[read]** (the decompiled line or the bytes are quoted
or cited) or **[inferred]** (a conclusion drawn from reads, not measured).

This complements `docs/research/phantom-map-border.md`, which reads the map
owner's teardown and the object sync's bind and unbind. This document starts
from the table itself: what it is, where it comes from, where it goes, and
everything that keeps a pointer into it.

## The answer first

1. **The table is not a map-object table. It is the enemy generator's
   per-map status table.** `*(ctx+0x40)` is the `EnemyGeneratorManager`, and
   `FUN_140419a70` returns its `EnemyGeneratorAreaChrStatus` for a map. Each
   0xa0 block is the network status of **one enemy generator**: the enemy's
   handle, position, HP-ish counters, flags. The "object sync" at
   `0x141616cf8` slot `+0x28` syncs **enemies**, not map objects. The names in
   the repository's docs are misleading and worth correcting.
2. **Lifecycle, all [read].** Created a few frames after the map's owner
   reaches load step `0xd`, deferred through a four-slot queue in the manager.
   Freed **synchronously**, in the same frame, at the owner's teardown step
   `0xd`, through the manager's heap (`*(DAT_141616cc0+0x40)`), not a per-map
   heap. The slot in the manager is zeroed right after.
3. **Consumers.** Everything inside the enemy generator code either looks the
   table up afresh through the manager every time, or holds its pointer in an
   object the teardown clears first. **The only holder of raw block pointers
   that survives the free is the object sync's record array**
   (`sync+0x10`, `record+0x10 = block`), on both host and guest. Nothing in the
   map unload path clears it; only the session lifecycle does
   (`FUN_140517080`).
4. **Force byte versus streamer: the same code.** The owner computes one
   "wanted" byte from either; the teardown that frees the table is identical.
   What differs is that the vanilla game never unloads the session's map, and
   the mod does.
5. **No double ownership was found.** The blocks are freed once
   (`FUN_14040db30`) and the pointer is zeroed right after. The one path that
   drops the slots without freeing (`FUN_140416220`) leaks; it does not
   double-free. The observed map loader writing into the old table's first
   block is ordinary reuse of freed memory.
6. **The remaining `MapModelComponent+0xc8` fault is not explained by the
   holders found here.** With the sync closed on both machines, no holder of a
   freed block remains in this subsystem. `0x000b0010` was not identified
   among the block fields that were read, but several fields were not
   decoded, so it may still be table content. Section 6 says what to measure
   to settle it.
7. **Fix.** Replace the injector's unlocked `count = 0` with the game's own
   unbind, `FUN_140517080(sync)`, called on **both** machines at the moment
   the bound map's table is about to be freed. That moment is the entry of
   `FUN_140416ac0` for that map, on the game thread. Then re-arm only once the
   destination's table exists. Details are in section 5.

## 1. What the table is

### The manager

`FUN_1401bd7f0` (the game manager's subsystem setup) **[read]**:

```c
if ((uVar1 & 0x40) != 0) {
    lVar5 = FUN_140833320(0x440,0x10,uVar4);
    ...
    plVar7 = (longlong *)FUN_1404155b0(lVar5,*(undefined8 *)(DAT_141616cc0 + 0x40));
    *(longlong **)(param_1 + 0x40) = plVar7;
}
```

`FUN_1404155b0` stamps `EnemyGeneratorManager::vftable` (`0x1410ed520`) and
keeps the heap it was given at `mgr+8` (`param_1[1] = param_2`) **[read]**.
It also builds `mgr+0x10` (`FUN_1401f6380`, 0x1dbb8 bytes, the per-map dead
counter the state-4 import calls `EnemyGeneratorDeadCounter`) and `mgr+0x18`,
an `EnemyGeneratorPacketCtrl` (`0x1410ed530`) **[read]**. Nothing here depends
on a session: the manager exists solo too **[inferred]**.

### Per map index (0..0x29), two slots

| slot | class | size | constructor |
| --- | --- | --- | --- |
| `mgr+0x20+idx*8` | `EnemyGeneratorAreaChrStatus` (`0x1410ed2d0`) | 0x28 | `FUN_14040dad0` |
| `mgr+0x170+idx*8` | `EnemyGeneratorAreaCtrl` (`0x1410ed2e0`) | 0x58 | `FUN_14040e720` |

`FUN_140419a70(mgr, map)` returns the first: `return *(param_1 + 0x20 +
uVar1*8)` with `uVar1 < 0x2a` **[read]**. The map id to index conversion is
the Arxan thunk `thunk_FUN_141b39d4f`; `FUN_140416770` shows its arguments:
`thunk_FUN_141b39d4f(*(ctx+0x38), mapId)` **[read]**.

`EnemyGeneratorAreaChrStatus` **[read]** (`FUN_14040dad0`, `FUN_14040dca0`):

| offset | meaning |
| --- | --- |
| `+0x08` | heap (the manager's) |
| `+0x10` | the manager |
| `+0x18` | blocks (`count * 0xa0`) |
| `+0x20` | count, `= AreaCtrl+0x28` (number of generators) |
| `+0x24` | map id, `= AreaCtrl+0x30` |

`EnemyGeneratorAreaCtrl` holds `+0x20`, an array of `count` pointers to
`EnemyGeneratorCtrl` (`0x1410ed338`, 0x90 bytes, refcounted at `+8`), built in
`FUN_14040e720` from the map's generator data **[read]**.

### The 0xa0 block

Block `i` pairs with generator `i`. `FUN_14040eb20` links them both ways
**[read]**:

```c
lVar4 = uVar6 * 0xa0 + *(longlong *)(*(longlong *)(param_1 + 0x18) + 0x18);
*(longlong *)(lVar1 + 0x68) = lVar4;          // EnemyGeneratorCtrl+0x68 = block
```

and `FUN_14040f590` gives the block its generator handle,
`((mapIdx & 0x3f) | genId << 6) << 4 | 4` (`FUN_14017b5b0`) **[read]**. The
fields that the readers and writers use **[read]**:

| offset | content | set by |
| --- | --- | --- |
| `+0x00..+0x0c` | the enemy's home position (from `chr+0x80..+0x8c`) | `FUN_14040fcc0` |
| `+0x10` | generator handle (type 4) | `FUN_14040f590` |
| `+0x14` | owning character handle (type 1 or 7, e.g. `…c17`) | `FUN_14040fcc0` |
| `+0x18` | the spawned enemy's handle (`thunk_FUN_141bf1e0a(block+0x18)` resolves it) | after the spawn, `FUN_14040f1f0` **[inferred; not decompiled]** |
| `+0x1c..+0x28` | current position and heading, floats | `FUN_140410280`, sync writer |
| `+0x2c` | a counter (the sync writes a signed 21-bit value) | `FUN_140410280`, sync writer |
| `+0x3c` (8 bytes) | flags; bit 48 (`block+0x42` bit 0) is the sync's "remote owned" | sync bind and host send |
| `+0x58` | a refcounted object (`DLReferenceCountObject`) | released in `FUN_14040f190` |
| `+0x60` | pointer into the character manager's param rows | `FUN_140358620(*(ctx+0x18), …)` |
| `+0x68` | generator id | `FUN_14040f590` |
| `+0x76..+0x81` | state bits | many |
| `+0x30`, `+0x35` (4, unaligned), `+0x39`, `+0x44..+0x4c`, `+0x6c..+0x7c` | written by `FUN_14040deb0`, `FUN_14040e270`, `FUN_14040f9c0`; meaning not established | |

The positions and handles here are the "floats such as `0x423a4d0b`" and
"handles such as `0x3c17`" of the measured damage **[inferred]**: a block is
full of both.

## 2. Life cycle

### Creation (deferred, both machines, solo and in session)

1. Owner load step `0xd`, `FUN_1403ca8d0` **[read]**:
   `FUN_140416a00(*(ctx+0x40), holder, …)`.
2. `FUN_140416a00` only **queues** the map index into the four-byte create
   queue at `mgr+0x332` and removes it from the destroy queue at `mgr+0x336`
   **[read]**:
   ```c
   *(char *)((longlong)iVar5 + 0x332 + param_1) = (char)iVar1;
   ```
3. The manager's update `FUN_140417810` drains the queue **only while the
   short at `mgr+0x3c6` is zero** **[read]**:
   ```c
   if (*(short *)(param_1 + 0x3c6) == 0) {
       ... if (-1 < *pcVar24) FUN_14041a5f0(param_1,(int)*pcVar24);   // +0x332
       ... if (-1 < *pcVar24) FUN_14041a900(param_1,(int)*pcVar24);   // +0x336
   }
   ```
   `mgr+0x3c6` is the count of generator list 7 (`+0x3b8 + 7*2`)
   **[read]**. So a map can be loaded and in state 5 **with no table yet**
   **[inferred]**, and `FUN_140419a70` returns 0 for it until the manager's
   next unblocked update.
4. `FUN_14041a5f0(mgr, idx)` **[read]**: allocates the `AreaChrStatus` (0x28)
   and the `AreaCtrl` (0x58) from `mgr+8`, stores them in the two slots
   **without checking that the slots are empty**, calls `FUN_14040dca0`
   (blocks) and `FUN_14040eb20` (links), and pushes every generator onto list
   0 (`mgr+0x370`, linked through `ctrl+0x48`). On a host (not "in someone
   else's world", `FUN_1405135f0() == 0`) and when `owner+0x1e4 == 0`, it
   first fetches the saved status from the map-state store
   (`FUN_1402e3330(*(mm+0x200), mapId)`) and `FUN_14040deb0` copies it into
   the blocks.
5. `FUN_14040dca0` allocates the blocks **[read]**:
   ```c
   lVar2 = (**(code **)(**(longlong **)(param_1 + 8) + 0x50))
                     (*(longlong **)(param_1 + 8),uVar4 * 0xa0 + 0x10,0x10);
   ... *(ulonglong *)(lVar2 + 8) = uVar4;  *(longlong *)lVar2 = lVar2;
   *(ulonglong *)(param_1 + 0x18) = lVar2 + 0x10;
   ```
   Heap slot `+0x50` (aligned allocate) of the manager's heap, with a 0x10
   header holding the base and the count.

### The heap

Every object of this subsystem comes from the heap passed to the manager,
`*(DAT_141616cc0 + 0x40)` **[read]**. That includes the
`EnemyGeneratorCtrl`s: the call at `0x14040e8bb` loads `r8` from the
`AreaCtrl`'s heap first (`14040e887: mov 0x8(%rbx),%r8`) **[read]**. It is a
general heap shared with other users, not the map's own: the write watch saw
the map loader's file data land in the old blocks **[measured, §11 of the
plan; consistent with this read]**.

### Destruction (synchronous, same frame)

Owner teardown step `0xd` in `FUN_1403cb1a0` calls
`FUN_140416ac0(*(ctx+0x40), *(owner+0x148))` **[read]**. The owner runs the
whole teardown in one call (`FUN_1403cc3a0` loops `FUN_1403cb1a0` until done;
see `phantom-map-border.md` §1) **[read]**. `FUN_140416ac0` does, in order
**[read]**:

1. For each of the nine generator lists `mgr+0x370..0x3b0`: unlink every
   `EnemyGeneratorCtrl` whose map index (`FUN_1404125e0` =
   `*(*(ctrl+0x50)+0x2c)`) is this map; if `ctrl+0x68` has a live enemy,
   `FUN_14040f480(block)` and `FUN_140316010(chr)` (mark it for deletion;
   the delete itself is **deferred** through the character manager's pending
   list at `+0x30` in `FUN_140359890`); remove the ctrl from the vector at
   `mgr+0x3d0`; `FUN_140412df0(ctrl)`.
2. If the manager's current area (`mgr+0x2c0`) is on this map, reset it
   (`FUN_140419bc0(mgr, area, 0, 0)`).
3. `FUN_14041a900(mgr, idx)`:
   ```c
   if (*(param_1 + 0x20 + idx*8) != 0)  FUN_14040e380();   // per-block notify
   if (*(param_1 + 0x170 + idx*8) != 0) FUN_14040eac0();   // ctrl+0x68 = 0, AreaCtrl+0x18 = 0
   if (*(param_1 + 0x20 + idx*8) != 0)  FUN_14040db30();   // save + free the blocks
   ... destroy and free AreaCtrl,       *(param_1 + 0x170 + idx*8) = 0;
   ... destroy and free AreaChrStatus,  *(param_1 + 0x20 + idx*8) = 0;
   ```
4. `FUN_14040db30` **[read]**: on a host only (same `FUN_1405135f0() == 0`
   test), exports every block with a generator to a 0x3410-byte stack buffer
   and copies it into the map-state store (`FUN_1401e7870(*(mm+0x200), mapId,
   buf)`); then releases each block's `+0x58` reference (`FUN_14040f190`),
   frees the array through heap slot `+0x68` and zeroes the pointer:
   ```c
   (**(code **)(*plVar2 + 0x68))(plVar2,*(undefined8 *)(lVar1 + -0x10));
   *(undefined8 *)(param_1 + 0x18) = 0;
   ```

So the free is **immediate** and happens **inside the owner update of the
frame in which the owner sees "not wanted" in state 4 or 5**. Only the enemy
characters' deletion and the `TargetGeneratorCtrl` removal (below) are
deferred.

### Other paths that touch the slots

- `FUN_140416360` (manager reset): `FUN_14041a900` on all 42 indices, then
  frees whatever is left **[read]**.
- `FUN_140416220` (manager clear, called from `FUN_1401be677`): zeroes both
  slot arrays and all lists with `memset` **without freeing** **[read]**. That
  is a leak if tables exist. It is not a second owner.
- `FUN_14041a5f0` overwrites both slots without freeing the old occupants
  **[read]**. If a map's creation ran twice without a teardown in between,
  the first table would leak and its generators would stay on the lists
  pointing at still-allocated blocks. No path that does that was found. Leak,
  not use after free.

## 3. Every consumer that holds a pointer into the table

| holder | what it holds | how it gets it | cleared when the table goes? |
| --- | --- | --- | --- |
| manager slot `mgr+0x20+idx*8` | the `AreaChrStatus` | `FUN_14041a5f0` | **yes**, `FUN_14041a900` |
| manager slot `mgr+0x170+idx*8` | the `AreaCtrl` (which points at the table via `+0x18`) | `FUN_14041a5f0`, `FUN_14040eb20` | **yes**, `FUN_14040eac0` zeroes `+0x18`; slot zeroed |
| `EnemyGeneratorCtrl+0x68` | **raw block pointer** | `FUN_14040eb20` | **yes**, before the free: `FUN_14040eac0` → `FUN_140412d20` → `*(param_1 + 0x68) = 0` |
| manager lists `+0x370..+0x3b0`, vector `+0x3d0` | generators (whose `+0x68` is the block) | `FUN_14041a5f0` and moves in `FUN_140417810`, `FUN_140418d50`, `FUN_140419270`, `FUN_140419460`, `FUN_140419d50` | **yes**, unlinked in `FUN_140416ac0` step 1 |
| `TargetGeneratorCtrl` (`0x1410ed778`) in `*(ctx+0x48)` | a **counted** reference to the generator (`FUN_1404206f0`: `*(param_2+8) += 1`) | `FUN_140411a40` | removal is deferred (`FUN_140420b80` only sets `+0x10 \|= 1`), but it keeps the generator alive, and the generator's `+0x68` is already 0. Its readers (`FUN_140196120`) read `ctrl+0x20..+0x2c`, not the block |
| spawned enemy | the generator pointer (`local_4f8 = param_3` in the spawn parameters of `FUN_140418690`) and handles | `FUN_140418690` → `FUN_140356060` | the block itself is not passed; only copies of its fields are **[read]**. Whether the character takes a reference on the generator was not read |
| **object sync records** (`*(0x141616cf8+0x28)`, `sync+0x10`, 255 × 0x18) | **raw block pointer at `record+0x10`** | guest `FUN_140517880` → `FUN_140515d50`; host `FUN_140517bf0` → `FUN_140515de0` | **no** |
| world snapshot export / import (`FUN_1402bf8f0` → `FUN_14040dfd0`, `FUN_1402c2fa0`/`FUN_1402b9ad0` → `FUN_14040e1d0`) | nothing kept: fresh `FUN_140419a70` lookup, copy by value, 0x34-byte records | – | n/a |
| map-state store `*(mm+0x200)` (3 slots) | nothing kept: fixed 0x3410-byte buffers per slot, filled by `memcpy` | `FUN_1401e7870` | n/a |
| `FUN_1402e3530` (another state export) | fresh lookup | – | n/a |
| `EnemyGeneratorPacketCtrl` handler `FUN_1401f6fd0` (`'N'`..`'R'`) and `FUN_140416610`, `FUN_140416770`, `FUN_140416820` | fresh lookup | – | n/a |
| an object walked by `FUN_140181a10` (3 × 0x24 slots at `+0x20`, stride 0xa0) | generator **handles** (type 4), resolved each time via `FUN_14017b7e0` → `ctrl+0x68` | – | n/a: the handle resolves to a generator whose `+0x68` is already 0, or to nothing |

`MapEntity`, `MapModelComponent`, `MapBulletSlotComponent` and
`MapProxyComponent<MapActionComponent>` hold **no** pointer into this table:
none of their code appears among the consumers above. They are where the
freed memory went next, by ordinary heap reuse, which is what the write watch
of §11 of the plan saw **[inferred from the inventory and that measurement]**.

The inventory was seeded from all the callers of `FUN_140419a70` (8) and of
the block's own methods, then checked against two scans of every instruction
in the binary that do not depend on that seed:

- the index idiom `lea r,[r+r*4]` followed by `shl r,5` (`* 0xa0`): 47
  functions;
- the pointer-walk idiom `add r,0xa0` or `lea r,[r+0xa0]`: 382 functions.

For all 411 distinct functions, a script listed whether each calls
`FUN_140419a70`, the handle resolver `FUN_14017b7e0`, or any of the block
methods (`FUN_14040f590`, `…f9c0`, `…fcc0`, `…f770`, `…f480`, `…f660`,
`…f6f0`, `…f300`, `…f1f0`, `…f190`, `FUN_140410020`, `FUN_140410280`,
`FUN_14040deb0`, `FUN_14040e270`, `FUN_14040e0f0`, `FUN_14040e510`, the
sync's `FUN_140515d50`/`de0`, the spawns `FUN_140418520`/`8690`). Fourteen
do. Thirteen are the functions already in the table above. The fourteenth,
`FUN_140181a10`, keeps handles, not pointers, and is now in the table. The
rest call none of them. Two of those were spot-read: `FUN_1403e24f0` and
`FUN_1403c7d60` are map manager arrays of their own, allocated with
`… iVar8 * 0xa0, 0x10` on their own heap (the result is the check
**[read]**; the conclusion that none of the remaining 397 is a consumer is
**[inferred]**: a consumer that reaches a block by some other path would be
missed).

The nine generator lists hold only `EnemyGeneratorCtrl` nodes: every store to
`mgr+0x370..0x3b0` found by a displacement scan is in the manager's own
functions and moves a node through its `+0x48` link **[read]**. So a map
unload does not mark the other player's copy for deletion through these
lists **[inferred]**. That answers item 3 of `phantom-map-border.md` §7.

### The packet handlers look up the wrong map, not a freed one

`FUN_1401f6fd0` resolves the table from the map `FUN_1402c6de0` returns for
the object `thunk_FUN_141b8bcd0()` hands it (an Arxan thunk, not read), then
indexes it with the byte the sender put in the packet **[read]**. That this
is the **receiver's** current map is **[inferred]**, and it is the same
unknown as item 1 of section 7:

```c
lVar2 = FUN_140419a70(lVar4,*puVar3);
... if ((uint)*param_3 < *(uint *)(lVar2 + 0x20)) { lVar4 = *param_3 * 0xa0 + *(lVar2 + 0x18); ... }
```

The index is bounded by the count, so this cannot write into freed memory.
But when host and guest are momentarily on different maps (every travel
leg), generator `k` of the sender's map is applied to generator `k` of the
receiver's map, including `FUN_140418d50` spawns via `'P'` **[inferred]**.
That is wrong enemies on the wrong map, not a crash.

## 4. Can the unload free the table under a consumer?

**Yes, exactly one: the object sync's records, on both machines [read +
inferred].**

- The bind stores the block pointer. Guest, `FUN_140515d50`:
  `*(longlong *)(param_1 + 8) = param_2;` (record `+0x10`); host,
  `FUN_140515de0`, the same **[read]**.
- The only code that clears the records is `FUN_140517a80` (guest) /
  `FUN_140517e70` (host), reached only through `FUN_140517080`, whose four
  callers are session lifecycle (join-controller event 1, member processing,
  two going-home warps; see `phantom-map-border.md` §3) **[read]**. The map
  owner's teardown reaches none of them **[read]**.
- Every reader and writer of records is bounded by `sync+0xc` **[read]**:
  `FUN_140518920` (`0x14`) in its validation loop; `FUN_140516910` (`0x15`),
  `FUN_1405164b0` (`0x16`), `FUN_140516630` (`0x17`), `FUN_140516820`
  (`0x18`): `if ((int)(uint)bVar2 < *(int *)(param_1 + 0xc))`;
  `FUN_140518230` (host send) loops `< *(int *)(param_1 + 0xc)`.
  `FUN_140518e20` has no test of its own but is only called from the two
  bounded ones. So **`count = 0` does close every path**, provided it lands
  before the free.
- All of them run under the sync's lock, `*(sync+0x78)` slot `+0x10`/`+0x20`
  (`FUN_140516380` for packets, `FUN_1405170e0` for the per-frame update,
  `FUN_140517080` for the unbind) **[read]**. The table's free on the owner
  update does **not** take that lock **[read]**. The injector's
  `ForgetSyncedMap` writes the count without the lock too.

What a stale record did, **[read]** from `FUN_140518920`'s store sequence into
`lVar6 = *(record+0x10)`:

```c
*(int *)(lVar6 + 0x2c) = (int)(uVar18 << 0xb) >> 0xb;
*(uint *)(lVar6 + 0x1c) = uVar20;  *(uint *)(lVar6 + 0x20) = puVar11[-3];  *(uint *)(lVar6 + 0x24) = puVar11[-2];
FUN_14040fcc0(lVar6);              // +0x14 = handle, +0x00..+0x0c = chr+0x80..+0x8c
*(byte *)(lVar6 + 0x7a) |= 1;      *(byte *)(lVar6 + 0x81) |= 8;
```

Before that, it resolves `thunk_FUN_141bf1e0a(lVar6 + 0x18)` from the stale
block and, if the resolve succeeds, writes positions and `0x101` into
`FUN_140312ac0(chr)` `+0x10..+0x44` **[read]**. With the block's memory
reused, `+0x18` is whatever the new owner keeps there. A resolve that happens
to hit a live character writes into **that** character, so part of the old
damage may have landed in objects far from the table **[inferred]**. The host
side (`FUN_140518230` → `FUN_1405174b0`, and the unbind's `FUN_140517e70`)
writes `block+0x3c |= 1<<48` and calls `(*(chr+0xe8))->vt[0x38]` on the
resolved character **[read]**.

**Force byte versus streamer.** `FUN_1403cc450` computes `owner+0x1eb =
owner+0x1e9 ? 1 : owner+0x1ea`, and state 4/5 with `+0x1eb == 0` runs
`FUN_1403cc3a0` **[read]**. The teardown above, the free included, is the
same whichever byte decided it. The difference is not in the code path. It is
that the mod unloads the map the sync is bound to, which vanilla never does
while a session lives (`phantom-map-border.md` §3) **[inferred]**.

**Two allocators on one block.** Not found. The only free is
`FUN_14040db30`'s, and it zeroes `+0x18` on the next instruction group; the
`AreaChrStatus` slot is zeroed right after its free; `FUN_14041a900` tests
the slot before each use, so running it twice is harmless **[read]**. The
"live table on top of a live object" case would need a double free somewhere
else. The crash hook's new table dump can test for that (section 6).

## 5. Fix proposal

### Step 1 (memory safety): unbind at the free, on both machines

Hook `FUN_140416ac0` (`+0x416ac0`, `void(mgr, holder)`), on the game thread.
At entry:

```text
sync   = *(base + 0x1616cf8) -> +0x28
mapId  = FUN_1403bb3c0(holder, &out)          ; the map being released (holder = owner+0x148)
if sync != 0 && *(u32*)(sync+0x18) == mapId && *(u32*)(sync+0xc) != 0:
    FUN_140517080(sync)                        ; the game's own unbind
call original
```

Why this and not the current `ForgetSyncedMap`:

- **Timing by construction.** It runs in the frame of the free, before
  `FUN_14041a900`, instead of "at least 700 ms before" on a heuristic. It
  also covers a free the mod did not order, such as the streamer dropping the
  map on its own.
- **It takes the sync's lock** (`*(sync+0x78)` slots `+0x10`/`+0x20`), so it
  cannot interleave with a `0x14` packet being applied on the net thread
  **[read]**.
- **It clears all 255 records** (`record+0x10 = 0`, flag bit 0 off), not
  just the count, and puts the state to 0, `+0x74` and `+0x198` to 0
  **[read]**. So a later count raise cannot bring back the old pointers.
- **On the host it has to run while the table is alive**, because
  `FUN_140517e70` writes `block+0x3c` and calls into each enemy through its
  handle **[read]**. The entry of `FUN_140416ac0` is the last moment it is.

Keep `ForgetSyncedMap` as a fallback, under the same map test, until the hook
has been measured.

Risk: `FUN_140517080` is called from inside the owner update rather than from
its usual session paths. Whether the main thread holds another lock at owner
teardown step `0xd` that the net thread takes in the opposite order was not
read **[unknown]**.

### Step 2 (keep the enemy sync alive): re-arm once the destination's table exists

This is `phantom-map-border.md` §6 steps 2 and 3, with one condition added
from this reading: **re-arm only when `FUN_140419a70(mgr, destination)` is
non-zero on that machine.** Table creation lags the owner's state 5 whenever
`mgr+0x3c6 != 0` (section 2). If the guest's gate is opened before the table
exists, `FUN_140517880` sets the state to 2 with **no** records
(`if (lVar4 != 0) { … *(param_1 + 0xc) = uVar11; }`), and it does not try
again until the next unbind **[read]**.

Order for one leg:

1. Both players on the destination, destination owner in state 5 on both.
2. On each machine, wait for `*(mgr+0x20+idx(dest)*8) != 0`.
3. Host: `FUN_140517040(sync)` (arm). Guest: `FUN_140517040(sync)` then
   `FUN_140516370(sync)` (gate).
4. Check on both: `sync+8` is 1 (host) or 2 (guest), `sync+0x18` is the
   destination on both, `sync+0xc` is equal on both. Records pair by index,
   so a mismatch means the two sides bound different maps.
5. Drop the origin's force byte. Step 1's hook covers the free whatever the
   order.

If step 4 fails on the guest because `FUN_1402c6de0` returns the join map
(see unknown 2), close the guest's gate as today. Step 1 still makes the free
safe.

### What this does not fix

- The `MapModelComponent+0xc8` fault (section 6).
- A guest reloading a map rebuilds its enemies from nothing:
  `FUN_14040db30` skips the export and `FUN_14041a5f0` skips the import when
  `FUN_1405135f0()` says "someone else's world" **[read]**. So enemies the
  host killed come back on the guest after an unload and reload
  **[inferred]**. That is world state (M7), not memory.
- The packet handlers applying one map's generator index to another
  (section 3).

## 6. What this table does not explain, and what to measure

**The `0x000b0010` fault.** The value was not identified among the block
fields that were read. Generator handles end in nibble 4 (`… << 4 | 4`),
character handles in 1 or 7, and `+0x00..+0x2c` hold positions and a
counter. The fields whose meaning was **not** established (`+0x30`, `+0x35`,
`+0x39`, `+0x44..+0x4c`, `+0x6c..+0x7c`) are candidates, so this is not a
proof that the value is foreign to the table. What does hold: with the sync
at `count = 0` on both machines and its gate closed on the guest, **no holder
of a freed block remains in this subsystem** (section 3) **[read +
inferred]**. So if `0x000b0010` is table content, it was written either
before the sync was closed and found later, or by a stale holder outside the
inventory, or it came from a live table sitting on a live object (two
owners). If it is not table content, look at the other join-time bindings to
the meeting map (`phantom-map-border.md` §5) **[inferred]**.

Measurements that would settle each point, in order of value:

1. **Hook `FUN_14040db30`** on both machines: log the table, the block range
   (`*(t+0x18)`, `*(t+0x20)`), the map, and at the same instant `sync+0xc`,
   `sync+0x18` and `sync+8`. It confirms the free timing, and shows whether
   the **host's** sync was really at 0 (the host binds through
   `FUN_1403bcd90(mm)` = `*(*(mm+8)+0x20)`, the map it stood on at bind, with
   nothing that follows a travel **[read]**).
2. **Watch the whole freed range**, not the first block: a write watch over
   `blocks .. blocks + count*0xa0` from the free until the next return, with
   writer return addresses, on both machines. The earlier watch covered one
   block of 56, and the sync addresses blocks by index.
3. **At the next `0x000b0010` fault**, compare the crashed object's address
   with the table ranges `DS2_CrashHook` now dumps. Inside a live table means
   two owners (a double free somewhere else). Inside an old freed range means
   a stale writer. Neither means another subsystem.
4. **Read `[[0x141616cf8+0x18]+0x40]`** in a live session on the guest. If it
   is non-null, `FUN_1402c6de0` takes the map from its slot `+0xf0` rather
   than the zone control's current map. That decides whether step 2 can
   rebind the guest.
5. **Read `mgr+0x3c6`** (`*(*(ctx)+0x40)+0x3c6`) on arrival: non-zero means
   the destination's table will lag its load.
6. After step 1, the control is the same as §11 of the plan: Brume → Majula
   twice, both machines, with no `excecao` and no `FALHA APARADA`, and the
   sync bound to the destination after each leg.

## 7. Unknowns

1. Which object `*(cf8[3]+0x40)` is, and what its slot `+0xf0` returns in a
   session (`FUN_1402c6de0`).
2. Whether a spawned enemy takes a reference on its `EnemyGeneratorCtrl`. The
   generator outlives the table either way (its `+0x68` is zeroed first), so
   this affects only the generator's own lifetime.
3. What drives list 7's count at `mgr+0x3c6`, the gate that defers table
   creation.
4. Whether the main thread holds a lock at owner teardown that conflicts with
   the sync's lock (the risk in step 1).
5. Consumers that reach a block without the index or pointer-walk idioms,
   and without calling any of the listed functions, would not be found by the
   scans of section 3.
6. The meaning of block fields `+0x30..+0x4c` and `+0x6c..+0x7c`.
