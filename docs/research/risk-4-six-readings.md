# Risk 4, read six ways: what closed, what survived, and what to do

Written 20/09 from six parallel Ghidra readings (`-noanalysis -readOnly`)
against `DarkSoulsII.exe` 1.03 / Calibrations 2.02, plus one reading of this
repo's own measurement history and one look at a live process's `/proc/maps`.
Addresses are at image base `0x140000000`. **[read]** / **[inferred]** as
elsewhere.

Risk 4 is the unexplained crash of
[map-model-fault](map-model-fault.md): a live, intact `MapModelComponent`
(vftable `0x1410eb558`, size `0xf0`) whose `+0xc8` held `0x000b0010…` on the
guest, with the host dying the same way on `+0xd8`, 17 ms apart. It is the
reason M8 item 6b has not been started: nine steps of new map-teardown code on
top of an unexplained map-teardown crash makes every later crash ambiguous.

## What is now closed

**The arena mechanism is dead.** The standing hypothesis was that a refcounted
entity survives its map's teardown and then has words rewritten when the map's
arena is reused. `owner+0x88` is **not a per-map arena**: it is a plain
`DLKR::DLAllocator*` handed in at construction, and it is the **world's single
shared allocator** — `owner+0x88 == A[0] == W[0]`, where `W` is
`*(DAT_1416148f0+0x38)`, the long-lived world object **[read]**. Every
candidate was checked (`FUN_1403cb1a0` and all fourteen teardown stages,
`FUN_1403cbb50`, `FUN_1403cba10`, `FUN_1403dc2f0`, `FUN_1403bd2e0`, and the
world object's own destructor): each allocates and frees *through* that
pointer and none ever frees, resets or reassigns it **[read]**. There is no
per-area entity pool either — a `MapEntity`'s own 0xc0 bytes come from the same
shared allocator. And a live entity's storage is never freed: the free sits
inside the `refcount == 0` branch.

So nothing is handed back while a referenced entity lives. The instruction that
follows is the useful part: **look for a stale pointer *out of* a surviving
object, not for a rewritten word *inside* one.**

Names, from the binary's own RTTI: the refcounted class is
`MapEntity : GameEntity : GameObject`, the count an `int` at `+0x8` starting at
0; the owner is `MapAreaCtrlOwner : MapAreaCtrl`, which has **four** entity
containers (`+0x158/+0x160/+0x168/+0x170`), and `FUN_1403cdf30` destroys one of
them **[read]**. A scan of all 108,590 functions found 1256 increment sites; the
only holder outside the owner is `PrefabOwnerComponent`, and its parts are
created through the area's own factory, never looked up globally — so no
cross-map holder exists **[read]**.

**A packed handle is dead.** `GameEntityComponent`'s layout is `+0x00` vftable
(slot `+0x20` attach, `+0x28` detach, `+0x30` pre-draw), `+0x08` owner entity,
`+0x10` next in the owner's list, `+0x18` the bucket type as a plain `int`
(`-1` = never bucketed), `+0x20/+0x28` the list node **[read]**. Bucket insert
and remove are pure pointer surgery; no handle is composed or stored anywhere,
and no handle field exists near `0xc8`/`0xd8` or in any base class. The
sub-object at `+0xc8` is itself built with bucket type `-1`, so it is never in
any bucket at all **[read]**.

**Neither crash was a null dereference.** Every reader of `+0xc8`, `+0xd0` and
`+0xd8` null-checks first — six readers of `+0xd8`, all guarded, and the
pre-draw's `+0xc8` read likewise **[read]**. Both fields held a **non-zero
invalid pointer**.

**The 0x10 gap between the two fields is a coincidence.** `+0xc8`, `+0xd0`,
`+0xd8`, `+0xe0` are four consecutive 8-byte sub-component pointers, zeroed by
four consecutive `MOV`s in the ctor; unrelated classes, different build
conditions **[read]**. What is meaningful is that they are *adjacent*: one
corruption event across `0xc8..0xe0` reaches several at once, which is why one
bug shows two crash signatures on two machines.

## The fields, named at last

| offset | what it is | size |
| --- | --- | --- |
| `+0x40` | `MapFlverModelCtrl` — the model | |
| `+0xb0` | int count | |
| `+0xb8` | `MapModelWindReactor` | |
| `+0xc0` | int nav id, mirror of the sub-object's `+0x38`, init `-1` | |
| `+0xc8` | `MapPartsNaviGraphLocationComponent` / `MapObjNaviGraphLocationComponent` | 0x40 / 0x50 |
| `+0xd0` | `MapModelPointCloudReceiveCtrl` | 0x140 |
| `+0xd8` | **`MapModelRumbleCtrl`** — `{vptr, float elapsed, float duration, float amplitude}` | 0x18 |
| `+0xcc`, `+0xd4`, `+0xdc` | **no writers and no readers**: the high dwords of the three pointers | |

`FUN_1401caaf0`, where the host died, is two lines — `if (*(float*)(p+8) <
*(float*)(p+0xc)) *(float*)(p+8) += dt;` — so there is no other way for it to
fault than a bad `p` **[read]**.

## The value is two mechanisms, not one

The five recorded corruptions are not one family. Comparing each against its
"should be", byte by byte **[read]**:

- **Rows 1–3** (`00b54001…`, `00b01001…`, `00b010ff…`): byte `+4` unchanged,
  bytes `+5` and `+6` overwritten. No dword store does that. These are the
  `rep movsb` partial block reuse out of `FUN_1404e00d0` that §17 of the tasks
  doc already measured on 16/09. **Do not re-attribute them.**
- **Rows 4–5**, both `MapModelComponent` faults: byte `+4` *did* change
  (`ff` → `10`). That is a **dword store at `pointer + 4`**.

An alternative reading — that rows 4 and 5 are also a 16-bit store, with the
real pointers being `0x00007f10…` — was killed by looking at the live process:
`/proc/<pid>/maps` gives the complete set of mapping high dwords as
`{0, 1, 2, 0x5555, 0x6fff, 0x7638, 0x7639, 0x7ffc, 0x7fff}`. There is no
`0x7f10` **[read, live process]**.

## What survived, and it is specific

`FUN_140518920`, the `NetEnemyManager` type-`0x14` packet handler, writes
`{+0x1c, +0x20, +0x24, +0x2c}` as four **dword** stores through a raw stored
pointer at `record+0x10`, with holes at `+0x18` and `+0x28` — and those holes
are exactly the low halves that survived **[read]**.

Two earlier doubts are settled:

- the 21-bit sign-extended field at `+0x2c` is **HP**, packed by
  `FUN_1405190a0` clamped to `[-99999, 999999]`. `0x000b0010` = 720912 sits
  inside that clamp **[read]** — much stronger than "fits in 21 bits";
- the index can **never** exceed the array: it is a byte, the array is a fixed
  255 slots, and the packet is rejected unless `count <= (rec >> 0x15 & 0xff)`
  **[read]**. The danger was never an overflow.

**The danger is a stale slot pointer, and the lifetime gap is named.**
`record+0x10` points into a per-map, heap-allocated array of 0xa0-byte records
owned by one of 0x2a per-area slots. It is nulled **only** by `FUN_140517080`,
whose four callers are all session-state transitions. The array it points into
is freed by `FUN_14040db30`, reached only from the map load/unload path. **No
call from the map path to the nuller exists** **[read]**. So a map teardown
with the session still in state 1 or 2 leaves up to 255 live records pointing
into freed memory — which is precisely "only with a session up", and precisely
this repo's own "releasing the session's map caused the travel crashes".

The geometry fits without needing a second bug: the **same** HP store at two
heap placements 0x10 apart. With the record element at `MMC+0xa0` the store
lands on `MMC+0xcc`, the high half of `+0xc8` — the guest's fault. At
`MMC+0xb0` it lands on `MMC+0xdc`, the high half of `+0xd8` — the host's.
Allocation is `n*0xa0 + 0x10` at 0x10 alignment, so 0x10-granular reuse offsets
are expected **[inferred]**. The base cannot be `MMC+0xb0` for the guest,
because that would put a **position float** on `+0xcc`, and every observed
value has top byte `0x00`, i.e. is a denormal below 2.4e-38 — not a world
coordinate **[read]**.

`FUN_140518230` runs every tick on both roles through the same stale pointer
and does an 8-byte read-modify-write at `E+0x3c`; it is the next place to look
**[read]**.

## And our own guard may be making one of the states

`FUN_1403f6300` nulls `+0xc8` **after** the call that frees it **[read]**:

```
1403f63ff:  mov    0x38(%rdx),%eax        ; caches the nav id into this+0xc0
1403f640e:  call   0x14040cea0            ; unregister + destruct + free
1403f6413:  mov    %r15,0xc8(%rdi)        ; r15=0, AFTER the call
```

No `finally`, no store before the call. **And no other path leaves `+0xc8`
non-null** — the unregister's one silent early return is a normal return, so
`1403f6413` still runs. **A fault is the only way**, and
`DS2_TravelWatchHook` wraps exactly this function in a `__try` so that a
faulting release leaks instead of closing the game.

The consequence is exact: `+0xc8` is released at step 3 of destroy and `+0x40`
at step 8, so a swallowed fault leaves `+0x40` **still non-null** — and
`+0x40 != 0` is precisely the condition that makes the pre-draw fall through to
the `*(this+0xc8)+0x38` read that crashes **[read]**. A successful teardown
nulls `+0x40`, the pre-draw takes its early exit, and `+0xc8` is never touched.
Worse on the next cycle: the next unload calls destroy again on the dangling
pointer (double free), and the next load's build sees the field non-null, skips
the allocation and writes the cached id through it (write-after-free).

This does not make our guard the *origin* — something has to fault inside the
release first — but it does mean the guard converts one fault into a recurring
crash instead of a leak.

## The bucket, and why the old negative proved less than it looked

`MapModelComponent`'s ctor builds a **second** component sub-object at `+0x60`
with bucket type `0xb`, stores `this+0x90 = this` and `this+0x98 =
FUN_1403f4f60`, and gives it `MapProxyComponent<MapModelComponent>`'s vftable.
Drawing that node dereferences **a raw target pointer and a raw code pointer
read out of the component's own memory**, every frame **[read]**.

**No destructor anywhere unlinks.** Membership is maintained only by matched
attach/detach calls **[read]**. Two paths skip it:

- `FUN_14040cea0` early-returns before the unlink when the component is not
  found in the owner's child list — and `FUN_1403ba070` frees it anyway in its
  next loop. That is a use-after-free, not a leak **[read]**;
- `FUN_1403f3bc0` ("level changed") passes flag 0, which skips the detach but
  still frees the model. The result is a node still in bucket `0xb`, drawn
  every frame, with no map resources — and that case is **designed for**, since
  the pre-draw early-exits on `+0x40 == 0`. What it does **not** survive is
  having no owner entity at `+0x8`, and nothing checks that **[read]**.

The walk validates nothing: no null check, no type check, no alive flag; the
only test is the list terminator **[read]**.

This is why the watcher that rebuilt the 32 buckets over ten legs and found
nothing proved less than it seemed: a node left behind by either path is
**perfectly well linked**. Catching it needs a per-node check of the owner
pointer at `component+0x8` and, for bucket `0xb`, of the target/code pair at
`+0x90`/`+0x98`.

## Measured on 20/09: the mine is real, and today's restriction stands on it

The no-crash test the reading asked for, run with a verified session **[read]**:

```
inst 1: estado 1 conta 149 mapa 0a100000 tabela 7ffffe47c610
    slot   0 uso  3  record+0x10 = 00007fffe849ac30
    slot   1 uso  3  record+0x10 = 00007fffe849acd0    <- 0xa0 apart
inst 2: estado 2 conta 149 mapa 0a100000 tabela 7ffffe47c610
```

The structure is exactly as read: one per-map array of 0xa0-byte records, a
raw pointer per in-use slot, 149 of them, host in state 1 and guest in state 2
on the same bound map. So a release of that map with the sync still bound
would leave 149 pointers writing into freed memory. The mechanism is not
theoretical.

**And it cannot fire today, for one reason only.** The same samples give
`joinCtrl+0x19c` = `0a100000` and `sync+0x18` = `0a100000`: **the bound map is
the session's map**, and `BeginLetGo` refuses to release that one. Both players
were standing in `0a040000` at the time, and `0a100000` sat at `estado 5
forçado 1`, held. Travelling out of it did not release it.

So risk 4's strongest surviving candidate is a landmine that **M8 item 6b is
the act of stepping off**. That is worth knowing before writing 6b, and it
turns 6b's step 2 from a precaution into the thing that keeps the game alive.

One correction to an earlier note in this repo: the sync does **not** never
rebind. It rebinds when a session forms, to that session's map. The two legs
measured earlier did not rebind because no new session formed during them. The
invariant is **bound map == session map**, not "bound map never changes".

**A second lock, and it is the same lock — corrected 20/09.** `BeginLetGo` now
also refuses a map the enemy sync is bound to, asked of the sync itself and
believed only with its vftable. It was added believing that `IsSessionMap` and
`sync+0x18` were two different sources that could disagree. **They are not.**
`DS2_BonfireInSession_SessionMap()` reads `*(*(0x141616cf8 + 0x28) + 0x18)` —
the same object and the same field. So the check can never disagree with the
one above it, and that is why it has never fired.

It is kept for one reason only: it believes the object through its vftable and
requires the state to be 1 or 2, which the older path also does, so it costs a
few reads and documents the rule at the place the rule matters. Nothing more
should be claimed for it.

## What to do, in order

1. **Make our `__try` leave a legal state.** The handler around
   `FUN_1403f6300` should write `0` into `this+0xc8` before returning. That is
   the only other legal value, it re-enables build's allocate path, and it
   costs one 0x40/0x50 block. Consider `this+0x40` too, which makes the
   pre-draw exit early at the price of leaking the model deliberately.
2. **Record `ExceptionAddress` in that handler.** Destroy dereferences `+0xc8`
   itself at `0x1403f63ff`, the same instruction shape as the crash, so only
   the fault address separates cause from consequence: a fault **at
   `0x1403f63ff`** means the pointer was already dead on entry and the leak is
   a symptom — look outside this class; a fault **inside `0x14040cea0`** and
   deeper means the object was alive and the unregister/free path broke.
3. **Teach the field guard about dangling pointers.** A dangling pointer is a
   perfectly canonical address, so "is this still an address" cannot see it.
   The `+0xc8` object's vftable is one of two known values (module-relative
   `0x10c71c8` and `0x10ea6a8`), so the guard can read it and refuse anything
   else.
4. **Null the enemy records when a map goes.** The lifetime gap in
   `FUN_140517080` versus `FUN_14040db30` is the strongest surviving candidate
   and it is ours to close: when a map is released with the session up, the
   records pointing into that map's array must be nulled first. This is also
   step 2 of the 6b plan by another name.
5. **Validate the bucket node**, per node: the owner at `component+0x8`, and
   for bucket `0xb` the pair at `+0x90`/`+0x98`.

## Discriminators, for the next fault

Dump `this+0xb0..0xf0` and compare:

- **the record writer**: `+0xc8` and `+0xd8` keep **sane low dwords** with
  garbage high halves, and `+0x40` and `+0xe0` stay valid;
- **heap reuse of the component itself**: everything is unrelated;
- under the `MMC+0xa0` overlay the position floats also land on `+0xbc`; under
  `MMC+0xb0` they **fully replace** `+0xd0`. If a dump shows `+0xb8` (guest) or
  `+0xd0` (host) intact, that overlay is wrong.

Also worth knowing: hardware watchpoints do not fire under Wine; the tool that
works is `DS2_TraceHook`'s page-protection watcher (`wp` in `DS2_Trace.req`),
pointed at `MapModelComponent+0xcc`.
