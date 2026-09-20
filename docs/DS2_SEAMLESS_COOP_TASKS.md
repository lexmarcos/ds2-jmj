# Seamless Co-op: the plan, in order

The brief is in [DS2_SEAMLESS_COOP_DESIGN.md](DS2_SEAMLESS_COOP_DESIGN.md).
What has already been measured of the client is in
[DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md). This file is the work list, and
it exists to be **edited**: when a milestone closes, it becomes a line of
"done" with the address and the proof, it does not disappear.

**M0 jumped ahead of everything**, on request: without walking a character over
to the other one with no human hand, each of these tests costs half an hour of
piloting. It is done — see
[DS2_INVESTIGATION_TOOLS.md](DS2_INVESTIGATION_TOOLS.md) for what it does and
what it does not do.

The order is not preference, it is dependency. **Everything from M2 down
assumes M1.** While a death undoes the session, respawn, spectator, party
wipe, group travel and bonfire reset have nowhere to happen.

A rule that holds for every item: **success is a positive signal**. For a
session, that is `RequestNotifyJoinGuestPlayer` followed by
`RequestNotifyJoinSession` reaching the server. Absence of an error proves
nothing.

---

## Done

| what | where | proof |
| --- | --- | --- |
| Multiplayer in the closed areas | `DS2ForceMultiPlayZone` + `DS2_InvadeAnywhere` | red mark in Majula |
| No 12 min limit on the session | `DS2PatchPhantomTimers` | [DS2_PHANTOM_TIMER_PATCH.md](DS2_PHANTOM_TIMER_PATCH.md) |
| The host re-summons the sign by itself | `DS2_RematchHook` | `Summoning sign` with nobody pressing anything |
| …including the **white** co-op sign | the hook does not look at the type | `Sign … type 1` summoned by itself |
| Every warp in the game mapped and interceptable | `DS2_SeamlessCoopHook`, `+0x1c2a80` | [DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md) |
| A guest's death goes to the last bonfire | reason 4/force 0 → `FUN_14044fde0` | measured with the switch on and off |
| **M0 — the harness walks by itself** | `DS2_NavHook` + `ds2os-dev where` / `goto` | 9 m from the bonfire to the other character, up a staircase, in 17 steps |
| **M3 — no effigy**, white sign | `DS2_HollowSummonHook` (`+0x2a1b1e`), with `--seamless` | Samuel and Chico hollow (state 1): `Summoning sign` → `RequestNotifyJoinGuestPlayer` → `RequestNotifyJoinSession`, `p2pSessionVerified: true` (14/09) |
| **M3 — no soapstone and no touch**, first half | `DS2_PartyHook` (guest: `placa 1`) + `DS2_RematchHook` (host: `alvo <jogador> <tipo>`), with `--seamless --auto-rematch` | no key pressed on either side: `Sign 1015 created` → `Summoning sign 1015` → `JoinGuestPlayer` → `JoinSession`, `p2pSessionVerified: true`, both hollow (14/09) |
| **M3 — no Soul Memory**, white sign | `DisableSoulMemoryMatching` in `DS2_WhiteSoapstoneMatchingParameters` and `DS2_SmallWhiteSoapstoneMatchingParameters` | with tiers that separate 5130 from 2551: `refused by matching 1` with it off, `sent 1` and the summon with it on (14/09) |
| **M3 — join once, with no ritual** | `DS2PartyGuest`/`DS2PartyAccept`/`DS2PartyPassword` (`DS2_PartyHook`), a party pass in `DS2_SignManager`, arrival from another map in `DS2_DeathInterceptHook`; `up --seamless --party` | password, four cases; Majula → Heide with no key press and no fall, `p2pSessionVerified: true` (15/09) |

---

## M1 — the session survives a death — **DONE on 12/09**

Refusing the end-of-session request keeps the session alive. Measured, with
the refusal armed on the guest's side only (`block 2`, `role 1` in
`DS2_Session.req`) and the white phantom killed by a fall:

    fim de sessao RECUSADO papel=1 estado=7 motivo=2 de=+0x2c9246

- **no warp happened** — the guest's `DS2_Seamless.log` stayed empty, that is,
  it was not sent home;
- **the host asked for nothing**, its log stayed empty too;
- and the session stayed up **on both sides**: the host's HUD kept listing
  "Chico" with an empty bar, and the guest's HUD kept showing the "Samuel" bar.

What is **not** resolved, and is M2: the guest stays dead where it fell.
Nothing picks it up. Refusing the end prevents the teardown; it does not make
anything respawn.

**And it does not hold indefinitely.** Measured afterwards: left like that,
the guest eventually received *"Disconnected from multiplayer session."* and
went back to its own world alive, with nothing being asked again. That is,
refusing the end of session **postpones** the teardown while somebody acts, it
does not cancel it for good: a dead guest that never gets up is a session that
expires.

That does not undo M1 — at the moment of the measurement there was no warp,
the host asked for nothing and both HUDs listed the other — but it changes what
it means. M1 buys time for M2 to happen; it does not replace M2.

What happens if the host walks to another area with a dead guest hanging off it
is also not measured.

### How it was before

**The milestone that unlocks the rest.** Today: the guest dies, `FUN_1402c3900`
(state 8) sends `RequestNotifyLeaveSession`, the session ends and it goes to
its own world.

What is already known:

- the session is a state machine, with the state in `objeto+0xf8`;
- **state 2** = `FUN_1402c2a80`, takes the guest into the host's world; it
  builds a warp request with `motivo 4`, third argument **1**, and the
  destination is map + position + orientation;
- **state 8** = `FUN_1402c3900`, tears it down; a warp with `motivo 4`, third
  argument **0**;
- the state transition goes through the session object's virtual slot `+0x30`;
- the role table at `0x1410c0050` decides whether a death undoes the sessions —
  the phantom is index 7, the byte at `0x1410c00c1`. **Zeroing that byte does
  not prevent the return**: the session hangs and the player leaves all the
  same. Measured.

The hypothesis to test, in this order:

1. on a guest's death, prevent the move to state 8 (a hook on the `+0x30`);
2. re-emit a warp in state 2's shape — `motivo 4`, third argument 1, the host's
   map, the destination position — instead of the warp back;
3. see whether the host still sees the phantom and whether the session stays
   alive.

### Measured 12/09: who asks for the end, and with what reason

With a real co-op formed (`RequestNotifyJoinGuestPlayer` followed by
`RequestNotifyJoinSession`) and the guest killed by a fall, the hook logged,
**on the guest's client**:

    fim de sessao pedido  papel=1  estado=7  motivo=2  motivo_anterior=0  de=+0x2c9246

And on the **host, nothing**. Its log stayed empty.

Three things come out of that:

- the state was **7**, as the static path predicted, and reason **2** is what
  ends up in `+0x1cc` and pushes the machine to state 8;
- the white co-op phantom's role is **1** (the red invader is 7);
- **the teardown is born entirely on the side of whoever died.** The host asks
  for nothing. That is what makes M1 plausible with a single hook: refusing on
  the guest's side may be enough, without touching the host's client.

The one asking is `+0x2c9246`, not yet deciphered.

How to prove the rest: no new `RequestNotifyLeaveSession` on the server, and
the phantom visible on the host's screen after the respawn.

Known risk: the warp **returns a byte**, and state `0x13` is where the machine
goes when it refuses. An ignored return hides the refusal.

---

## M2 — respawn inside the session — **DONE on 14/09, in the cases measured**

**The eight steps are done (14/09).** The criterion was observed in every case
measured, with two players: the host's death and the guest's, by HP and by
falling, with the bonfire in the loaded map, including when the guest's record
points at another one (steps 6 and 7), and by HP with the bonfire in another
map, in both directions between Heide and Majula (step 8). What was not
measured — a fall with the bonfire in another map, other map pairs, three
players — is in [DS2_TO_VALIDATE.md](DS2_TO_VALIDATE.md). The criterion,
defined by the project's owner on 13/09:

> dies → respawns → stays in the **same** session, and the host carries on
> playing normally. It holds for the death of **either** of the two, host or
> phantom.

The automatic re-summon measured on 12/09 — the guest goes home, puts the sign
down, and the host's `DS2_RematchHook` brings it back by itself — is **a
workaround**: it is another session, with a load, and it depends on the player
putting the sign down again. It is recorded in
[DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md) as a fallback plan, not as a
delivery. I declared M2 closed with it on 12/09 and I was wrong.

**Why stitching the existing session back together after a warp does not
close it** — measured on 12/09: the guest's machine completes the join up to
state 7; the host's stays at `0x10` the whole time, without noticing the death;
and the host sends `RequestNotifyLeaveGuestPlayer` **twenty-three seconds**
later — a timer, not a reaction.

**And why no warp is going to close it** — read in the binary on 13/09: every
warp, of any kind, goes through the loader's teardown, which destroys the local
character (`ctx+0xd0`), the remote characters and the map, and rebuilds
everything. That is what takes the phantom out of the host's world. The session
manager (`ctx+0x22f0`) survives the reload; the other player's presence does
not. There is no warp in the game that does not reload.

### Next: not letting the death become a death

Every measured source of death converges on one byte, `*(chr+0xb8)+0x759`, and
the local player consumes it in `FUN_14013c720`, slot `+0x20` of
`ChrDeadActionCtrl` (measured on 13/09; the `+0x10` the external review pointed
at never runs for it). Intercepting there — cancelling the death, restoring the
HP, putting the character 1.1 m in front of the bonfire **with no warp** and
applying the consequences by hand — touches no session at all, and is the same
hook on the host and on the guest.

In order, each step with its positive signal. The first four are solo, with no
session, and cost no illegal disconnect:

1. **Teleport with no warp.** — **done on 13/09.** The authoritative position
   is the `hkpRigidBody`'s (`*(*(ChrPhysicsCtrl+0x320)+0x20)+0x1a0`), and the
   teleport that worked writes the translation, the swept transform and the
   game's own copies in a single request: Samuel went from Heide's bonfire to
   the Cathedral of Blue (69 m) with no load, standing and controllable.
   Details in DS2_SEAMLESS_COOP.md. Note: `where` does **not** follow a
   teleport; the live position is `PlayerCtrl+0x90`.
2. **Live bonfire coordinates.** — **done on 13/09.** The list
   `*(*(ctx+0x70)+0x58)` has the loaded map's bonfires (3 in Heide); each
   one's id is `**(*(*(obj+0xb8)+0x20)+0xe0)`, and the node with the record's
   id (`0x7ba7`) is the one whose spawn point is 0.000 m from where the game
   put Samuel. The full recipe is in DS2_SEAMLESS_COOP.md.
3. **Intercept the death.** — **done on 13/09.** `DS2_DeathInterceptHook`
   detours `FUN_14013c720` for the local character only; `DS2_Death.req`
   chooses `observe` (the default) or `cancel`. With the HP zeroed and
   `cancel`, the death does not happen: HP back on the same frame, the
   controller in state 0, no warp, and the character walked 2.5 m. The first
   death let through on the same connection printed
   `First ... RequestNotifyDeath`, the control proving that the 2956 cancelled
   before it never reached the server. Details in DS2_SEAMLESS_COOP.md, "Death
   measured, and held".
4. **HP.** — **done together with 3.** `PlayerCtrl+0x168` current, `+0x16c`
   minimum (-99999), `+0x170` base maximum, **`+0x174` effective maximum**
   (already with the hollow discount: 915 → 869 → 823 over two deaths). The
   cancellation gives `+0x174` back; without giving it back, `FUN_14016a650`
   sets the byte again on the next frame.
5. **Put it together solo.** — **done on 13/09.** The `respawn` mode refuses
   any death of the local player and charges for it with the game's own
   functions, with no reload: souls to a bloodstain at the place of death (the
   old one removed), hollowing with the appearance and the maximum HP, Estus
   refilled, and the character standing at the spawn of the record's bonfire
   with full HP. A fall goes through the same path and leaves the bloodstain at
   the edge. Measured with zeroed HP, a fall into the void, two deaths in a row
   and a human character; no `RequestNotifyDeath` and no warp. Details in
   DS2_SEAMLESS_COOP.md, "Respawning and paying for the death".
6. **With a session.** — **done on 14/09.** Chico summoned into Samuel's
   world, both in `respawn`. The phantom's death and the host's, by HP and by
   falling, respawn at the bonfire with no load and the session continues:
   60 s later no `RequestNotifyLeaveGuestPlayer`, the host at `0x10`, the guest
   at 7, and each one **sees** the other at the bonfire. The first
   `RequestNotifyDeath` on Chico's connection only went out on the ordinary
   death used to finish, after seven refused ones. The bill follows the game's
   own checks for who pays: the phantom keeps its souls and does not hollow
   (the obfuscated check `0x14016f7d0` is that exemption), and the death
   counter goes up for both. It cost one discovery: the phantom's HP 0 reached
   the host once before being given back, and its copy died there ("Phantom
   Chico has been vanquished"); the hook now also refuses the pending death of
   a player's copy. "YOU DIED" appears and the HUD comes back. Details in
   DS2_SEAMLESS_COOP.md, "With a session".
7. **Shared bonfire.** — **done on 14/09.** The game does not record a bonfire
   for whoever is in someone else's world (`FUN_1401caf50` and `FUN_1401cb950`
   only record for the local character when slot `+0x58` of the context says it
   is not in someone else's world). The channel did not need the server: the
   game only uses channel 0 of Steam's P2P session, and `DS2_CoopChannelHook`
   uses 7. The session host (the `+0xad` mark the game sets on the member that
   owns the lobby) announces `{mapa, tipo, id}` every 2 s, and a guest's death
   respawns at the announced bonfire. Measured with Chico's record at `0x7ba2`
   and Samuel's at `0x7ba7`: zeroed HP 69 m away and a fall into the sea took
   Chico to Samuel's bonfire, with the switch off he went to his own, the
   host's death followed the host's record, the session stayed at `0x10`/7 and,
   on the way out, Chico went home to his own `0x7ba2`. Details in
   DS2_SEAMLESS_COOP.md, "The host's bonfire".
8. **A bonfire outside the loaded map.** — **done on 14/09**, in the shape the
   project's owner chose: the last bonfire, with a load. Every load in the game
   goes through a warp, but the loading by parts, frame by frame, does not.
   `DS2_BackreadHook` forces the owner of the bonfire's map
   (`MapAreaCtrlOwner+0x1e9`) with the parts and hands the streamer
   (`FUN_1403dc8e0`) the bonfire's navigation cell in place of the player's,
   which is where the search for the parts starts; with no cell, the map
   arrives with no ground. The respawn holds the character at its last position
   on the ground, waits for state 5 (500 ms), takes it to the bonfire and lets
   go when the streamer sees it standing on the new map. In a session, the map
   under the other player's copy stays loaded with the parts around it; in the
   first session, before that, Chico's game closed right after respawning, and
   the cause was not read. The reverse direction found a defect in the teleport:
   it did not move the position the fall controller measures the landing from,
   and a respawn 24.5 m below the death charged two deaths. Measured solo and in
   a session, in both directions between Heide and Majula: one charge per death,
   with no warp, the session at `0x10`/7 more than 60 s after each death, the
   other player still on each one's screen, and a legal exit. Details in
   DS2_SEAMLESS_COOP.md, "A bonfire on another map".

---

### The history of the approach that did not close

Started on 12/09. The problem is defined precisely now, and the missing piece
has a name.

**The state after M1:** the guest stays **dead where it fell**, inside the
host's session. Both HUDs keep listing the other. Nothing picks it up.

Three things measured on the way here:

- **the end-of-session request is a one-shot.** Refused, it is not repeated:
  writing `clear` afterwards does not make the game try again, and the guest
  stays dead and in the session. That is, refusing does not postpone the
  teardown, it cancels it.
- **no warp is emitted**, so there is no request to rewrite — the approach of
  swapping the destination, which solved the trip home, has nothing to grab
  here.
- **`jogador+0x64` is the archetype**, the same number the server calls
  `archetype`. Found by diffing the host's struct against the guest's, and now
  published by `DS2_NavHook`; `ds2os-dev where` shows it as `papel`.

**What is missing is a revive**, and it is a primitive the game has to have:
the **Ring of Life Protection** resurrects you standing, with full health,
**without reloading the area** — it is the only revive in DS2 that does not go
through a load. Finding what that ring fires is the shortest path to M2.

### What the sweep found, and what it did not

A breakpoint sweep over the 385 functions in `0x140185000`–`0x140195000`, with
the game idle first and then an ordinary death: more than 130 fired. The whole
range is the character subsystem and a death touches nearly all of it, so the
list itself points at nothing. The **callers** do: `+0x44e9c8`, `+0x44ee7c`,
`+0x44eec6`, `+0x44eee7`, `+0x44ef6e`, `+0x44ef80` — all glued to the warp
request's constructor.

That establishes that `0x44e9b0`–`0x44fde0` is the **player state manager**,
and that it operates on the same `*(contexto+0x70)` the respawn already uses:

| function | what it does |
| --- | --- |
| `FUN_14044e9b0(obj, x)` | passes `x` on to seven sub-objects |
| `FUN_14044ed40(obj, saida)` | builds the warp request from the record |
| `FUN_14044ee60(obj)` | **resets** ten sub-objects (`+0x40`…`+0xb8`) and clears `*(obj+0x10)+0x21` |
| `FUN_14044fde0(obj)` | respawn at the last bonfire |

`FUN_14044ee60` is called at `0x1401bf730` as `mov rcx,[rbx+0x70]; call`, which
confirms the object — but the surrounding code is **teardown**, a queue of "if
the pointer exists, reset it", not resurrection.

**The revive was not found.** The evidence piles up in the direction that DS2
does not have one, outside the Ring of Life Protection path: getting up is
baked into the area load that comes after a death.

### The hypothesis M1 opened, and which is the next test

If the revive comes together with the area load, the question stops being "how
to resurrect standing" and becomes:

> **does the session survive an area load by the guest?**

M1 proved that the session object survives a death. Whether it survives a load
is still unknown. It can be measured without writing a line of code: with a
session up and the guest **alive**, have it use a Homeward Bone — which is a
voluntary area load — and see whether the session is still listed on both HUDs.
If it survives, M2 becomes "let the death follow the normal path and re-enter
state 2", and needs no revive at all.

### The area load test: the game does not even let you try

Measured on 12/09, with a co-op session up and the guest **alive** in the
host's world: opening its inventory and picking the Homeward Bone shows the
action menu with **"Use" greyed out**. A summoned phantom cannot use it.

And it is not just the guest. **The host cannot either**: with the same session
up, Samuel's inventory shows "Use" greyed out on the Homeward Bone in exactly
the same way. The rule is not about being a phantom — it is about **there being
a session**. While a group exists, neither side loads an area of its own
accord.

That is stronger than the assumption that was in the design, and more useful:
the door to open is not "let the phantom use items", it is **"allow an area
load with a live session"**, a single switch, which holds for both sides.

The most obvious path to it has also been ruled out. The warp entry refuses
reasons outside 1 and 4 by consulting `FUN_140248940`, which returns the
inverse of a virtual in slot `+0x1b0` of `*(contexto+0xd0)`. That virtual is
`0x140314440`, four instructions:

    xor eax,eax ; cmp dword [rcx+0x168],eax ; setg al ; ret

that is, `*(mgr+0x168) > 0`. Measured on both clients with the session up:
**777 on the host and 811 on the guest**, both positive and growing — it is a
time counter, not a permission. The warp's gate lets reason 5 through on both
sides; what blocks it is the menu layer, before the warp ever gets requested.

**What that does to M2.** The two ways out become one:

1. ~~let the death follow the normal path and re-enter state 2~~ — depends on a
   load the guest is not allowed to do;
2. **resurrect standing, with no load**, which is the only form left — and it
   still needs the revive primitive that has not shown up.

That is: M2 requires creating a capability the game removes from the guest on
purpose. It is not tuning a parameter, it is opening a closed door.

### Two attempts, and what each one proved

**Flag 1 (the shape of the entry): refused.** The hook built the right request
and the warp said no. Both preconditions were good at the moment of death —
`ctx+0x24ac = 0x1e`, `ctx+0x24b1 = 0x40`, bit 2 clear — so what refused was the
gate `FUN_140248940`.

**Flag 0 (the shape of the teardown): accepted, and in the wrong place.** The
warp went through (`aceito=1`), **no end-of-session request ever existed** —
the session log stayed empty on both sides —, the guest came back **alive and
standing**, and the session stayed at **state 7 on both clients** (the state 7
handler carries on running with the same pointer). The host kept listing
"Chico" on its HUD.

But the guest ended up in **its own world**: I moved the host and it saw
nothing, and the map and position I sent in the request were ignored.

That corrects an inference of mine: **the flag is not just the gate's key, it
chooses the meaning of the destination.** With it on the request is "go to this
place in this map"; with it off it is "go home", and the request's destination
is not looked at. Only the shape of the entry carries a destination — and only
the gate blocks it.

The balance: we went from "dead and stuck" (M1) to "alive, session objects
intact on both sides, wrong world". One thing is missing, and it is small: the
gate is `*(mgr+0x168) > 0`, with `mgr = *(contexto+0xd0)`. Measured positive in
the middle of the session (777 and 811) and evidently **not positive at the
moment of death** — which is the only moment that matters.

### The third attempt: the entry's flag is accepted — and the host freezes

Staged again with the hook at `flag 1` and the gate raised if needed. What
fired was not the guest's death, it was the **host's death** — which on the
guest's client goes through the same terminal, `FUN_140190950`. The log:

    morte de fantasma: papel=1 flag=1 [+0x24ac]=1e [+0x24b1]=40 portao=811 aceito=1

Three things at once:

- **the shape of the entry is accepted.** With the gate open — 811 at that
  moment, and it did not even need raising — the reason 4 warp with flag 1 goes
  through. The refusal in the first attempt really was the gate, not the shape;
- **no end-of-session request existed** on either side;
- the guest stayed **alive, standing and in place**, instead of being sent
  back.

**But the host froze.** Its screen stopped on the frame of its own death and
did not change another pixel; the HUD kept listing "Chico". The process is
alive — the position publisher keeps counting, and its position is already the
bonfire's — that is, **the logic ran and the presentation stopped**.

The explanation that fits everything known: the host dies, asks for the end of
the session and waits for the guest to leave, which the hook replaced and never
sent. It is the risk the external review marked as number one, only from the
opposite side to the one predicted — it is not the phantom's corpse that does
not get up, it is the host hanging on a goodbye that never comes.

**What that demands of M2:** the intervention cannot be only on the side of
whoever dies. Either the host has to be told by another route, or the hook has
to tell "the guest died" from "the host died" apart — in the second case there
is nothing to save, the session ends anyway and replacing the goodbye only
freezes both.

### The measurement in the right case, and the wall

With the guest dying (reason 2, M2's case), the hook at flag 1 and the gate
raised:

    morte de fantasma: papel=1 mapa=0a1f0000 sabor=1 destino=6.19,-18.52,209.05
    flag=1 [+0x24ac]=1e [+0x24b1]=40 portao=0 (erguido) aceito=1

- **`portao=0`** — the counter is **zeroed at the moment of the guest's death**,
  and positive (811) at the host's death. That is exactly why the first attempt
  with flag 1 was refused and the third was not. The gate is the
  `*(mgr+0x168)` of `*(contexto+0xd0)`, raised by a call and put back.
- **`aceito=1`** with the shape of the entry.
- **No end-of-session request**, on either side.
- The guest stayed **alive and standing**, and the host kept listing "Chico".

**But it is not in the host's world.** I moved the host and the guest's screen
did not change; it has its own bloodstain at its feet. Reproducible: it
happened the same way in both measurements that got this far.

**The wall, said precisely:** the state 2 request works *because the state
machine emits it while transitioning into the host's world*, with all the peer
preparation around it. Re-emitting only the warp reproduces **the movement**,
not the **entry**. The warp moves the player inside the world it is already in;
what decides which world it is in is the session, not the request's
destination.

That is: M2 is not a warp request, it is a **re-entry into state 2** — and that
state's handler receives a `param_2` that comes from the network, with map,
position and orientation. Without that payload (or without synthesising it),
there is no entry.

Where **not** to look, already checked: the first `0x200` bytes of the player
object have no HP and no death flag. A live-versus-dead diff there only shows
name, archetype and position, and a 2 KB sweep found no pair of equal integers
that looks like (current health, maximum health). The HP lives behind some
sub-object.

### The original plan, for reference

Depends on M1. The guest dies, loses its souls, leaves the bloodstain where it
died and reappears at the bonfire **of the host's world**, still in the
session, and walks back.

Open: which bonfire. The guest's respawn record (`*(contexto+0x70)`, fields
`+0x164` map / `+0x168` type / `+0x16c` point) points at **its own** bonfire.
To respawn in the host's world it is necessary either to swap that record while
it is in the session, or to build the request with the host's map and point.

---

## M3 — join once, with no ritual — **DONE on 15/09**

**Done:** the effigy and Soul Memory no longer block joining through a white
sign, and the join already happens with no soapstone and no touch when both
sides get the order (see "No effigy" and "Joining with no soapstone" in
[DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md)).

**Missing, for a join with no ritual for real:**

1. ~~**Who sends the order.**~~ **Done 15/09**: `DS2PartyGuest` (the guest puts
   the sign down on arrival and puts it back on its return, with a 30 s grace
   period when it disappears in its own world) and `DS2PartyAccept` (steam ids
   whose signs the host summons by itself); `ds2os-dev up --seamless --party`.
   The join, `session end` and an automatic rejoin measured with no key press
   and no file.
2. ~~**The password.**~~ **Done 15/09**: `DS2PartyPassword` becomes the sign's
   and the poll's `name_engraved_ring` (bit 31 set); the server only matches
   party with party of the same code, before type and Soul Memory. A player
   with a password who is not a guest summons the white signs that arrive.
   Joining by password alone, different passwords, a public host against a sign
   with a password and a host with a password against a public sign were all
   measured.
3. ~~**Far from each other.**~~ **Done 15/09**: the server offers a poll with a
   password the signs of the same code from any area, with the position
   rewritten to where the host is; the host summons; the guest lands at a wrong
   point of the host's map (its own sign converted, in empty space) and the
   death hook takes it to the host's bonfire on arrival, before it falls and in
   any death mode: an owner role for a phantom, the host announcing from a map
   different from the one it came from, nothing under its feet. Measured twice
   from Majula to Heide in `observe` (a teleport 10 s after the summon,
   `p2pSessionVerified: true`, no death), with the control in the same map not
   firing.
4. ~~**One rematch and one join in the same hook.**~~ **Done 15/09**: the join
   is entirely `DS2_PartyHook`'s (its own detour of `AddSign`), measured with
   `DS2AutoRematch` off.

Measured before touching anything: **a hollow guest does place a white sign**
(`Sign created: type 1` with state 1) — the block was only on the **host**,
which when hollow received the sign from the server and did not get the "Touch
Summon Sign" prompt. `CLAUDE.md` said that a hollow character does not place a
white sign; the 12/09 block that produced that was the illegal disconnect.

Leftovers of the effigy and Soul Memory, not measured:

- `FUN_1402a1bf0` refuses the **use of an item** of some types when hollow
  (the table `0x1410d64f4` with 1): probably the orb and the red soapstone. Not
  touched — PvP is not a co-op join;
- the Small White Sign Soapstone only had its configuration opened, not a sign
  measured;
- the VPS still has Soul Memory on: the config lives in `Saved/`, outside git,
  and the code's default has not changed.

Take out of the join everything that is ceremony today:

- **no soapstone**: a way of joining by password, not by a sign on the ground.
  The sign's path is already located — `FUN_14029ec00`, the job in `param_1`,
  the constructor above it at `+0x286239` — and serves until joining by
  password exists;
- **no effigy**: today the red sign is refused when hollow and the white one is
  not; neither of them should block joining;
- **no Soul Memory**: the server already has the per-item parameters
  (`DS2_WhiteSoapstoneMatchingParameters` and company in
  `Source/Server/Config/RuntimeConfig.h`). Opening it is configuration, not
  code. See [DS2_SOUL_MEMORY_MATCHMAKING.md](DS2_SOUL_MEMORY_MATCHMAKING.md).

---

## M4 — authoritative world state — **taken up again on 19/09** (see [DS2_WORLD_STATE.md](DS2_WORLD_STATE.md))

Doors, levers, elevators, shortcuts, illusory walls and Pharros mechanisms
opened by the host show up open for whoever joined. The closest neighbouring
work is [DS2_FOG_GATES.md](DS2_FOG_GATES.md), which has already found the class
and the per-frame test of the area barriers.

**Measured 15/09** ([DS2_WORLD_STATE.md](DS2_WORLD_STATE.md)): the game already
does half of the flags. The guest receives **all** of the host's event flags on
joining (map and global ones, proven with bits set only in the host's memory)
in place of its own (a bit only the guest has disappears in the session and
comes back at home), does not take them home, and in a session the host
propagates every change through the P2P packet `0x20` (`FUN_140474a60` →
`FUN_14051e6b0`), while the guest can only change the map ones
(`FUN_14025cdb0`).

The copy made at the join is a **snapshot** that the host exports and the guest
imports in the warp (`FUN_1402bf8f0` → `FUN_1402c2fa0`, measured), and besides
the flags it carries `EventValueManager`, `EventBonfireManager`,
`MapStateActManager` (map object state) and `EnemyGeneratorDeadCounter`
(despawn). `ds2os-dev flags` reads and compares the two accounts' flags.

**Measured 19/09** — and this one is travel's doing, not the game's. A map's
three flag categories live in a small arena that only a **warp** ever gives
back (`FUN_14044f7a0`; the teardown's own notification, `FUN_1404746a0`, is a
bare `ret`). Bonfire travel changes the set of loaded maps without a warp, so
after twelve maps the guest's table held 36 nodes over 9 slots — Majula, Heide
and Brume writing the same 25 bytes — and the host was standing in a map with
no category at all, where `FUN_1404750b0` drops every flag write and nothing
goes out on the `0x20`. Fixed in `DS2_BackreadHook`: a map that finishes
unloading gets the release the warp would have given it.

**Missing:**

0. ~~**The guest's flags for a map loaded after joining.**~~ Measured and
   fixed 19/09. Copy 1 of the arena is filled only by the snapshot at the
   entry warp and holds three maps, so a map reached by travelling started at
   **zero** — at Heide the host had `131000022` and `131000086` and the guest
   had nothing, not even his own save's bits. The host now publishes each
   loaded map's three blocks and the guest writes them in as the map
   registers (`kKindMapFlags` on the co-op channel); a leg to Brume Tower read
   identical on both sides afterwards, including the two bits only the host
   had.
1. ~~**Operate a real mechanism** and see whether its state is a flag.~~ Done
   19/09 on a lever in Heide, pulled by hand with the trace armed. It is a
   flag: the lever went from state 10 to **30 on both machines**, and
   `105400` was set through the emevd dispatcher on each, the host hitting
   `25cec0` to send it and the guest `25ce10` to receive it. The guest never
   sent, which is right — `105400` is a global and the filter refuses a
   non-host those.
2. **Whatever is not a flag** (`MapObjStateActComponent` keeps per-object
   state): find where the guest receives it, or does not receive it.
   **Narrowed 19/09 to one object with a position.** With the map flags now
   carried and byte-identical, a census of all 399 state-act components on
   both machines after a leg to Brume Tower disagreed on exactly one, at
   `(-167.7, -5.2, 436.3)`: state 20 on the host, 10 on the guest. So its
   state is not a flag, and nothing carries it after the entry warp. The game
   applies such state with `FUN_1401f30e0` on the `MapStateActManager`
   (`*(ctx+0x38)+0x1f8`), which is the shape the fix should take — the same
   publish-and-seed the flags now use.
3. **A mechanism operated by the guest** in the host's world: the map flag goes
   through the filter; the object still has to be checked.
   **Half done 19/09.** The guest got no prompt at all — at a lever in Heide
   the host read `A: Pull` and the phantom 0.6 m away read nothing. It is the
   unmodded game's rule and it is authored per object: the prompt's 24-bit
   role mask comes from the object's own row, and `row[0x1a]` bit 0 means "a
   white phantom may use this action". Every bonfire has it set (which is why
   a guest can rest); the lever has it clear. `DS2_PhantomActionHook` sets it
   for every map-object action with one immediate at `+0x453b47`, and the
   phantom now reads `A: Pull`. The same bit, from the fixed mask
   `FUN_140453b80` writes for the guides the event scripts create, is what
   kept a phantom from **talking to an NPC**; widened at `+0x453b87`. Played
   by hand on 20/09: in the host's world the guest talked to the Emerald
   Herald and took Vigor from 4 to 5, and the level was still there back in
   his own world. Pulled by hand with the patch in, the lever
   went from state 10 to **30 on both machines**, and the trace caught the
   flag `105400` being set through the emevd dispatcher on each and crossing
   on the game's own `0x20`. What is still open is a mechanism with no flag
   behind it, which is the same hole as item 2.

---

## M5 — individual loot

Each player opens their own chest and gets their own item. Not investigated.
An honest prerequisite: understanding how the game decides that a pickup has
already happened, and whether that decision is local or comes from the host.

---

## M6 — boss reward for everyone

Souls, the boss soul and an item for each participant, without the phantom's
reduction. Not investigated.

---

## M7 — what was done together goes into both saves — **mechanism done on 15/09**

**Done 15/09**: `DS2_ProgressCarryHook` (with `--seamless`). The guest watches
the P2P packet `0x20` (`FUN_14025ce10`), keeps the **global** flags the game
applied coming from the host (the ones `FUN_14025cdb0` only accepts from the
host) in `DS2_Carry.pending`, and on returning to its own world (role 0, the
manager without the someone-else's-world mark, 5 s standing still) writes them
through the game's setter (`FUN_140474a60`). Map flags stay in the host's
world. The host's earlier progress arrives in the join snapshot, not in the
`0x20`, and that is why it does not cross. `DS2_Carry.req` accepts
`flag <id> <0|1>`, `le <id>`, `status`, `limpa`.

Measured with Samuel hosting and Chico as the guest in Heide:

    10:46:15.500  Samuel  flag 109999 <- 1 pelo setter do jogo: antes 0, depois 1
    10:46:15.500  Samuel  flag 131000199 <- 1 pelo setter do jogo: antes 0, depois 1
    10:46:15.520  Chico   recebida flag 109999 = 1 (papel 1): guardada para o meu mundo
    10:46:15.520  Chico   recebida flag 131000199 = 1 (papel 1): flag de mapa: fica no mundo do host
    (session end)
    10:47:16.854  Chico   no meu mundo: flag 109999 <- 1 (antes 0, depois 1)

`flags` with Chico at home: `109999` on, `131000199` off. After restarting
Chico's process (a new boot), `109999` was still on: it is in the save. Both
flags were turned back off in both saves and checked after quitting to the
title and coming back in.

**Missing:**

1. **A real boss flag.** That a boss's death is a global flag is a hypothesis;
   no boss has been killed in a session. It needs a fight.
2. **What else is global and should not cross**: if picking up an item in the
   host's world is a global flag, the guest loses the item in its own world
   (which goes against M5); the same holds for NPC state (M10). With no
   measurement, the hook passes everything that is global.
3. The flags `100100` and `100110`, which the host sets and sends when loading
   a map (`FUN_1404747c0`), cross too; they look harmless, they have not been
   looked at.

Read for items 1 and 2, among the setter's callers: `FUN_14040fdb0` sets, on a
character's death, the flag at `+4` of its parameter (a candidate for "boss
killed"), and `FUN_1401826d0` sets two flags from a record (`+0x10`, `+0x14`)
and adds an event value (`+0x18`), which looks like treasure picked up. Both
only run on whoever is **not** a guest, so on the host, and they go out through
the `0x20`. The flags' category comes from the parameters and has not been
read.

Bosses killed and quests done together carry over to the save of whoever
joined; the host's **earlier** progress does not. This is the second half of
the problem and probably the biggest job after M1: it requires the guest's
client to write flags from a world that is not its own into its own save.

Start with a boss, which is a single flag and easy to check, before any quest.

---

## M8 — bonfire and travel — **done on 15/09, hardened through 19/09**

What is still missing is listed at the end of this section, "What is still open
 in M8 (19/09)".

**Done 15/09**: `DS2_BonfireInSessionHook` (with `--seamless`). The world's
owner rests at the bonfire with phantoms in the world, and the session stays.
In a session the game refused in **three** places, all of them asking
`FUN_14025f690` ("a multiplayer session is up", state 1 or 2):

1. `FUN_1401cb950`, the rest: answer yes → message `0x453`, the "Cannot use
   bonfire" box (`0x453` is a text id, not an event; the summon failure table
   `FUN_140212730` uses ids of the same kind);
2. `FUN_14017ed90`, the rest job in state 2: `FUN_14025ea40` other than 0 →
   cancels the menu (`FUN_1401994e0`), and the character stands up;
3. `FUN_140199a70`, the menu queue in state 10 (the bonfire menu): answer yes →
   cancels the menu.

1 and 3 answer "no" through a detour of `FUN_14025f690` that looks at the
return address (`+0x1cb9d9`, `+0x199c2e`) and only when the local player owns
the world; 2 is a byte patch (`74 19` → `eb 19` at `+0x17ee9d`), reachable only
with a rest already started.

Found piece by piece, each one measured: breakpoints on the branches of
`FUN_1401cb950` (with a session it went straight to the message; without one,
it started the job), the probe reading of the queue (`+0x54` of
`*(ctx+0x70)+0x50`: 10 with the menu open alone, 0 in a session) and a write
watch on that field. With the three swapped in memory and then with the build
`2dc983c9`: Samuel hosting sat down, the "Heide's Ruin" menu opened with
Chico's phantom beside him, `p2pSessionVerified: true` before, during and a
minute after closing the menu. The phantom does not get the option to rest (its
prompts are "Light torch" and "Pick up item").

On entering state 2 the job runs `FUN_14017fd70` (`FUN_140417210`,
`FUN_1403c1b50`, `FUN_14044f880`), the candidate for the reset of the host's
world.

**Reset and notice on the guest — done 15/09.** Measured before, with Chico
hosting and Samuel as a phantom in Heide: an enemy (550 HP, at (-55.9, -8.0,
260.0)) killed on the host disappeared on the guest too — the death replicates
—, but after the rest it **came back only on the host**; on the guest it stayed
dead. The game's rest tells the session nothing.

Now `DS2_BonfireInSessionHook` speaks through its own P2P channel
(`DS2_CoopChannel::SendHostEvent`, announcement types 2 and 3):
`FUN_14017dc40` (the rest has started) sends `RestStarted`, and the guest shows
the box **"A player is resting at a bonfire."** with the game's own network
message function (`FUN_1404fe2a0` in `*(ctx+0x22e0)`, title
`FUN_140503620(0, 0xcc)`); `FUN_14017fd70` (the reset: enemy generators, map
objects, events) sends `WorldReset`, and the guest runs the **same**
`FUN_14017fd70` on its copy of the host's world, on the game's thread. Measured
with the build `502fb6d0`:

    14:29:54.978  Chico   host: descanso na fogueira 00007ba7; aviso para a sessao
    14:29:54.992  Samuel  convidado: o host descansou (...ha 6 ms); aviso mostrado
    14:29:57.861  Chico   host: o mundo foi reiniciado pelo descanso
    14:29:57.876  Samuel  convidado: mundo do host reiniciado aqui tambem (ha 7 ms)

The enemy killed again before the rest was back, 550/550 at the same position,
on **both** clients; the box appeared on Samuel's screen and closed with A;
`p2pSessionVerified: true` the whole time.

**Travel with a vote — done 15/09.** The host picks the bonfire from the travel
list; `DS2_BonfireInSessionHook` holds the choice **in the list**
(`FeGroupTestBonfireTransitionList` slot `+0x80`, `FUN_1400d5170`) and sends
`TravelVote` through the channel; each guest gets the game's yes/no box
(`FUN_1404fe1c0`, the same one as `FeSubStateCommonWindow`) with "The host
wants to travel to another bonfire. Travel together?", and the answer goes back
to the host (a type `0x20` announcement).

- **No**, or no answer in 30 s: the host gets "Travel canceled: a player
  declined." (or "...not every player answered.") and stays in the list,
  standing, with its respawn record intact.
- **All yes**: `TravelLeave`; each guest leaves the session through the path
  the host's travel already used — `+0x120 = 1` in
  `NetSummonJoinMultiplayCtrl`, which state 7 (`FUN_1402c3830`) turns into an
  end of session with reason 3 —, the host waits for the group to leave (1.5 s
  with no members), the choice goes ahead, and the party brings everyone
  together at the new bonfire with M3's arrival from another map.

Measured with the build `3235c402` (Chico hosting, Samuel as the guest):

    16:28:24.347  Chico   escolha de fogueira segurada na lista; votacao 2
    16:28:24.361  Samuel  votacao 2 aberta (caixa 10)
    16:28:28.445  Samuel  votacao 2 respondida sim
    16:28:28.479  Samuel  saio da sessao para o host viajar (+0x120 0 -> 1)
    16:28:30.328  Chico   convidados fora da sessao em 366 ms; a escolha segue
    16:29:47.974  Samuel  chegada de outro mapa: levando para fogueira do host (The Far Fire)
    16:30:02      p2pSessionVerified: true, os dois em Majula, sem morte

A decline was measured at 16:31:16 (an answer in 6 s) and no answer at 16:27:34.

Three attempts that did **not** work, so as not to repeat them: holding the
warp (`FUN_1401c2a80` reason 2) — the loading transition had already started
and the host sat there with no menu; holding phase 1 of the travel
(`FUN_140184a10`, `*(*(ctx+0x70)+0x70)+0x40`) — the character was already in
the travel animation and froze in it; and the host travelling with the phantom
still in the world — the host's game **closed twice** (`c0000005` at
`+0x3f510f`, `FUN_1403f4f60`, a character already released inside the
`CharacterManager`), and one of those times it was Chico's, which lost 10
disconnect points (40 → 50).

**Leaving through the travel does not count as an illegal disconnect (measured,
with a control).** The counter is at `*(*(*0x141616cf8+0x30)+0x68)+0x1c0`
(`MultiPlayPenaltyCtrl`, vftable `0x1410d0f00`): `+0x08` armed, `+0x0a` points,
`+0x0c` punishment left; in the save at `+0x488` bit 0, `+0x47a` and `+0x47c`
(`FUN_14024fe40`). Joining a session arms it; a legal end disarms it without
adding; a client that disappears while armed adds `+0x1c`/`+0x20` of the
parameter (10), with a block at `+0x22` (100) and a 36000 s punishment. In the
four exits through travel that were measured (13:53, 15:23, 15:33 and 16:28)
the guest went from armed 1 to 0 with the points unchanged (Samuel 10, Chico
40/50); the **control** was the host's crash at 13:53, which cost Samuel
0 → 10. The old points are from earlier disconnects.

**The guest rests, and the travel can be anyone's — done 15/09.** Decided with
the user: the host's rest does **not** heal the guest (measured: 726/854 before
and after); whoever wants the healing sits at the bonfire. The guest's rest
resets the world **for everyone**; and a proposal for a bonfire the host has
not lit is cancelled with a notice.

The guest never saw "Rest at bonfire" because of **two** blocks:

1. `FUN_140453ce0`, the event action entries (the prompt list, which
   `FUN_1404554e0` feeds and `FUN_140455f80` shows): an entry of type 13 or 14
   (the bonfire; texts `0x6d`/`0x6e` through the table `0x1410ef2a0`) is
   discarded, even before the distance check, when slot `+0x58` of the context
   (`FUN_1405135f0`) says the player is in someone else's world. Found with a
   breakpoint at the entry: on Samuel as a phantom, at the bonfire, the type
   `0x0e` entry passed the `+0xa0` mask and died there. With the `jne` at
   `+0x453dd8` swapped for a `nop` the prompt appeared immediately. The hook
   opens that block only while the local player is a white phantom (an invader
   still gets nothing), with expected bytes both ways;
2. the script's query 130602 (`FUN_140513440`, "in a session as a guest"),
   answered "no" for a white phantom within 3 m of a loaded bonfire. Whether it
   is still necessary with block 1 open was not isolated; it stays.

The guest's rest does not reset its own copy: it sends `GuestEvent RestStarted`
to the host, which shows "A player is resting at a bonfire.", relays the notice
to the other guests (except whoever rested) and runs its own reset, which
reaches everyone as `WorldReset`.

The travel: a guest that picks a bonfire from its list (the list shows the
bonfires of the host's world) is held on the choice and sends `TravelPropose`.
The host refuses if there is already a vote (`Busy`) or if it has not lit that
bonfire (`NotLit`, "Travel canceled: the host has not lit %ls."); otherwise it
opens the vote with the guest already counted as a yes. The boxes now name the
destination, with the bonfire's name (text category `0x12`) and the area
(category 5): "The host wants to travel to The Far Fire (Majula). Travel
together?" or "A player wants to travel to ...". Once approved, the leaving and
the travel are the host's; the host travels through the game's functions
(`FUN_1401843b0` + `FUN_140184830` + `FUN_14044fe30`).

Measured with the builds `44e4f654` (block 1 by probe) and `618742e8` (in the
hook), Chico hosting and Samuel as the guest at The Far Fire:

    18:11:13.856  Samuel  descanso na fogueira 122a no mundo do host; aviso ao host
    18:11:13.870  Chico   o convidado descansou na fogueira 122a; aviso a todos e reinicio o mundo
    18:11:13.887  Samuel  mundo do host reiniciado aqui tambem (pedido ha 0 ms)
    (Samuel HP 400/915 -> 914 ao sentar; de novo com 618742e8 às 18:26:48)
    18:13:23.948  Samuel  proponho viajar para The Far Fire (Majula)
    18:13:23.961  Chico   votacao 1 ... proposta por um convidado; caixa aberta
    18:13:51.596  Chico   votacao 1 recusada; Samuel: "Travel canceled: a player declined."
    18:14:39.668  Samuel  proponho de novo; 18:14:55.099 Chico responde sim
    18:14:55.132  Samuel  saio da sessao para o host viajar (+0x120 0 -> 1)
    18:14:57.000  Chico   convidados fora em 383 ms; viagem iniciada para a fogueira 122a

After the travel: the penalty disarmed on both with the points unchanged
(Samuel 10, Chico 50), the party came together again and
`p2pSessionVerified: true` with Samuel at Chico's The Far Fire. The named box
of the host's vote appeared on Samuel at 18:28:26. With the build `74247cca`,
a guest that leaves the question unanswered also gets the reason when it
closes: "Travel canceled: not every player answered." on both at 18:39:34.

**Group travel, without leaving the session — done on 15/09.** The first
shape — the guests left the session, the host travelled with the game's travel
and the party brought everyone together again — **is not travelling together**:
measured in real use, the guest went back to its own world with "Summoning
canceled." and only reappeared in the host's world about 75 s later (19:39:47
it left, 19:41:06 it came back; three travels in a row the same). It is the
same workaround M2 had already rejected.

Now nobody leaves: each machine takes its own player to the bonfire through
M2's step 8 path — `DS2_DeathIntercept::GoToBonfire`, which holds the
destination map beside the current one (`DS2_BackreadHook`), waits for state 5,
focuses on the bonfire's cell and teleports, with no warp at all. The host
writes its own respawn record at the new bonfire (`FUN_1401843b0` +
`FUN_14044fe30`). The travel takes ~2.5 s per player and the session stays
verified the whole time.

Three things were learned by the games closing (four crashes, two of them
closing both games: 40 disconnect points on Samuel, from 10 to 50, and 10 on
Chico, from 50 to 60):

1. **Closing the bonfire menu by a direct call kills the guest.** Calling
   `FUN_1401994e0` from the hook's tick closed the menu on the host and
   brought the guest down twice, in the same millisecond as the call
   (`c0000005` writing to 0 inside the menu's teardown). What the game does is
   something else: the rest job cancels the menu when there is a session. So
   the patch `+0x17ee9d` comes out for an instant and the **game** closes the
   menu — measured live before it became code: the menu closed in under 1 s,
   the character standing, the session verified;
2. **Releasing the other player's map during the travel kills whoever is
   travelling.** An attempt to save memory stopped holding the map under the
   other player's copy while the travel loaded; the guest closed inside the
   `CharacterManager` (`+0x3f4fac`, a character already released) — the same
   crash the travel with a warp used to give. The other player's map is still
   kept;
3. **The two machines cannot bring the map in at the same time.** When host and
   guest loaded together, both games closed (once in the allocator, once on the
   frame the old map was released). Now the **host travels first** and only
   calls the guests after it has arrived and stood still for 1.5 s on the new
   map ("cheguei na fogueira %04x em %llu ms; os convidados podem vir").

And a fourth, with no crash: the destination bonfire can **already be in the
list** because the map is loaded for the other player's copy — but only with
the parts around it. The guest arrived in Heide with no ground and fell to its
death. Any map that is not the one under your feet goes through the held map
and the focus.

Measured with the build `2cda8f19`, Chico hosting and Samuel as the guest: the
guest's proposal in Heide → the host accepts → the host arrives in Majula
in 2.2 s → the guest follows; both standing at The Far Fire, each seeing
the other, `p2pSessionVerified: true`. With the build `9fccbc75`, three round
trips in a row Heide↔Majula through the request file, with both arriving
together and no fall.

The old shape stays as a fallback: if the destination map cannot be brought in
(`DS2_DeathIntercept::MapReachable` false), an approved vote goes back to
sending the guests out and the host travels through the game.

**The guest's bonfire list — fixed on 15/09.** The bonfire table
(`*(*(ctx+0x70)+0x58)+0x20`, 0x18 per entry, id ushort) has a "lit" column per
world, and the one the guest reads (`+0x44` = 1) only got the bonfires of the
maps it had loaded in the session: with both in Heide, its travel list had 2
areas, and the host had 13. Now the host publishes its own lit ones as a bitmap
over the table's order (channel packet `0x30`, 96 bits, 77 entries in 1.03) and
the guest writes into its column — 74 bonfires aligned in the first second, and
its list became the same as the host's. The table is checked before it is
written (count, column and increasing ids).

**The rest notice is gone.** The box "A player is resting at a bonfire." was
modal and got in the way of a fight; by the project owner's decision (15/09)
resting no longer notifies anyone. The world reset still holds for everyone.

**A loading screen for the travel — done on 15/09.** The travel raises the
curtain the game's own travel uses: `ctx+0x1178 = 1` (which already turns off
the action prompts and the death timer) and
`FUN_140b06270(*(0x1416751f8)+0x80, 1)`, which turns off the drawing of the
world; the game still writes the area's name over it. It comes down 1.2 s after
the character is in place, and always before 25 s. Nobody sees the character
hanging between the two maps any more.

**Joint travel has been off by default since 16/09.** An approved vote goes
back to the path measured as stable (the guests leave through the legal exit,
the host travels through the game's travel, the party brings everyone
together); `DS2_Bonfire.req` accepts `junta liga` to try the one without
leaving. The reason is below, and the cost of finding it out was Samuel
reaching **100 points** of illegal disconnect — the block — with the punishment
timer at 36000 s.

**The guest still crashes after arriving — open.** With the curtain and with
the origin map held for 15 s, the guest's game still closed twice out of five
travels, always **after** it had arrived and was standing:

- `+0x3f4fac` / `+0x3f510f`, in the `CharacterManager`'s character pre-draw
  task (`FUN_1403f4f60`, `mov 0x38(%rcx)` with `rcx` coming from `chr+0xc8`
  pointing at already released memory, pattern `000b15..`), 1 to 3 s after the
  arrival — and, both times, right **after** the curtain came down;
- `+0x1cbf40` (`FUN_1401cbf20`), a list walked with a released node, with the
  new map's id in `r15`.

Solo it never happens: eight round trips in a row, with no session, without a
single crash.

**What the watcher showed (16/09).** `DS2_TravelWatchHook` logs, in a window
around the travel, who destroys what: the `MapEntity` destructor
(`FUN_1403b9ea0`), the release of a `MapModelComponent` (`FUN_1403f6300`) and
the two teardowns by map index. In a travel that did **not** crash, the log
brings hundreds of releases of components from the map left behind, all of them
within the frame. In the one that crashed (`+0x40d33b`), the list walked by the
frame (`FUN_14040d2e0`, the update of every registered component: node at
`dono+0x20`, previous at `+0x00` and next at `+0x08`) had a node whose owner
**had already been released** — a vftable stamped by the allocator — and none
of the four watched doors had released that owner. That is: **somebody frees
without unlinking from the list**.

The frame is now walked by the hook, which checks the owner's vftable before
calling and unlinks the dead node from the list. That took that crash away and
brought the next one: `+0x3f687a`, **inside** the component's release, in an
already released sub-object. Which means: the dead reference is not only in the
frame's list; it is inside live components. It is a memory lifetime problem in
the game's streaming with two maps and a session up, and it does not close with
a spot fix — which is why joint travel was turned off.

Two fixes from this round are worth having either way:

- the longer `keep` wins (the travel's 30 s request was being shortened to 5 s
  by the keep that the other player's copy renews every frame);
- the arrival map can only be held after the character steps on it: the
  physical contact still answers with the map it left at the moment of the
  jump.

And one that was invisible: **the death profile was null**, so every entry into
the world left the hook in `observe` and no death in a session was charged by
M2 — the guest's death became the game's warp, which tears the world down. Now
the profile is `respawn`. In a session, it is always the **guest's** machine,
and what it has extra is the other player's copy, which changed maps along with
it. That is the lead that is left: who keeps `chr+0xc8` for another player's
copy and what happens to that field when its map changes with no warp.

The cost so far: 80 points of illegal disconnect on Samuel (from 10) and 10 on
Chico. The save `antes-viagem-junta` was taken before all of this.

**Second round of fixes (16/09), the crash still open.** Three main attempts,
each measured with two players, and each one **moved** the crash instead of
closing it:

1. **The watcher now guards the pre-draw of the model component itself**
   (`FUN_1403f4f60`), not the character's update: before the game touches the
   component, it checks the component and the three objects it points at
   (`+0x40` the model instance, `+0xc8` the frame's registration, `+0xd0` the
   follower); a component that has already been released is skipped. That
   showed that, **standing still**, everybody passes (0 skipped). The crash
   only comes when a map is **torn down**.
2. **The travel now holds both maps whole** (a `keep` with every part), and the
   `keep`s **add up** and never shrink — before, the `keep` that the other
   player's copy renews per frame demoted the map to the parts underneath it,
   and the map went out in two phases (the parts on arrival, the rest 30 s
   later). It did not close it: `+0x40d2c7`, inside the `unlink` of the frame's
   registration (`FUN_14040d2b0`), writing `proximo->[0] = anterior` with
   `proximo` already released.
3. **The curtain now opens the game's own loading screen** (`FUN_1405014b0`,
   event `0x67` to the front-end object `0x4c5c574`) and hides the HUD
   (`FUN_1404ffef0(frontend, 0xffdffbff)`), besides turning off the drawing of
   the world as before. Measured on the guest's screen: the HUD disappears, the
   letterbox bars and the loading spinner appear — but the world is **still
   drawn** behind them (the call that turns the drawing off, `FUN_140b06270`,
   is a no-op outside a real warp). It is better than the character floating
   with a full HUD, but it is not the black screen.

**What is clear now.** The crash is always during the **teardown** of a map's
`MapModelComponent`s (`FUN_1403f6300`, called by `FUN_1403f4500`), and the
address changes with every attempt (`+0x3f4230`, `+0x40d2c7`, `+0x3f687a`,
`+0x3f4fac`). The pattern in the registers is always the allocator's stamp
(`00b540…`, `00b010…`): a node of the frame's registration (`prev`/`next` at
`+0x20`/`+0x28`) was **freed without being unlinked from the list** — the other
player's copy, which changed maps along with it. The frame's watcher
(`FUN_14040d2e0`) unlinks the dead node **when it walks that bucket**, but the
teardown (`FUN_14040cea0` → `FUN_14040d2b0`) runs into the dead neighbour
**before** that, when a live component tries to unlink itself. And the `keep`
through the owner's force does not prevent the teardown by **map index**
(`FUN_1401c5dd0`), which is another path.

**The next lead, concretely:** either guard `FUN_14040d2b0` itself (checking
that `prev`/`next` are alive before writing to them — it is tiny but very
heavily called), or prevent the teardown by index of a held map during the
travel's window (the watcher already intercepts `FUN_1401c5dd0`; today it only
logs). Both touch a hot path and need a measurement with two players, which
costs a point. In the meantime joint travel **stays off by default** and the
vote falls back to the stable path (the guests leave through the legal exit,
the host travels through the game's travel, the party brings everyone
together).

**What already holds, independent of the crash:** the `keep` that adds up and
never shrinks; the guarded pre-draw (a real block against the crash, not just a
log); the game's loading screen with the HUD hidden; and `DS2_Bonfire.req`
accepts `votar <mapa> <fogueira>` on the host to exercise the whole vote
without the list.

## The guest's crash — closed on 16/09

**One guard only, in the right place.** A map's whole life goes through one
call: `FUN_1403cc3f0`, the per-frame update of the `MapAreaCtrlOwner`, whose
state machine loads the map, streams the parts and, at the end, tears
everything down. **All** the crashes that were left came from inside that
teardown, at a different address every time — `+0x3d8782`, `+0x3c1bf8`,
`+0x40d2c7`, `+0x40cee3`, `+0x3f4230`, `+0x3f647e`, `+0x3ece30`. Chasing them
one by one only changed the address: there were four rounds of spot guards, and
at each round the crash reappeared somewhere else.

`DS2_BackreadHook` now wraps that whole call in a `__try`. A failure there
leaves the map **half done** — memory the session does not get back — instead of
closing the game and costing the guest ten points of illegal disconnect.

**Measured on 16/09, with both players**: ten legs in a row Heide↔Majula (five
round trips), the host first and the guest after, with the session verified the
whole time. **No game closed.** The guard caught **60 failures** in the map's
cycle and the game carried on through all of them; both finished standing at
the same bonfire in Heide, `p2pSessionVerified: true`, the penalty unchanged
(Samuel 10, Chico 50, armed 1 because the session was live).

Smaller guards, on the same principle, cover the rest of the path: the pre-draw
(`FUN_1403f4f60`), the post-physics (`FUN_1403f41d0`), the component's release
(`FUN_1403f6300`), the exit from the frame's list (`FUN_14040cea0`) and the
list search (`FUN_1401cbf20`). A failure in any of them skips the frame or
leaks the component.

**The corruption itself is still unexplained, and is written down here for
whoever comes back to it.** The signature is always the same and it is odd: the
**high** half of a live pointer shows up overwritten, and the low half stays
right.

    00b54001410e86d8   deveria ser 00000001410e86d8   (vftable +0x10e86d8)
    00b01001410eb518   deveria ser 00000001410eb518   (vftable +0x10eb518)
    00b010fff06b8588   deveria ser 00007ffff06b8588
    000b0010e81d77b0   deveria ser 00007fffe81d77b0

The values that show up are always from the same family (`00b010`, `00b540`,
`000b0010`, and once `a140a140a140a140` in the whole qword). It happens **only
with a session up** — solo, fourteen travels in a row on two different builds,
not a single failure — and **on either of the two machines**: on 15 and 16/09
almost always on the guest, but on 16/09 at 05:37 it was the **host** that
closed. What the two have in common in joint travel is holding another player's
copy that changed maps.

Two readings are still standing and this sample cannot choose between them:

- **a lost write with a 2-byte stride**: some 16-bit-at-a-time copy runs into a
  live object, and the two bytes swapped in the middle of a pointer are the
  edge where it starts or ends;
- **use after free**: a partially reused block gives exactly the same picture.

One thing has been checked and helps separate them: `DLRegularHeap`'s `free`
(`FUN_1408572d0`, slot `+0x68` of the allocator's vftable) **does not fill the
freed block with any pattern** — it writes free-list pointers into the
neighbours (`+0x10`, `+0x18`). So a repeated 16-bit value is not poison from
**this** allocator; and the mark of a use-after-free here would be a heap
address (`00007fff...`) showing up where something else should be, not
`a140a140`.

**Two theories were tested and discarded**, which is worth more than the ones
left:

1. **"somebody frees a node without unlinking it from the frame's list"** — that
   was the 15/09 conclusion. The watcher started rebuilding the list's 32
   buckets every frame, leaving only nodes whose owner is still a live object.
   In ten legs **no bucket needed rebuilding**. The list was always intact; it
   is not that;
2. **memory pressure from the travel forcing the whole map** — the travel asked
   for the destination's 128 parts. Measured solo on 16/09, with the mask at
   **zero** the map loads the same and the ground arrives the same way, because
   what brings the ground is the **focus** (the navigation cell handed to the
   streamer), not the mask. The component footprint was identical (188 in
   Heide, ~570 in Majula), so the mask was never the cost. The travel stopped
   asking for it either way.

**And a lesson in method:** the first version of the guards tried to *guess*
which fields of a component were healthy (`+0x40`, `+0xc8`, `+0xd0`, the
vftable embedded at `+0x50`) and skipped the component when one of them did not
look like a live object. That threw away **600 updates per boot** of perfectly
normal map objects: those fields legitimately hold things that are not objects
with a vftable. Guessing which field is sane is how you break the game while
trying to save it. What stayed was the `__try`, which only fires on a real
failure and has no false positives.

**Corrected the same day, before it became a wrong conclusion.** The first
reading of this section said "ten legs with no crash". That was not true, and
the mistake was in the method: when the travel gives up (30 s with no map), it
writes **"viagem concluido" all the same** and leaves the character where it
was. The test expected that line, so a travel that never happened counted as a
success. Rereading that run's logs: the host travelled 28 times out of 30, but
the **guest only travelled 5** — the other 13 gave up. The absence of crashes
was inflated by travels that never went anywhere. The test now only counts a
leg when **both** log "o personagem esta nele" in the destination map.

And the cause of those give-ups was mine: the travel had started asking for the
map with the parts mask **zeroed**. Measured with both players, a forced owner
with an empty mask stays in state 0 forever (`estado 0 forcado 1 quer 1`, all
masks zero): **the force byte says "keep this map", not "load it"; what loads
is having a part requested.** Solo it seemed to work only because the player
had just come from that map and the streamer still wanted parts of it. Worse on
the guest, because the host goes first: the host's copy in the guest's world
forced the destination with no parts, and the guest's own request found an owner
that would never load. The masks came back.

**With real travels, the crash is still there.** Measured again after all of
that: four legs with **both** arriving, and on the fifth a game closed — this
time the **host's**, at `+0x36f846`, with no guard having fired. The guards
catch a lot (sixty failures in one boot, all survived) and are not useless, but
they do not settle the question.

**The new lead, and it is the best so far.** In that last crash the register
carried `a140a140a140a140` — a **16-bit** value repeated. That fits the old
signature: the two bytes swapped in the middle of the pointers (`b0 10`,
`b5 40`) are the same phenomenon seen at the edge, where the filling starts or
ends. That is, **something writes memory in 2-byte steps and runs into live
objects**; the value changes every time, so it is data being copied, not a
constant poison. It is not ASCII in UTF-16 (the high byte would be 0x00), so it
is not this hook's text boxes.

### Found on 16/09: it is the Havok rigid body, and it is use after free

**The write watcher answered.** And the first thing it corrected was the plan:
I had been saying that a **hardware write** breakpoint was missing. It **does
not work on this machine**, and the repository already said so in
`DS2_AREA_RESTRICTION.md` ("Hardware watchpoints do not work under Wine...
accepted by all sixty threads and never fires"), with the warning that it is
worth knowing before reaching for the obvious tool again. The tool that does
work was already built: `DS2_TraceHook`'s **page protection** watcher (`wp` in
`DS2_Trace.req`).

**The chain, measured live on the host:**

    PXCharacterRigidBody  (o corpo físico do personagem)
      +0x110  ->  hkpRigidBody  (o corpo rígido do Havok)
                    +0x18  ->  ponteiro para o heap dos mapas

The host's repeated crash was reading exactly that path (`FUN_140bd15b0`:
`*(rcx+0x110)`, then `+0x18`, then `+0x8`).

**And the watcher showed who writes the `hkpRigidBody`'s `+0x18`:** a
`rep movsb` (`+0x1c31725`) copying **0x1d28 bytes**, coming from
`FUN_1404e00d0`, which **allocates** a buffer and copies inside it. The
destination of the copy landed on top of the field. That is, the
`hkpRigidBody`'s memory was **recycled** into another buffer while the
character's `PXCharacterRigidBody` was still pointing at it.

**That settles the question that was open.** It is not a lost write with a
2-byte stride; it is **use after free with address reuse**. The values that
looked like loose data were the block's new occupant seen through the old
pointer, and that is why they varied so much: `a140a140a140a140` and
`3e2a256f2b7fe62c` are floats from the new buffer, `005c003200470053` is UTF-16
text from a file path, and `00b54001410e86d8` is a block only partly reused,
with the old pointer's low half still standing.

**And it closes with the structural cause.** Joint travel moves the character
between maps **without the load** that would rebuild things. The Havok rigid
body belongs to the physical world of the map being torn down; the character's
`PXCharacterRigidBody` keeps hold of it. Solo it does not bite; with a session,
and with a remote copy crossing along, it bites.

**The fix now has a target.** It is not "rebuild presence" in general: it is
making sure the character's rigid body is recreated in the destination's
physical world, or that the pointer is reseated, instead of being carried
across the teardown. That is much smaller than the whole coordinated re-entry
`DS2_SEAMLESS_TRAVEL_ARCHITECTURE.md` proposes, and it is the first experiment
to do.

**What is still not proven:** the exact point where the `hkpRigidBody` is
freed. The chain and the reuse are measured; the site of the free is not.

### The fix, and what it measured (16/09)

The path from the character to the body is
`chr+0x100` → `+0x40` (`PXCharacterRigidBody`) → `+0x110` (`hkpRigidBody`),
confirmed live. `DS2_DeathInterceptHook` now checks that pointer every frame,
for the local player **and** for another player's copy, and **drops** the body
(writes 0) when it has provably gone. There are three tests, from the cheapest
to the most expensive:

1. the pointer is not an address (some bit above 47);
2. what it points at has no vftable from this module;
3. the body's `+0x18`, which is the field the animation chain reads, holds
   something that is not an address. This is the one that catches a block
   **already handed to another owner**, because a reused block can perfectly
   well have a believable vftable again.

Nothing is freed: the block already belongs to someone else. And zero is a
legitimate state, not a patch-up: `FUN_140bd1500`, the game's own release,
writes 0 into those fields, and `FUN_140bd15b0` tests for null and returns
before reading anything.

**Measured with two players, 24 legs:**

| | before | with the fix |
| --- | --- | --- |
| exceptions on the host | 39 | **0** |
| failures caught in the task (host) | 42 to 59 per run | **0** |
| games closed | intermittent | none |

And the closing is exact: the fix's lines name the character
`00007FFFEB7B9E60` and the `PXCharacterRigidBody 00007FFFEB7C1440`, which are
the **same** subject and the **same** object as the chain traced by the write
watcher. The reason logged all three times: "o corpo nao tem vftable do jogo".

**The host's crash stopped existing at the cause**, not through a guard. The
task executor's guard, which existed precisely to catch that, had nothing to
catch.

**What is left, and it is the second dependency that was already expected.**
The guest still logs one failure in the pre-draw (`+0x3f510f`) during the run,
besides 32 occurrences of `+0x17eda9`, which is old and harmless (the rest job,
the game carries on). That is: the same kind of dangling pointer exists
somewhere else on the guest's side. The agreed decision rule still holds: if a
third dependency shows up, that is the signal to stop fixing them one by one
and rebuild presence as `DS2_SEAMLESS_TRAVEL_ARCHITECTURE.md` proposes.

**Missing:** repeating it over more runs (24 legs is a single measurement),
exercising the deferred teardown of the origin with travels spaced further
apart than the 30 s of the hold, and finding where the guest's dangling pointer
comes from.

## Travel contract and group barrier — done on 16/09

The first half of `DS2_SEAMLESS_TRAVEL_ARCHITECTURE.md`, the part the review
itself says is necessary in **both** of the architectures it proposes.

**The contract.** `DS2_DeathIntercept::TravelOutcome()` answers
`Idle`/`Moving`/`Arrived`/`Failed`. `Arrived` is only written when the map
reached is the destination one, never by time. That replaces the ambiguity of
`Moving()`, which meant "arrived **or** gave up" and which on 16/09, at 05:23,
made the host announce an arrival and call the guests to a map it had given up
loading thirty seconds earlier. Every give-up path marks `Failed`, including
the one for the map that never loaded — that one was missing in the first
version and reproduced the same defect immediately.

> **Corrected on 16/09, by a review from gpt-6-astra.** This paragraph said
> that `Arrived` is written only by the **physical contact** under the
> character. The implementation is weaker. `ContinueSettle` decides by
> `CurrentMap() == s_settle.Map`, and `CurrentMap()` reads the map of the part
> the *streamer* registered the player in (context → map manager → streamer →
> part → owner → map). The real contact — `ContactHandle` and `MapIndexUnder`,
> read from the character's physics — goes into the diagnostics and into the
> choice of which map to hold, **not** into the arrival decision. The real
> guarantee is "the streamer says the ground under it belongs to the
> destination map": it is still evidence of being in the right place, it is
> still not time, and it is not the direct contact. The phrase also appeared in
> `DS2_PRESENCE_REBUILD_PLAN.md`, in the list of what to preserve, and was
> corrected there the same way.

**The barrier.** The two do **not** load at the same time, on purpose: doing
that closed both games on 15/09. They arrive about a second apart, each behind
its own loading screen, and **nobody leaves it** until the last one arrives.
Each machine sends a receipt to the host (`GuestEvent::TravelArrived` /
`TravelFailed`, with the vote's number), the host collects one receipt per
participant and sends `HostEvent::TravelRelease`; every curtain comes down on
that single message.

**Measured, with two players, through a real vote:**

| leg | host's arrival | guest's arrival | screens came down |
| --- | --- | --- | --- |
| Heide → Majula | 15:36:32.947 | 15:36:33.933 | **18 ms** apart |
| Majula → Heide | 15:39:04.189 | 15:39:05.406 | **16 ms** apart |

A second of difference in the physical arrival, tens of milliseconds in when
the two of them appear. That is what "travelling together" was meant to mean.

A failure or a timeout leaves nobody stuck: if somebody fails or the wait goes
past 25 s, the host releases the group all the same, with a notice, and the
guest has its own emergency exit five seconds later.

**Two real defects appeared along the way and were fixed:** the outcome that
was not being marked for the map that does not load, above, and the **respawn
record written before the travel**. That second one was suspect for calling the
game's travel request constructor before the movement; it is now written
**after** the arrival. (The investigation also cleared that hypothesis: the map
that would not load was state poisoned by a `reload`, see below.)

**A bench hazard that cost an hour.** After a `reload` (back to the title and
in again), the backread **brings in no map at all** in that instance: the owner
stays at `estado 0 forcado 1 quer 1` with all the masks full, forever. The
other instance, on the same build, loads normally. Closing and coming up clean
fixes it. Chasing that as if it were a code defect leads to wrong conclusions
about the travel.

**What is missing, and it is the same as before.** The guest still closes on a
dangling pointer in the pre-draw (`+0x3f4f2b`, `FUN_1403f4f60`), which the
rigid body fix does not cover because there the object is a
`MapModelComponent` and not the character's body. By the agreed rule, that is
the **second** dependency; the third is the signal to stop fixing them one by
one and rebuild presence (the second half of the review), which depends on a
primitive nobody has proven exists: removing a presence without leaving the
session.

> **Corrected on 16/09: the paragraph above names the wrong function, and that
> changed the diagnosis.** `+0x3f4f2b` is **not** in `FUN_1403f4f60`. The PE's
> exception directory gives `FUN_1403f4f10` at `0x3f4f10..0x3f4f53` and
> `FUN_1403f4f60` at `0x3f4f60..0x3f527e`: they are disjoint functions, and the
> crash lands `+0x1b` into the first one. Nothing in either `.text` section
> calls `FUN_1403f4f10` directly — it is a virtual method, slot `0x1410eb588`
> of the vftable whose name, `MapModelComponent`, is right after it at
> `0x1410eb5c0`, and whose slot `0x1410eb598` is the `FUN_1403f6300` this
> project already hooks.
>
> The consequence: the pre-draw's `__try` **was never on the stack** while the
> function that breaks was running. The "second dependency that escaped the
> guard" never escaped anything — it was the guard that was on the wrong
> function, exactly the mistake already made with `FUN_1403152f0`. **The third
> dependency rule was not triggered**, and the count that led to the review was
> inflated by that attribution.
>
> The right guard is in `ModelTickHook`. What it does is what its sibling does,
> and nothing more: on a failure, it writes what names the object at `+0x40`
> while it still exists, to arm the page watcher. It does **not** write null
> there by analogy with `DropDeadRigidBody`: the null test at `+0x3f4f26`
> proves the contract of *that* consumer and not of the others —
> `FUN_1403f4f60` reads the same field — and for the rigid body the null path
> was read at the call site before any write. That reading has not been done
> here yet.
>
> Next targets, two neighbouring slots of the same vftable: `0x1410eb578` →
> `FUN_1403f4c20`, which reacts to the backread change and calls virtual
> release/load, and `0x1410eb580` → `FUN_1403f4ce0`, which tears a registration
> down. The bounded question is **who owns the model instance at `+0x40` and
> which native operation swaps it**.

### The right guard was installed, and the crash moved (16/09, 18:56)

The guard on `FUN_1403f4f10` went up in both installations, with a positive
receipt (`tique do modelo ... guardado`), the session verified
(`p2pSessionVerified: true`) and one clean leg before it: the host arrived at
18:53:46.482, the guest at 18:53:47.649, curtains 18 ms apart.

On the second leg the guest closed. **The counters were all at zero, including
`tique 0`** — the new guard never fired once. The crash was somewhere else:

    excecao c0000005 em +0x17b260, lendo 0xffffffffffffffff
    rbx=00b540ffe83053f0     <- o no morto, com o carimbo do alocador
    rdx=00000001410eb558     <- a vftable do no anterior: MapModelComponent

`+0x17b260` is in `FUN_14017b240` (`0x14017b240..0x14017b2a2`), which **walks a
list at `objeto+0x18`, reads each node's vftable and calls slot 0**, following
`+0x10`. That is exactly what `FUN_1401cbf20`
(`0x1401cbf20..0x1401cbf82`) does — and this file already guards that one.

The two are **the same code, emitted twice**. The 0x62 bytes are identical
except for three, at offset `0xe..0x10`, which is a `call`'s relative and has
to differ because the functions are at different addresses:

    FUN_14017b240  48895c2408574883ec20488bd9 e87ef22c00 488b5b18...
    FUN_1401cbf20  48895c2408574883ec20488bd9 e80ece2100 488b5b18...

**And the step `mov (%rbx),%rdx; mov %rbx,%rcx; call *(%rdx)` appears 74 times
in `.text`.** We guard 1 of 74.

**The conclusion that forces.** Guarding consumers does not end: the disease is
not in whoever walks the list, it is in **`MapModelComponent` being freed along
with the map's heap while it is still linked into lists**. Each new guard only
pushes the crash on to the next copy of the same loop. The fix has to be on the
side of whoever frees — unlinking the node from every list that holds it — and
not on the side of whoever reads. It is `CLAUDE.md`'s lesson about inventorying
by sweep and not by pattern, charged again.

That does **not** point at rebuilding presence. What dies here is a map
component, and the lists are internal to the game; taking the players' copies
away cleans none of them. It points at option B of
`DS2_PRESENCE_ASTRA_REVIEW.md`, and the target already named above —
`FUN_1403f4c20`, the native reaction to the backread change — takes priority
over any presence work.

**Not measured yet:** whose the component that dies is (a player copy, a map
object or a world character). The guard on `FUN_1403f4f10` is still installed
and writes that when it fires; on this run it did not fire because the other
copy of the loop got there first.

**The run's cost:** the guest closed with a live session, which is an illegal
disconnect. The host stayed at 10 points, unarmed. The guest was at 60 before
the run — 50 at the previous check in this session, so ten points were charged
at some moment before this test.

### The mechanism, read in Ghidra (16/09)

A static reading, with nothing run. It explains the crash above and says where
the fix fits.

**The list.** `FUN_14040cca0(entidade, componentes, quantos)` is the **attach**:
it links each component into `entidade+0x18`'s list, writes
`componente+0x08 = a entidade`, registers it in the frame's registry through
`FUN_14040d280`, and calls the component's slot `+0x20`. That is the list the
74 `GetComponent<T>()` walk.

**The unlink is complete.** `FUN_14040cea0(entidade, componente, liberar)` —
which this project already hooks — does, in this order:

1. calls the component's slot `+0x28` (for `MapModelComponent`,
   `FUN_1403f4ce0`, which takes it out of the frame's registry if bit 2 of
   `+0xe8` is set);
2. **takes the component out of `entidade+0x18`'s list**, walking through
   `+0x10`;
3. zeroes the component's `+0x10` and takes it out of the registry through
   `FUN_14040d2b0`;
4. zeroes `componente+0x08`, the pointer back to the entity;
5. only then, if `liberar` is true, finds the owning heap through
   `FUN_1408389e0` and frees it there.

And `FUN_1403f6300`, the component's normal release, calls `FUN_14040cea0`.

**Conclusion.** The game's path **always unlinks before freeing**. The only way
a dead component stays in an entity's list is for its memory to disappear
**without** that function running — which is exactly what happens when the
map's heap is destroyed whole: the blocks go at once, no destructor runs, no
unlinking happens. If the entity that lists that component survived the map, it
is left with a dead node, and the first `GetComponent<T>()` that goes through
there brings the game down.

In our travel that is what survives: the character crosses with no load, so its
entity stays alive while the origin map dies underneath.

**The fix that suggests**, and it is not rebuilding presence: before releasing
the origin map, make every component allocated in the heap that is about to die
unlink itself from its entity, by calling `FUN_14040cea0(entidade, componente,
0)` — unlink without freeing, because the heap is going to free it anyway. The
three pieces already exist in this project: `FUN_1408389e0` says which heap a
pointer belongs to (the watcher's `WhoseHeap` uses it), `FUN_14040cea0` is
already hooked, and the entity is at `componente+0x08`.

**Not measured:** whether the list that broke is that of a character that
travelled, of a player copy or of another entity; and whether there is a better
native point, called when a map is unloaded normally, that the travel ought to
be going through. The crash's stack goes through `FUN_14036f800`, which calls
the `FUN_140bd15b0` of the rigid body fix, which suggests a character — but
suggesting is not measuring.

### The unlinking was built, it ran, and it unlinked nothing (16/09, 19:33)

The commit `60fe3b53` arms, before the travel releases the origin map, a 10 s
window in which every component that passes through the frame hooks is unlinked
from its entity when the two are in different heaps. Installed in both
installations, the build checked by the receipt, the session verified, one leg
with both arriving and 34 ms between the curtains.

**Result: `desligamentos: 0 feitos, 0 falharam, 0 com heap desconhecido`, on
both machines.** The window was armed at the right moment (`desligando
componentes de heap alheio por 10000 ms`, 19:33:52.083 on the host and
19:33:53.083 on the guest), so the function ran for ten seconds and found
nothing to do.

**What was measured live to understand the zero**, with a breakpoint on
`FUN_1403f4f10` to catch a live component and the MemProbe to read from it:

    componente 7fffe8443640
      +0x00  1410eb558   vftable do MapModelComponent
      +0x08  7fffe843fe20   a entidade
      +0x20  7ffff03a51c8   o elo do registro do quadro
      +0x30  1410eb5a8   uma segunda vftable: sub-objeto em +0x30
      +0x60  1410eb518   uma terceira: outro nó, com a mesma entidade em +0x68
    entidade 7fffe843fe20
      +0x00  1410e7b68      +0x18  7fffe83a8540   a cabeça da lista

And the entity's list, walked node by node through `+0x10`, **contains the
component** as its second element, with `entidade` at `+0x08` in all fourteen
nodes read. That is: the layout model is right, the node **is** the base of the
component (the registry link at `+0x20` matches what `FUN_14040d280` writes at
`nó+0x20`), and the code's proofs would have passed.

**So what blocked it was the heap comparison.** A component at
`0x7fffe8443640` and an entity at `0x7fffe843fe20` are neighbours: the same
heap. The rule "a component in a heap different from its entity's" does not
describe what happens — at least not among the components that pass through the
three frame hooks.

**What that refutes.** The part of the mechanism read in Ghidra still stands:
the game always unlinks before freeing, so a dead node in a list can only come
from memory that disappeared without that path running. What falls is my
inference about **which** memory disappears: it is not "a component of the
origin map hanging off an entity that survived", because component and entity
live together. The crash's dead address (`00b540ffe83053f0`, that is
~`0x7fffe83053f0` under the stamp) is from the same neighbourhood too.

**The committed fix is, today, a no-op.** It stays installed because it costs
nothing and because its instrumentation is what is left to measure, but it does
not fix what it was built to fix.

**What is left to measure, and it is cheap:** logging, once per window, the
heap indexes of both sides even when they are the same — today the code only
speaks when they are unknown, and that is why "there was nothing cross-heap"
and "the rule does not apply" come out identical in the log. After that, the
right tool is the page watcher over the node that dies, which has already found
the rigid body once.

**Cost:** none. The exit was through the legal path and the penalty stayed the
same on both, 10 and 70.

### The cross-heap rule is dead, with a number (16/09, 20:04)

`a18747c4` added two things: the sweep of the entities' component lists, which
cuts a rotten link by stitching the tail back, and the count of how many
components were seen **in the same heap** — which was the datum missing to tell
"there was nothing cross-heap" from "the rule does not apply".

Twelve legs, all with both arriving, no game closed, guards 0 → 0, a legal
exit, the penalty the same on both at the end (10 and 70).

    conta 1: visto 6017963 no mesmo heap, 0 em heap alheio
    conta 2: visto 7035751 no mesmo heap, 0 em heap alheio

**Thirteen million observations and not one exception.** Component and entity
always share a heap. The rule "a component in a heap different from its
entity's" describes nothing that happens in this game, and the unlinking built
in `60fe3b53` is dead as a fix — not for lack of opportunity, but because it is
based on a false hypothesis. **It should be removed**, because code that
pretends to fix is worse than code that is absent; what is worth keeping from
it is the counter, which is what produced this measurement.

**The rotten-link sweep was not tested.** `elos podres cortados: 0` across the
twelve legs, and no `COMPONENTE CORROMPIDO`. Since there was no crash, that is
the case with no information, exactly as predicted before running: twelve clean
legs had already happened before the sweep existed. It stays installed because
it costs nothing and because it only depends on "this is still a pointer" and
"this still has a vftable from this game" — no hypothesis about heaps.

**What this does not prove.** That the sweep prevented the crash. The crash on
the 16th came on the second leg of a run; this one was twelve without falling,
which is suggestive and is not proof — the crash was never deterministic. To
attribute it, you would need either to see the sweep cut (and then the cut is
the signal) or a much longer run against the earlier distribution.

**What to do next**, in order of cost: remove the cross-heap unlinking; run a
long run to see whether the sweep ever cuts; and, if the crash comes back with
no cut at all, the page watcher over the node that dies, which is the tool that
already found the rigid body.

### Forty legs with the clean build, and the sweep never cut (16/09, 20:36)

`955ab5fa` took the whole unlinking out — 262 lines — and left the sweep and
the counters. Forty legs, both arriving on all of them, none failed, no game
closed, guards 0 → 0, a legal exit, the penalty the same at the end (10 and
70).

    conta 1: 16838748 no mesmo heap da entidade, 0 em heap alheio, 0 desconhecido
    conta 2: 16734704 no mesmo heap da entidade, 0 em heap alheio, 0 desconhecido
    elos podres cortados: 0 nos dois

Adding the previous run: **52 legs in a row with no crash**, and **forty-six
million** components observed without a single foreign heap.

**What is proven.** That the cross-heap rule is false, now with an order of
magnitude more samples. That does not go back.

**What is not.** That the sweep fixes anything. It **never cut**, so in 52 legs
no entity list had a rotten link. Two readings fit, and nothing here separates
them:

- the damage never showed up in these 52 legs, and the sweep sat idle — which
  would also explain the 40 clean legs from before it existed;
- or the damage shows up in an entity that does **not** pass through the three
  frame hooks, and the sweep never looked at the right list.

The second is the one that matters, and it is testable: the crash on 16/09 came
from an entity reached through `FUN_14036f800`, through the physics. Nothing
guarantees that that entity has a model component going through the pre-draw.

**So the sweep cannot be credited, and the crash cannot be called resolved.**
It is intermittent: it came on the second leg of a run and then disappeared
for 52. A clean run does not refute it.

**The next step is the page watcher** (`wp` in `DS2_Trace.req`), armed over the
node while it still exists. It is the tool that found the rigid body, it is the
only one that answers **who writes**, and in this session it was put off three
times in favour of hypotheses the measurements knocked down one by one.

### The third family of crash, and the decision to redo the travel (16/09, 20:44)

With the games handed over to the user for normal play, **the host crashed** on
a travel to a destination the tests never touched: **Throne Floor (Brume
Tower), map `32240000`**, from the DLC. The two automated tests were always
between `0a040000` and `0a1f0000`.

    excecao c0000005 em +0xfd8592, lendo 0x0
    rax=0  rbx=00007fffed419d30
    retornos: +0xfd7fcd +0xfd7db2 +0xfd8442 +0xa34e93 +0xa27435

    140fd8585:  call FUN_140a11100
    140fd858a:  lea  0x30(%rsp),%rdx
    140fd858f:  mov  %rbx,%r8
    140fd8592:  mov  (%rax),%r9      <- rax veio nulo
    140fd8598:  call *0x50(%r9)

**This is not the same disease.** There is no freed pointer and no allocator
stamp: a lookup (`FUN_140a11100`) returned **null** and the caller read
`(%rax)` without checking. It is a lookup for something that should be loaded
and was not. Counters at the moment of the crash: all zero,
`elos podres cortados: 0`.

The guest paid no penalty (it stayed at 70); the host crashed before its own
could be read.

**The user's conclusion, and it closes this line of work:** the approach is
**unsafe by construction**. Bringing the destination map in beside the current
one and teleporting leaves the world half initialised, and the failure modes
are open-ended — each fix reveals the next, now in three distinct families:

1. a freed pointer with address reuse (the rigid body — fixed);
2. a dead node in a list walked by 74 copies of the same loop (never reproduced
   after the sweep, never credited);
3. a lookup that returns null because the destination is not really loaded
   (today).

The third one alone is enough: it says that a map forced in beside the current
one **is not equivalent to a loaded map**, and no guard fixes that — a guard
prevents the game from closing, it does not make the resource exist.

**Decided: the travel will be redone through alternative D** of
`DS2_PRESENCE_ASTRA_REVIEW.md` — throw the current transport away and use the
game's native loading on both sides, so that everything gets rebuilt and the
whole class of failures stops existing by construction. The plan is being drawn
up separately.

**What survives, on today's reading and subject to the plan:** the
`Idle/Moving/Arrived/Failed` contract, the group barrier and the
`TravelRelease`, host first, the type proofs, the `__try` guards as a net with
the rule that a trip is an architectural failure, and the channel between the
machines. The transport (forced backread, teleport, contact) and the curtain we
draw ourselves is what goes.

**Three corrections of method in this round**, all of them mistakes of mine and
all of them useful:

1. the corruption detector first required `+0x50` to be a vftable **of this
   module**: 40 false positives, because the field legitimately points outside
   it;
2. then it was widened to `+0x40`, `+0xc8` and `+0xd0`, which **are not
   pointers** in many components: it started reporting UTF-16 text, the float
   1.0 and pairs of coordinates as if they were damage;
3. what works is looking at **one** field that is provably a pointer and asking
   only whether the value **can be an address** (any bit above 47 set means it
   cannot).

**A sweep that proved unnecessary, and why it was taken out.** During one of
those rounds the watcher started rebuilding the frame list's 32 buckets every
frame. Over more than forty legs **no bucket needed rebuilding** — the list was
always intact. Besides being useless, it kept one frame's registry pointer to
use in another, which is a way of corrupting memory while claiming to protect
it. It went.

**That is why joint travel is off by default again.** A crash costs the guest
ten points of illegal disconnect and there is no Bone of Order left in this
playthrough; the default has to be the path that costs nothing.

### What the morning round of 16/09 added

**Two assumptions of mine became proof, and the sequence more than doubled.**
The backread hook was writing into the map's owner — the force byte and **six
sixteen-byte blocks** of parts mask — without ever checking the object's
vftable, while the reading in the same file always checked. And the mask came
from the other player's copy through a chain (`parte → dono → info → conjunto`)
which also did not prove the owner was a map owner. Both now require proof.

The measured result, with real travels counted only when **both** step onto the
destination: from **four** legs before a crash to **ten**.

And a hypothesis fell, which is also worth having: the warning "not a
MapAreaCtrlOwner" **never fired**. The owner was always legitimate, so the
masks never ended up in a foreign object; what improved was the mask's chain,
not the target.

**The host has a crash of its own, and it repeats.** Twice (05:37 and 08:52),
always on the host, with the **same stack**: the `CharacterManager`'s
post-physics task (`FUN_140359e80` assembles the items) going down through
`FUN_140354e80` → `FUN_140314e90` → `FUN_140370bf0` → `FUN_14036dc50` →
`FUN_14036f800` → `FUN_140bd15b0`, where an expected pointer holds **floating
point data** (`a140a140a140a140`, then `3e2a256f2b7fe62c` — two floats each).
It is the animation chain, and the host is the one holding the guest's copy.

`FUN_140354e80` is the generic **task executor**: it calls the work
(`tarefa[2](tarefa[1], arg, tarefa+3)`) and then the completion (slot 0 of the
task itself). The watcher now reimplements it with the work under `__try` and
the completion **always** — a character loses a frame of animation instead of
everyone losing the session, and the task's bookkeeping still balances.

### That was it. Joint travel has been on by default since 16/09

Measured with two players, counting a leg only when **both** log "o personagem
esta nele" in the destination map:

| build | legs before a game closed |
| --- | --- |
| before the guards | 1 to 3 |
| map cycle guard | 4 |
| + type proof on the owner and on the parts mask | 10 |
| + task executor guard | **40, with no crash at all** |

Forty Heide↔Majula legs in a row, **no failed travel, no game closed, no
disconnect point spent** (Samuel 10, Chico 50 from start to finish),
`p2pSessionVerified: true` the whole time. Across them the host caught **59**
failures in the task executor and the guest 63 in the map's cycle, and both
carried on playing. Forty is where the test stopped, not where it broke.

The memory corruption **is still there** — the guards stop the dying, they do
not stop the corrupting. What changed is that the two places where it got as
far as being fatal are now covered, and the cost of a failure is one lost frame
of animation on one character. For anyone who wants to go after the cause: the
signature, the theories already discarded and the hardware write breakpoint
path are all just above.

**An idea drawn up and never written:** holding the other players' copies still
during the travel, since it is the copy that slides between maps without the
load that would rebuild it. With forty clean legs it did not justify itself,
and the price would be the other player frozen for a few seconds on screen. It
is noted as a possible lever if the crash comes back.

> **Corrected on 16/09.** This paragraph called it "ready" and cited
> `DS2_DeathIntercept::HoldCopies`. **That function never existed**: the name
> only appears here, in no commit under `Source/` (`git log -S` finds none).
> Found when gpt-6-astra was asked to look for it in the code. It also took the
> idea apart: hiding the drawing solves the appearance, skipping the pre-draw
> solves one consumer, freezing the position does not stop the resources from
> dying underneath the copy. Without a safe resume operation, freezing only
> postpones the moment of failure.

**Missing:**

- **the guest's crash after arriving, above, is the next job**;
- **the other crashes are not proven resolved**: each fix was measured once,
  against a failure that did not happen on every travel;
- **Majula → Heide through the vote** was not measured on the final build (only
  through `ir`, which does not exercise "host first"); the host's respawn record
  at the new bonfire was checked once (`0a040000/0000122a` after the travel);
- `NotLit` and `Busy` not measured: the guest's list only shows bonfires the
  host has lit, so `NotLit` only appears in a run;
- the notice box and the vote box are modal: somebody has to press;
- three or more players (one more Steam account): relaying the notice of a
  guest's rest to the others is not testable;
- resting with an invader in the world: the hook does not tell them apart.

- resting resets the world for everyone, with a notice first
  ("A player is resting at a bonfire");
- fast travel moves the group, with a vote;
- the Bonfire Ascetic does **not** synchronise: the intensity belongs to the
  host's world.

---

### The guest's object sync dies with the map the session began in (18/09)

The crash that followed the guest through every group travel for two days was
the object sync writing into freed memory. On a guest the sync binds once, at
the join, to the object table of the session's map (`FUN_140517880`), and only a
real load binds it again; travel here is not a real load. When the backread
lets that map go, the host's object packets keep arriving and `FUN_140518920`
writes each one into a block that has since become a `MapEntity`.

`DS2_BonfireInSession_ForgetSyncedMap` now drops the records (count at `+0xc`)
and closes the guest's rebuild gate (`+0x198`) as that map's release begins.

- **Superseded the same evening.** The backread now never releases the map the
  session began in (`DS2_BonfireInSession_IsSessionMap`), with the streaming cap
  raised from two maps to four, so the sync is never dropped and the host's enemy
  state keeps reaching the guest. 32 consecutive group travels over the four
  bonfires with the session up; see DS2_NATIVE_TRAVEL_PLAN.md, section 12. The
  "objects" were the enemy generators of that map.
- **Missing:** evicting the neighbour map the game streams in near Heide's first
  bonfire before forcing the destination, so a leg never needs four maps
  (docs/research/streaming-budget.md).

### What is still open in M8 (19/09)

Group travel works, including between the heavy DLC maps, and a long session
played by hand on 19/09 went through without a crash, a session drop or a
character left stuck. What is written down here is what is known to be missing
or unproven, for whoever picks this up.

**Cost and looks**

1. **A travel that has to make room takes about 15 s behind the black screen.**
   Most of it is fixed waiting: 3 s before the map left is chosen, 6 s with the
   player off it (so the map's objects stop their effects), 2 s after it is
   down. Those three came from single measurements; each could be replaced by a
   real condition (effects gone, release complete) instead of a clock.
2. **The loading screen is only black.** The fade is drawn over the front end
   (flag 1 of `FUN_14039a510`), so the area's art and name appear only on
   arrival. Putting the fade under the front end (`ctx+0x1168`) is untried.
3. **The travel's arrival uses the game's relocation lookup but not its
   facing or ground snap** (`FUN_1401cb1b0` yaw, `FUN_14041e3e0`). A character
   once ended up inside a wooden frame at Undead Refuge (reported 19/09); it
   did not happen again after the lookup was added, and the cause was never
   proven.

**The budget (see DS2_NATIVE_TRAVEL_PLAN.md §15-§17)**

3b. ~~**The streamer's own neighbour loads were not budgeted.**~~ Fixed on
   19/09, after both games died of it. At Threshold Bridge with Forest of
   Fallen Giants as the session's map (940 targets against Majula's 312), the
   streamer asked for `0a110000` (875) on top of 1406 in use and both hit the
   game's `out of memory` trap at `+0x1bee1c4`. The same leg had passed three
   times that afternoon over Majula, at 771 in use — which is why the failure
   depends on where the session started and looks random. `GateOwnerLoad` now
   clears the wanted byte of a map that would not fit and lets it in when
   there is room; after 3 s it takes the heaviest map nobody needs down. **The
   character can now reach a boundary whose map is held outside**, and what
   that looks like — a wall, a fall — has not been seen yet.
4. **The per-map target costs are measured on this save.** Another character,
   another world state or another DLC install can change them; a map never seen
   is assumed to cost 1400. The 150-entry margin is a guess.
5. **The chameleon limit of three maps is read and logged but not enforced.**
   Only the TargetManager decides whether a destination fits.
6. ~~**"NO ROOM, loading anyway".**~~ It fired on 19/09 and killed the host,
   exactly as this item said it would: Brume Tower (1126) asked for on top of
   941 in use, 25 s of making no room, loaded anyway, `out of memory` at
   `+0x1bee1c4`. The leg is now refused **at the vote** when the destination
   cannot fit beside the session's map, and the give-up path never loads over
   budget again.

   **What made it fire after a week of clean routes is worth keeping in
   mind**: the map the session began in is never released, so its cost is
   spent for the whole session. Every route that had ever been run started in
   **Majula, which costs 312**; this session started in **Forest of Fallen
   Giants, which costs 940**. That leaves 958 of the 1898, and Brume Tower
   needs 1126. Nothing in the travel code had changed. A budget measured from
   one starting point says nothing about another.

6b. **A heavy session map now makes heavy destinations impossible**, which is
   the honest consequence of the refusal above and not a good ending. The
   piece that would give those legs back is releasing the session's map while
   everybody is travelling — the game's join bindings assume it never
   unloads, so it needs the same care the rest of §17 took.

   Ten Ghidra readings and a plan in
   [docs/research/releasing-the-session-map.md](research/releasing-the-session-map.md),
   whose four measurements are now three done and one that cannot be
   scheduled:

   - the join controller's live shape — **done 20/09**. Host reads 0; the
     guest holds a live controller in state 7 whose `+0x19c` names the map
     the session began in and never moves, even with the guest standing in
     another one. `+0x19c` is the session's map and `+0x1a0` is the way home,
     separated by reading, with a session staged across two maps.
   - `mgr+0x3c6` across a leg — **done 20/09**, it never leaves 0, so the
     gate that defers the enemy table cannot get stuck.
   - `sync+0x08` / `+0x18` — **done 20/09**. The record array is built for
     the map the session began in and travel never rebinds it; the
     destination never gets one at all.
   - the `MapModelComponent` fault's `+0xd0` / `+0xd4` — **cannot be
     scheduled**; it needs the fault to happen again.

   That last one is why this is not started yet. Risk 4 is an unexplained
   crash in exactly the subsystem 6b rewrites, and it hit both machines 17 ms
   apart. Building nine steps of map-teardown code on top of it makes every
   crash during the work ambiguous, which is the trap §0 already paid for.
   The cheaper thing first is the **order**: the plan's finding 1 says vanilla
   is notify → wait → release and the mod does the reverse, which is a smaller
   change than 6b, improves the release path already in use every leg, and is
   the best candidate to explain risk 4 before 6b is written.
6c. **The party hook fired two summons 16 ms apart, and the second poisoned
   the first.** — **fixed on 20/09** (`ab9cee17`). A summon now claims a
   twenty second window and nothing else is summoned inside it, so the
   duplicate is logged and dropped; and because the host side was purely edge
   driven, one lost edge used to be final, so it now sweeps the sign
   collection itself every five seconds. A sign still in the collection is a
   summon that did not happen; a sign that was taken is gone from it, so a
   live session produces no sweep and no summon. The sweep reads the
   `SummonSignSetCtrl` the tick hands it, never a stored pointer, because a
   world reload rebuilds that object; the same handle twice backs off from a
   minute, doubling, because a sign nobody is standing behind fails into a
   dialog and a dialog eats the first button of any menu walk; and the quiet
   window is longer than the join grace, so a slow join is not re-summoned
   mid-join.

   The backoff is put back to its first minute as soon as the sweep sees that
   the handle it last summoned has left the collection, which is what a summon
   that worked looks like. Without that, a host that reloads twice would
   charge the next guest four minutes for two successes — the handles restart
   at `80000001` every time the collection is rebuilt, so the same number
   comes back meaning a different sign.

   Confirmed on 20/09: one `invocando a placa 80000001`, and
   `p2pSessionVerified: true` with no intervention, followed by two clean
   travel legs. Then the regression that the new sweep needed, on the
   hardened build: `session end`, the host quit to the title and re-entered,
   `retoma`. The sweep ran off the live controller through the reload, said
   `nada para revisar` — the right answer for a collection that had just been
   rebuilt empty, and the exact case that would have dereferenced the old
   cached pointer — and fifty seconds later found the guest's new sign and
   summoned it. A second session formed, verified. **The duplicate guard
   itself was not exercised**: no `ignorada; a invocacao ... ainda esta em
   curso` line appeared on either boot, which means the second `AddSign` never
   came, not that it was dropped. The positive signals here are the sessions
   forming and the sweep surviving a world reload. What follows is the
   measurement that found the defect. Measured repeatedly on 20/09: the server's poll says
   `1 signs cached, sent 1`, and the host's log shows

   ```
   14:36:39.910  host: invocando a placa 80000001 do parceiro ...
   14:36:39.926  host: invocando a placa 80000011 do parceiro ...
   ```

   two handles for the same guest, 16 ms apart, neither carrying the
   `(revisao ao retomar)` suffix — so both come from `AddSignHook`, not from
   `RescanSigns`. The handles differ by `0x10`, the collection's generation
   counter, so the same sign is being added twice. `ConsiderSign` summons
   **every** white sign it is handed, with no "one at a time" and no "a
   summon is already in flight".

   The three failures that follow are all the same cause, and all three were
   seen on 20/09: *"Player was unable to join multiplayer session"*,
   *"Someone has already used this summon sign"* and *"The sign has
   disappeared"*. Worse, the dialog that results **stops the hook for good**:
   it never summons again, and neither `retoma` nor `reload` restarts it —
   only a full `down` plus `up`, which then often fails the same way on its
   first attempt.

   This made the bench unusable for most of 20/09 and blocked the M8 6b
   measurements.
7. **A guest's copy is judged to have left a map by distance** (40 m from the
   parking bonfire). A copy that does not get there keeps the map, and the
   travel waits the full 30 s.

**Effects, lighting and lists the game does not clean**

8. **Only a DLC map's teardown kills effects** (the ids of its own sfx bank,
   plus the handle-unlink detour). Whether a base-game map's teardown ever
   leaves an effect running has not been measured.
9. **The lighting sweep runs only while a map is being taken down.** Map
   objects and parts also cache lighting entries (`FUN_1401c4aa0`,
   `FUN_1403f4f60`, `FUN_1403f54a0`); they die with their map, so they are
   believed safe, and that is not proven.
10. **The sign-area purge was found by its symptom.** The game's purge for a
    released map is a bare `ret`, and nothing says that list is the only one
    the game leaves behind; no audit of the other per-map registries was done.

**Not tested**

11. **Shulva (`0x32230000`) and the other DLC maps** have never been travelled
    to; only Brume Tower and Frozen Eleum Loyce.
12. **A session begun somewhere other than Majula.** The session's map is held
    for the whole session and is where everyone waits while room is made.
13. **Three or more players**, which this machine cannot do.
14. **The "duty fulfilled" guard** (a phantom sent home when an area's boss is
    dead) fired once on 18/09 and has not been exercised since.
15. **A boss killed during a session**, which is what M6 needs and what would
    exercise 14 for real.

## M9 — spectator and party wipe

A death inside a boss fight becomes spectator mode; everyone dead is a party
wipe with the boss reset and the session alive; the victory of whoever is left
counts for whoever died earlier. Depends on M1 and M2.

**Not fully testable here**: a spectator with two alive and a party wipe of
three need three players, and this machine has two Steam accounts.

---

## M10 — NPC quests

Lucatiel, Benhart, Pate, Creighton, Navlaan, Cale, Gilligan, Carhillion,
Felkin. State follows the host during the session; whatever happens together
synchronises. The most expensive item on the list and the easiest one to
corrupt a save with — leave it for last, behind M7.

---

## M11 — invasions, covenants, Company of Champions

- `allow_invasions` turns on invasion against the group (3v1, or more than one
  invader);
- a covenant is individual and does not change on joining;
- the host's Company of Champions imposes the difficulty on everyone without
  changing anyone's covenant;
- enemy despawn follows the host and is not marked in the save of whoever
  joined.

---

## What blocks, and it is not a lack of work

- **Three players.** Two Steam accounts on this machine, a peer-to-peer session
  keyed on the Steam id. Everything about 3+ players goes unverified locally.
- **Addresses tied to one version.** Everything here is 1.03 Calibrations 2.02.
  A game update moves everything; that is why every hook checks the bytes
  before writing.
