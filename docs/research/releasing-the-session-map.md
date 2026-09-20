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
2. **Unbind the enemy sync**, `FUN_140517080(sync)`, on **both** machines. —
   **written and verified 20/09**, `DS2_EnemySyncHook`. See below.
   On the host it must run while the table is still allocated, because its
   clear writes into the blocks. The verified hook point is the entry of
   `FUN_140416ac0`, mirroring its `slot > 0x29` early-out and guarding on
   `sync+0x08 != 0`, not on the count. **No lock inversion is possible**; the
   game thread holds nothing there.
3. **Purge the sign areas** of that map by hand — the game's own purge is a
   bare `ret`. Already done.
4. — **already built, and its trigger fixed 20/09.** See below. Clear the lighting cache of every character whose entry belongs to
   that map's bank: `chr+0x460`, `+0x468`, `+0x470`, `*(chr+0xb8)+0x250`, and
   on the **model** `*(chr+0xf0)`. **No native call does this**, and it is
   what the remote copy would otherwise die of.
5. **Get the local player's streamer part off the map** before
   `owner+0x148` is freed. The parking already does this.
6. **Let the owner go through the stock step machine.** Never by hand: the
   only thing that cancels a queued enemy-table create is
   `FUN_140416ac0`, reached only from the teardown's case `0x0d`.
7. — **written and verified 20/09**, `DS2_BonfireInSession_RepointSessionMap`.
   See below. Repoint `joinCtrl+0x19c` on the guest — **four bytes**, never eight;
   `+0x1a0` next door is the way home.
8. — **written and verified 20/09**, `DS2_EnemySync::TableReady`. See below.
   Wait for the destination's table, `table != 0 && *(int*)(table+0x24)
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
  [session-map-rebind](session-map-rebind.md). — **measured 20/09, it reads
  exactly as the static reading said.** See below.
- **`mgr+0x3c6` across one of our travels.** — **measured 20/09, it never
  leaves 0.** See below.
- **`sync+0x08` and `sync+0x18`** on both machines through a leg. — **measured
  20/09: the array is built for the session's map and travel never rebinds
  it.** See below.
- **On the next `MapModelComponent` fault**, read `*(this+0xd0)` and
  `*(this+0xd4)` too. If only one word is ever wrong, the network-record
  writer is out.

Six more readings on 20/09 answered most of what follows, and risk 4 with
them: [risk-4-six-readings](risk-4-six-readings.md). In short — the arena
mechanism is dead, the value is two different mechanisms of which only one is
ours, and the strongest surviving candidate is a lifetime gap that step 2 of
the order above already half closes: the enemy records are nulled only by a
session-state transition, never by the map path, so a map torn down with the
session up leaves up to 255 records pointing into a freed array.

## What none of the ten could establish

- What `0x000b0010` is. — **answered 20/09**: a 21-bit HP field from the enemy
  sync's packet store, landing on the high half of a pointer through a stale
  record. See [risk-4-six-readings](risk-4-six-readings.md).
- Whether a map's arena is reclaimed while a refcounted entity survives. —
  **answered 20/09: it is not, because there is no per-map arena.**
- Whether a character's Havok body references the map's collision world.
- What resets `sync+0x08`, i.e. when the record array is rebuilt.
- Whether the enemy-table sweep runs during a loading screen — which decides
  whether peak target occupancy can exceed the budget's steady-state sum.
- Which multiplay type co-op is, in the gate table at `0x14157c3b0`.

## What was measured on 20/09

Two legs with a verified session (`p2pSessionVerified: true`), Samuel hosting
in Majula and Chico summoned by the party hook: Majula → Shulva
(`0a040000` → `32240000`, bonfire `8f2f`) and back to Majula (`122a`). Both
players arrived on both legs, the session survived both, and the anchors were
re-resolved afterwards and were the **same objects**, so nothing below is a
stale pointer reading zero.

**`mgr+0x3c6` never leaves 0.** `mgr` is `*(*0x1416148f0 + 0x40)`; the count
of generator list 7 read 0 on host and guest at rest, through the whole
outbound leg (85 paired samples at 0.5 Hz) and through the whole return leg
(120 samples at 1 Hz on the host, which also never saw a different value).
So the gate that defers the enemy table — "nothing is still dying from the
last map" — is open the entire time, in both directions. **Risk 1 is not
hypothetical in the bad direction**: what the plan feared was the count
staying non-zero and starving the destination's table forever, and that does
not happen on our travel.

Two caveats, both honest. 1 Hz cannot rule out a transient shorter than a
second inside the load, and the load itself is about two seconds; what it does
rule out is a count that gets stuck. And **neither leg tore down the session's
map**: Majula stayed loaded in both directions, and only Shulva was built and
released. So this clears the gate for a *destination's* teardown, which is
what today's travel does — it does not clear it for the teardown the plan
actually proposes, which is releasing Majula while everybody is away. That one
is still unmeasured, because nothing does it yet.

**The network enemy record array is built once, for the session's map, and
travel never rebinds it.** The `NetEnemyManager` at `*(0x141616cf8 + 0x28)`
was confirmed by its vftable, `0x1410fb580`, on both instances. Through both
legs:

| field | host | guest |
| --- | --- | --- |
| `+0x08` state | `1`, never changed | `2`, never changed |
| `+0x0c` count | `56`, never changed | `56`, never changed |
| `+0x18` bound map | `0a040000`, never changed | `0a040000`, never changed |

`0a040000` is Majula, the map the session began in — not the map either
player was standing in **[read]**. That the 56 records are Majula's enemies is
**[inferred]**: what was read is a count and a map id, not the records
themselves. What is certain is that the count did not move and the binding did
not follow, while both characters spent three minutes in Shulva.

That answers "what resets `sync+0x08`", from the list of what none of the ten
could establish, with a negative: **a travel leg does not**. It also says the
destination map is, for the network enemy layer, not a place either player is
in: there is no record array for it and nothing builds one. Whatever releasing
the session's map does to this object, it will not be *losing* a binding the
destination was using, because the destination never had one.

The two instances reported byte-identical addresses for this object
(`0x7ffffe47c360`, with `+0x10` and `+0x38` matching too). That is Wine
giving the same game the same heap layout from the same allocation sequence,
not a shared mapping; the vftable check is what makes each reading its own
process's.

**The join controller's field is where it was read to be, and it is
guest-only.** With the session verified, the host's `joinCtrl` read `0` and
the guest's read `0x7ffffe5bf5e0`, state 7, with `+0x19c = 0a040000` — Majula,
the map the session began in. Chico then travelled to Shulva and back: his
local map went to `32240000` and returned, the controller pointer never
changed, the state never left 7, and **`+0x19c` never moved off `0a040000`**
at any point. So the staleness the plan describes is real, live, and does no
harm on its own — today's travel walks the guest into another map with his
join controller still naming Majula, and nothing dereferences it. It becomes a
problem only when somebody tries to free Majula, which is what step 7 exists
for.

**The two fields are now told apart by reading, not by comment.** In the
samples above `+0x19c` and `+0x1a0` both read `0a040000`, because Chico was
summoned in Majula and Majula is also where he came from, so nothing
distinguished "the session's map" from "the way home". The staging that
separates them is one session end away: travel the pair to Shulva, end the
session there — the host stays, the guest dies home to his own bonfire record
— and let the party hook summon again across the two maps. Done 20/09, with
Samuel in Shulva and Chico in Majula:

| field | value | what it is |
| --- | --- | --- |
| `joinCtrl+0x19c` | `32240000` | Shulva, the **host's** map, where the summon happened |
| `joinCtrl+0x1a0` | `0a040000` | Majula, where the **guest** was standing |

So `+0x19c` is the session's map and `+0x1a0` is the way home, exactly as the
static reading named them **[read]**. Step 7's "**four bytes, never eight**"
is now a rule this bench can check was obeyed: after the write, `+0x19c` must
name the destination and `+0x1a0` must still name wherever the guest came
from. Eight bytes would send him home to the map being freed.

**And the record array follows the map the session began in, by count as well
as by id.** The same session, begun in Shulva instead of Majula, reads
`+0x0c = 207` and `+0x18 = 32240000` on both machines, where the Majula
session read `56` and `0a040000`. The count tracks the map, which settles the
`[inferred]` above: the records really are that map's enemies **[read]**.

## Step 2, in place

`DS2_EnemySyncHook` sits at the entry of `FUN_140416ac0`, the per-map
character release the teardown reaches at its case `0x0d`, and unbinds the
enemy sync when the map coming down is the one the records belong to. All four
prologues are verified before anything is hooked, and the guest's gate turned
out to be two instructions, `+0x198 = 1; ret`.

**The second argument is not a map id.** The first build wrote down whatever
it saw rather than assuming, and it saw `00007fffe8400310` — a pointer. So
nothing was unbound on a guess. `FUN_1403bb3d0`, the slot lookup the release
opens with, is three instructions:

```
mov 0x8(%rcx),%rax    ; -> the backread owner
mov 0xc(%rax),%eax    ; -> its area slot
ret
```

and `+0x08` / `+0x0c` of that owner are exactly `DS2_BackreadHook`'s own
`kOwnerMap` and `kOwnerIndexField`. The map id is `*(owner + 0x08)`, and the
release's `cmp $0x29; ja` is mirrored on the same index.

Measured across four legs with a verified session, both machines **[read]**:

```
19:01:20  soltando personagens do mapa 0a040000 (slot  1), ligado 0a130000, estado 1, 76 registros
19:01:51  soltando personagens do mapa 0a170000 (slot  9), ligado 0a130000, estado 1, 76 registros
19:02:59  soltando personagens do mapa 0a1e0000 (slot 32), ligado 0a130000, estado 1, 76 registros
19:03:31  soltando personagens do mapa 0a1f0000 (slot 12), ligado 0a130000, estado 1, 76 registros
```

Real map ids, sane slots, the bound map named correctly on both roles (state 1
host, 2 guest). **The unbind never fired, and that is the point**: every map
released was some other map, because the bound map is the session's map and
two separate refusals keep it. Step 2 is a no-op until step 6 lifts them —
which is exactly what it is for.

The same run showed why 6b is worth the work, without being asked to: the
session had formed in the DLC ice map, so the session map cost **1400** of an
1898 budget and every heavy destination was refused at the vote.

## Step 4 was already written, and was watching the wrong thing

`SweepLighting` in `DS2_DeathInterceptHook` already does more than step 4 asks
for. It compares **five** cached-entry fields — `chr+0x460`, and the model's
`+0x2a0`, `+0x2a8`, `+0x228`, `+0x230` — against the bank of the map that is
going, and `ResetLighting` clears the character's three fields, the physics
region byte and six fields on the model. It already runs from `UpdateHook`,
which is **every player character, local or another player's copy**. So the
remote copy was covered.

What it was watching was `DS2_Backread::Unloading()`, and that names **only
the budget's chosen victim**. Every other release — a keep expiring, an
explicit request, the session ending, and in time the session's own map — goes
through `BeginLetGo`, and got **no sweep at all**. 6b would have walked
straight into that, because releasing the session's map is exactly a
`BeginLetGo` release, and the stale entry is what the other player's copy dies
of.

The sweep now takes `DS2_Backread::Releasing()` as a second source and names
in its line which of the two found the map. Measured on a five-leg campaign
with a verified session, all five arriving on both machines **[read]**:

```
19:13:35.624  lighting: character 00007FFFE8829180 still held map [9]'s lighting (soltura); cleared
19:13:35.624  lighting: character 00007FFFE883E430 still held map [9]'s lighting (soltura); cleared
...
   orcamento=0  soltura=40
```

**Forty clears, every one from the new source and none from the old** — 40 is
the log's own cap, so there were at least that many. Before this change not
one of them happened: those characters kept a released map's lighting entry
and nothing ever took it away. This is a bug fixed today, not only a
precondition.

The guest logged none this run, which is the honest limit of the measurement:
the fix is proven on the host, and the guest did not exercise it.

## Step 8, and the index that could not be read

The predicate is `table != 0 && *(int32_t*)(table + 0x24) == mapId`, with
`table = *(mgr + 0x20 + index*8)` and `mgr = *(*0x1416148f0 + 0x40)`.

**The index was the obstacle.** The game's own lookup, `FUN_140419a70`, gets
it from `FUN_1403bcec0(world, mapId)` — and that is an Arxan trampoline
(`jmp 0x141b39d4f`) which cannot be read statically. Rather than chase it, the
claim that the **backread owner's own `+0x0c`** is the same index space was
measured. On 20/09, every loaded map on both instances **[read]**:

```
inst 1: mgr 7ffff06ab420  lista7=0  fila=[255,255,255,255]
   [ 1] 0a040000 estado 5 -> tabela 7fffe8a3cba0  +0x24=0a040000  PRONTA
   [ 7] 0a130000 estado 5 -> tabela 7fffe827e420  +0x24=0a130000  PRONTA
inst 2: mgr 7ffff03a5b80  lista7=0  fila=[255,255,255,255]
   [ 1] 0a040000 estado 5 -> tabela 7fffe8a31c00  +0x24=0a040000  PRONTA
   [ 7] 0a130000 estado 5 -> tabela 7fffe819fe10  +0x24=0a130000  PRONTA
```

Four for four, both roles. The same run gives two facts worth keeping: the
create queue's empty slot is **`0xff`**, not 0, and the gate at `mgr+0x3c6`
was 0 throughout.

**The table is built before state 5.** Watching a destination across a leg,
the first sample after the vote already had `idx=12`, owner **state 4**, and
`+0x24` equal to the destination. The research warned off keying on state 5
because the enqueue happens earlier; the measurement says state 5 is *late*,
not early, which is the same warning from the other side.

The C++ then verified itself against live maps rather than against its author.
`TableReady` is reported from the release hook, where the map certainly had a
built table a moment before, with the gate and the queue beside it — four legs
with a verified session, both machines **[read]**:

```
19:40:43  soltando personagens do mapa 0a040000 (slot 1) ... tabela pronta, lista7 0, fila 255/255/255/255
19:41:14  soltando personagens do mapa 0a170000 (slot 9) ... tabela pronta, lista7 0, fila 255/255/255/255
19:42:54  soltando personagens do mapa 0a1f0000 (slot 12) ... tabela pronta, lista7 0, fila 255/255/255/255
```

## Step 7, and what "never eight bytes" is protecting

Reading the guest's controller live gave the reason the rule exists. On a
guest in state 7, summoned into `0a130000` from Majula **[read]**:

```
vftable 0x1410d7bd8   estado(+0xf8) 7
  +0x194 = 00000000        the duel field, unused in co-op
  +0x198 = 00000001
  +0x19c = 0a130000        the session's map
  +0x1a0 = 0a040000        the map to return to
  +0x1a4 = 41286bac   10.526
  +0x1a8 = 40bd8f2b    5.924
  +0x1ac = c1820962  -16.255
```

Those three floats are Majula's bonfire spawn to three decimals — where the
guest was standing when he was summoned. **The way home is a sixteen-byte
block, not one field.** Eight bytes at `+0x19c` would send him home to the map
being freed; sixteen would drop him at the session map's coordinates in
whatever map he lands in.

So the repoint writes four, refuses on a `+0x19c` that is not the value it was
told to expect — the rule a `.text` patch follows — and reads the sixteen
bytes before and after, putting them back and undoing the repoint if they
moved.

Driven by hand on a live session with `repontar <map>` in `DS2_Bonfire.req`,
and read back independently by the harness rather than by the hook that wrote
it **[read]**:

```
antes   +19c=0a130000  +1a0=0a040000  volta=(10.526, 5.924, -16.255)
depois  +19c=0a1f0000  +1a0=0a040000  volta=(10.526, 5.924, -16.255)
        convidado: mapa da sessao repontado 0a130000 -> 0a1f0000 (lido de
        volta 0a1f0000); volta para 0a040000 intacta
```

The field moved, the block did not, and `p2pSessionVerified` stayed true
through both the repoint and the restore. The request exists because step 6 is
what will call this, and an operation nobody calls is an operation nobody has
checked.