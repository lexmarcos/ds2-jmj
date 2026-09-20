# Respawn, bonfires, and whether they need the map loaded

Research made on 20/09 in Ghidra (`-noanalysis -readOnly`) and `objdump`,
against `DarkSoulsII.exe` 1.03 / Calibrations 2.02. Nothing was run in either
game. `ctx = *0x1416148f0`, `PD = *(ctx+0x70)`. **[read]** / **[inferred]**.

For M8 item 6b. The short answer is the good one.

## The answer

**Nothing in the respawn machinery is owned by a map owner.** Every table it
consults either lives for the whole play session or is registered and
unregistered by the map owner's own staged load and teardown — in the right
order, with the slot **erased rather than nulled**. A map released
mid-session leaves a **clean miss**, not a dangling pointer **[read]**.

And **the game already warps to a bonfire in an unloaded map, every time you
die**: the request carries the map id, resolved without the map; the loader
brings that map to state 5; the position is looked up only then **[read]**.
That is exactly the manoeuvre the mod needs.

## Two bonfire tables, and the trap of conflating them

1. **The record array — whole session, map-independent.**
   `EventBonfireManager = *(PD+0x58)`; `mgr+0x20` is a flat array of 0x18-byte
   records sorted by id, count at `+0x28`. `FUN_14017c110` finds the index
   from a bonfire id and `FUN_14017c230` gives **its map id** — no map
   involved **[read]**. It is freed only on the trip back to the title.
2. **The live component list — follows each component.** `mgr+0x08` is an
   intrusive list of live `MapObjBonfireComponent`s; init pushes, terminate
   unlinks. A lookup on a released map returns **0** **[read]**.

And a third, per map: the **map-point table**, `EventPointManager =
*(PD+0x30)`, a vector of per-map area ctrls. Created at the owner's load and
destroyed at **teardown step `0x0e` — the first step**, before any resource
is freed, by an **erase** rather than a null **[read]**. So a lookup for an
unloaded map simply does not find it and returns 0, and every caller
null-checks.

## A guest never builds its own respawn warp

The death terminal forks **[read]**: with a session, it hands off to the
session (`FUN_1402c9220`); solo, it builds a local respawn. The go-home
builder `FUN_1402c3900` reads a block filled at join time — the guest's own
respawn record and transform, copied as **three ints and some floats, no
pointer** **[read]**.

`FUN_14044fe30`, which sets the respawn point, stores exactly three ints.
Its only other act is one hard-coded special case where it **copies** a
point's position out and hands the values on; nothing retains the entry
pointer **[read]**.

## Warping to a bonfire in an unloaded map

```c
FUN_1401843b0(req, bonfireId, reason):
    FUN_14017c110(&idx, bonfireId);      // id -> record index
    FUN_14017c230(&idx, &map);           // record -> MAP ID, no map needed
    req = { family 3, reason, map, ..., bonfireId };
```

and the context tick will not resolve a position until the destination is
loaded **[read]**:

```
1401beb83: call 0x1403bcec0    ; map id -> index
1401beb9f: call 0x1403bcf40    ; -> owner+0x1e8 == 5
1401bebc3: cmpq $0x0,0xd0(%rdi); the player does not exist yet
1401bebd7: call 0x1401c3c60    ; only now: resolve the position
```

`FUN_1401c3c60` case 3 goes through the **live** bonfire component and case 4
through the map point and its param row — both present, because the guard
already established state 5. On a miss both leave the position at the origin:
a wrong spawn, never a crash **[read]**.

## What this means for the mod

- Releasing the session's map is **safe for everything the native warp path
  does**.
- **The danger is placement that bypasses the loader** — teleport plus
  streamer focus, which is what the mod's travel does. `FUN_14017f170`,
  `FUN_1404799f0` and `FUN_140451930` all return 0 for a released map, and
  the resolver then leaves the position at `(0,0,0)` **silently**. If the mod
  resolves a position before the map is back at state 5, that is what it
  gets.
- **Never cache a `MapObjBonfireComponent*` or a point object across a
  release.** The component is unlinked and the area ctrl freed at teardown
  step `0x0e`.
- One in-game cache exists: `EventTaskPointCtrl` stores a resolved point at
  `+0x30` and recomputes only when it is null. Almost certainly per-area and
  destroyed with the map — **[inferred]**, not verified.

## What this could not establish

- The body of `FUN_1403bcec0` (an Arxan-folded dispatcher). From its first
  real instruction and the streamer layout it is "index of this map in the
  game's map list", negative only for an **unknown id** — so the `< 0`
  fallback in the death builder is a bad-id guard, **not** a not-loaded
  guard **[inferred]**.
- The listener chain between the session hand-off and the go-home builder:
  both ends found, the vtable in between not walked.
- What `owner+0x168 → +0x3c` means, the second readiness flag in the guard.
- Which of the three go-home modes a white phantom actually gets: it comes
  from a role row that was not dumped.
