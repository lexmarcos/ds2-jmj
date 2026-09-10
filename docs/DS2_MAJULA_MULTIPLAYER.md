# Multiplayer in Majula, and the other closed areas

Solved on 2026-09-10. A player places a Red Sign Soapstone in Majula,
another player finds it, touches it, and a red phantom arrives. Verified
by the join handshake reaching the server, not merely by the absence of
an error:

    14:37:42  Samuel  Sign 1007 created: type 4, area 0x009932c0
    14:37:56  Chico   Summoning sign 1007
    14:38:01  Samuel  Sign 1007 removed by its owner
    14:38:07  Chico   First RequestNotifyJoinGuestPlayer
    14:38:09  Samuel  First RequestNotifyJoinSession

Those last two had never appeared once in this project before. In Heide
they arrive within seconds of a sign being consumed, and now they do in
Majula too.

**It is not a Majula special case.** The same three changes open Things
Betwixt, the tutorial area, which is the most closed area in the game:

    15:02:32  Samuel  Sign 1009 created: type 4, area 0x0098e4a0
    15:03:05  Chico   Summoning sign 1009
    15:03:11  Samuel  Sign 1009 removed by its owner

Its permission mask reads `0`, meaning nothing at all, not even ghosts,
against Majula's `4` and Heide's `7`. So this is not a patch for one
town; it opens the areas the game closes, as a class. That was the wider
goal all along, and it came free with the narrow one.

This document is the reference. The investigation that got here,
including three approaches that failed and why, is in
[DS2_STICKY_SIGNS.md](DS2_STICKY_SIGNS.md).

## Three things are required, and none is enough alone

The game refuses multiplayer in Majula in three independent places. All
three have to be answered, and the order in which they were found is not
the order in which they act.

### 1. The multiplay zone

`DS2ForceMultiPlayZone` in the injector config, with `DS2ForcedZoneId`
set to `103110`, which is Heide's zone. The hook writes it into the map
block record at `[block+0x20]`.

Majula natively reports `-1`, meaning the player is not inside any
multiplay zone at all. The zone table is global rather than per map, so
looking up Heide's zone from inside Majula returns a valid record.

### 2. The area's permission bit

`kTargetMask` in `DS2_UnlockAreaMultiPlayHook`, now `63`. The hook finds
`NETWORK_AREA_PARAM` by its own type name and raises every row below the
target.

Two sites test **bit 3** of the byte at `+0x18` of the row and refuse
the item when it is clear, both branching to `xor al,al; ret`:

    1402a5fbb  testb $0x8,0x18(%rax) ; je 1402a6000   summon signs
    140272222  testb $0x8,0x18(%rax) ; je 1402721a0   invasion orbs

The hook wrote `7` for a long time, which leaves bit 3 clear, so it had
never once written the bit it exists to write.

### 3. The multiplay block counters

Three bytes patched in both clients:

    pokemod blockcond 3c0890 b001c3 48895c

`+8` and `+9` of the object at `[[0x141616cf8 + 0x18]]` are two
saturating counters, raised and lowered by a per-frame updater that
edge-tracks `FUN_1403c0890`. Majula reads `01 01`, Heide `00 00`.

Three consumers read them, and each is a different failure on screen:

| site | reads | failure the player sees |
| --- | --- | --- |
| `FUN_1402a0ff0`, summon push handler | sign owner's `+8` | "Player can no longer be summoned" |
| `FUN_1402c2a80`, guest join controller | guest's `+8` | error 0x13 |
| `FUN_1402bf440` at `0x1402bf46d` | **host's** `+9` | error 6, "Disconnected from multiplayer session" |

This now ships as `DS2_UnblockMultiPlayHook`, which writes the same
three bytes at install time and restores them on uninstall. It refuses
to apply if the bytes are not what it expects, so a game update moves
the offset and the hook declines rather than corrupting whatever is
there.

Patch the source rather than poking the counters: the object holding the
edge flag can be rebuilt when a client loads into another world, which
would re-raise a poked counter mid-session.

`FUN_1403c0890` looks up a per-map record by packed map id and wants a
non-zero first byte with `[block+0x16] == 0`. Majula's `+0x16` is
already zero, so the per-map record is the failing term: Majula simply
has no multiplay data in that table. That is the underlying "why", and
it is a better place for a permanent fix than the patch above.

## Applying it

All three ship in the injector, gated together behind
`DS2ForceMultiPlayZone` in `Injector.config`, and need a build. See
[[injector-built-on-ci]] in the project memory: `Injector.dll` needs
MSVC, so the GitHub Actions workflow `injector-linux.yml` builds it,
`gh` fetches the artifact into `~/Downloads/injector`, and
`ds2os-dev game prepare --force-zone` installs it into both game
directories.

To try a change without a build, the same patch can be written into a
running client through the probe. **The two instances have separate
installations**, each with its own request file, so it has to be written
to both:

    /mnt/ssd/SteamLibrary/steamapps/common/Dark Souls II Scholar of the First Sin/DS2_MemProbe.req
    /home/suel/steam2/.steam/debian-installation/steamapps/common/Dark Souls II Scholar of the First Sin/DS2_MemProbe.req

That is live memory and is lost whenever a client restarts.

## Verifying

Read the counters on both clients:

    chain mgr 1616cf8 18 16

Expect `00 00 00 01`. Majula reads `01 01 00 01` unpatched; Heide reads
`00 00 00 01` naturally, which is the baseline the patch reproduces.

Then place a sign and summon, and look for
`RequestNotifyJoinGuestPlayer` and `RequestNotifyJoinSession` in the
server log. Their absence is the failure; a missing error message is
not the success.

## The control

Run with the block patch active, a summon in Heide still works: sign
1008 placed in `0x009d5170`, summoned, consumed, no rejection, phantom
delivered. So the patch opens the closed areas without disturbing the
ones that already worked.

Three areas have now been tested with one build, spanning the whole
range of the permission mask: Things Betwixt at `0`, Majula at `4`,
Heide at `7`. All three place, summon and deliver.

### Invasion across areas works too

    15:09:29  Break-in target list: area 0x009d5170, 1 candidate of 2 clients
    15:09:29    candidate '3:Chico': area 0x0098e4a0, invadable yes
    15:09:29  Invading '3:Chico' across areas. Sending area 10020000 cell 103110

An invader in Heide reached a target in Things Betwixt, and the session
held. This had dropped every time before the block patch, and nothing
about the invasion path was touched to fix it.

That confirms what the counters predicted rather than merely agreeing
with it. One accept controller serves both signs and invasions, it reads
the **host's** `+9`, and the host was the player in a closed area in
every case that failed. Two symptoms, one cause, and one patch closed
both.

`DS2_InvadeAnywhere` is what lets the target list offer a player
standing in another area; without it the server filters them out before
the client ever sees them.

The join handshake lines do not repeat in that run, because the server's
census logs only the first of each message type per client and they had
already fired. Their absence there is expected and is not a failure.

## What is not done

- Heide reads `7`, with bit 3 clear, and accepts a sign anyway. Every
  measured area now behaves, so this blocks nothing, but it means the
  permission bit is not the whole story and the model is incomplete.
- The root cause is untouched. `FUN_1403c0890` fails because Majula has
  no per-map multiplay record; filling that record in would be cleaner
  than patching the condition, and would explain rather than override.
- `DS2_StickySigns` is retired. The client asks for signs itself once
  the mask is right, so the server never needs to fabricate a list.
