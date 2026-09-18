# The streaming budget: why a third map stays in state 0

Research note, 18/09. Read-only work in Ghidra (`/home/suel/tools/proj-a5`) and
objdump against `DarkSoulsII.exe` 1.03 / Calibrations 2.02, preferred base
`0x140000000`. Nothing here was measured on the live game; every claim about
the binary names its address and the line it rests on. **Read** marks what the
decompiler or disassembly says. **Inferred** marks what I conclude from it.

## 1. The limit

### 1.1 The check (read)

`FUN_1403cc450` is the state machine of one `MapAreaCtrlOwner`. The only way out
of state 0 is its `case 0`:

```c
// FUN_1403cc450, case 0
if (*(char *)(owner + 0x1e9) == 0) owner[0x1eb] = owner[0x1ea]; else owner[0x1eb] = 1;
...
case 0:
  if (cVar5 /* owner+0x1eb */ != 0) {
    if (*(longlong *)(DAT_1416751f8 + 0x80) != 0) {
      if (FUN_140b04ca0(*(DAT_1416751f8 + 0x80)) != 0) goto refuse;     // gate A
      mgr = *(DAT_1416148f0 + 0x38);
      if (mgr != 0) {
        iVar6 = FUN_1403bcf20(mgr);   // *(int *)(*(mgr+0x10)+0x18)
        iVar7 = FUN_1403bcda0(mgr);   // *(short *)(*(mgr+0x08)+0x1b4)
        if (1 < iVar6 - iVar7) goto refuse;                              // gate B
      }
    }
    *(owner+0x1f0)->[0x11] |= 1;  owner[0x1e8] = 1;  return;
  }
refuse:
  *(owner+0x1f0)->[0x11] &= ~1;  return;
```

The instructions (objdump, `+0x3cc4c9` to `+0x3cc4fa`):

```
1403cc4e1: call 0x1403bcf20          ; ebx = world-list count
1403cc4eb: call 0x1403bcda0          ; eax = owners in state 0
1403cc4f0: 2b d8                     sub  %eax,%ebx
1403cc4f2: 83 fb 01                  cmp  $0x1,%ebx
1403cc4f5: 48 8b 5c 24 30            mov  0x30(%rsp),%rbx
1403cc4fa: 7f 1d                     jg   0x1403cc519   ; refuse
```

What the two numbers are (read):

- `FUN_1403bcf20(mgr)` returns `*(int *)(*(mgr+0x10)+0x18)`. `mgr+0x10` is a
  `MapWorldList` (built by `FUN_1403ddac0`, vftable `0x1410e9af0`, called from
  `FUN_1403bd0f0`); `+0x20` holds the map ids and `+0x18` their count
  (`FUN_1403bce90` indexes it that way).
- The streamer (`mgr+0x08`) builds one owner per entry of that list:
  `FUN_1403dc0a0` loops `iVar1 = *(int *)(list+0x18)` times (refusing lists of
  `0x2b` or more), allocates a `0x210`-byte owner for each and `INC word ptr
  [RBX+0x1b6]` (`0x1403dc1c3`) for each one that initialises. So the first
  number is the number of owners, all maps of the world (at most 42), not the
  number loaded.
- `FUN_1403bcda0(mgr)` returns `*(short *)(streamer+0x1b4)`. Its only writer is
  `FUN_1403dc3e0` (the streamer's frame, `0x1403dc824 INC word ptr
  [RBX+0x1b4]`), and the only caller of `FUN_1403bcda0` is this gate. Every frame,
  **after** updating all owners, `FUN_1403dc3e0` zeroes the dword at `+0x1b2` and
  counts: owners with `+0x1e8 == 0` go into `+0x1b4`, owners with `+0x1e8 == 5`
  go into `+0x1b2`.

So **gate B is: owners - owners in state 0 <= 1, or the owner may not start
loading.** At most two owners may be outside state 0 at once, and a new load
may only start while at most one is. The two maps are the player's own map and
one more.

**There is no memory budget, pool or queue in this check.** It is an immediate
`1` in a compare, and the counter is a count of state bytes. The value is the
byte at **`+0x3cc4f4`** (file offset and module offset agree, since the image
base is `0x140000000`).

### 1.2 What counts against it (read)

States 1 to 7 all count. That includes 6 and 7, the teardown: `case 5` with
`+0x1eb == 0` runs `FUN_1403cc3a0` (which loops `FUN_1403cb1a0` until it
returns true) and falls through to `state = 6`. `case 6` waits for the parts
controller `*(owner+0x1f8)+0x10` to read 0, then frees it (`FUN_1403cc1d0`) and
goes to 7. `case 7` waits for the resource object `*(owner+0x1f0)+0x10` to read
0, then goes to 0. **A map being released holds its slot until it reaches 0.**

A refused owner only clears bit 0 of `*(owner+0x1f0)+0x11` and returns. It tries
again the next frame, so any change that lifts the gate takes effect on an owner
that is already forced and waiting.

### 1.3 The two other gates (read, meaning inferred)

- The whole budget check is skipped when `*(DAT_1416751f8+0x80)` is null. I did
  not find what that object is. `FUN_140b04720` is its getter, with 42 callers.
- Gate A: `FUN_140b04ca0(obj)` is `obj[8] != 0 && FUN_1408f9120(&DAT_1418757c0)
  != 0`. `FUN_1408f9120` locks, reads `[+0x10]` and unlocks. `FUN_140b05e90`
  increments that counter around a request it hands to `*(obj+0x138)`.
  Inferred: a count of asynchronous requests still in flight. It is a transient
  "busy" gate, not the cap. The resource object's own update, `FUN_1403d20e0`,
  checks the same gate before it opens the map's files.

### 1.4 The count is stale inside a frame (read, consequence inferred)

The counters are rebuilt **after** the owner loop in `FUN_1403dc3e0`, so every
owner's gate in frame N reads frame N-1's count. Two owners that are both in
state 0 and both wanted in the same frame both pass a check that reads 1, and
three owners end up outside state 0. The cap is therefore "at most one other
map busy as of last frame", not a hard ceiling. Two consequences:

- vanilla can briefly have three maps in flight. That is weak evidence that the
  control heap has some headroom, and no more than that.
- an eviction scheme (section 4.2) must keep the evicted neighbour unwanted
  until the destination has left state 0. Otherwise both race in the same
  frame and the neighbour can come back.

### 1.5 How this explains the measurements (inferred)

| measured | owners outside state 0 when D is forced |
| --- | --- |
| Majula held + Heide current, force Iron Keep | Majula (5), Heide (5) → 2 → refused forever |
| Heide current + No-man's Wharf `0x0a1e0000` streamed by the game, force D | Heide (5), Wharf (5) → 2 → refused forever |
| Heide current only | 1 → D goes 0→1 |

The travel then waits for the destination's nav cell before it moves the focus
(`ResolveFocusCell` in `DS2_BackreadHook.cpp`), and the nav only comes in with
the load. So nothing ever pushes the neighbour out and D never gets a slot.
The travel deadlocks: the focus needs D's nav, and D's load needs the slot the
neighbour holds.

**A comment in the hook disagrees with the binary.** `OwnerUpdateHook` says "a
forced owner with an empty mask never leaves state 0". `case 0` does not read
any mask: the only inputs are `+0x1eb`, gate A and gate B. The 16/09 symptom
(the host's copy forced the destination on the guest, whose own request then
never loaded) matches gate B, because the guest was already holding two maps.
Inferred, but the decompile leaves no mask-based alternative.

## 2. How the game streams neighbours

### 2.1 The graph search (read)

- `FUN_1403be060` (the world's frame, once) finds the part under the player's
  feet (`FUN_140312ba0`, kind byte `0xa2 == 2`) and the nav cell, and calls
  `FUN_1403dc8e0(streamer, pos, cell, part, flag)`. That stores the part at
  `streamer+0x28` and its map index (`FUN_1403ba380(part)` = `*(*(part+0x28)+0xc)`)
  at **`streamer+0x30`**, and calls `FUN_1403dadd0(streamer+0x18, pos, cell, 0)`.
  With `cell == -1` it resolves one itself: nav map `FUN_140badb90`, then the
  nearest cell `FUN_140babf90(navmap, pos, 10.0, 0x40)`. A changed cell restarts
  the search.
- `streamer+0x18` is the backread object (`0x870` bytes, `FUN_1403da4d0`). Its
  `+0x860` is an `NvBackreadSearch` (`FUN_1403da900` → `FUN_140bae610`).
  `FUN_1403da960` starts `FUN_140bb2d60(search, cell, max(+0x3c, +0x2f4))`
  and, once the async search reports done (flag bits 2/4 at `search+0x30`),
  walks the reached cells (the tree at `+0x848`). It feeds each one into two node
  lists, `+0x38` and `+0x2f0`, via `FUN_1403db560`.
- Thresholds, from `FUN_1403db080` for both lists: `[0] = 25.0`
  (`0x41c80000`, add), `[1] = 30.0` (`0x41f00000`, keep), `[2] = 0.2`, `[3] =
  1.3`. `FUN_1403db1a0` moves each node through `node+0x1a` states (1 new, 2
  wanted, 3 fading) with a timer at `node+0x1c`, frees it when it expires, and
  ORs the part mask of every node in state 2 or 3 into a per-map slot (16 bytes
  per map index). Inferred: the parts along the nav graph within about 25 to 30
  units of the player are requested, with roughly a second of hysteresis.

### 2.2 "Wanted" (read)

`FUN_1403dc930` (`streamer`, called once a frame from `FUN_1403dc3e0`, **before**
the owner updates) turns those slots into the owners' fields.

- **Graph mode** (`streamer+0x1f0 >= 0x2a`): for each owner index `i`, the masks
  come from the backread slots, from `FUN_1403cbb20` → `FUN_1403ccac0` (the
  owner's `+0x168` connection list, which adds masks to its own map and, through
  `FUN_1403bcc50` → `FUN_1403dbd80` → `FUN_1401c8cd0`, to connected maps) and
  from `FUN_1403dd530` (the current part). Then `owner+0x1ea = 1` if
  `i == streamer+0x30` (the player's map is always wanted), else `1` iff either
  summed mask for `i` is non-zero (`0x1403dd004` / `0x1403dd0cb` / `0x1403dd0d4`).
  The same function writes `+0x20`, `+0x30`, `+0x50`, `+0x60` and `+0x70`.
  `FUN_1403dc3e0` writes `+0x10` and `+0x40`.
- **Explicit mode** (`streamer+0x1f0 < 0x2a`): `owner+0x1ea = (i ==
  streamer+0x1f0)` for every owner (`0x1403dc9ef`). The target gets the masks at
  `streamer+0x1d0/+0x1e0`. **Every other owner has `+0x20..+0x3c` and
  `+0x50..+0x7c` set to the empty mask, every frame.**

So a neighbour unloads by itself (5→6→7→0) only when no reached cell and no
connection puts a part of it in the masks, and only after the node hysteresis.
While the player stands in Heide near the Wharf passage, the Wharf stays wanted.

### 2.3 The per-map "allowed" byte (read)

At the top of every streamer frame, `FUN_1403dc3e0` fills the 42 bytes at
**`streamer+0x188`**:

```c
for (i = 0; i < 0x2a; i++)
  streamer[0x188 + i] = FUN_1402db4f0(*(DAT_1416751f8 + 0x368), mapId(i));
// all set to 1 when either pointer is null
```

`FUN_1402db4f0` looks the map id up in a tree and returns `count > 0`
(`FUN_140b2c390`). Its meaning is inferred: whether the map's data is available.
Every mask source in graph mode checks this byte for the map it writes into:

- the backread accumulation in `FUN_1403db1a0` (`*(char *)(idx + param_3) != 0`,
  where `param_3 = streamer+0x188`),
- the `+0x5a8` accumulation in `FUN_1403da960` (same test),
- the owner loop in `FUN_1403dc930` (`*(char *)(streamer + 0x188 + i) != 0`
  before `FUN_1403cbb20`),
- `FUN_1401c8cd0`, the connection fan-out (`*(char *)(idx + param_4) != 0`).

The only exceptions are the current map (`streamer+0x30`, always wanted) and
explicit mode. **A zero in `streamer+0x188[i]` makes map `i` look exactly like
a map the graph search does not reach: empty masks, `+0x1ea = 0`.** The game
then takes it down along the same 5→6→7→0 path as when the player walks away.

The other caller of `FUN_1402db4f0` is `FUN_1401ea510`. It checks the next map's
parts and sets `*(DAT_1416148f0+0x2461) = 1` when they are not ready. Inferred:
the "wait for the next map" stop when a player reaches a passage early. With
the byte at 0 it returns early and sets nothing.

### 2.4 The game's own "only this map" (read)

`FUN_1403bcc60(mgr, mapId, mask*)` puts the map's index in `streamer+0x1f0`,
its mask in `+0x1d0/+0x1e0`, and sets `+0x1f6 = 1`. `FUN_1403bccf0(mgr)` sets
`+0x1f0` back to `-1` with empty masks. The one caller of each is `FUN_1404834e0` /
`FUN_140482fd0`, from `FUN_140481900`, the update of the object at `ctx+0x1170`
(called at `0x1401bfaef`). Inferred: this is the game's in-world warp or load
sequence. It then waits on `FUN_1403bcfe0` → `FUN_1403dc010`, which is true when
every owner is in state 0 or 5 and ready (`FUN_1403cbcb0`).

This is the game's native way to evict everything except one map. In a travel
without a loading screen it also evicts the map under the player, and it
blanks the visibility masks of every other owner every frame. So it is not the
lever to use, except behind a black screen with the player frozen.

## 3. Can the limit be raised, and what depends on it

### 3.1 The constant (read)

It is one byte: `+0x3cc4f4`, the immediate of `83 fb 01` at `+0x3cc4f2`.
`02` allows three owners outside state 0, and `03` allows four. Nothing else
reads `+0x1b4`, and nothing else calls `FUN_1403bcda0`. The only site
that encodes "two maps" in this subsystem is this compare.

Sites I checked that turned out **not** to be a per-map limit:

- `MapTextureManager` (`mgr+0x180`, vftable `0x1410ec810`, `FUN_1403ff830`) has
  two lists at `+0x18/+0x20` and `+0x28/+0x30`. The `2` in `FUN_1403fffa0` and
  in `FUN_1403bcfe0` counts two categories (flag bit 6 at `+0x48`), not maps.
- The per-map arrays (masks, allowed bytes, owners) are all sized 42.

### 3.2 The memory behind it (read, with the size unknown)

`FUN_140512ba0` (called from `FUN_1401c1ab0`, a virtual of `GameManagerImp`;
the matching teardown is `FUN_140512ab0` from `FUN_1401c10b0`, so inferred to
run on each world entry and exit) builds `*DAT_141616cc0`:

- `[0]` is a `DynamicHeapMemoryTemplate<WinAssertHeapStrategy<...DLRegularHeap...>>`
  named **"MapSeamlessControl"**, of **`0xd80000` bytes (13.5 MiB)**, carved
  from heap 2: `FUN_140b21530(heap, parent, 0xd80000, "MapSeamlessControl")`
  at `0x140512c92` (`41 b8 00 00 d8 00`) and `FUN_140278d60(..., 0xd80000, 0)`
  at `0x140512cb4`.
- `[+0x10 + 8k]`, 25 entries as (allocator, name) pairs: the even entries
  (`MapControl`, `ObjControl`, `NaviMeshControl`, `EnemyControl`, `EventControl`,
  `AiControl`, `DamageControl`, `BulletControl`, `SoundControl`, `SignControl`,
  `StateActControl`, `FrontendControl`) are the MapSeamlessControl heap. The odd
  entries (`MapResource`, `ObjResource`, `NaviMeshResource`, ... ,
  `MorphemeNetworkInstance`) are heap 2 itself (`FUN_140aee210(2)`).
- The map manager is allocated at `0x1401bbf51` and built by
  `FUN_1403bc960(obj, DAT_141616cc0[+0x10])`, so it runs on the MapControl
  allocator. The streamer (`FUN_1403bd230`), every owner (`FUN_1403dc0a0`), and
  each owner's per-load objects (`FUN_1403cbed0`: `0x370` at `+0x148`, `0x20` at
  `+0x1f8`) come from that same allocator (`owner+0x88`). The map's parameter,
  ESD and sound files in `FUN_1403d20e0` go to the resource allocators
  `+0x28/+0x48/+0x58/+0x98`, which are heap 2.

What I could not find: **the size and headroom of heap 2**, and where the map
geometry and textures go. `FUN_140aee210` and `FUN_141cf3fc0` jump through
obfuscated stubs. Whether a third map fits in 13.5 MiB of control objects, and
in heap 2, is a question for the live game. The heap's strategy is
`WinAssertHeapStrategy`, and inferred from the name, an allocation it cannot
satisfy is an assert, not a null. Most callers do test for null
(`FUN_1403cbed0` does), but I did not audit the deep ones.

Inferred: the cap of two is FromSoftware's budget for these heaps. It was
probably set on the 2014 consoles and carried over to SotFS. Raising it is
safe only as far as those heaps allow, and that has to be measured.

### 3.3 Reading the heap live

`FUN_140278d00` (vftable slot `+0x50` of `0x1410d3058`) returns
`FUN_140857050(H+0xb8) - FUN_140278920(H+0xb8)`, that is total minus used.
`FUN_140857050` reads `*(H+0xb8+0x438)` = **`H+0x4f0`** (the total, expected
`0x00d80000`). The large-block part of "used" is `*(H+0xb8+0x18)` =
**`H+0xd0`**. `FUN_140278920` then adds the small-object pages, so `H+0xd0` is a
lower bound. `H = *(*(base+0x1616cc0) + 0)`:

```
ds2os-dev probe --instance N "chain msc 1616cc0 0 1280"
```

This reads `H[0..0x500)`. The first check is that `H+0x4f0` really reads
`0xd80000`. If it does not, the layout reading is wrong and nothing below
should be trusted.

## 4. Proposal

The rule the binary imposes: **at the frame the destination leaves state 0, at
most one other owner may be outside state 0 on that machine, as of the
previous frame.** Each machine has its own streamer, so this holds on host and
guest separately.

### 4.1 Raise the cap (primary, once the heap is measured)

Add a `.text` patch in the style of `DS2_UnblockMultiPlayHook`, on both machines
at injection:

| where | expect | write |
| --- | --- | --- |
| `+0x3cc4f0` | `2b d8 83 fb 01 48 8b 5c 24 30 7f 1d` | byte `+0x3cc4f4` → `02` or `03` |

- Leave the `jg` in place: a ceiling is still needed, just a higher one.
- Prefer `03` (four maps): the player's map, the held meeting map, the
  destination, and the neighbour the game streams in by itself (No-man's Wharf
  at Heide) already make four. With `02`, the Heide case is back to state 0.
- The patch takes effect immediately, even on an owner already waiting in
  state 0 (1.2). No relaunch is needed beyond loading the DLL.
- The cost is memory, and it is unknown (3.2). Measure before shipping
  (section 6). If the MapSeamlessControl heap runs short, its size is the two
  `0xd80000` immediates at `0x140512c92` and `0x140512cb4`. Inferred: those
  are read on each world entry, so a patch made at the title screen applies to
  the next world. That would only move the problem into heap 2, whose
  headroom is also unknown.

### 4.2 Make room instead (no change to the cap)

If the heap cannot take a third map, the travel must free the slot before it
forces the destination:

1. **Maps the mod holds**: release them with the existing two-stage release
   (bits back, then force byte) **before** forcing the destination, and wait
   for their owner's `+0x1e8` to read 0, not 6 or 7 (1.2). Holding the meeting
   map for the whole session (the fix that needed it) is incompatible with a
   cap of two, by construction.
2. **Neighbours the game streamed**: detour `FUN_1403dc930` (`void(streamer,
   arg)`, one caller, `FUN_1403dc3e0`) and, before calling the original, write
   `0` into `streamer+0x188[i]` for each victim index `i`, every frame, from the
   moment the travel starts until the destination owner reaches state 5 and
   the focus has moved. Victims are the owners outside state 0 that are neither
   the player's map (`streamer+0x30`) nor the destination, nor a map where the
   other player stands. Then the game itself computes empty masks and
   `+0x1ea = 0` for the victim in the same pass that consumes them. It tears
   the map down along the path it uses when the player walks away, and it
   cannot bring the map back while the byte is held at 0 (1.4). When the
   override stops, the next frame recomputes the byte from `FUN_1402db4f0`, so
   nothing persistent is left behind.
   - Why not write `owner+0x1ea = 0` in the existing owner-update hook: that
     runs after `FUN_1403dc930` has already written and used the masks for
     the frame. The owner would tear down while its masks still ask for parts,
     which is the "released with part bits still on" shape that crashed on
     17/09. The `+0x188` byte is upstream of both.
   - Why not `FUN_1403bcc60`: it also evicts the map under the player and blanks
     every other owner's visibility masks every frame (2.4).
3. Only then force the destination. With the player's map and the destination
   as the only two, gate B passes.

If the victim is a map where the other player's copy stands, 4.2 cannot free
it, and only 4.1 helps.

## 5. What is unknown

- What `*(DAT_1416751f8+0x80)` is. When it is null, the cap is skipped
  entirely.
- The meaning of `FUN_1402db4f0`'s tree (`DAT_1416751f8+0x368`), and whether
  zeroing its answer for a map has an effect outside the streamer. The only
  other caller is `FUN_1401ea510`, which it makes return early.
- The size and headroom of heap 2, where the map geometry and textures live,
  and whether three or four maps fit.
- Whether `WinAssertHeapStrategy` asserts or returns null on exhaustion.
- Whether any part of the teardown (`FUN_1403cb1a0`, parts controller
  `FUN_1403cf480`) misbehaves for a neighbour torn down while the player
  still stands next to it. The game does this in normal play, but it has not
  been measured during a session.

## 6. Measurements that would confirm it

All of these are MemProbe reads, on both instances.

1. **The gate.** With two maps loaded and a third forced, read the streamer:
   `chain st 16148f0 38,8 512` (`*(*(ctx+0x38)+8)`, `0x200` bytes). Expect
   `+0x1b6` (u16, owners) minus `+0x1b4` (u16, state-0 count) `== 2`, `+0x1b2`
   (u16, state 5) `== 2`, and the forced owner's `+0x1e8 == 0` with its `+0x1e9
   == 1`. Also read `+0x30` (the player's map index) and `+0x1f0` (expected
   `0xffffffff`, graph mode).
2. **The patch.** `pokemod` `+0x3cc4f4` `03` with expected `01`, both instances,
   in the Heide + Wharf case. The forced owner's `+0x1e8` should go
   0→1→…→5 in `DS2_Backread.log` without anything else changing, and
   `+0x1b6 - +0x1b4` should read 3. Put the byte back afterwards.
3. **The heap.** `chain msc 1616cc0 0 1280` with one map, with two (Majula +
   Heide, then Brume Tower + a neighbour, the largest pair available), and with
   three after the patch. Record `H+0x4f0` (total, expect `0xd80000`) and
   `H+0xd0` (used, lower bound). If two maps already use more than about 60%
   of it, raise the cap only together with the heap size, or not at all.
4. **The release time.** From the moment a mod hold is dropped, count the
   frames the owner spends in 6 and 7 before it reads 0. That is the delay 4.2
   must wait before forcing, and part of why a 30 s travel budget runs out.
5. **The eviction.** A MemProbe poke of `streamer+0x188[i]` does not hold,
   because `FUN_1403dc3e0` rewrites the array at the top of every frame, so this
   needs the `FUN_1403dc930` detour in a test build. Standing at Heide's first
   bonfire, zero the byte for the Wharf's index (the owner whose `+0x08` reads
   `0x0a1e0000`; its index is at `+0x0c`) and watch that owner go 5→6→7→0 with
   no `excecao` in `DS2_Crash.log` and no `FALHA APARADA` in `DS2_Backread.log`.
   A destination forced after that should pass the gate.
