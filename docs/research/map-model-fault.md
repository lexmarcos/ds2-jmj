# `MapModelComponent + 0xc8`: the fault that closing the sync did not explain

Research made on 20/09 in Ghidra (`-noanalysis -readOnly`) and `objdump`,
against `DarkSoulsII.exe` 1.03 / Calibrations 2.02. Nothing was run in either
game. Addresses are at image base `0x140000000`. **[read]** / **[inferred]**
as elsewhere.

[object-table-lifecycle.md](object-table-lifecycle.md) §6 left this open:
with every holder of the enemy table closed, a crash on
`MapModelComponent+0xc8` remained, with `0x000b0010` in it.

## The headline

**At the fault, `this` was a perfectly readable `MapModelComponent`.** The
load `mov 0xc8(%rdi),%rcx` succeeded and the `test %rcx,%rcx` right before it
proves a null slot is handled **[read]**. So this is **not** a stale
component pointer surviving a map release — it is **a single word written
into a live object's pointer field by somebody else**.

```
1403f5103:  mov  0xc8(%rdi),%rcx     ; rdi = MapModelComponent* (this)
1403f510a:  test %rcx,%rcx
1403f510d:  je   1403f512f           ; a null slot is fine
1403f510f:  mov  0x38(%rcx),%eax     ; <- the fault
```

`0x1403f510f` is inside `FUN_1403f4f60`, the component's pre-draw callback.
The host's two recorded addresses are the same family: `FUN_1401caa60` is
called only from `FUN_1403f4f60` and `FUN_1401caaf0` only from
`FUN_1403f4f10`, both on `*(this+0xd8)` **[read]**. So **the guest faults on
`+0xc8` and the host on `+0xd8`, of the same class.**

## The class and the field

`MapModelComponent`: vftable `0x1410eb558`, size **0xf0**, ctor
`FUN_1403f4410` **[read]**. `MapModelComponent : MapEntityComponent :
GameEntityComponent`.

`+0xc8` is a pointer to the entity's **navigation-graph location component**,
created lazily by `FUN_1403f54a0` (the build virtual) and released by
`FUN_1403f6300` **[read]**. Its only three writers are `0` (ctor), a fresh
allocation (build) and `0` (destroy) — **nothing in the class can compose a
value there**.

Every dereference of `+0xc8` on this class was inventoried: six sites, of
which exactly **one** can see a bad value, and it is the one that crashed
**[read]**.

> A trap for future reports: the sibling class at `FUN_1403f3d50` /
> `FUN_1403f41d0` uses `+0xc8` as a **two-bit flag byte**, and
> `DS2_BackreadHook`'s guard list already wraps `FUN_1403f41d0`. A crash that
> names `+0xc8` without the vftable is ambiguous.

## What `0x000b0010` is not

- **Not a compile-time constant**: zero occurrences as an immediate in the
  whole program; the four byte matches in the file are incidental data
  **[read]**.
- **Not a map id**: map ids have the shape `area<<24 | block<<16`
  (`0x0a040000`) **[read]**.
- **Not heap poison**: `DLRegularHeap::free` writes no fill.
- **Not anything `+0xc8` can legally hold.**

## The one candidate writer

`FUN_140518920`, the `NetEnemyManager` `0x14` packet store, writes into the
per-record object — a **raw stored pointer** at `record+0x10` **[read]**:

```c
*(int  *)(lVar6 + 0x2c) = (int)(uVar17 << 0xb) >> 0xb;   // 21-bit, sign-extended
*(uint *)(lVar6 + 0x1c) = uVar19;
*(uint *)(lVar6 + 0x20) = puVar10[-3];
*(uint *)(lVar6 + 0x24) = puVar10[-2];
```

Three things line up:

1. `+0x1c` and `+0x2c` are exactly the two offsets the repo measured
   overwritten on a crashed `MapEntity` **[read]**.
2. They are **0x10 apart** — the same distance as the guest's `+0xcc` and the
   host's `+0xdc`. If `record+0x10` were `MapModelComponent + 0xb0`, one
   writer explains **both** faults on **both** machines **[inferred]**.
3. The `+0x2c` store is a sign-extended 21-bit packet field whose range is
   `±0x100000`, and `0x000b0010 = 720912` **fits** **[read]**.

And one thing does not: the index is bounded by `record count`, and a count of
0 aborts before any store — the repo measured the host's count as 0
**[read]**. So this writer cannot explain the host side unless something else
binds a second table.

**Not identified**, honestly. The strongest available test is whether a live
`NetEnemyManager` record ever points at `MapModelComponent + 0xb0`.

## The mechanism that fits every fact

`FUN_1403cdf30`, the entity container destroyed in the owner's teardown, is
**refcounted** **[read]**:

```c
*(int *)plVar2 = (int)*plVar2 + -1;
if ((int)*plVar2 == 0) { destroy; free; }
```

**An entity whose refcount is still held by anything else survives its map's
teardown**, stays linked in the global component bucket, and keeps being
pre-drawn every frame. That produces an object that looks completely live —
intact vftable, intact neighbours — while the map it came from is gone; if
its arena is later reused, single words inside it get rewritten by whatever
now occupies that memory **[inferred]**.

That is the only mechanism found that fits every observed fact at once: a
live-looking object, one word wrong, only with a session up, and all 32
buckets holding live owners.

**It is not closed.** `FUN_1403cc450` does not touch `owner+0x88`, the map's
allocator, so whether the arena is returned while a refcounted entity can
still live in it was **not established**.

## The delivery path, named

`FUN_14040d2e0` walks a global table of **0x20 buckets** and calls slot
`+0x30` of every component in a bucket; the call site at `0x1401c000f` passes
type `0xb`, the `MapProxyComponent` bucket, and that slot leads to
`FUN_1403f4f60` **[read]**. So every frame one global list is walked and the
pre-draw is entered for **every** `MapModelComponent` in the game, whatever
map it belongs to.

Being the delivery path does not make it the cause — and the repo's own
watcher already rebuilt "the list's 32 buckets" for ten legs and never found
a stale node. **That negative corroborates this reading**: the stale-node
explanation is dead, the corrupt-slot one stands.

## Two cheap measurements, for whoever comes back

1. **On the next fault, read `*(this+0xd0)` and `*(this+0xd4)` as well.**
   `FUN_140518920` writes `lVar6+0x20` and `+0x24` in the same breath as
   `+0x1c`; if the `record+0x10 == this+0xb0` mapping is real, those two must
   be corrupt too. If only one word in the object is ever wrong, that writer
   is out.
2. **Instrument bucket `0xb`** to check each component's owner entity against
   the live map list — not for unlinked nodes (already proven absent) but for
   a **live node whose entity belongs to a map that is gone**, which is what
   the refcount mechanism predicts.
