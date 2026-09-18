# Rematch after death

What happens between a PvP death and the same pair's next invasion, measured
on this machine's two instances (Samuel, account 1; Chico, account 2), in
Heide's Tower of Flame, local server.

This document exists to answer one design question: **how much of the way
back can the server walk on its own?** The short answer is "almost none", and
the reason is the item.

## The server cannot start an invasion

It was already recorded in [DS2_STICKY_SIGNS.md](DS2_STICKY_SIGNS.md) and
still holds: a fabricated `PushRequestBreakInTarget`, sent without the invader
having used the orb, produces nothing — not in Majula, not in Heide, which is
the control where an ordinary invasion works.

> An invasion needs the **invader's own** client to be in an invading state.
> Nothing the server sends fakes that.

So an automatic rematch is not a server feature. The server can make the
pairing deterministic; whoever restarts the session is the invader's client.

## Using the orb requires human form

With the character **hollow**, pressing X with the Cracked Red Eye Orb
selected in the belt does absolutely nothing: no animation, no message on
screen and no request reaching the server. It looks like a button that does
not work, and that is how it cost time.

The control that separates the two possible explanations:

| state | place | X with the orb |
| --- | --- | --- |
| hollow | right on the bonfire | nothing |
| hollow | away from the bonfire | nothing |
| human | away from the bonfire | "Attempt to invade another world?" |
| human | **at the same spot**, right on the bonfire | "Attempt to invade another world?" |

And the Red Sign Soapstone is the same, measured 12/09 with the same control:
hollow, pressing X places no sign at all; a Human Effigy without the
character taking a step and the sign goes down at once (`Sign 1002 created`).
**Human form counts for both items.**

The last row is the one that matters: the effigy was used without the
character taking a step, and the dialog started appearing. The blocker is
human form, not the bonfire.

## But dying as an invader does not take human form away

This is the finding that changes the project, and it nearly went unnoticed
because the first reading of the data was wrong.

What hollows you is dying **in your own world**. A death as an invader,
inside the host's world, sends the player home **still human**.

The confusion: in the first round the invader died as a phantom, came back,
and the orb did not work — but between those two things it had fallen off a
cliff **in its own world**, and it was that second death that left it hollow.
The next effigy looked like what fixed the duel death. It was not.

**Caveat, from 12/09:** on one night of testing the same player came up
hollow after deaths that only happened as a phantom, and needed an effigy to
put the sign back down. Either there is a case where a phantom death hollows
you, or one of the deaths in that sequence was in its own world without my
noticing. Both observations are recorded on purpose; what decides it is an
isolated round, with the character human, a single death as a phantom and the
item tested right afterwards.

The clean measurement, with no effigy involved:

    01:42:17  3:Chico   last position inside the host's world
      (death by falling, as a phantom)
    01:42:52  3:Chico   Location: back at its own bonfire
    01:42:54  3:Chico   Break-in target request: target 1
    01:42:54  server    Invading '1:Samuel' across areas.

Two invasions in a row, the second two seconds after getting home, with no
Human Effigy in between. The effigy counter did not move.

**Consequence:** the rematch costs no item beyond the orb itself, and the
human form requirement does **not** need to be removed for the invader. It
would only show up if the same player also died in their own world.

## The full rematch cycle, measured

    death as an invader                 t+0       (±10s: the fall is only
                                                   visible on screen, and the
                                                   last position in the host's
                                                   world is at 01:42:17)
    LeaveSession                        t+12s     (measured in the first round)
    back in its own world               ~t+35s
    X, left, A                          ~t+37s
    invasion request at the server      ~t+37s
    phantom in the host's world         ~t+55s

| stretch | who controls it | can it be shortened? |
| --- | --- | --- |
| death → LeaveSession, 12s | the dead player's client | only by touching the client |
| loading back, ~20s | loading | no |
| **three button presses, ~2s** | **the player** | **this is what the automation removes** |
| pairing + loading, ~20s | network and loading | no |

Worth saying plainly: out of roughly 55 seconds, the player accounts for
**two**. An automatic rematch does not make the duel faster; it takes the
player's attention out of the loop. Anyone wanting to cut the rest would have
to attack the 12s wait or the end of the session itself — make the phantom
respawn in the host's world instead of going home, which is how the arena
works.

## The positive signal, for the orb path

The measurements above end in a phantom visible on screen, which is the weak
evidence this project keeps warning against. The full handshake was recorded
on 12/09, on a freshly restarted server — which matters, because the census
only records the **first** message of each type per client, and that is why
these lines vanish in a long session:

    02:21:02  3:Chico   Break-in target request: target 1
    02:21:02  server    Invading '1:Samuel' across areas.
    02:21:12  1:Samuel  First RequestNotifyJoinGuestPlayer
    02:21:14  3:Chico   First RequestNotifyJoinSession

These are the two lines CLAUDE.md calls the success signal: ten seconds
between the request and the guest inside the world.

Worth noting what nearly ruined that round. For a while **no** session would
form, neither by orb nor by sign, because there were two games open on the
same account. Everything before the end looked right — the server routed the
push, the target answered `RequestSendMessageToPlayers` — and then one side
said "Summoning failed. Timed out." and the other "Disconnected from
multiplayer session." Closing the extra client fixed it immediately.

## Quitting to the title is refused during a session

The **Quit Game** entry shows in the system menu and stays selectable, but
pressing A does nothing while a PvP session exists — on both sides, host and
phantom. That is how `game leave` failed for 180s without saying why.

From what was seen here, that leaves death, the timer and disconnecting. The
leave items (Separation Crystal, Homeward Bone) were not tested.

## The summon push is accepted — and the session still does not form

Measured 12/09 with the `debug_summon.req` trigger and the pair having
already duelled once (it is the previous duel that leaves the host's id and
the opaque blob it sent on the server).

    02:22:16  1:Samuel  Summoning sign 1003              the real duel
    02:25:27  3:Chico   Sign 1004 created: type 4        the sign back on the ground
    02:25:43  server    Rematch: replayed the summon of sign 1004 by player 1
    02:25:43  3:Chico   Sign 1004 removed by its owner
    02:25:5x  3:Chico   "Summoning canceled. Unable to join multiplayer session."
              1:Samuel  nothing. no dialog, no message

The two middle lines are the good part, and it is a new result: **a client
with a sign on the ground acts on a push nobody asked for.** It takes the
sign back and tries to join, which is exactly what it does on a legitimate
summon. The fabricated invasion push does not even produce that — an invader
standing still is not waiting for anything.

The bad part is the last line. The host never knew a thing. Whoever opens the
session is the host's client when it presses the button on the sign; the
server cannot create that state, and replaying the stored `player_struct` is
not enough — either it is single use, or the listening side is missing. The
phantom tries to connect to a host that is not expecting anyone, and the game
says so in as many words.

**What this means for the feature:** the rematch by red sign also needs a
client patch, only on the other side. On the orb path it is the invader who
has to act; on the sign path it is the **host**, which would have to reissue
the summon of the same pair's sign on its own. The server does its half: it
remembers the pair and knows how to resend the push.

An intermediate version that needs no patch at all: the phantom puts the sign
back down (one press), the host presses A on it (one press). Nothing is
consumed, and the sign reappears in the same place, next to the host.

## Which push the host obeys, and which it ignores

Three pushes, the same host standing in the same place, measured 12/09 on the
local server. What changes between them is only who receives it and what the
other side is doing:

| push | to whom | what happens |
| --- | --- | --- |
| `PushRequestBreakInTarget` | idle host | **the host reacts in 1s**: sends `RequestSendMessageToPlayers`, and if the invader is waiting the session forms |
| `PushRequestVisit` | idle host | **nothing**, on all four types (0 Blue Sentinels, 1 Bell Keepers, 2 Rat, 3) |
| `PushRequestSummonSign` | phantom with a sign on the ground | the phantom takes the sign back and tries to join, and fails because nobody opened a session |

The control ran in the middle of that, right after the four ignored visits:
an ordinary orb invasion, `RequestSendMessageToPlayers` at 02:48:24,
`RequestNotifyJoinGuestPlayer` at 02:48:34 and `RequestNotifyJoinSession` at
02:48:36. The machine was forming sessions; it is the visit that does
nothing.

The likely reason the visit is ignored: it is the covenants' mechanism, and
the host's client checks whether a visit **of that type** is legal where it
is. Heide is neither Bell Keeper nor Rat territory, and the zone patch forces
the activity area to 103110 anyway. It was not investigated further, because
the result was already enough to rule the path out.

**What this teaches about the rematch:** the server has the two halves apart
and they never meet. The invasion push opens the session on the host; the
summon push makes the phantom accept. What is left is to prove they serve
each other — that is the next experiment, and it is cheap.

## No combination of pushes closes the session

After a real red sign duel — with the host's blob stored, the host human and
the phantom putting the sign back down — the server tried to reconnect on its
own in three ways. Measured 12/09, always with the pair in the same place:

| attempt | phantom | host | result |
| --- | --- | --- | --- |
| summon only | takes the sign back, tries to join | nothing on screen | "Unable to join multiplayer session" |
| visit only | — | **inert**, on all 4 types | nothing happens |
| invasion only | — | opens a session: `RequestSendMessageToPlayers` in 1s, and its own sign disappears ("Your summon sign has disappeared") | nothing happens |
| invasion + summon, on the same tick | takes the sign back, tries to join | opens a session | fails |
| invasion, 4s, summon | takes the sign back, tries to join | opens a session | fails |

Both halves exist and **do not meet**. The simplest reading is that they are
not the same session: the host opens an *invasion* session and the phantom
tries to join a *summon* one. The game has no push that tells a client "you
summoned so-and-so" — that state is born when the player touches the sign,
and only then.

A detail that came for free: the host **refuses a second invasion push**
while the first is pending, with `RequestRejectBreakInTarget`, reason 1.

### The blob is not interchangeable

The `player_struct` the summon carries has to be the one the host sent **in a
summon**. I tried lending it the blob the same player had sent when creating
a sign of its own: the phantom ignored the push entirely, it did not even
take the sign back. With the genuine blob it acts every time. So a rematch by
sign is only possible for a pair that has already duelled once — which, for
the requested feature, is exactly the case.

### A hollow host sees no sign at all

It took an hour to show up. With the character hollow, the other player's red
sign simply **does not exist** in its world: no prompt, nothing on the
ground, and the server delivering the sign in the list normally. One Human
Effigy without the character taking a step and the "Touch Summon Sign" prompt
appears at the same spot.

It mirrors what was already measured for the orb, and together they make the
rule: **human form is required on both sides of a duel by sign** — of the one
summoning, to see the sign, and of the one placing it, to use the item.

## Where the client sends each sign message

Found by searching for the protocol ids as immediates (`FindImmediate.java`,
in `/home/suel/tools/scripts`). All of them land in the same neighbourhood as
the invasion sends, which reinforces that this is the game's network layer
and not gameplay code:

| function | immediate | message |
| --- | --- | --- |
| `FUN_1406a2610` | `MOV EDX,0x398` | `RequestSummonSign` — **the host's action**, the A button on the sign |
| `FUN_1406a24f0` | `MOV EDX,0x396` | `RequestRemoveSign` |
| `FUN_1406a1170`, `FUN_1406a1de0` | `MOV EDX,0x394` | `RequestCreateSign` |
| `FUN_1406a0910` | `MOV EDX,0x39b` | `PushRequestSummonSign` |
| `FUN_1406a1840` | `SUB EBX,0x39b` | dispatch by id |
| `FUN_1406a6300` | `MOV EDX,0x3d2` | `RequestGetBreakInTargetList` |
| `FUN_1406a6fb0` | `MOV EDX,0x3d3` | `RequestBreakInTarget` |

`FUN_1406a2610` is the target if the rematch needs a client patch: it is the
send that only happens once the host touches the sign.

## The client's path when the host touches the sign

Mapped on 12/09 with the breakpoint sweep (see
[DS2_INVESTIGATION_TOOLS.md](DS2_INVESTIGATION_TOOLS.md)): 520 entries armed
in the range `0x140270000–0x1402a0000`, the game left alone for fifteen
seconds so that whatever runs per frame disarms itself, the log wiped, and
then the touch.

Confirming the "Summon this dark spirit?" dialog leads to a class with a real
name, from the RTTI:

    FUN_1402a5970   constructor of NetSvrSummonSignSummonJob
                    vftables at 0x1410d63a8 and 0x1410d6448

And, unlike everything else on this path, it **does have a static call
graph**:

    FUN_1402a14c0  ->  FUN_1402a41b0  ->  FUN_1402a2ca0  ->  FUN_1402a5970

The top is small enough to fit here:

```c
void FUN_1402a14c0(undefined8 param_1, undefined4 *param_2)
{
    undefined4 local[6];
    local[0] = *param_2;
    FUN_1402a41b0(param_1, local);
}
```

`FUN_1402a41b0` takes the sign manager (`FUN_1402128d0`), resolves the
**dword** it received through virtual slot `+0x98` — that is, the argument is
a *sign handle*, not the sign — and, passing the checks, builds the job.

**This is the entry point the rematch needs.** Both arguments were captured
at runtime, with `bp 2a14c0 deref rdx 4`, on two different summons:

    summon of sign 1000    rcx=0x7ffffe591000  [rdx]=25000080  -> handle 0x80000025
    summon of sign 1003    rcx=0x7ffffe591000  [rdx]=45000080  -> handle 0x80000045

Two things are settled:

- **`param_1` is the `NetSvrSummonSignManager`**, and the pointer was the
  same on both summons — including after closing and reopening the game. A
  hook can hold on to it safely within a session.
- **The handle changes with every sign.** 0x80000025 and 0x80000045 for
  different signs from the same player, in the same place. The high bit is a
  tag; the rest looks like an index plus a generation. So **replaying the
  stored handle does not work**: the hook has to find the new sign's handle,
  by enumerating through the manager or by intercepting the point where a
  sign is registered.

That is the rematch's next real blocker, and it is a small one:
`NetSvrSummonSignInterface` has a `GetSummonSignListJob`, and the manager
resolves handles through virtual slot `+0x98`.

## The rematch working, on the host's side

12/09, 05:32–05:34, local server, with `DS2AutoRematch` on on both sides:

    05:32:06  1:Samuel  Summoning sign 1000            the normal duel, the player touched the sign
              hook      "o jogador invocou a placa 80000015"
    05:33:03  3:Chico   dies by falling in the host's world
    05:33:2x  host      DS2_Rematch.req -> "revanche armada"
    05:33:37  3:Chico   Sign 1001 created              the phantom puts the sign back down
    05:33:40  hook      "revanche: invocando a placa 80000025 que acabou de chegar"
    05:33:40  1:Samuel  Summoning sign 1001            <- with nobody touching anything
    05:33:46  3:Chico   Sign 1001 removed by its owner  the phantom being pulled in
              screen    Chico back as a red phantom in Samuel's world

The host pressed nothing. The detour on "add sign" saw the new sign arrive,
read the handle from the out parameter (`0x80000025` — different from the
first duel's `0x80000015`, as expected) and called the same path the A button
calls.

**This is the missing half.** The server knows how to remember the pair, and
now the host's client knows how to start over.

### The canonical signal, with the census cleared

The round above happened on a server that had already spent its "first of
each type" on the join messages, so it does not prove anything on its own.
Repeated at 05:39 right after a `reload`:

    05:39:13  3:Chico   Sign 1000 created
    05:39:37  hook      "revanche: invocando a placa 80000015 que acabou de chegar"
    05:39:37  1:Samuel  Summoning sign 1000
    05:39:43  3:Chico   Sign 1000 removed by its owner
    05:39:47  1:Samuel  First RequestNotifyJoinGuestPlayer
    05:39:49  3:Chico   First RequestNotifyJoinSession

The last two lines are the signal this project demands, and they show up
after a summon **no player asked for**. Twenty-four seconds between the sign
going down and the session forming, twenty of which are the client's sign
poll interval — that is what can be shortened later, if it is worth it.

### What is still not automatic

The rematch is armed by a request file (`DS2_Rematch.req`), not by the death.
The real trigger is the client noticing that the session ended in a death,
and the path to that is already mapped in
[DS2_SESSION_END_CLIENT.md](DS2_SESSION_END_CLIENT.md): the session state goes
to 8 and reason `2` reaches the end request. What is missing is wiring one to
the other.

And there is still the question of when **not** to rematch: the pair moved
away, the player wants to stop, or the sign that arrived belongs to somebody
else. With two players on the server the sign that arrives is always the
pair's; with more, the hook needs to identify the owner, which is the item
field that has not been read yet.

## The closed cycle, with nobody arming anything

Last round, 12/09 at 06:04, with the build where the summon itself turns the
rematch on:

    05:58:11  1:Samuel  Summoning sign 1003          the only human touch
              hook      "o jogador invocou a placa 80000015; revanche ligada"
    06:03:xx  the session ends
    06:04:05  3:Chico   Sign 1004 created            the sign goes back on the ground
    06:04:58  hook      "revanche: invocando a placa 80000025 que acabou de chegar"
    06:04:58  1:Samuel  Summoning sign 1004
    06:05:04  3:Chico   Sign 1004 removed by its owner
    06:05:11  3:Chico   First RequestNotifyJoinSession

The `JoinSession` on the last line comes from a client that had just been
restarted, so its census was clean and the line is honest.

Between the first summon and this one, **nobody pressed anything on the
host**.

## How to turn it on, and what the feature is today

Two pieces, one on each side, and both are born off:

| where | flag | what it does |
| --- | --- | --- |
| server | `DS2_AutoRematch` | remembers each sign owner's last summon and knows how to resend the push; on its own it does **not** form a session |
| client | `DS2AutoRematch` | the `DS2_RematchHook`: summoning once turns the rematch on **with that sign's owner**, and every new sign from the same player and the same type is summoned on its own |

In the harness: `ds2os-dev up --auto-rematch`, or
`ds2os-dev game prepare --auto-rematch` and relaunch (the config is read at
injection).

The hook writes to `DS2_Rematch.log`, next to the DLL. Writing `0` to
`DS2_Rematch.req` turns it off; `alvo <player id> <type>` arms it with that
opponent; anything else turns it on with the remembered opponent (and, with
none, summons nothing).

**What it is today:** after a duel by red sign, the phantom that came back
puts the sign down and the host summons it on its own, with nothing pressed.
Twenty-four seconds between the sign going down and the session forming,
twenty of which are the client's sign poll interval.

**The pair only.** Until 14/09 the hook summoned any sign that arrived: with
three players, a stranger's — red, white, any of them — would enter the
host's world with nobody asking (raised in the review of PR 6). Now it
remembers, by handle, the owner and the type of every sign that goes through
`AddSign` (`FUN_140213160`), and the summon the player makes fixes that pair
as the opponent.

The fields, measured on a red sign with a breakpoint at `+0x213234`, where
the entry has just been filled in (`rdi`):

| entry | `AddSign` argument | measured value |
| --- | --- | --- |
| `+0x20` | 5th | 1006, the sign's id on the server (changes with every sign) |
| `+0x24` | 6th | 3, Chico's player id (stays) |
| `+0x28` | 3rd | 7, the type on the client (the white one arrives as 1; on the server the red one is 4) |
| `+0x38` | 7th | the steam id in hex, `0110000140d6d6d1` |

Measured 14/09 with DLL `04b5114f`, Samuel hosting and Chico human, signs
from the belt and from the inventory, and each duel ended by the spirit's
death:

| step | `DS2_Rematch.log` | server |
| --- | --- | --- |
| Samuel summons red sign 1007 | `invocou a placa 80000025 (jogador 3, tipo 7); revanche ligada com ele` | `Summoning sign 1007` → `JoinGuestPlayer` → `JoinSession` |
| Chico puts down a **white** one (1008) | `é do jogador 3 tipo 1, a revanche é com o jogador 3 tipo 7; ignorada` | the sign delivered, no `Summoning` |
| Chico puts down the **red** 1010 | `revanche: invocando a placa 80000045 do jogador 3` | `Summoning sign 1010` → `JoinGuestPlayer` → `JoinSession`, nobody pressed anything |
| `alvo 99 7`, and Chico puts down red 1011 | `é do jogador 3 tipo 7, a revanche é com o jogador 99 tipo 7; ignorada` | `sent 1` on Samuel's poll, no `Summoning` |

A real third player is still untested on this machine; `alvo` is the honest
way to show the refusal by owner with two accounts.

## What this leaves as a project

In order of value:

1. **Using the orb on its own** when the session ended in a death and the
   pair is still up (client). It is the requested feature, and it is small
   now: no effigy needed, no need to touch human form, just fire the same
   path the three presses fire. It saves two seconds on the clock and all of
   the player's attention.
2. **Not ending the session on death** (client). It is the only path that
   really cuts the ~50 seconds, and it is much deeper: the phantom would have
   to respawn in the host's world instead of going back to its own.
3. **Remembering the pair on the server**. With two players the target list
   already has only one candidate; this only starts to matter with three or
   more, which this machine cannot test.

The entry point for (1): the sends are at `FUN_1406a6300`
(`RequestGetBreakInTargetList`, 0x3d2) and `FUN_1406a6fb0`
(`RequestBreakInTarget`, 0x3d3), found by searching for the protocol ids as
immediates. `getCallingFunctions` found no caller for either — which in a
project opened with `-noanalysis` does not prove there are none. If the call
graph does not lead to the X button, the way in is a runtime breakpoint
(`DS2_Trace.req`) on `FUN_1406a6fb0`, which is the send that only happens
**after** the YES; the target list one may go out earlier, so the game can
decide whether to show the dialog.

## The rematch also serves co-op, and that was not expected

Measured 12/09, without touching anything: the guest placed a **white sign**
(`Sign 1002 created: type 1`, against the red one's `type 4`) and the host's
`DS2_RematchHook` summoned it on its own —
`revanche: invocando a placa 80000031 que acabou de chegar` — followed by
`Summoning sign 1002` on the server and by `RequestNotifyJoinGuestPlayer`
with `RequestNotifyJoinSession`.

The hook does not look at the sign's type: it reacts to any sign that enters
the client's registry. For co-op that closes half the loop for free — the
guest puts the sign down and the host pulls it in with nothing pressed,
including after a death. The half that is missing is the sign putting itself
back down on the guest's side.

Also worth noting what this implies about hollowing: the **white** sign goes
down with the character hollow (that is how the measurement was made), while
the red one, on the same character and at the same spot, does not. The two
items do not have the same rule.

## Not measured yet

- Whether the Red Sign Soapstone can be used hollow. The test that seemed to
  prove it could was done with the character human without my knowing, so it
  is worth nothing. The orb is the only item measured.
- Whether a **host** that dies can still be invaded without doing anything.
  It hollows, but nobody needs human form to be invaded.
- Whether death by falling and death by kill produce the same chain: here the
  fall showed no `RequestNotifyDeath`, but the server's census only records
  the **first** message of each type per client, so that is not evidence.
- The arena (`DS2_QuickMatchManager`, implemented on the server) is the
  game's own rematch loop, on fixed maps and with per-match registration. It
  has never been exercised in this fork.
