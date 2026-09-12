# Still to validate

Multiplayer works in the closed areas, but "works" currently means: two
players, almost always on one machine, with two item types, in three of
the game's thirty-one areas, up to the moment the phantom appears.
Everything below is untested rather than known good.

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

## Likely fine, but unverified

### One pair across the internet

Until 2026-09-11 both clients always ran on the same box, so every Steam
peer-to-peer session was loopback. That day the first pair on two
machines met: a Linux client and a Windows client on two different home
connections, both started by `loader-main` against the production
server. Both reported Majula inside the forced zone, so the area patch
reached the Windows client too. The Linux player's Cracked Red Eye Orb
found the Windows player as the only candidate, and the invasion landed:
the phantom arrived in the Windows player's world.

The server could not have shown that last part. The peer-to-peer
session goes through Steam rather than the server, and the server logs
nothing when one forms (`LogFirstMessageOfEachType` was off). The two
clients reporting positions a few metres apart afterwards was suggestive
and no more; the confirmation is the player's own account.

Still untried: stricter NAT, worse latency, a summon rather than an
invasion, any pair but that one.

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

### The fog patch, in one area and one pair

`DS2RemovePhantomFog` takes away the barrier that pens a phantom into the
host's area, by holding every fog wall in the state it has when nobody is
visiting. Confirmed in Heide, one invasion, one pair of players: the barrier
does not appear, both move freely, and the boss gate still works.

Not tried: any other area, a white sign summon rather than an invasion, a
session that lasts, more than one guest, and what the server makes of a guest
who walks somewhere its own area filtering did not expect. It is off by
default and off in the loader for exactly that reason.

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

### The rematch after a death

Measured in [DS2_REMATCH_AFTER_DEATH.md](DS2_REMATCH_AFTER_DEATH.md), with
two gaps left open:

- whether the Red Sign Soapstone can be used hollow. The test that
  looked like it proved yes was run on a character who turned out to be
  human, so it proves nothing. The orb is the only item measured.
- whether a fall death and a kill death produce the same message chain.
  The fall showed no `RequestNotifyDeath`, but the server census only
  logs the first message of each type per client, so that is not
  evidence either way. A staged kill needs the two characters next to
  each other, and the terrain around the Heide bonfire kept killing the
  phantom on the way over.

The first reading of these measurements was wrong in a way worth
remembering: a death as an invader looked like it cost human form,
because an accidental second death **in the invader's own world** sat
between the duel and the test. The fix was to run the loop again with
nothing in between.

### A revanche por red sign, e o que o servidor tem sem entregar nada

`DS2_AutoRematch` e o gatilho `debug_summon.req` estão no servidor e
funcionam no que prometem: o par é lembrado e o push é reenviado. Mas
**sozinhos não formam sessão nenhuma** — está medido em
[DS2_REMATCH_AFTER_DEATH.md](DS2_REMATCH_AFTER_DEATH.md). A flag nasce
desligada e deve continuar assim até existir a metade do cliente; caso
contrário ela vira a mesma armadilha que o `debug_invade.req` virou.

Em aberto, em ordem de quanto bloqueiam:

- **O `SignHandle` sobrevive a uma placa nova?** O handle capturado no
  toque é de uma placa que deixou de existir. Se o handle da placa
  recolocada for outro, o hook precisa enumerar em vez de repetir.
- **Qual índice da tabela em `0x1410c0050` é cada papel.** Zerar todos
  trava a morte; para o co-op seamless é preciso saber qual entrada
  mexer, e o tipo vem de `rcx+0xe0` num objeto transitório.
- **A Red Sign Soapstone pode ser usada hollow?** Continua sem resposta:
  o teste que parecia provar que sim foi feito com o personagem humano.
- **O host hollow não vê placa** foi medido uma vez só, com o controle
  no mesmo ponto (efígie, prompt aparece). Vale repetir num outro lugar
  antes de virar regra.

## Not started

From the original brief, and unrelated to any of the above: arena
selection in the loader, and removing the max-player cap.
