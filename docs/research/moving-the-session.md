# Can a live session be moved to another map?

Research made on 20/09 in Ghidra (`-noanalysis -readOnly`) and `objdump`,
against `DarkSoulsII.exe` 1.03 / Calibrations 2.02. Nothing was run in either
game. **[read]** / **[inferred]**.

The other half of M8 item 6b: instead of tearing the pinned map out, can the
session simply be **re-pointed** at the destination?

## The verdict

**Partially supported — the message exists, the acceptance does not.** And
one surprise: **the vanilla host *can* change map with a live session.** It is
not torn down at the warp; it gets a grace period of at most 300 s and then
ends. Nothing in that window follows the host.

## The message that says "the world owner is in map X"

It is **packet `0x0a`**, 36 bytes, `{map, x, y, z, ?, angle, ?, ?, ?}`
**[read]**. Received, it reaches `FUN_1402c2a80` — the guest's entry warp —
which writes `joinCtrl+0x19c` (the session map, see
[session-map-rebind.md](session-map-rebind.md)) and warps on reason 4.

`0x0e`, 100 bytes, also carries a map id: the host's map and position, sent
once at the handshake's `0xf → 0x10` transition **[read]**.

## The four gaps

1. **`FUN_1402c2a80` refuses outside ctrl state 2.** A second `0x0a` on a
   live session does not re-target it — it drives the controller to state
   `0xb` with code 1, i.e. it **kills the session**. The guest's state
   machine has no "re-enter" case **[read]**.
2. **Nothing sends `0x0a` after the handshake.** Its only senders sit in
   Accept state `0xa`; the established-state handler never sends it
   **[read]**.
3. **No re-arm of `sync+0x198`.** Only the state-4 snapshot import sets it,
   and the unbind clears it — so even with `+0x19c` rewritten, the enemy sync
   stays unbound **[read]**.
4. **The host-side grace timer is a deadline, not a hand-off** — see below.
5. **No "host changed area" event exists at all.** The five broadcasters
   carry event codes 0–4 with no payload, and the controller treats code 4
   purely as "abort if not yet established, otherwise start the clock"
   **[read]**.

## The 300 s grace timer — which this mod already knows

Every warp calls `FUN_1402c7ec0`, which broadcasts a **hardcoded event 4** to
every Accept controller **[read]**. The handler `FUN_1402bd0d0` case 4:

```c
*(uint *)(ctrl + 0x1b8) |= 0x10;            // the warp flag, no timestamp
if (state(+0x150) != 0x10) { abort, reason 6 }   // not established
break;                                       // established: just record it
```

and `FUN_1402be090`, called at the top of the host's per-tick handler,
consumes it **[read]**:

```c
if ((*(byte *)(ctrl + 0x1b8) & 0x10) == 0) return;
if (clock - *(float *)(ctrl + 0x1b4) <= 300.0) return;
end the session; state = 0x13;
```

> **This is the same 300 s the mod already fights.** `DS2_SeamlessSessionHook`
> clears bit `0x10` of `ctrl+0x1b8` on every host tick — so **gap 4 is
> already closed in this repo**, and that clearing is now explained: it is
> not a workaround for a mystery watchdog, it is the vanilla "the host warped"
> flag, and the mod is telling the game the host never warped.

That grace window is also, **[inferred]**, why a pinned map appears to work
for a while at all.

## What imitating vanilla would take

All **[inferred]** as a plan, on top of the reads above:

1. On the **host**, hook `FUN_1402bd0d0` case 4 so that instead of only
   setting the flag it re-sends `0x0a` through `FUN_1402cf020` with the new
   map and the guest's landing position.
2. Neutralise `FUN_1402be090`'s 300 s branch — **already done**.
3. On the **guest**, relax `FUN_1402c2a80`'s `state == 2` guard to accept the
   established state too.
4. Drive `FUN_140517080` then `FUN_140516370` around that re-entry so the
   enemy sync re-binds.

Four patches, three of them still to write. **The game has no supported path
that does this by itself.**

## Two corrections to earlier notes

- `root[3]+0x40` is the **Join** controller — "me, as a guest in someone
  else's world" — and is **null on a host**. `root[3]+0x48..0x58` is a
  *vector* of **Accept** controllers, "guests in my world" **[read]**.
- `FUN_14051e3d0` is **not** "the" packet registry: it is one of about twenty
  registration sites and covers only ids `0x20`, `0x2a`–`0x2c`, `0x31`
  **[read]**. The full table was recovered; the session-control family is
  `0x04`–`0x12`, and `NetEnemyManager` owns `0x14`–`0x18`.

## Not decoded

The payloads of `0x04`, `0x06`, `0x08`, `0x0c`, `0x0d`, `0x0f`, `0x10`,
`0x11`, `0x12`. Only `0x0a` and `0x0e` were **verified** to carry a map id.
