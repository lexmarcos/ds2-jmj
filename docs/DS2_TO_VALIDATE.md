# Still to validate

Multiplayer works in the closed areas, but "works" currently means: two
players, on one machine, with two item types, in three of the game's
thirty-one areas, up to the moment the phantom appears. Everything
below is untested rather than known good.

Ordered by what would hurt most if it turned out to be wrong.

## Blocking a release

### The duel is never played out

Every successful test ends at the phantom arriving. Nothing has checked
that a duel in Majula behaves like a duel: damage exchanged, one player
dying, the session ending cleanly, souls awarded, the loser going home,
both clients returning to a sane state. A session that forms and then
misbehaves is worse than one that never forms, because it costs the
player their run.

Test it end to end, and then test the ugly endings too: the host quits
mid-fight, the phantom quits, one client is killed.

### Only three areas have been tried

Tested: Things Betwixt (`0x0098e4a0`, mask 0), Majula (`0x009932c0`,
mask 4), Heide (`0x009d5170`, mask 7). `NETWORK_AREA_PARAM` has 31 rows
and the unlock hook now raises **every** one below 63.

Nothing has checked what raising a mask does in areas that were already
open, or in the arena, or in boss rooms. The permission byte has six
bits and only one is understood; the others are being set wholesale.

Suspects worth a look before shipping: boss arenas, the Undead Purgatory
and the Belfries, which have their own invasion rules, and anywhere with
a scripted NPC invader.

### The forced zone is Heide's

`DS2ForcedZoneId` is `103110`, borrowed from Heide. Every area now
claims to be inside Heide's multiplay zone.

That zone record carries whatever settings Heide has, and a zone is
plausibly where phantom limits, session length and matchmaking ranges
live. If those turn out to be per-zone, the whole game is silently
running on Heide's rules. Nobody has read that record.

### One machine, one network path

Both clients run on the same box, so every Steam peer-to-peer session
so far has been loopback. Nothing is known about behaviour over a real
connection, with real latency, between two machines, or through NAT.

This is the single largest gap between "it works here" and "it works for
players".

## Likely fine, but unverified

### Only two item types

Tested: Red Sign Soapstone (sign type 4) and Cracked Red Eye Orb
(break-in type 0). Untested: White and Small White Soapstone and their
Sunlight variants, the Dragon Eye, the Blue Eye Orb, and the Mirror
Knight sign, which has its own manager and its own message set.

Co-op through a white sign in Majula is the obvious thing a user will
try first and it has never been run once.

### Two players only

Never tested with three or more. DS2 allows more than one phantom, the
server has a max-player cap that the original brief wants removed, and
the counters this project patches are per-client. Nothing is known about
how they behave with a crowd.

### The hook's failure and uninstall paths

`DS2_UnblockMultiPlayHook` refuses to patch when the bytes are not what
it expects, and restores them on uninstall. Neither path has ever run.
The refusal path is worth forcing deliberately, by pointing it at a
wrong offset, to confirm it declines cleanly rather than taking the game
down.

### One game build

Every offset in this project is hardcoded against version 1.03,
Calibrations 2.02. A patch moves them all. The mask hook finds its param
by type name and survives; the two code patches do not, though the new
one at least refuses rather than corrupting.

## Changes made along the way that nobody has checked

### Partial protobuf parsing

`Frpg2ReliableUdpMessageStream` now uses `ParsePartialFromArray`. That
was the right call: our `required` fields are guesses from captured
traffic and a wrong guess was silently dropping a client's message.

But it is in **shared** code, so it changes Dark Souls III too, and it
weakens a check that used to catch genuinely malformed messages. Nothing
has tested DS3 since. At minimum, confirm a DS3 server still runs.

### Sticky signs and the debug invasion trigger

Both are still in the tree. `DS2_StickySigns` is retired and off, and
was never validated in the state it was left in. The `debug_invade.req`
trigger was proven not to be a usable oracle and is dead weight; it
should probably be removed rather than left for someone to trust.

### The message census only logs firsts

`LogFirstMessageOfEachType` reports the first of each type per client,
which hid a sign being created and removed repeatedly and cost a wrong
diagnosis. Anyone reading that log should know it is a census, not a
trace.

## Understood incompletely

### Heide reads 7 and accepts a sign anyway

Bit 3 of the permission byte is tested in two places and refuses the
item when clear. Heide has it clear and places signs regardless. Either
it reaches the permission by another route, or the byte read by the
param decode is not the byte those sites consult for that area.

This blocks nothing, and every area measured now behaves. But it means
the model is wrong somewhere, and a wrong model is exactly what made the
mask look innocent for so long.

### The root cause is untouched

`FUN_1403c0890` fails in a closed area because the per-map multiplay
record it looks up is missing, not because its other term fails. Filling
that record in would explain the behaviour instead of overriding it, and
would not need a code patch at all.

Worth finding where that table lives and what a present record contains.

## Not started

From the original brief, and unrelated to any of the above: arena
selection in the loader, and removing the max-player cap.
