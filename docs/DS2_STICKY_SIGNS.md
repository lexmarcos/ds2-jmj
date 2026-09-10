# Sticky signs: serving signs to areas that refuse them

Majula refuses the Red Sign Soapstone. [DS2_AREA_RESTRICTION.md](DS2_AREA_RESTRICTION.md)
records the hunt for why, and where it ended: the client never registers
the item as being used, three routes to a non-zero sign type all fail,
and forcing the animation with two byte patches makes the soapstone fire
but the sign does not stick. The placement half is locked inside the
game.

This document is about the other half.

## The idea

Placing a sign and seeing a sign are two different things, and only one
of them is the client's decision.

Placing is client-side: the game decides whether the item may be used,
builds the sign, and sends `RequestCreateSign`. That is the blocked path.

Seeing is server-side: the client polls with `RequestGetSignList`, naming
the area it stands in and the cells it is searching, and the server
answers with whatever it likes. Nothing about that answer is the
client's to refuse. The server is ours.

So a player standing in Majula can be shown a sign that lives somewhere
else entirely, as long as the answer is dressed up as local.

## What the server does

The cache is keyed on `{cell_id, online_area_id}`. A sign placed in
Heide is filed under a Heide cell, and a client searching Majula's cells
will never match it. Two ways to fix that, and the choice matters:

- **Re-tag the sign** onto the searcher's area. Rejected. The sign's
  owner still believes it is where they left it, and the server's own
  `ActiveSummonSigns` list on the owner's client would drift out of
  agreement with the cache. Removal and disconnect both walk that list.
- **Leave the sign where it is, and lie only in the response.** Taken.
  The sign is reported with the *searcher's* cell and area, so the
  client accepts it as local, but nothing about the stored sign changes.

The cost of the second is that the summoner then sends
`RequestSummonSign` naming a location the sign was never filed under, so
the keyed lookup misses. Sign ids are unique across the whole cache, so
both the summon and the reject handler fall back to a lookup by sign id
alone when the keyed one fails.

## The gate

A client reports `online_activity_area_id` alongside its location.
DS3OS documents the field as being set to zero when no online activity
can happen where the player stands. That is the client volunteering
exactly the fact this feature needs, so it is the gate: the sticky pass
runs only for players who report zero.

The gate is an assumption about Majula, not yet a measurement. A gate
that quietly never fires is indistinguishable from a client that ignores
the response, which is why the diagnostic sits *outside* it and reports
the value either way.

## Turning it on

`DS2_StickySigns` in the server config, off by default. The server must
be restarted to pick up a config change, and a restart invalidates every
connected client's auth token, so the game has to be relaunched too.

## What this does and does not answer

It answers whether a player standing in Majula can see and touch a sign
at all. It deliberately does not move the sign's owner: the ghost stays
in Heide with a sign it legitimately placed, so its client state stays
consistent and the summon push should be accepted on its own terms.

It does not answer where the sign appears. A sign's position lives
inside `player_struct`, an opaque blob the server only validates and
never parses, so the sign carries its owner's Heide coordinates into
Majula's geometry. It may well land underground or out of reach. That is
a separate problem, and a much smaller one: it is a matter of finding
the position inside the blob and rewriting it, which can be done by
diffing two structs captured from two known positions.

Nor does it make the soapstone usable in Majula. The host still cannot
place a sign of their own. What it buys is the direction that matters
for a duel: one player waiting, one player arriving.

## Result: the Majula client never asks

Measured on 2026-09-10 with two clients on one machine, one in Heide's
Tower of Flame and one in Majula, over six minutes.

| Client | Area | Sign polls in six minutes |
| --- | --- | --- |
| Heide | `0x009d5170` | 5, one per minute, like clockwork |
| Majula | `0x009932c0` | 0 |

The Heide client's poll reports an online activity area of 103110, so
the gate correctly declined it. The Majula client sent no
`RequestGetSignList` at all, so there was nothing for the gate to decline
or accept.

The gate itself was wrong, which a later measurement showed: the Majula
client reports an online activity area of 103110 too, not zero. That is
Heide's multiplay zone, and it appears there because
`DS2ForceMultiPlayZone` is already on in the injector config, writing
103110 into the map block record. So the Majula client already believes
it stands in a valid multiplay zone, and it still does not poll. The
zone is not what decides this, which is the same conclusion the area
investigation reached from the other direction.

**This kills the approach in its server-only form.** The server cannot
put a sign in front of a player who never asks for one, and there is no
push message that adds a sign: the DS2 message set has
`PushRequestRemoveSign` and `PushRequestSummonSign` but no counterpart
that creates one. Signs exist for a client only as entries in a
`RequestGetSignListResponse`.

The code is kept, off by default. It is correct, it costs nothing when
disabled, and it becomes useful the moment the client can be made to
poll in Majula. What it needs is a client-side patch that makes the
polling happen, which is the same class of problem as the item-use gate
and has not been located.

## Where this leaves the goal

The measurement points somewhere better. Look at what the server can
send unprompted:

    PushRequestBreakInTarget      invasion
    PushRequestVisit              blue sentinel
    PushRequestJoinQuickMatch     arena

In an invasion the target is entirely passive. It does not use an item,
does not poll, and does not decide anything. The invader uses an orb
somewhere the game permits it, and the server tells the target they are
being invaded. Every gate that blocked the soapstone lives in the
item-use path, and the target never runs it.

That is now the live approach. See `DS2_InvadeAnywhere` in the server
config, and the section below.

## Test procedure (sign version, kept for reference)

Two accounts, `AllowDuplicateSteamIds` on so one copy of the game can
hold both sessions.

1. Ghost, in Heide, human, places a Red Sign Soapstone. Confirm the sign
   exists the ordinary way before trusting anything else. **A hollow
   character is refused the soapstone and the refusal looks exactly like
   Majula's** — this cost hours once already.
2. Host, in Majula. Read the server log. The `Sign poll` line reports
   the area, the activity area the gate reads, the cells being searched
   and whether the sticky pass ran.
   - No `Sign poll` line at all: the Majula client does not poll for
     signs, and this whole approach is dead. Fall back to the invasion
     route or ship the native arena.
   - `sticky skipped`: the gate assumption is wrong. The line reports
     the real activity area value; change the gate to match it.
   - `sticky eligible` and an `offered` line: the server did its part.
3. Look at the ground in Majula. A red sign visible is the result this
   was built to get.
4. Touch it. The summon falls back to the sign-id lookup, the ghost gets
   `PushRequestSummonSign`, and the Steam peer-to-peer session forms.
   Whether the ghost's client accepts that push is the real risk, and
   step 4 is the only way to find out.

Verify in the first successful session that the phantom arrives hostile.
The sign type is copied straight through, so a red sign should stay a
red sign, but co-op and duel mechanics diverge after the summon and this
is the point where that would show.

## Invading a dead area

`DS2_InvadeAnywhere` in the server config, off by default. Three changes,
all server-side:

- An online activity area of zero stops disqualifying a player from
  being invaded. Sitting at a bonfire and burning an effigy still
  protect them; only the dead-area rule is lifted.
- The invader is no longer restricted to targets standing in the
  invader's own area.
- The invasion push is rewritten to name the *target's* area and cell
  rather than the invader's. Normally the two are the same place, so the
  request's own location serves for both. Once an invasion can cross
  areas it does not, and a client told it is being invaded somewhere it
  is not standing has no reason to cooperate.

The last one needs the target's cell, so `DS2_PlayerState` now keeps the
cell id the client reports alongside the area it already kept.

### What is still unknown

Whether the target's client accepts the push while standing in Majula.
That is the same question the sign approach ended on, asked of a
different code path, and the reason to expect a different answer is that
this path does not touch the item-use state machine at all.

### Test procedure

1. Both clients connected, both human, neither sitting at a bonfire.
   The server log now prints a `Location` line per player every fifteen
   seconds, giving the area, activity area, cell and position it reports.
   Read it first and confirm Majula reports what you think it does.
2. The invader needs an invasion item in a quick slot. A Cracked Red Eye
   Orb is the ordinary one.
3. Invader uses it, anywhere the game permits. The log prints a
   `Break-in target list` line with the candidate count, then one line
   per connected client with its area, activity area and whether the
   server considers it invadable.
   - No line at all: the invader's own client refused the item, which is
     a problem in the invader's area, not the target's.
   - Lines present, target `invadable no`: a server-side filter still
     rejects it, and the line says which value caused it.
   - Target listed as a candidate: the server did its part.
4. On selecting the target, an `Invading ... across areas` line reports
   both locations. Then watch the Majula client: does the invasion
   arrive, and does the Steam peer-to-peer session form.

### Firing an invasion without an orb

The test above needs the invader to hold an invasion item, which is a
separate errand from the question being asked. So the server will fire
the push directly when `DS2_InvadeAnywhere` is on:

    echo "<invader player id> <target player id>" > Saved/default/debug_invade.req

The file is read once a second and deleted. Player ids come from the log
line each client prints on login. An optional third number sets the
break-in type; it defaults to the red eye orb.

This skips the item, the target list and every matching rule, so it
proves nothing about whether an ordinary invasion would be allowed. It
asks one question only: with the push delivered, does the client
standing in Majula act on it.

## The Majula client accepts an invasion

Measured on 2026-09-10, with a real Cracked Red Eye Orb rather than a
fabricated push. Invader in Heide's Tower of Flame, target in Majula.

The first attempt sent the packed `player_location` cell, `0xffc003ff`,
as the push's cell id. The target answered `RequestRejectBreakInTarget`.

The second sent `103110`, the target's online activity area, the shape
the client itself uses in `RequestBreakInTarget`. The target did not
reject. It sent `RequestSendMessageToPlayers`, which is how a client
reaches its peer, and the invader's screen went from nothing to
**"Disconnected from multiplayer session."**

That is the first time anything in this investigation got a client in
Majula to take part. The sequence, end to end:

    Samuel   RequestGetBreakInTargetList     1 candidate of 2 clients
    server   candidate '3:Chico'             area 0x009932c0, invadable yes
    Samuel   RequestBreakInTarget            target 3, area 10310000, cell 103110
    server   push rewritten                  area 10040000, cell 103110
    Chico    RequestSendMessageToPlayers     the target reaching for its peer
    Samuel   "Disconnected from multiplayer session."

Two things had to be true for the candidate line to appear at all, and
both are `DS2_InvadeAnywhere`: the same-area filter had to go, and the
push had to name the target's own ground.

A session started and then dropped. What has not been established is
whether a session between these two instances has *ever* held, so the
disconnect is not yet attributable to Majula. The control is to put both
players in the same area and invade again.

### What the rejection taught, and what it cost to learn

A fabricated push, sent from a request file with no orb involved,
produced nothing against Majula. It also produced nothing against a
player in Heide, where invasions plainly work. The control ran before
the conclusion, which is the only reason the first result was not read
as "Majula refuses invasions".

An invasion needs the invader's own client to be in an invading state.
Nothing the server sends can fake that. The debug trigger is kept for
poking at a client that is already engaged, but it cannot start
anything, and it is not evidence on its own.

## The control: a same-area invasion works completely

Both players moved to Heide's Tower of Flame, same orb, same server.

    Samuel  RequestBreakInTarget   target 3, area 10310000, cell 103110
    server  push                   area 10310000, cell 103110
    result  Samuel is a red phantom in Chico's world

The orb count went from 99 to 98, so the item was consumed, and the
invader is standing in a part of Heide the host had walked to, not where
the invader used the orb. The peer-to-peer session between these two
instances holds perfectly.

That settles the earlier ambiguity. The disconnect on the cross-area
invasion is not the setup, not Steam, not the two local clients. It is
the invasion crossing areas.

One more thing fell out of the move, unasked. The moment Chico arrived
in Heide, the log printed:

    3:Chico   First DS2_Frpg2RequestMessage.RequestGetSignList

Same client, same session, same character. It asks for signs in Heide
and never asks in Majula. The census result was not an artefact of two
different clients or two different logins.

## Where the wall actually is

Three facts, all measured, that together draw the boundary:

1. A player standing in Majula **can** be invaded. The client accepts
   the push, answers it, and joins a session.
2. A player standing in Majula **cannot** start anything. The Cracked
   Red Eye Orb is refused there exactly as the Red Sign Soapstone is,
   with no prompt at all, away from any bonfire.
3. An invasion whose invader and target are in different areas starts
   and then drops on the invader's side.

So the target half of multiplayer is already open in Majula. The
initiating half is not, and it is the same gate the whole area
investigation has been circling: item use.

The invader is in a correct invading state when the session drops, so
the invader's client is not refusing to invade. It is refusing to arrive
somewhere other than where it asked to invade. The server cannot tell it
otherwise: `RequestBreakInTargetResponse` is empty and, in the protocol
notes, never received at all. The destination reaches the invader over
the peer-to-peer session, not from us.

That leaves two ways forward, both client-side:

- Make the invader's client accept a session in an area other than its
  own. It already has the session; it just will not stay.
- Make the invader's client report Majula as its area while it stands
  somewhere the orb works, so the invasion it asks for and the session
  it gets agree. The area id's location in memory is already known from
  the area probe work.

The second is closer to what already exists and does not need the
disconnect's cause pinned down first.

## The mask the game tests, and the value the hook was writing

Two places in the game read a byte at `+0x18` of a param row and refuse
the item when bit 3 is clear:

    1402a5fbb  testb $0x8,0x18(%rax) ; je 1402a6000   summon signs
    140272222  testb $0x8,0x18(%rax) ; je 1402721a0   invasion orbs

Both branch targets are `xor al,al; ret`. The row comes from a lookup
keyed on the online area id: `1402503a0` unpacks a byte-packed id into
the decimal form (`0x0a1f0000` becomes 10310000) and indexes a table at
`[obj+0xc8]`. Row offset `+0x18` is exactly where the earlier decode put
`NETWORK_AREA_PARAM`'s bitmask, and bit 3 is `enableMultiPlay` in DS3's
paramdef for the same layout.

That one bit gating both signs and orbs matches the measurements
perfectly: both items are refused in Majula, both work in Heide.

**`DS2_UnlockAreaMultiPlayHook` was writing 7, which leaves bit 3
clear.** Every run of that hook has written a value that cannot satisfy
the test it exists to satisfy. The constant said 7 because Heide reads 7
and accepts signs, and because forcing 63 was believed to break Heide.
That second belief is void: it was measured on a hollow character, and a
hollow character is refused the soapstone everywhere. It was never
re-run on a human. The constant is now 63.

### But the predicate did not fire

Breakpoints on `1402a5fbb`, `1402a5fc1`, `1402a6000` and `1402a6012`
were armed in a live instance and none was reached, either idle or on an
item press, while `140250e50` armed alongside fired immediately. So the
tracer worked and that code did not run.

What did run, reached from `+0x50ca69` inside the quick-slot loop:

    1401a8b90   from 50ca69
    1401a8f70   from 50ca69
    1401a9230   from 1a8cdf

and `14024f350`, the per-item dispatch, was never reached.

One caveat keeps this from being conclusive, and it is the same caveat
that has bitten before: **no sign was ever placed during these presses**,
in Heide, where placing one works. So it is not established that the
press exercised a full item use at all. Until a press demonstrably
places a sign with the breakpoints armed, "the predicate is not on the
live path" is a hypothesis, not a result.

The next measurement is therefore small and specific: in Heide, human,
not invaded, arm `1401a8f70` and `14024f350`, press the soapstone, and
confirm a `RequestCreateSign` reaches the server in the same moment.
Only then does the absence of `14024f350` mean anything.
