# What the vanilla game does on a warp, and why releasing a map under a live session is hard

Research made on 20/09 in Ghidra (`-noanalysis -readOnly`) and `objdump`,
against `DarkSoulsII.exe` 1.03 / Calibrations 2.02. Nothing was run in either
game. Addresses are Ghidra's at image base `0x140000000`.

Each finding is **[read]** or **[inferred]**.

Companion to [session-map-rebind.md](session-map-rebind.md), which is the
enemy sync half of M8's item 6b. This one asks the prior question: what order
does the game itself use, and is there a vanilla shape to imitate?

## The three answers

1. **`FUN_1401c2a80` tears nothing down.** It is a request. The teardown is a
   32-state machine on `GameManagerImp`: the state int at `ctx+0x24ac`, the
   flag byte at `ctx+0x24b1`, the fade timer at `ctx+0x24b4`, and a table of
   32 handlers at `0x1410c49e0` **[read]**. `0x1e` means "in the world".
2. **Vanilla always ends the session first, and waits for it.** The order is
   **notify → wait for confirmation → release locally**, never the reverse.
   That is the opposite of what the mod has been doing, and is the likeliest
   reason releasing a map killed the guest **[read + inferred]**.
3. **There is no per-map release on the warp path at all.** The release is
   wholesale, in one state, and that state destroys **every** character. The
   only two primitives in the binary that take a map id are
   `FUN_14044f7a0(eventMgr, areaId)` and `FUN_1404170c0(chrMgr, areaId)`
   **[read]** — a mod has to build its release out of those.

## The state order

```
warp:  0x1e -> 0x1f -> 0x1d -> 0x1a -> 0x17 -> 0x14 -> 0x15 -> 0x12 -> 0x13
             -> 0x16 -> 0x18 -> 0x19 -> 0x1b -> 0x1c -> 0x1e
quit:  ...                            0x15 -> 0x11 -> 0x0f -> 0x0c -> ...
```

Taken from each handler's `MOV dword ptr [reg+0x24ac], N` **[read]**. The
fork is at state `0x15`, `0x1401bee8b`: `TEST AL,0x8` — bit 3 of `ctx+0x24b1`
is "this is a warp" and sends it to `0x12`; clear means "quit to title" and
sends it to `0x11`. **The deep manager teardown lives on the quit branch, not
the warp branch.**

### A — request time, before anything local moves

At the very top of `FUN_1401c2a80`, before the destination is even stored
**[read]**, it calls `FUN_1402c7ec0(session, reason)`, which broadcasts
vtable slot `+0xe0` with **cause 4** to every session slot on the vector at
`+0x48..+0x50`, then fires subscriber slot `0x38` on `root+0x50..+0x58`.

That broadcast is one of a family, and the only thing that differs per cause
is which subscriber slot fires **[read]**:

| function | cause | subscriber slot |
| --- | --- | --- |
| `FUN_1402c7b00` | 0 | `0x18` (and closes the session directly when the vector is empty) |
| `FUN_1402c7d40` | 1 | `0x30` |
| `FUN_1402c7dc0` | 2 | `0x28` |
| `FUN_1402c7e40` | 3 | `0x20` |
| `FUN_1402c7ec0` | **4** | `0x38`, carrying the warp reason |
| `FUN_1401c2cf0` | `0xffffffff` | quit to title |

**Every one of them means "this slot is over".** There is no cause that means
"I am changing map but staying" **[read]**.

### C — state `0x1f`, the gate

`FUN_1401c0250` ticks the subsystems and refuses to advance while a session
is live **[read]**:

```
1401c026f  call 0x140513660      ; is a session still live?
1401c027a  cmovne %esi,%edi      ; yes -> proceed-flag := 0
1401c0335  je   0x1401c03aa      ; not allowed to leave yet
```

`FUN_140513660` reports live while any slot on that same `+0x48` vector
answers, or while `root[0]+0xa4 != 0` **[read]**. `FUN_140513600` reads an
arming byte at `ctx+0x22f0 + 0x3a`, written only by `FUN_140513820` **[read]**
— the game's own "should I wait for the session" switch.

So the first release call cannot run until the membership is gone.

### D and L — the one reversible pair

State `0x1d` suspends, state `0x1b` resumes, and they are exact inverses
**[read]**:

| suspend (`0x1d`) | resume (`0x1b`) | what |
| --- | --- | --- |
| `FUN_14044ee60(ctx+0x70)` | `FUN_14044ef30` | EventManager pending list |
| `andb $0xf7, chrMgr+0x304` | `orb $0x8` | a ChrManager flag |
| `FUN_140246db0(ctx+0xa0)` | `FUN_140246dc0` | state 3 → 2 → 3 |
| **`FUN_140513340(ctx+0x22f0)`** | **`FUN_1405133c0`** | **the net sync** |

`FUN_140513340` clears the peer slots (`FUN_14051bff0`), takes the object
sync at `root+0x30` from 3 to 2 and frees its `+0xb8` helper
(`FUN_14028fe50`), and clears a byte on `root+0x18` **[read]**.
`FUN_1405133c0` puts all of it back **[read]**.

**This is the part of the vanilla order that is designed to be reversible,
and it is the shape a seamless map swap should imitate.** Keep them paired:
an unpaired suspend leaves `root+0x30` stuck at 2 with its helper freed.

### G — state `0x14`, the step with no safe equivalent

```
movq $0x0, 0xd0(%rbx)        ; the local character pointer, dropped
...
call 0x1403bd2e0  (ctx+0x38) ; the whole MapArea manager
call 0x14044f4f0  (ctx+0x70) ; the whole EventManager
call 0x140416360  (ctx+0x40) ; the ChrManager
```

and `FUN_140416360` is **[read]**:

```c
for (i = 0; i < 0x2a; i++) FUN_14041a900(mgr, i);
for (each of the 42 slots) if (*slot) { destructor; free; *slot = 0; }
```

**All 42 character slots, unconditionally, with no map filter and no "keep
the remote players" predicate.** The other player's character lives in that
array. There is no selective variant anywhere in the binary.

So a mod that releases a map without ending the session **cannot run state
`0x14`**, and there is no smaller call to substitute: the MapArea and
EventManager releases in the same state are whole-world too.

### What the warp branch does instead

State `0x12` **[read]**: `FUN_140416220(ctx+0x40)`, `FUN_14044f2c0(ctx+0x70)`,
`FUN_1403bd230(ctx+0x38, newMap)` — which frees the old map-area controller
and allocates a fresh one — and `FUN_1405136a0(ctx+0x22f0)`. **No "release
map M" call anywhere**: the release already happened wholesale in `0x14`.

State `0x13` rebuilds the local character, but only when `ctx+0xd0 == 0`
(`0x1401bebc3`) **[read]** — so a mod that skips `0x14` also skips the drop
and must move the character itself.

The session objects survive a warp: `root[0]`, `root[3]` and `root+0x30` are
only destroyed by `FUN_140513830` in state `0x11`, which the warp branch
skips **[read]**. The object sync goes 3 → 2 → 3 on a warp and only reaches 1
on a quit.

## What a mod would have to do

1. **Skip the notification.** There is no vanilla cause meaning "I am moving
   but staying"; a bespoke message has to go over the mod's own channel.
2. **Neutralise the gate.** `FUN_140513660` never goes false with a live
   session, so state `0x1f` never advances. The clean lever is the arming
   byte at `ctx+0x22f0 + 0x3a` through `FUN_140513820` **[inferred; not
   tested]**.
3. **Imitate the suspend/resume pair**, `FUN_140513340` / `FUN_1405133c0`,
   around the swap. Keep them paired.
4. **Never run state `0x14`.** Build the release from the two per-map
   primitives instead: `FUN_14044f7a0(ctx+0x70, map)` and
   `FUN_1404170c0(ctx+0x40, map)`. Those two are the vanilla shape of "one
   map went away" and are the only calls on this path that take a map id.
   The mod already calls the first one, from `DS2_BackreadHook`.
5. **Never let `ctx+0xd0` be null while the session is live.**

## What this does not establish

- **The Arxan-hidden prologues.** Every state handler is reached through an
  obfuscated trampoline; only the visible `.text` bodies were read, and state
  `0x1d` opens with a virtual call on an object loaded in the hidden part
  that was not resolved. Order **between** states is solid (it comes from the
  `MOV [+0x24ac], N` writes); order **within** a state is read but not
  provably complete.
- States `0x18` and `0x19`, both on the way back in.
- Who invokes `GameManagerImp` vtable slot `+0x88` (`FUN_1401c2080`, "this
  map went away"): no code caller was found, only the vftable entry. Its two
  callees are invoked directly by `FUN_1403ca8d0` stage 1 instead.
- **A slot to reconcile.** This reading puts the object sync with the 3/2/1
  lifecycle at **`root+0x30`**, while the mod's code and
  [session-map-rebind.md](session-map-rebind.md) call `root+0x28` the enemy
  sync (the one `FUN_1405170e0` updates). They are most likely two different
  objects rather than a contradiction, but nothing here proves that.
