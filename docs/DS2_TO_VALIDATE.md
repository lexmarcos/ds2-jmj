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

### Group travel with the session's map held (18/09)

The 32 consecutive legs of the first run used the Tower of Prayer (Shrine of
Amana) and Ironhearth Hall where Brume Tower and Threshold Bridge were meant
(see `DS2_NATIVE_TRAVEL_PLAN.md` §13). On the right four bonfires, with the
effects cleared before a DLC teardown, 14 of 14 legs held, seven of them
leaving Brume Tower. Only those maps, one save. Not checked:

- **Other DLC maps.** The effects clear runs for every `0x32xxxxxx` teardown,
  but only Brume Tower has been left. Shulva and Frozen Eleum Loyce are the
  same shape and untested.
- **What the effects clear costs.** It removes every live effect at the
  teardown, the phantom's glow and the bonfire flames included; whether each
  comes back by itself has not been looked at on screen, nor whether anything
  held a pointer (not a handle) to an effect it removed.
- **The "duty fulfilled" guard.** Written after one occurrence at Threshold
  Bridge; not exercised since. Nor has a boss actually been killed in a
  session with it in place.
- **The arrival bonfire's flame after leaving Brume Tower.** Measured 18/09:
  the effects clear takes it, and the timeact that lit it never spawns it
  again (research: `MapTimeActTrackSfx`, `FUN_1403e56d0`). A scoped kill per
  map owner is mapped out but not written.
- **Other maps and bigger ones.** The map heap read 57-60% with the session's
  map held plus one; a leg can need four maps, and a larger area (Drangleic
  Castle, Shrine of Amana) may not fit. Read `H+0xd0` against `H+0x4f0`,
  `H = *(*(base+0x1616cc0))`, on arrival.
- **A legal session end after travelling.** The host's native unbind
  (`FUN_140517e70`) walks the enemy sync's records; with the session's map held
  they stay valid, but a session that began elsewhere and was re-formed has not
  been ended after a travel.
- **The held map across a whole play session**: a session that begins in one
  map and plays for hours keeps that map loaded the entire time.

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

Since 14/09 the DS2 server also writes `Notify RequestNotify<type>: ...` for
**every** notify message it receives (`DS2_LoggingManager`), and a line when
a lost client's sign is finally dropped. Measured the same day: a second
summon on the same connections logged `Notify RequestNotifyJoinGuestPlayer`
and `JoinSession` with no census line. The census still applies to every
other message type. The `fields` in those lines are logged as received; what
most of them mean is still unknown.

### `game leave` from a bonfire, right after a session

Three times (T3, 14/09 17:4x and 18:19, each right after a session ended),
`game leave --instance 1` failed with `leave_failed` on Samuel standing at
the Majula bonfire; Chico, the guest, left cleanly every time. The screen the
last two times showed the bonfire's **Item box**:
the walk's first `press start` was lost, `press a` rested at the bonfire and
the rest of the walk navigated its menu. Two `press b` and a retry
recovered it. Why only the host pays: Samuel stands on the bonfire, where the
first A prompt is `Rest at bonfire`; Chico's is `Touch your bloodstain`,
which does nothing harmful when the lost `start` lets `press a` through. Not yet known whether the lost press is the post-session
state, the prompt toggle left on the bonfire, or timing; `leave` does not
check that its first press opened the menu.

### `teleport` and `goto-map` from the harness

Measured 14/09 with Samuel alone: the cathedral bonfire and back (12 m
down, no fall damage), and Heide → Majula → Heide with `goto-map`, the
contact on each map as the respawn measured it. Not yet exercised: a write
refused because the character moved (only a unit test with a fake reply),
the overlap landing on a Majula rock (it did not happen in either direction
this time), a teleport of the guest inside a session, and any map pair other
than Heide and Majula.

After the teleport to the cathedral, the Nav's own pose (`ctx+0xa8` chain)
stayed at the old bonfire, 68 m away, with its tick advancing; only the new
`live` field followed. `goto` and `goto --to-instance` still read that pose.
Unknown whether walking brings it back; `nav::read` should prefer `live`.

### `injector check` is mingw, not MSVC

`ds2os-dev injector check` parses the injector with mingw after rewriting SEH
and stubbing Detours. It catches typos, undeclared names and wrong types; it
cannot see MSVC-only behaviour, the Windows SDK's own headers, link errors or
anything inside a `__except` filter (rewritten away). A clean check is not a
green CI run. Three files already fail under mingw at HEAD and only new
errors are counted there: `Entry.cpp`, `ReplaceServerPortHook.cpp`, and
`DS2_LogProtobufsHook.cpp`, which uses `std::atomic_size_t` without including
`<atomic>` — latent, it compiles only because MSVC pulls the header in.

Measured 14/09: the receipt's `build` came back as the pushed commit on both
instances after `injector fetch` → `up`. Later the same day `fetch` waited
four minutes on run 34903919120 while it built, then downloaded it. It has not
yet met a failed run.

### The timer patch log throttle

`DS2_TimerParamPatch.log` now writes the first 100 breakpoint hits, every
`write_failed`, and then one line a minute carrying `suppressed_since_last`
(injector 858f4cd2). The build is installed, but the throttle has not been
seen working: the breakpoint only fires while a phantom timer runs, and
instance 2 idle in the world for fifteen minutes wrote nothing but its
`installed` line. The proof is a co-op session of a few minutes, then
`suppressed_since_last=` lines a minute apart after the first 100 hits.

### `save prune` on the real store

`save prune` was run for real only against a copy of the store (same names
and dates, tiny files): 57 snapshots became 28, three rescues kept per account
and `conta2-antes-de-nivel1` left alone as a chosen label. The real store,
36 rescues and about 230 MB, has only been through `--dry-run`.

### `human` in other saves, and after a warp

`ds2os-dev human` walks the Inventory blind and was measured only on Samuel
and Chico's saves, where the Human Effigy is the second consumable, and only
from the menu's state after a load (tab Equipment). Unknown: whether a warp
or a bonfire rest resets the remembered tab the way the title screen does,
and what the walk does in a save with another inventory order — it fails
honestly (`still_hollow`), but the presses may have used another item first.
Whether the remembered tab explains the `game leave` that got stuck in the
Item box on 14/09 (T5) is a hypothesis, not a measurement.

### M3: hollow and Soul Memory, beyond the white sign

Measured 14/09 with Samuel and Chico, both hollow, at Heide's bonfire: a white
sign summon with the host's hollow filter patched, and with Soul Memory
matching off across artificial tiers. Not measured: the Small White Sign
Soapstone (config opened, no sign placed), the red signs and orbs (their
item-use gate in `FUN_1402a1bf0` is untouched), whether the patched filter's
other refusal (`FUN_1402aabf0() == 8`) ever bites, a host in another area than
the sign, and the VPS, which still matches by Soul Memory.

### Entering from another area

Measured twice, Majula to Heide, 15/09, in `observe`: the guest's join lands
on its own sign converted into the host's map (empty space), and the death
hook's arrival check (role owner → phantom, host announced in another map,
map 0 or the host's under the feet) teleports it to the host's bonfire before
it falls. Same-map control did not trigger. Not measured: a pair of maps where
the converted point is over ground of a **third** map (the streamer would
report neither 0 nor the host's map, and the check gives up after 30 s), a host
whose bonfire announcement never arrives (no channel, or the host never rested
since boot), a host with its bonfire in a map other than where it stands, and
any area pair besides Majula → Heide. The VPS server has no party pass.

### M8: resting at a bonfire in a session

Measured 15/09 with Samuel hosting and Chico's phantom beside him at Heide's
Ruin: with `DS2_BonfireInSessionHook` the host sits, the bonfire menu opens
and the session stays verified for a minute after the menu closes. Traveling
from that menu (Heide's Ruin → The Far Fire) ends the session, and the party
rejoins the guest at the host's new bonfire in 75 s with no input. With the
vote (build `3235c402`) the travel waits in the list for every guest's Yes, the
guests leave legally, and the party rejoins at the new bonfire; decline and
timeout leave the host in the list. The guest's world reset and the rest
notice were measured once each. Travel costs no penalty points (four leaves
measured). A white phantom now gets "Rest at bonfire" (builds `44e4f654`
by probe, `618742e8` in the hook): it sat at The Far Fire in Chico's world,
healed 400 -> 914, its rest reset the host's world and showed the notice on
the host; its own travel proposal opened the host's vote naming "The Far Fire
(Majula)", a decline told the guest, and an accepted one made the guest leave
legally and the host travel, with no penalty points and the party back
together. A guest that leaves the host's question unanswered sees "Travel
canceled: not every player answered." when it times out (build `74247cca`).
The guest's rest resetting the world was measured by the log lines on both
machines, not by an enemy coming back (that was measured for the host's rest).
Only type 14 of the event action entries is let through for a white phantom
(build `dec12dd2`, where the guest again sat and healed 400 -> 914); type 13, most likely lighting an unlit bonfire, stays
refused and was never offered to a guest.

Travel now moves everyone without leaving the session (build `2cda8f19`): the
host goes first, and once it has stood still in the new map for 1.5 s the
guests follow, each through the backread hold and focus. Measured: a guest's
proposal in Heide accepted by the host put both at The Far Fire in 2.2 s plus
the guest's own move, seeing each other, session verified; and three Heide
<-> Majula round trips through `DS2_Bonfire.req`'s `ir`. The travel now puts the game's own loading curtain up
(`ctx+0x1178` plus `FUN_140b06270`), so nobody sees the character between maps;
it comes down 1.2 s after the traveller is standing. Still failing: the
guest's game closed twice in five travels **after** arriving, in the character
pre-draw task and in a list with a freed node - never once in eight solo
travels. Not measured: that
the earlier crashes this cost are actually gone (each fix was measured once,
against a failure that did not happen on every travel), any map but Heide and
Majula, a map the backread hook cannot bring in (the old leave-and-rejoin
path is still there for it, now untested), three or more players (they would
all follow the host at the same instant, which is the shape that crashed both
games), travelling with enemies awake or mid-fight, and what the host's own
`Bonfire intensity` does to a guest that arrives this way. The guest's travel
list is now the host's lit bonfires, written into the guest's copy of the
bonfire table every second; not measured: a bonfire the host lights during the
session, a guest that has lit fewer bonfires than the host, and any table but
1.03's 77 entries. Not measured: three or more players (a guest's rest notice relayed
to the other guests), leveling or attuning in it, burning a Bonfire Ascetic
with a phantom present, resting with an invader in the world, a proposal the
host refuses as not lit or busy, whether query 130602's answer is still needed
once the prompt gate is open, a guest's travel to any bonfire but the one it
stands at, and any bonfire but Heide's Ruin and The Far Fire.

### M7: flags carried into the guest's save

Measured 15/09 with one unused global flag (`109999`) set by the host's own
setter in a session: it crossed as packet `0x20`, the guest kept it, set it in
its own world after `session end`, and it survived a restart of the guest's
process; a map flag set the same way did not follow the guest home. Not
measured: a real boss kill (whether boss flags are global at all), a pickup or
an NPC change in the host's world (if those are global, the guest's world gets
them too, against M5 and M10), a guest that dies or quits before reaching home
(`DS2_Carry.pending` should apply on the next arrival), a host that turns a
global flag off, and any flag the host sends while loading a map.

### M4: the host's world at join

Measured 15/09, both characters in Heide: a guest carries the host's event
flags in place of its own at join (a bit set only in the host's memory
appeared on the guest, a bit set only on the guest disappeared), for map and
global categories, and gets its own back at home. The join goes through the
world snapshot (`FUN_1402bf8f0` export → `FUN_1402c2fa0` import), not packet
`0x20`. Not measured: any real door, lever, elevator, illusory wall or Pharros
mechanism; a flag changed **during** a session (the host cannot rest at a
bonfire in one, "Cannot use bonfire"); `MapStateActManager`,
`EnemyGeneratorDeadCounter` and `EventBonfireManager` beyond reading that the
snapshot carries them; any map but Heide; a guest's own action on a mechanism
in the host's world. See docs/DS2_WORLD_STATE.md.

### Entering without a soapstone

Measured once, 14/09, with both characters hollow at the same bonfire: the
guest's sign placed through `DS2_Party.req` (`placa 1`) and the host's rematch
armed by hand (`alvo 3 1`) formed a session with no button pressed. Not
measured: other sign types through `placa` (the type goes through the game's
own mapping, `FUN_14029c9b0`), the two players in different areas or cells, a
host already in a session, what `placa` does while the player is a phantom, a
stranger's client seeing the party sign, and the red sign path.

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
  logged the first message of each type per client then, so that is not
  evidence either way. The server now logs every `RequestNotifyDeath`;
  the fall has not been repeated since. A staged kill needs the two characters next to
  each other, and the terrain around the Heide bonfire kept killing the
  phantom on the way over.

The first reading of these measurements was wrong in a way worth
remembering: a death as an invader looked like it cost human form,
because an accidental second death **in the invader's own world** sat
between the duel and the test. The fix was to run the loop again with
nothing in between.

### The red sign rematch, and what the server has without delivering anything

`DS2_AutoRematch` and the `debug_summon.req` trigger are on the server and do
what they promise: the pair is remembered and the push is sent again. But
**on their own they form no session at all** — it is measured in
[DS2_REMATCH_AFTER_DEATH.md](DS2_REMATCH_AFTER_DEATH.md). The flag is born
off and should stay that way until the client half exists; otherwise it
becomes the same trap that `debug_invade.req` became.

Open, in order of how much they block:

- ~~Does the `SignHandle` survive a new sign?~~ **No**, and the hook already
  handles it: it reads the new handle from the output parameter of the
  function that registers the sign. The red sign rematch works end to end.
- **The hook summons any sign that arrives**, not just the pair's. With two
  players that comes to the same thing; with three it is wrong. The item
  field that identifies the owner still has to be read.
- **Which index of the table at `0x1410c0050` is each role.** Zeroing them
  all wedges death; for seamless co-op it is necessary to know which entry
  to touch, and the type comes from `rcx+0xe0` in a transient object.
- ~~Can the Red Sign Soapstone be used hollow?~~ **Answered 12/09: no.**
  Hollow, X places no sign; with a Human Effigy, at the same spot and with no
  walking, the sign goes down. It holds for both online items.
- **A hollow host does not see a sign** was measured only once, with the
  control at the same spot (effigy, the prompt appears). Worth repeating
  somewhere else before it becomes a rule.

### The warp, and seamless co-op

The warp path is mapped and the hook exists
([DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md)). What it delivers is **where
the player lands**, not the session: `RequestNotifyLeaveSession` still goes
out and the session still ends. Open:

- **Can the session survive a death?** The game never loads an area inside
  the host's world for a guest, so this may simply not exist. It is the
  question that decides whether real seamless co-op is possible or whether
  the path is dying and meeting again through the rematch.
- **What other warp reasons exist** besides 1 (bonfire) and 4 (end or start
  of a session), and what the gate at `0x140248940` charges anyone who is
  neither. `DS2_Seamless.log` answers that on its own with use.
- ~~Does a guest's respawn record still point at its own bonfire while it is
  a phantom?~~ **Yes**: the redirection of a co-op phantom returned
  `mapa=0a1f0000 ponto=00007ba7`, which is the same point as an ordinary
  death of its own in its own world.
- ~~The host's death with a phantom inside~~ **measured 12/09**: the host
  does an ordinary death (reason 1, to its own bonfire) and the guest goes
  through the same `+0x2c3bde` with the same position shape as when it is
  the one dying. One hook covers both cases.
- **The three param bytes** (`+0x2c/+0x2d/+0x2e` of the role's row) have only
  been read by deduction from the behaviour. The row still has to be read in
  memory and checked role by role, and it is still unknown what end reason 4
  is, which returns `2` without consulting the row.
- **When the guest's sign is not on top of its bonfire**, the redirection
  should visibly change the landing place. Both measurements were made with
  the sign at the same spot as the bonfire, so the swap is proven in the
  request, not on screen.

### The held death (`DS2_DeathInterceptHook`)

Measured solo only, only with Samuel, in Heide, on 13/09
([DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md), "Death measured, and
held"). Open:

- **Only two of the ten sources of `+0x759` have been exercised**: zeroed HP
  and a fall. Lethal enemy damage (`FUN_14013a9b0`), the three in
  `FUN_14013cc30` (among them animation event `0x19`, which grab attacks use),
  `+0x145f3f` (cause `0x6e`), `+0x31b753`, `+0x37046b` (landing) and
  `+0xd1c7f8` have not. The ones in `FUN_14013cc30` set the byte **inside**
  the consumer and would slip past the check; the log says `SEM +0x759 ANTES`
  when that happens.
- **Instant death** (`FUN_14013c500`, types 1 and 2) is only logged. It has
  not shown up once: neither in the fall nor in the death by HP.
- **A character standing with zeroed HP for one frame**: the hook gives the
  HP back on the same frame the byte appears, but whatever runs between
  `FUN_14016a650` and the controller in that frame sees HP 0. Nobody has
  looked at what that sets off.
- **With a session**: measured 14/09 — the other side saw the death through
  HP replication, and cancelling only on the client that dies was not enough.
  The copy's refusal is in the step 6 section, below.
- **The undone fall was measured in a single volume**: Heide's water, bit 51.
  The volumes that set bit 52 (types 3, 4, 7, 8), death by landing damage
  (`FUN_140372c00`, causes `0xa0`/`0x3c`) and type 10, which only asks for
  the camera, have not been exercised.
- **The return to "the last position on the ground"**, used when the record's
  bonfire is not in the loaded map, has never run. Falling near an edge may
  put the character back on the edge.
- **Standing inside a death volume without being in the air** (if any map has
  that) turns on the falling camera with no death for the hook to refuse, and
  the camera would be stuck.
- **Orientation is not written** in the teleport: the character arrives at the
  bonfire facing wherever it was facing.

### Step 5's respawn and step 6's session

Measured in Heide, with Samuel and Chico, solo and in a session
([DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md), "Respawning and paying for the
death" and "With a session"). Resolved 14/09: the death counter, the
obfuscated hollow check (it is the phantom's exemption), "YOU DIED" and the
phantom's bill. Open:

- **Spell uses and states** (poison, curse, etc.) are not restored; only the
  Estus and the HP. Resting at a bonfire restores them through a SpEffect
  dispatcher whose targets are jumps into obfuscated code (`0x14014c183` is
  the call that refills the Estus, surrounded by others);
  `ItemInventory2SpellList` has no refill method in sight. Neither character
  has any spells to measure with a write watcher.
- **The online bloodstain** does not go out: the job runs and sends nothing,
  because the phantom recorder never records the frame with the character
  dead (`0x1000`). Switch `mancha_online` off.
- **The other player's copy**: the refusal was measured with the death byte
  set by hand. The natural race — HP 0 going out over the network before
  being given back — happened once before the fix and none of the seven times
  after it; it has not been seen being refused.
- **The refusal holds for any `PlayerCtrl` that is not the local one.** A
  remote player's copy is type 2 in `chr+0x54`; if an NPC phantom were a
  `PlayerCtrl`, it would be immortal in the `cancel`/`respawn` modes. Not
  measured.
- **A real death on the other side** (that machine without the hook, or in
  `observe`) is refused on the copy over here while `copias` is on: the
  "vanquished" and the `RequestNotifyKillEnemy` disappear. The session still
  ends from that side.
- **Sin** (`FUN_140202ae0`): a type 1 death *reduces* it in a session when
  `FUN_14018fc90` finds a certain member in the session; it is not touched.
- **The death sound** (`FUN_1401905c0`, BGM 2) and event `0x25` of
  `FUN_14037d9f0` were left out.
- **The banner and the HUD**: `FUN_1404fffb0` also zeroes `+0x31c` and has
  `FUN_140507360` rebuild four HUD objects that have not been read (a boss
  bar, for instance). Measured outside a fight.
- **Bonfire outside the loaded map**: the return to "the last position on the
  ground" in a respawn by HP would leave the character where it died, paying
  for the death.
- ~~**The guest uses its own bonfire's record.**~~ Resolved in step 7
  (below).

### The host's bonfire (step 7)

Measured 14/09 with two players in Heide, both bonfires in the same map
([DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md), "The host's bonfire"). Open:

- **Three players or more.** The host is the one the game marks (`+0xad` of
  the member), and a guest joining announces nothing more; with two accounts
  there is no way to see a second guest receiving the announcement, nor a
  third joining.
- **The host's record changing during the session** (lighting or resting at a
  bonfire with a phantom present, if the game allows it): the announcement
  goes out the moment the record changes, but that has not been exercised.
- **A type 1 or 2 record** (event point, the map's "player start"): the search
  is by bonfire id, and those fall back to their own record's bonfire. Only
  type 0 has been seen.
- **The host on a loading screen.** With no frames, the host stops announcing
  in 2 s and the last announcement is good for 30 s; a guest that dies after
  that goes to its own bonfire.
- **A player without the mod in the session** receives the announcements on
  channel 7 and never reads them; they sit in Steam's queue (24 bytes every
  2 s). Not measured.
- **The host's bonfire outside the loaded map** started being loaded in step
  8 (below).

### The bonfire in another map (step 8)

Measured 14/09 with two players, between Heide and Majula, in both
directions, solo and in a session
([DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md), "A bonfire on another map").
Open:

- **Death by falling with the bonfire in another map.** The path is the same
  as the HP one (the respawn holds the character at the last position on the
  ground while the map arrives), but only deaths by HP have been measured in
  that condition.
- **Other map pairs.** Only Heide ↔ Majula. A map that uses the world offset
  (`ctx+0x2530`) when entering has not been exercised, and one that takes
  more than 1800 frames (30 s) to reach state 5 gives up and leaves the
  character at the last position on the ground, already charged.
- **Stepping on the wrong map.** On a manual trip back Majula → Heide with
  the focus released early, Samuel landed on a Majula rock at Heide's
  bonfire. The respawn sends it to the bonfire again every 90 frames when
  that happens, but in the nine respawns in another map that were measured
  the character stepped on the right map within 10 frames, and the resend
  never ran. All of Majula, solo, did not put the rock there.
- **Keeping the other player's map.** Why Chico's game closed in the first
  session was never read — `DS2_Crash.log` came later. Keeping the whole map
  passed in one session (two deaths), and keeping it by parts, which is what
  is in the build, in another (two deaths). A copy that arrives at a map on
  top of an object (type 1) keeps every part until it steps on a part. It is
  8 maps kept at most, by index.
- **The copy walking inside the kept map.** The keeping holds only the set of
  the part under the copy, swapped at each new part, without the sum of the
  neighbouring parts the streamer would do for a player there — and at
  Majula's bonfire that set is a single bit. In the two deaths in a session
  the copies were standing still.
- **Memory.** Neither keeping the whole map nor keeping it by parts has had
  its cost measured.
- **Walking back between maps with a phantom.** The phantom fog between areas
  is still there (`--remove-fog` is the experiment); step 8 only goes from one
  map to another in the respawn.
- **Three players or more**: two maps kept for two copies, and a guest
  respawning in a third one's map. Not testable with two accounts.
- **During a boss fight, or with the boss fog closed**, the bonfire in another
  map has not been tried.
- **Chico's HP** stayed at 853 of 854 after each respawn in another map, with
  the bonfire above and below the death. Not investigated.
- **Teleport and the fall controller in the same map.** The teleport now
  moves the position the fall is measured from (`*(*(chr+0xe0)+0xb0)+0x20`);
  before that, a respawn at a bonfire well below the death in the same map
  could count as a fall. No such case was measured, before or after.

## The login that resolves the official hostname, after a reboot

Measured 13/09, on the first launch after restarting the machine: the game
showed "The DARK SOULS II service is not available" and **never connected to
the local server**. With `ss` sampled every 10 ms, the attempts went to
`44.235.83.177:50050` and `44.235.102.125:50050` — which is exactly what
`frpg2-steam64-ope-login.fromsoftware-game.net` resolves to —, that is, the
port already swapped by the injector and the **official hostname**. At the
same time, the module's UTF-16 string (`0x1410d4ab0`) already said `127.0.0.1`,
and there were ASCII copies of the official hostname on the heap (one with the
FQDN trailing dot).

At the title, before pressing START, those ASCII copies do not exist: they are
made at login. Relaunching the game fixed it immediately, and the next login
went to `127.0.0.1`. What made that process use the old name was not
discovered. Two conditions were present and may matter: that account's Steam
had just been opened by the game itself (exit code 53, `steam://run`), and the
server had come up before Steam logged in. For the loader this matters: a
player would see the same error and would not know it is just a relaunch.

## Not started

From the original brief, and unrelated to any of the above: arena
selection in the loader, and removing the max-player cap.
