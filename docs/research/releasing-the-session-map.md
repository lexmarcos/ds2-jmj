# Releasing the session's map: what ten readings say, and the order to do it in

Written 20/09 from ten parallel Ghidra readings, all `-noanalysis -readOnly`,
against `DarkSoulsII.exe` 1.03 / Calibrations 2.02. **Nothing here was run in
either game.** This is M8 item 6b: the map a session began in is pinned for
the whole session, its target cost is spent the whole time, and a heavy
pinned map blocks travel to a heavy destination.

The ten readings are in this directory:
[session-map-rebind](session-map-rebind.md),
[warp-teardown-order](warp-teardown-order.md),
[moving-the-session](moving-the-session.md),
[enemy-table-races](enemy-table-races.md),
[net-map-id-inventory](net-map-id-inventory.md),
[remote-copy-and-the-map](remote-copy-and-the-map.md),
[respawn-and-the-map](respawn-and-the-map.md),
[map-model-fault](map-model-fault.md),
[target-manager-cap](target-manager-cap.md), and the earlier
[object-table-lifecycle](object-table-lifecycle.md).

## The five things that changed

1. **The vanilla order is notify → wait → release.** The game tells the
   guests at request time, *before it touches anything local*, and then
   blocks its own teardown until the membership is gone. The mod has been
   doing the reverse. That is the likeliest root cause of every guest death
   on a release.
2. **Raising the 2048 cap would not save the work.** It is 31 instructions
   and realistic, but the chameleon vector caps loaded maps at **three**
   regardless. The release is still needed.
3. **The session's map has a name and three writers**:
   `NetSummonJoinMultiplayCtrl + 0x19c`, written at construction and by the
   summon packet, and never again. `sync+0x18` is only a cache of it. **The
   staleness is guest-only** — the host already rebinds to its local map.
4. **Most of what was feared is safe.** Respawn and bonfires need no map
   loaded, and the game already warps to a bonfire in an unloaded map every
   time you die. Every owner-mediated lookup fails safe. Seven of the eight
   consumers of the enemy table re-look-up each frame.
5. **The dangerous set is small and named**: the enemy sync's record
   pointers, the lighting cube cached on **every** character, the streamer's
   raw part, the sign areas, and three unguarded coordinate handlers nobody
   had noticed.

## The two routes are one route

"Release the map" and "move the session" are not alternatives. The guest's
bind source has to be repointed either way, because otherwise it rebinds
straight back to the map you are trying to free. The difference is only how
much of the session's own notion of *where it is* you carry along.

**Take the smaller one**: repoint the field, do not re-run the join. A second
summon packet on a live session does not re-target it — it **kills the
session** ([moving-the-session](moving-the-session.md) gap 1).

One encouraging discovery: of the four patches a full imitation would need,
**one is already in this repo**. `DS2_SeamlessSessionHook` clears bit `0x10`
of `ctrl+0x1b8` every host tick, and that bit is the vanilla "the host
warped" flag whose 300 s timer ends the session. The mod was already telling
the game the host never warped, without knowing that is what it was doing.

## The order

On the machine that releases, **with the map still loaded**:

1. **Tell the other side first.** Vanilla's order is not decoration. There is
   no vanilla message for "I am moving but staying" — it has to go over the
   mod's own co-op channel — and the other machine must have acted on it
   before anything local is freed.
2. **Unbind the enemy sync**, `FUN_140517080(sync)`, on **both** machines.
   On the host it must run while the table is still allocated, because its
   clear writes into the blocks. The verified hook point is the entry of
   `FUN_140416ac0`, mirroring its `slot > 0x29` early-out and guarding on
   `sync+0x08 != 0`, not on the count. **No lock inversion is possible**; the
   game thread holds nothing there.
3. **Purge the sign areas** of that map by hand — the game's own purge is a
   bare `ret`. Already done.
4. **Clear the lighting cache of every character** whose entry belongs to
   that map's bank: `chr+0x460`, `+0x468`, `+0x470`, `*(chr+0xb8)+0x250`, and
   on the **model** `*(chr+0xf0)`. **No native call does this**, and it is
   what the remote copy would otherwise die of.
5. **Get the local player's streamer part off the map** before
   `owner+0x148` is freed. The parking already does this.
6. **Let the owner go through the stock step machine.** Never by hand: the
   only thing that cancels a queued enemy-table create is
   `FUN_140416ac0`, reached only from the teardown's case `0x0d`.
7. **Repoint `joinCtrl+0x19c`** on the guest — **four bytes**, never eight;
   `+0x1a0` next door is the way home.
8. **Wait for the destination's table**, `table != 0 && *(int*)(table+0x24)
   == mapId`. Not `blocks != 0`: a map with no generators has a valid table
   with none. Poll with a hard timeout and treat the timeout as failure —
   there is no retry anywhere in that path.
9. **Re-arm**: `FUN_140517040`, then on the guest `FUN_140516370`, in that
   order, because the arm clears the gate. Then verify state, map and the
   per-record generator ids on both machines before going on.

## The risks, ranked

1. **The enemy-table create can be lost silently, four ways.** The queue is
   four slots and drops when full with no retry; failure is terminal; a
   hand-rolled release leaves a create queued; and the drain is gated on
   `mgr+0x3c6`, which means *"nothing is still dying from the last map
   change"* — **which every travel raises**. If the mod keeps the source
   map's enemies alive, the destination may never get a table at all.
2. **Arming the sync before the table exists kills it for the session.** The
   caller sets the state whether the bind bound anything or not, and nothing
   returns it to 0 but an unbind.
3. **Three unguarded coordinate handlers.** If a released map's sign,
   bloodstain or ghost records are still cached, `FUN_1403bce00` returns 0
   and the code faults near null. Not covered by `DS2_NetSyncGuardHook`.
4. **The `MapModelComponent` fault is still unexplained.** The best
   hypothesis is a **refcounted entity surviving its map's teardown**,
   staying in the global component bucket and being pre-drawn every frame
   while its arena is reused. It is not closed, and it hit **both** machines
   17 ms apart, which nothing read accounts for.
5. **Placement that bypasses the loader.** Our travel teleports and focuses
   the streamer instead of letting the loader run; resolving a position
   before the map is back at state 5 gives `(0,0,0)` **silently**.
6. **Leaving the copy in place is untested.** The guards suggest it is safe
   once the lighting cache is cleared, but nobody has seen it.

## What to measure before writing any of it

All of the above is static. These are cheap and turn reading into evidence:

- **The live shape of the field**, with the probes in
  [session-map-rebind](session-map-rebind.md): on a host the join controller
  reads 0; on a guest it is non-null and its `+0x19c` stays at the summon map
  while `NetPlayerWatcher+0xc` follows him.
- **`mgr+0x3c6` across one of our travels.** If it does not return to 0, risk
  1 is not hypothetical and the whole plan needs a different trigger.
- **`sync+0x08` and `sync+0x18`** on both machines through a leg, to see when
  the record array is actually built and for which map.
- **On the next `MapModelComponent` fault**, read `*(this+0xd0)` and
  `*(this+0xd4)` too. If only one word is ever wrong, the network-record
  writer is out.

## What none of the ten could establish

- What `0x000b0010` is.
- Whether a map's arena is reclaimed while a refcounted entity survives.
- Whether a character's Havok body references the map's collision world.
- What resets `sync+0x08`, i.e. when the record array is rebuilt.
- Whether the enemy-table sweep runs during a loading screen — which decides
  whether peak target occupancy can exceed the budget's steady-state sum.
- Which multiplay type co-op is, in the gate table at `0x14157c3b0`.
