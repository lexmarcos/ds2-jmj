# Travel by native loading (option D): the plan

Survey done on 16/09 by a Fable agent, in Ghidra `-readOnly` and `objdump`,
without running anything in the games. Version 1.03 Calibrations 2.02.

It is the answer to the decision recorded in `DS2_SEAMLESS_COOP_TASKS.md` to
redo travel from scratch, after three distinct families of crash proved that
forcing the map alongside and teleporting is unsafe by construction.

Read it with `DS2_PRESENCE_ASTRA_REVIEW.md` (which defines options A to D) and
`DS2_PRESENCE_REBUILD_PLAN.md` (the presence primitives).

## The conclusion that decides the direction

**Pure D — native loading on both sides preserving the same session — is not
available today**, and for two independent reasons. **The first of them was
measured on 16/09 and fell** (see the box in phase 1); the second still stands:

1. **The host's native travel probably ends its own session.**
   `FUN_1402bd0d0` (slot `+0xe0` of the host ctrl) is called by `FUN_1402c7ec0`
   on every warp from that machine, with the reason. **Case 2** — which is the
   host's menu travel — does:

       *(ctrl+0x1b8) |= 2;
       if (3 < *(int*)(ctrl+0x30) - 1U) {     // ou seja, (ctrl+0x30) >= 5
           FUN_1402be230(ctrl, 0x15);          // manda 0x15 ao par
           FUN_1402be1e0(ctrl, 9);             // +0x150 = 0x12: sessão encerrada
       }

   `ctrl+0x30` is the role of the summoned guest, written by `FUN_1402bd560`
   from the join message, indexed into the table of 20 roles at
   `0x1410c0050` — where the **white phantom is index 7**. If it is 7, `3 < 6`
   is true and the session dies before the loading starts.

   **This was never exercised**: M8 only makes the host travel natively after
   the guests have left legally, so case 2 has never run with a live guest.

2. **The guest's native re-entry was already blocked** — the `0xd` packet is
   only accepted by the host at `0xe` and by the guest at `0xc`, and an
   established session vetoes it (see `DS2_PRESENCE_REBUILD_PLAN.md`).

So D closes in one of two ways, and neither is "just swapping the transport":

- **D-3a:** the host travels natively, **the guest is re-summoned** (a new
  session at the destination). Fully native, zero dangling pointers, already
  proven end to end. It costs the summoning ritual (~75 s) and is not seamless.
- **D-3b:** the host travels natively, the guest does a native load **and the
  mod rebuilds the presence** — that is, **D = native loading + option A**.
  It preserves the session; that is where the real cost is.

## 1. The native path

Every warp goes through `FUN_1401c2a80` (`+0x1c2a80`), slot `+0x40` of the
global context `*(0x1416148f0)`. **Bonfire travel** is the chain above it:

| step | function | offset |
| --- | --- | --- |
| the menu picks the bonfire | `FUN_14017fdb0` | `+0x17fdb0` |
| builds the request | `FUN_1401843b0(&req, id, motivo)` | `+0x1843b0` |
| starts the travel | `FUN_140184830(travel, &req)` | `+0x184830` |
| phase machine | `FUN_140184a10(travel)` | `+0x184a10` |
| **the warp** | `FUN_1401c2a80(ctx, &req, flag)` | `+0x1c2a80` |
| writes the respawn point | `FUN_14044fe30` | `+0x44fe30` |

Prologues checked against the executable:

    +0x17fdb0  4c 8b dc 56 48 81 ec 10 01 00 00 48 8b
    +0x1843b0  48 89 5c 24 08 48 89 74 24 20 57 48 83 ec 60
    +0x184830  40 53 48 83 ec 60 8b 02 48 8b d9 89 01
    +0x184a10  40 53 48 83 ec 20 48 8b d9 48 8b 0d
    +0x44fe30  48 89 5c 24 10 57 48 83 ec 60 83 7a 04 01
    +0x1c2a80  40 55 41 54 41 56 48 8d 6c 24 e0 48 81 ec
    +0x2bd0d0  83 fa 05 0f 87 4b 01 00 00 53 48 83 ec 20

**Fields of the request:** `+0x00` the destination family (3 bonfire, 4 own
world, 2 player-start), `+0x04` the **reason** (1 death, 2 travel, 4
entry/return), `+0x08` the map id, `+0x18` the spawn point.

**The reason gate** in `FUN_1401c2a80`: it accepts if `ctx+0x24ac == 0x1e` and
`!(ctx+0x24b1 & 2)`; after that reason 1 always passes, reason 4 with flag 0
passes with no gate, and reasons 2 and 4-flag-1 go through the gate
`FUN_140248940`, which is `*(mgr+0x168) > 0` — the multiplay time counter,
positive in a live session.

The **host** travels with reason 2 flag 0. **A guest entering the host's
world** uses reason 4 flag 1, and that form is **only built by the session's
state 2 handler** from a network payload — not by the bonfire chain.
**A guest going home**: reason 4 flag 0.

**Why native loading solves the corruption:** the loader at `ctx+0x24ac`
goes through state `0x14`, which destroys the map and the characters and zeroes
`ctx+0xd0`, and through `0xb`, which recreates them. The world is rebuilt, not
slid across, so the three families of crash have nowhere left to exist.

## 2. The borrowed world has four components, and the warp rebuilds one

| component | where it lives | does a flag 1 warp rebuild it? |
| --- | --- | --- |
| the "in another world" bit | `ctx+0x24b1 & 0x40` | **Yes** |
| the go-home record | `session+0x1a0..+0x1c8` in the join ctrl | Not rewritten; it survives as long as the object exists |
| the host's world snapshot (flags, bonfires, objects) | imported by `FUN_1402c2fa0`, **only in join state 4** | **No** |
| remote presences (the other player's copy) | registered in **join state 5**, materialised by `0xd→0xe→0xf` | **No** |

This explains attempt 4 once and for all, where the guest went back to its own
world: reissuing just the warp moves the player, turns the bit on, and remakes
neither the snapshot nor the presence.

**A warning worth gold:** the guest must **never** call `FUN_14044fe30` —
it writes the **local** player's respawn record, which goes into the save and
into the go-home record. A guest that writes the host's bonfire there
contaminates its own save. Only the host writes.

## 3. The watchdogs — there are two

**300 s** (`FUN_1402be090`, `DAT_1410d7b40 = 300.0f`, bytes `00 00 96 43`):
it fires with bit `0x10` of `ctrl+0x1b8` armed and `ctrl+8 - ctrl+0x1b4 > 300`.
Case 0 of `FUN_1402bd0d0` arms **and refreshes** `+0x1b4`; case 4 arms and
**does not refresh**. Handling inside the transaction: save `+0x1b8` and
`+0x1b4`, refresh `+0x1b4 = ctrl+8` if the bit is armed, restore at the end. It
is a data write into a per-frame field, reversible — do not switch it off
globally.

**~23 s, the silence one** — and this is **the most likely killer**. Measured
in attempt 9: the host dropped a silent guest ~23 s after it stopped sending.
During a native load the guest has `ctx+0xd0 = 0` and sends nothing for
seconds. It ranks ahead of the 300 s one.

## 4. What survives

| piece | survives | how it changes |
| --- | --- | --- |
| the `Idle/Moving/Arrived/Failed` contract | yes | `Arrived` now comes from the **end of the native load** (`ctx+0x24ac` back to `0x1e`, `ctx+0xd0 != 0`), not from the streamer. "Never by time" stays |
| barrier + `TravelRelease` + host first | yes, intact | the receipt's trigger changes; host first becomes **more** necessary, because the guest imports the host's snapshot |
| type proofs | yes | |
| the `__try` guards | yes, as a net | they must from now on **never** fire; a firing becomes a sign that the load did not happen |
| `DropDeadRigidBody` | becomes a **gauge** | it must stop firing |
| the channel between the machines | yes, central | it carries the transaction and, in D-3b, the presence blob |
| the transport (forced backread, teleport, contact) | **no** | it goes entirely; it is the source of the corruption |
| the curtain we draw ourselves | **open** | it probably becomes the game's own screen; `FUN_140b06270` is inert outside a real warp and **has not been tested inside one** |

## 5. The phases

> ## Phase 1 measured on 16/09: **passed**
>
> A live, verified co-op session, without travelling. The host's controller
> (`0x7ffffe5bf120`, vftable `0x1410d7998`, `+0x150 = 0x10`) has
>
>     ctrl+0x30 = 1
>
> The static prediction was 7. **It is wrong.** With 1, case 2's condition is
> `3 < (unsigned)(1 - 1)` = `3 < 0` = false, and the branch that sends `0x15`
> to the peer and ends the session **does not run**. The host's native travel
> **does not kill the session**.
>
> The arithmetic was checked in the binary and not only in the report's
> decompilation: in `FUN_1402bd0d0`, `param_1` is a `longlong*`, so
> `param_1[6]` is the byte `+0x30` and `param_1+0x37` as a `uint*` is `+0x1b8`.
> It checks out. And the other side of the comparison is worth recording: it is
> true for `x >= 5` **and also for `x == 0`**, which wraps in unsigned — so
> zero is as dangerous as seven.
>
> The scan by vftable returns two addresses; the second (`0xa38fb08`) is a
> false positive — its `+0x150` is a pointer, not a state.
>
> **Scope of this measurement:** one sample, one guest, a white phantom
> summoned by the party system, right after the summon. It does not cover an
> invader or other roles, and it was not observed across a whole session.
>
> **Consequence:** D-same-session does **not** need the host-side hook that
> suppresses the session end. That risk, which was the most expensive in the
> plan, is out. The path moves on to phase 2.
>
> **As a bonus, for phase 2:** on the same controller, `+0x1b8 = 0x261` — bit
> `0x10` is **not** armed — and `+0x1b4 = 0.0` with the clock `+0x08` at
> 65.9 s. That is, `+0x1b4` was never refreshed; if something arms that bit
> after 300 s of session, the watchdog fires **immediately**, with none of the
> 300 s of slack.

**Phase 1 — zero cost, and it is the one that can kill D entirely.** Read
`+0x30` of the host ctrl (`NetSummonAcceptMultiplayCtrl`, vftable
`0x1410d7998`) **in a live co-op session, without travelling**. If it is >= 5
(static prediction: 7, the white phantom), the host's native travel ends the
session, and D-same-session then requires a new hook on the **host** side
suppressing the session end in case 2 — a class of intervention that has never
existed here (every suppression so far is the guest's). If it is < 5, the host
travels natively without ending it and D comes cheap.

> ## Phase 2 measured on 16/09: the warp passes, and **kills the host through the presences**
>
> Two runs, the same destination (`0a1f0000`, bonfire `7ba7`), the same
> `StartTravel` function. There is only one difference between them, and that
> is why they work as control and experiment.
>
> **Control — guest outside the session.** Through the fallback path
> (`junta desliga` → vote → `TravelLeave` → `StartTravel`): the host travels
> and **arrives** in Heide (`-18.5, 209`). The controller's object disappears
> in the same second, but because of the guests leaving, not because of the
> warp.
>
> **Experiment — guest inside the session.** Through the `nativo` request,
> which calls `StartTravel` without sending anyone away:
>
>     21:36:58  estado=0x10 papel=1 flags=0x261 armado=False relogio=54.2
>     21:36:59  host: viagem iniciada para a fogueira 7ba7
>     21:37:01  estado=0x10 papel=1 flags=0x271 armado=True  relogio=57.2
>     21:37:03  excecao c0000005 em +0x5180a8, lendo 0xffffffffffffffff
>
> **Three things measured, in order of importance.**
>
> **1. Phase 1 confirmed itself in action.** At the instant of the warp the
> state stayed `0x10` and the role stayed 1: the case 2 branch that ends the
> session **did not run**. The host can warp without the session being ended by
> that path.
>
> **2. The warp arms the 300 s watchdog and does not refresh the mark.** The
> flags went from `0x261` to `0x271` — bit `0x10` came on — with `+0x1b4` still
> at `0.0` and the clock at 57.2 s. It is exactly the configuration the survey
> predicted as dangerous, and now it is measured: from then on the session is
> on a timer that fires when the clock passes 300.
>
> **3. And the host crashed 3.7 s later, inside the presence code.**
>
>     1405180a0:  sub  $0x28,%rsp
>     1405180a4:  mov  0x60(%rcx),%rax     rcx = 0x7fffe81ed980
>     1405180a8:  mov  0x18(%rax),%edx     <- aqui
>
>     rax = 3f800000293988ec  -> dois floats (1.0 e 4.1e-14), nao um ponteiro
>     retornos: +0x5184b0 +0xa480d2 +0x10c5118 +0x517154 +0x5140c0
>
> The whole stack is in `0x51xxxx`, the region of the remote presence registry.
> The object's `+0x60` field returned floating-point data where the code
> expects a pointer.
>
> **What the control/experiment pair isolates.** Same warp, same destination,
> same bonfire: **without presences it arrives clean; with presences it kills
> the host in 3.7 s.** The warp is not the problem — the remote presences being
> torn down underneath a live session are.
>
> **Consequence for the plan.** What was inference becomes measurement:
> "remote presences are **not** rebuilt by a warp" is too weak. They are torn
> down, and the session machinery keeps walking over what is left. So
> **D-3b requires removing the presences before the warp** — `FUN_14051c820`
> per player, the primitive from `DS2_PRESENCE_REBUILD_PLAN.md` — and
> recreating them afterwards. That is, **D = native loading + option A**, now
> with empirical support and not just from reading.
>
> **Cost.** The host crashed (Samuel was at 30 before). The guest **did not
> pay**: it stayed at 80, and was stopped without `--force` afterwards.
>
> **And a method mistake that cost dearly, recorded so as not to repeat it:**
> before this experiment I stopped the games with `game stop --force` thinking
> there was no session; the party had re-summoned and there was. **Twenty
> points**, ten on each character (Samuel 20→30, Chico 70→80). `--force` exists
> precisely to run over the check that prevents this. Never use `--force`
> without reading the session state first.

**Phase 2 — native travel by the host only, no guest, cost ~zero.** Instrument
`FUN_1402be090` and the warp chain; confirm that the host's controller survives
beyond 300 s and that the watchdog does not arm. This is also where the
question of whether the game's loading screen works as a curtain is answered.

> ## The first half of 3b, measured on 16/09: **the hypothesis fell**
>
> The rule decided on is travelling together with the **session preserved**,
> which is 3b. The cheapest experiment for the first half: if the host dies
> 3.7 s after the warp because the presences are torn down underneath a live
> session, then removing them first should make the crash go away.
>
> **Removing works.** `presenca` read the registry `0x7FFFFE479D80` with 1
> alive, entry 0, state 2, role 1, net id 257, character `0x7FFFEB7B9E60`.
> `presenca retira` called `FUN_14051c820` and three seconds later the registry
> read **0 alive**.
>
> **And the session survived the removal** — `p2pSessionVerified: true`, the
> host at `0x10`, the guest at 7, packets crossing. This is the first
> **measurement** of the central premise of `DS2_PRESENCE_REBUILD_PLAN.md`,
> which until now was static reading: `FUN_14051c820` removes a presence
> without touching the session.
>
> **But the host crashed all the same**, at the same address and at the same
> time:
>
>     22:03:27.531  presencas (antes da viagem nativa): 0 viva(s)
>     22:03:27.531  host: viagem iniciada para a fogueira 7ba7
>     22:03:31.200  excecao c0000005 em +0x5180a8, rax=00000000bf7c1c5d
>
> **Where I got it wrong.** Phase 2's "control" — the fallback path that
> arrived clean — removed the presences **and** ended the session. I attributed
> to the presences what could have come from either of the two. With presences
> out and the session alive the crash continues, so **the presences are not the
> cause**. It is the third hypothesis of mine knocked down by measurement
> today, and all three had the same shape: a plausible explanation held up by a
> test that did not separate the variables.
>
> **What the crash is, precisely.** `FUN_1405180a0(param_1)`:
>
>     uVar4 = *(param_1 + 0x60);      // devolveu bf7c1c5d, um float
>     iVar1 = *(int *)(uVar4 + 0x18); // <- +0x5180a8, aqui
>     ... &DAT_1410c0050 + papel * 0x10
>
> It is a **role** lookup, in the same table of 20 roles that case 2 of
> `FUN_1402bd0d0` indexes. Called by `FUN_140518230(objeto, float)`, which is a
> **per-frame update**. That is: the warp invalidates an object and the network
> machine keeps running over it on the next frame.
>
> **What that changes in the plan.** 3b does not start with "remove the
> presences". It starts with **stopping the network machine during the warp**,
> or stopping it from walking over the object the warp brings down. Removing a
> presence is still necessary for the rebuild, but it is not what avoids this
> crash.
>
> **Cost.** The host crashed (Samuel 30 → 40, counting the phase 2 crash). The
> guest stayed at 80 and was stopped without `--force`.

> ## Who `param_1` is and who invalidates it (16/09, static reading)
>
> **The dispatcher.** `FUN_140514020(param_1, delta)` is the network layer's
> per-frame tick, and it walks the slots of the global object `DAT_141616cf8`:
>
>     raiz[0] = *raiz        -> FUN_140520ed0      (e lido em +0xa4 e +0xb4)
>     raiz[1] = raiz+0x08    -> FUN_14051e860
>     raiz[2] = raiz+0x10    -> FUN_14025b460
>     raiz[3] = raiz+0x18    -> FUN_1402c9540
>     raiz[4] = raiz+0x20    -> FUN_14051c940      o registro de presencas
>     raiz[5] = raiz+0x28    -> FUN_1405170e0      <- o que quebra
>     raiz[6] = raiz+0x30    -> FUN_140291170
>
> So **`param_1` is `*(0x141616cf8 + 0x28)`**: a sibling of the presence
> registry, in the same network object, ticked every frame. It has a state at
> `+0x08` (0, 1 or 2), a lock at `+0x78` that `FUN_1405170e0` closes at the
> start and opens at the end, and it only calls the function that breaks when
> the state is 1 or 2.
>
> **The object that dies.** Inside `FUN_140518230` there is a vector:
>
>     registros = *(param_1 + 0x10)     contagem em param_1 + 0x0c
>     cada registro tem 0x18 bytes:
>       +0x00  etiqueta (o caminho que quebra exige 0x7f00)
>       +0x04  float que acumula o delta do quadro
>       +0x0a  bandeiras; bit 0 "em uso", bit 1 "ja tratado"
>       +0x10  **um ponteiro para um objeto do jogo**
>
> It is that `registro+0x10` that goes to `FUN_1405180a0`, which reads its
> `+0x60`, then that thing's `+0x18`, and indexes the role table
> `DAT_1410c0050`. In both crashes `+0x60` returned floating point
> (`3f800000293988ec` and `bf7c1c5d`), and the two addresses were neighbours
> (`0x7fffe81ed980` and `0x7fffe81ed160`): elements of the same collection,
> reused.
>
> `FUN_1405180a0` also calls `FUN_14017b7e0(param_1 + 0x10)`, which is from the
> same family as the `GetComponent<T>` calls that have bitten us before — the
> object is of the character type.
>
> **Who invalidates it:** the warp itself. The loader goes through state
> `0x14`, which destroys the map and the characters and zeroes `ctx+0xd0`. The
> object at `registro+0x10` dies there, and **nothing cleans the record** — the
> "in use" bit stays on, the tag stays `0x7f00`, and on the next frame the
> network layer walks over it. It is the same old disease, in a new list.
>
> **This is reading, not measurement.** What is left to measure is who writes
> over the object, and now there is a target with an address: the chain
> `*(*(0x141616cf8 + 0x28) + 0x10) + i*0x18 + 0x10` gives the object **before**
> the warp, and the page watchpoint (`wp` in `DS2_Trace.req`) can be armed on
> it while it still exists. It is the tool that found the rigid body, and it is
> the first time in this investigation that it has an address to watch.
>
> **And it suggests a fix cheaper than rebuilding a presence:** clear the
> "in use" bit of the records tagged `0x7f00` before the warp, or hold the
> state of `*(raiz+0x28)` at 0 during the travel transaction, which is what
> stops `FUN_1405170e0` from calling the function that breaks. Neither of the
> two has been tested.

> ## The sync test, and what it revealed by accident (16/09, 22:24)
>
> The live reading had closed nicely: the `*(raiz+0x28)` subsystem keeps the
> **current map id** at `+0x18`, the state at `+0x08`, and thirty records of
> `0x18` bytes with a pointer to a character at `+0x10`. **The two addresses
> from the two crashes were records 11 and 24 of that vector**, both tagged
> `0x7f00` and with flags `0x11` — exactly the combination that reaches the
> role lookup. The fit was perfect.
>
> The fix: write 0 into the state, which is the game's own idle state and
> switches off the branch that breaks.
>
> **It did not work, for two reasons, and the first is the one that matters.**
>
> **1. The write does not stick.** `sync para` took the state from 1 to 0 at
> 22:23:59. Thirteen seconds later, at the moment of the travel, the travel's
> own log read **state 1**: the game's state machine had restored it on its
> own. The intervention was not in force when it mattered. Any fix here has to
> be a detour in `FUN_1405170e0`, not a write.
>
> **2. And I changed two variables again.** This run's destination was Majula
> (`0a040000`) and not Heide (`0a1f0000`), because the host was already in
> Heide. So this run is not comparable to the two before it.
>
> **The crash changed address**, from `+0x5180a8` to `+0x3ce81c`:
>
>     1403ce810:  mov 0x10(%rbx),%rax      o vetor
>     1403ce814:  mov (%rax,%rdi,8),%rsi   indice 0x4e
>     1403ce818:  mov 0x30(%rsi),%rcx      devolveu NULO
>     1403ce81c:  mov 0x58(%rcx),%eax      <- aqui
>
> With `rax = 0x7fffe81ecfd0`, inside the same neighbourhood as the sync
> objects. **There is no way to tell whether it changed because of the
> destination or because of the intervention**, and the intervention was not
> even active. It is one more list with a dangling reference.
>
> ## The conclusion I would draw, and it is unpleasant
>
> This is the third distinct consumer to die for the same reason today: the
> entities' component lists (74 copies of the same loop), the map's character
> sync list, and now this vector. **Fixing consumers never ends** — it is the
> same lesson as this morning, charged again at night.
>
> The warp brings the world down **assuming nothing else points at it**. A live
> session does point at it, through several structures at once. There does not
> seem to be a single point to intervene at.
>
> That pushes the reading towards: **"the host warps while the session is
> alive" may simply not be viable by patching.** What works today, and worked
> cleanly both times it was tested, is the warp with the session **ended** —
> which is D-3a.
>
> The project's rule is a preserved session, so the way out, if it exists, is
> not to stop the session touching the world during the warp: it is to
> **suspend the session** during the crossing and resume it on the other side,
> without it being ended from the server's point of view or from the saves'.
> That has not been investigated, and I do not know whether it exists.

> ## The network's silence: it worked, and the crash waited (16/09, 22:50)
>
> The detour in `FUN_140514020` — the single tick from which the whole network
> layer is walked — was armed before the warp and released when the world came
> back.
>
> **The first run proved nothing and the log said why:** the window opened and
> closed in three milliseconds, with **zero** ticks skipped, because at the
> moment of arming, the world was still standing. The exit condition had only
> one edge. Corrected to two — the world falling and then coming back.
>
> **The second run worked:** 192 ticks skipped in 5.3 s, with the world falling
> and coming back. No network subsystem ran while there was no world.
>
> **And the host crashed all the same, in the same millisecond the window
> closed:**
>
>     22:50:42.945  rede: parei a batida ate o mundo voltar
>     22:50:48.278  rede: o mundo voltou; 192 batida(s) puladas
>     22:50:48.279  excecao c0000005 em +0x5180a8
>
> At `+0x5180a8`, which is the sync's role lookup — the same one as before.
>
> **What that says, precisely.** The silence was not too little: it ended too
> early. When the loader goes back to idle and the character exists, the
> **sync list is still the old map's** — at the moment of the travel it read
> `mapa 0a1f0000` and the destination was `0a040000`. The first tick after the
> silence walks the old list and dies.
>
> **The fix the evidence points at**, and it is a one-liner: when releasing the
> window, **put the sync's state at 0 first**, before letting the tick through,
> so that `FUN_1405170e0` takes the branch that rebuilds instead of the one
> that walks. The stand-alone write at 22:23 did not stick because it was
> thirteen seconds earlier and the machine restored it; at the exact instant of
> the release, the next tick is the one that reads.
>
> **But it is the fifth intervention**, and the four before it had this same
> shape: good evidence, a plausible fix, and the crash showing up in the next
> place. That is worth saying before spending more.
>
> **Two corrections to what I had already written here.** The crash is **not**
> deterministic, and it is **not** immediate either: the 22:41 run looked
> clean, and the host died minutes later, which was only discovered when
> `session end` failed for lack of a process. Any measurement from here on has
> to watch for minutes, not the 3.7 s of the first case.
>
> **Cost so far:** Samuel from 10 to 70 points over the night. Chico intact at
> 80 — the guest has never paid for this line of work.

> ## The verdict of the night of 16/09: native travel with a live session
> cannot be fixed by patching
>
> The last fix — zeroing the sync's state at the exact instant the network's
> silence ends — **worked once and failed the next time**.
>
>     23:01:46  sync (o mundo voltou): estado 1 -> 0
>     23:01:46  rede: o mundo voltou; 193 batida(s) puladas
>     ... 120 s vigiados, nenhuma queda. Host em Majula, sessao verificada.
>
>     23:05:04  sync (o mundo voltou): estado 1 -> 0
>     23:05:04  rede: o mundo voltou; 153 batida(s) puladas
>     23:06:08  excecao c0000005 em +0x1e5c30      <- 64 s depois
>
> **Five interventions, five different addresses:**
>
> | intervention | where the crash ended up |
> | --- | --- |
> | a guard in the pre-draw (`FUN_1403f4f10`) | `+0x17b260`, a `GetComponent<T>` |
> | a sweep of the entity lists | `+0xfd8592`, a lookup that returns null |
> | removing the presences | `+0x5180a8`, the sync's role lookup |
> | silencing the network tick | `+0x5180a8`, in the ms the window closes |
> | zeroing the sync on releasing the window | `+0x1e5c30`, 64 s later |
>
> Each fix was technically correct about what the previous one revealed, and
> none of them finished the job. The warp brings the world down assuming
> **nothing** points at it; a live session points at it through too many
> structures, and they are not enumerated anywhere.
>
> **What is proven and worth keeping:**
>
> - the host **can** warp natively without the session being ended by case 2
>   (`ctrl+0x30 = 1` measured, and the session stayed verified in both runs);
> - `FUN_14051c820` removes a presence **without touching the session**
>   (measured, no longer just reading);
> - the network tick **can** be silenced during the load
>   (192 and 153 ticks skipped, the world falling and coming back);
> - and a complete native travel with the session preserved **is possible** —
>   it happened, once, with the host arriving in Majula and the guest still a
>   phantom in Heide.
>
> **What is not:** that it is repeatable. It is one in two with the last fix,
> and the crashes are intermittent and sometimes late, which makes any short
> run inconclusive.
>
> **Recommendation:** stop patching consumers. The paths that remain, in order
> of honesty:
>
> 1. **D-3a** — both leave the session, travel natively, and the party
>    re-summons on the other side. It works today, it is entirely native, and
>    it costs the summoning ritual. It is not seamless, and it is the only path
>    with zero measured crashes.
> 2. **Find the enumeration**, if it exists: what the game does when *it* ends
>    a session before a warp, which is the clean path of both control runs. If
>    there is a function that undoes everything the session hung on the world,
>    calling it before the warp and redoing it afterwards is a root fix and not
>    a patch. It has not been looked for.
> 3. Keep patching, knowing that the sixth intervention has the same shape as
>    the five before it.
>
> **Cost of the night:** Samuel from 10 to 90 points. Chico intact at 80. The
> guest has never paid for this line of work — every crash was the host's.

> ## The measured rate, and what the silence really bought (16/09, 23:36)
>
> With penalty points no longer a scarce resource (the harness's `save restore`
> undoes them), it became possible to measure a rate instead of an anecdote.
>
> **The test had to be corrected first.** After a travel the partner's presence
> **disappears** — seen in a screenshot from the user, and it is what the plan
> predicted. Without a presence the travel is the safe case, so a series of
> travels back to back measures the easy case and returns a pretty, false
> number. `taxa.sh` waits for the party to re-summon and only counts the
> attempt when the log confirms `1 viva(s)` before travelling.
>
>     tentativa 1 (Majula):  limpa (193 batidas puladas)
>     tentativa 2 (Heide):   limpa (154)
>     tentativa 3 (Majula):  limpa (194)
>     tentativa 4 (Heide):   limpa (158)
>     tentativa 5 (Majula):  CAIU em +0x3f39b3, 3 s depois
>
> Adding the two earlier runs of the same build: **5 clean and 2 crashes**.
> Before the fix it was 4 crashes in 5.
>
> **What the silence bought, precisely.** It removed the **network layer**
> family of crashes — `+0x5180a8` and `+0x1e5c30` did not come back. The crash
> in attempt 5 is something else: it happened **inside** the window, with the
> network silent (there is no "o mundo voltou" line), and at `+0x3f39b3`, which
> is the `MapModelComponent` neighbourhood — the same family as this morning's,
> the one `DS2_TravelWatchHook` guards.
>
> That is: silencing the network solved the network, and now what trips over
> the world's demolition is the map layer. The warp is dangerous for more than
> one subsystem, and each one needs its own treatment — or one that covers them
> all.
>
> **Honest reading:** this is a real, measured improvement, from ~20% to ~70%
> clean travels, and it is **not** a fix. Travelling together that fails one
> time in three is no good for playing.
>
> **Also confirmed visually:** after the travel the partner's phantom **no
> longer appears** in the host's world. The session stays verified and the
> packets cross, but the presence was destroyed and not rebuilt. The second
> half of 3b is still entirely ahead, and now it has proof on screen.
>
> **And one confounder ruled out:** twelve screenshots in a row, without
> travelling, with the session up, brought nothing down. `game shot` on its own
> does not kill the game; a capture in the unstable window after the travel may
> find it already broken, which is a different thing.

> ## Where travelling together got to (17/09)
>
> **Arrival works, and it is reproducible.** Three independent runs with the
> full ruler:
>
>     host:      papel 0, Heide, fogueira 7ba2
>     convidado: papel 1, ao lado do host
>     sessao:    verificada
>
> The recipe, in order, and each step was measured separately:
>
> 1. the host removes the presences (`FUN_14051c820` per live entry) —
>    measured: it removes without touching the session;
> 2. the host travels through the native chain with the network tick silenced
>    during the load (`FUN_140514020` silenced, ~155 ticks skipped);
> 3. **three seconds later** the guest travels by the direct warp with flag 1,
>    the "I am a phantom in someone else's world" flag — zero seconds and
>    twelve seconds both fail, the interval matters;
> 4. the host recreates the presence with the blob captured on entry to
>    `FUN_14051b0e0`; the recreated copy **moves** when its owner walks.
>
> The network only comes back **two seconds after** the world settles. Without
> that the guest died 14 and 21 ms after the release, two out of two.
>
> **What is missing: the session drops 180 to 190 seconds later.**
>
> It is not the 300 s watchdog — the `ctrl+0x1b4` mark was refreshed by hand
> and the drop came all the same, at 190 s. The server says who leaves: **the
> guest**, through `RequestNotifyLeaveSession`. And its log shows why, in two
> lines:
>
>     canal 7: enviados=0 recebidos=333
>     sessao ...: 1 membros (so eu)  ->  0 membros
>
> The guest's P2P session loses the host from its member list and then it
> leaves on its own. The guest's warp breaks its own participation, not the
> host's. `FUN_1402c2820`, which writes the exit's `+0x120`, has a `+0xf8 < 3`
> guard and the active guest is at 7, so **it is not through there** — the path
> has not been found yet.
>
> ## Bench traps that invalidated runs
>
> Recorded because each of them had me measuring nothing while thinking I was
> measuring:
>
> - the backup used as the base had the **host in Heide and the guest in
>   Majula**; different maps never form a session, and the symptom reads as a
>   broken party. There is now a `base-majula` with both in the same place;
> - **`session end` pauses `DS2_Party` and `up` does not resume it** — without
>   an explicit `retoma` no session forms after a cleanup;
> - **`game stop` fails silently with a live session**, the games carry on with
>   the old state and the next `up` does nothing. A whole cycle ran with the
>   guest already dead without my noticing, and produced a "the session
>   survived six minutes" for a session that did not exist;
> - **surviving is not passing**: there was a run with both games standing, no
>   crash, and no co-op at all — the guest back in its own world, role 0.
>   The ruler has to check a live process, a verified session, the phantom role
>   and the destination map, on arrival **and** throughout the watch.

> ## Travelling together working, at all four destinations (17/09, 13:30)
>
> **A different transport for each side**, and that asymmetry is the central
> finding. It came from the pair of experiments that isolates the cause:
>
>     convidado viaja sozinho -> sessao sobrevive 5 min, segue fantasma
>     host viaja sozinho      -> sessao cai em 10 s, convidado vai para casa
>
> The **host's** native warp is what kills the session. The guest's is safe.
> So each side uses whatever does not break it:
>
> | side | transport | why |
> | --- | --- | --- |
> | host | the old one: backread + teleport | there is no warp, so the session is never touched; 52 legs without a single session drop |
> | guest | native warp with flag 1 | a real load, which is what solves its corruption; and it is safe for the session |
>
> The guest does **not** need the network silence — that was made for the
> host's warp. Taking it out is one piece fewer.
>
> ### Measured, four destinations in a single session, three minutes of watching each
>
>     Majula       (122a, 0a040000)   2.2 m entre os dois
>     Heide        (7ba2, 0a1f0000)   1.7 m
>     Iron Keep    (4cc2, 0a130000)   1.1 m
>     Brume Tower  (2d82, 140b0000)   1.7 m
>
> In all of them: `p2pSessionVerified: true`, the guest at role 1, no crash on
> either side. At the end: the host in state `0x10`, the guest in state 7, both
> on the member list, packets crossing, and the presence registry with one live
> entry.
>
> **The four map ids**, which were not documented anywhere:
> Majula `0a040000`, Heide `0a1f0000`, **Iron Keep `0a130000`**, **Brume Tower
> `140b0000`**. And the first bonfires: `122a` The Far Fire, `7ba2` Tower of
> Flame, `4cc2` Ironhearth Hall, `2d82` Tower of Prayer.
>
> ### Correction of 17/09: the arrival was right and the co-presence was not
>
> A screenshot from the user showed what my ruler was not measuring: **the
> guest does not see the host**. The host sees the guest's phantom; the guest's
> screen is empty, and it cannot use the bonfire.
>
> The coordinates matched to within 1.7 m and the session was verified, but
> coordinates being close does not prove co-presence. The "I am in someone
> else's world" bit is right (`ctx+0x24b1 = 0x40` on the guest, `0x00` on the
> host), so the problem is not the flag 1 warp.
>
> **The point is that the presence rebuild is one-way.** Measured, the normal
> state of a session is symmetrical:
>
>     host      -> 1 presenca: papel 1 (o convidado), net id 513
>     convidado -> 1 presenca: papel 0 (o host),      net id 32512
>
> The guest's warp destroys **its own** registry, which is where the host's
> copy lives, and I was only recreating on the host. Recreating on the guest
> **does not work**: the request is accepted, the pending slot is filled and
> consumed, and none of the five entries is born.
>
> The likely cause is the shortcut I took knowingly: I keep the **pointer** of
> the member captured on entry instead of re-resolving it from the live list
> (`FUN_140520040`), which is what `DS2_PRESENCE_REBUILD_PLAN.md` told me to
> do. On the host the pointer survives; on the guest, apparently not. The birth
> gate in `FUN_14051dbb0` is not the problem - it only requires clearance for
> role `0xe`, and ours is 0.
>
> ### And the old transport does not reach everything
>
> Tested at all four destinations with both sides on it: Majula and Heide pass
> (1.2 m and 0.9 m), **Iron Keep fails** - the host goes and the guest stays
> 857 m behind, because the backread only brings maps the streamer can reach
> from the current one. For a distant destination the guest really does need
> the native warp, which makes the presence rebuild mandatory and not optional.

### What is still not done
>
> The travel is triggered by two requests (`ir` on the host and `fantasma` on
> the guest), not by the vote. What is left is wiring the recipe to the voting
> path and to the barrier, so it becomes a single action.

> ## The borrowed world after the warp: empty (17/09, 15:30)
>
> The user reported that the guest, arriving in Heide through the recipe,
> **cannot use the bonfire**. Control first: in a normal summon in Majula the
> guest sees "A: Rest at bonfire" and rests (`convidado: descanso na fogueira
> 0000122a no mundo do host`). After the recipe, standing at Heide's `7ba2`
> with "Samuel" beside it, nothing.
>
> Three measurements, all on the same bench:
>
> - **`esd 4000`** on the guest: **0 queries** of an event script in 4 s.
>   The host, at the same spot: 4 queries per frame (130301, 130311, 130321,
>   132003). The map's scripts are evaluating nothing on the guest.
> - **`ds2os-dev flags`**: the guest with **0 flags on in every category**
>   (10, 20, 13100…), the host with dozens. Not even the flags from its own
>   save: copy 1 of the `EventFlagBuffer` (the "someone else's world" one)
>   was zeroed by the load and never filled in.
> - **`ctrl+0x19c`** on the guest's join controller still reads
>   `0a040000`, the map of the original summon.
>
> It is exactly row 3 of the table in section 2: the host's snapshot arrives
> **once**, in join state 4, and the phantom warp does not repeat it. The
> bonfire is only the visible symptom; the guest's whole world (doors, dead
> enemies, event values) is its own save's and not the host's — M4 undone by
> the travel.
>
> **The mechanism, read in Ghidra.** The host exports in its controller's `0xd`
> state (`FUN_1402bddb0` dispatches on `+0x150`; the handler is
> `FUN_1402bf8f0`, which builds a ~33 KB blob on the stack from the map at
> `*(R+0x5b8)+0xc` — and that field already reads the new map on both sides —
> and sends message `0xc`). The guest receives it in `FUN_1402cde30` case `0xc`
> → `FUN_1402d09c0` (parse and validation) → slot 10 of the join controller,
> `FUN_1402c2fa0`, which imports **only if `+0xf8 == 4`**; in any other
> state it writes `+0xf8 = 5` and `+0x120 = 1`, which ends the session on the
> next frame. The import ends in state 5 (presences from the blob's records)
> and 6 (the "I am in" handshake, which sends a message to the host).
>
> **The remedy, `d36f3f69`.** A `snapshot` request (and the guest event
> `SnapshotPlease` over the channel): the host calls `FUN_1402bf8f0` from its
> own tick when the controller is at `0x10`; on the guest, a detour in
> `FUN_1402c2fa0` — only when asked — points `+0x19c` at the current map, sets
> `+0xf8 = 4`, lets the original import, and puts `+0xf8 = 7` back, without
> going through states 5 and 6, because the host's controller is already well
> past them. The presences are still recreated by the mod.
>
> **A trap that cost two crashes.** The decompiler shows seven parameters for
> `FUN_1402c2fa0`; it reads an **eighth** at `rbp+0x7f` (`+0x2c334c`, into
> `r8d`), the count of the 0xd0 records that `FUN_1404434c0` walks. The detour
> with seven arguments killed the guest at `+0x12d5a8` on the next two summons,
> at the same address. Count the accesses to `[rbp+0x67..]` at the entry before
> trusting a signature.
>
> **Not yet measured:** whether after the `snapshot` the flags match, the
> scripts go back to evaluating and the bonfire appears — that is the next
> test.

> ## The `snapshot` measured, and the four-leg campaign (17/09, 16:43–17:07)
>
> With the automatic request (`AskSnapshot` on arrival from the phantom warp)
> and the host back at `0x10` after exporting, the guest's bonfire **started
> working** — the defect the user pointed out. Proof of one leg
> (Heide, 16:43), with the full ruler:
>
> - **equal flags** on both sides after the `snapshot` (before: the guest
>   zeroed; category 10 matched again);
> - **scripts evaluating**: 4 `esd` queries per frame on the guest,
>   against 0 before;
> - **`A: Rest at bonfire`** on the guest's screen, and it rested
>   (`convidado: descanso na fogueira 00007ba2 no mundo do host`);
> - **a verified session** after all of it (`p2pSessionVerified: true`).
>
> **The host back at 0x10.** The export (`FUN_1402bf8f0`) moves the host's
> controller from `0x10` to `0xe`, where it waits for the guest's "I am in" —
> a message the guest does **not** send, because states 5 and 6 of the import
> were skipped. With the host at `0xe` the session carried on with packets and
> both presences, but the ruler read it as unverified and the game's own checks
> for a host that is playing land on `0x10`. Writing `0x10` back live made it
> verified again. Read in the binary (`FUN_1402c03e0`, the `0xf` handler):
> it is the state that closes the guest's join on the host's side.
>
> **The go-home record did not move, and that is the right thing.** Probed
> across the four legs at `ctrl+0x1a0` (0x48 bytes) and in `character`: it
> stayed `0a1f0000/00007ba7` — the guest's home bonfire, in Heide — from the
> join through to the rest, on every map. The guest travels as a phantom, rests
> in the host's world (which restarts the **host's** world), and its own
> respawn point is never contaminated. It is exactly the warning in section 2:
> only the host writes `FUN_14044fe30`.
>
> **The campaign: 4 of 4 legs, with one crash on the last.** Iron Keep, Brume,
> Majula, Heide, one joined to the next without stopping the session. The first
> three passed in full — warp, automatic request, import, presence, rest,
> two minutes of watching with a verified session. The **fourth (Heide)**
> brought the guest down three seconds into the warp, before it arrived:
>
>     17:06:12  excecao c0000005 em +0x2fd4b0 (thread 360), lendo 0xffff...
>     17:06:12  excecao c0000005 em +0x3f64a7 (thread 360), lendo 0x8
>     17:06:12  excecao c0000005 em +0x3f6326 (thread 360), lendo 0x20
>
> It is the teardown of the guest's old map (Majula) while Heide loads:
> `FUN_1403f6300`/`FUN_1403f4500` (the `MapModelComponent` destructor),
> called from `FUN_1403ba070` → `FUN_1403ce966`, over a rotten node in the
> component list — the same class as the host's crashes, but on the **worker
> thread 360**, not on the game thread (364) where `DS2_TravelWatch`'s sweep
> runs. The ceiling on exception reports, raised from 32 to 256 in this run,
> is what let all three be recorded; without it the crash would have gone
> unlogged. The sweep does not reach thread 360 — it is the same limit noted in
> the death of 17/09 at `+0x2f0987`. **It is intermittent**: the same Heide
> passed clean at 16:43. It stands as the next stability target: a teardown
> guard that runs on the worker thread, or silencing that list during the
> guest's warp.

> ## The hybrid wired into the game, and the crash is the vote's and not the travel's (17/09, 18:00)
>
> **What was left to wire up.** Sitting at the bonfire and travelling never
> went through this day's work: the phantom warp, the `snapshot` and the
> presence recreation had **one** caller each, the `DS2_Bonfire.req` reader.
> The game's path had only two modes — both sides on the old transport, or the
> guests **thrown out of the session** when the map did not come. `99fb8d29`
> wires up the hybrid: in `TravelGo`, a guest that cannot reach the map
> notifies the host (`GuestEvent::WarpNotice`), removes its own presence, waits
> for the host to remove its own, travels by warp, asks for the host's world on
> landing at the destination, recreates the presence and only then reports
> arrival. The barrier gets 70 s when there is a warp;
> 25 s is the old transport's budget and would cut the travel in half.
>
> **The crash the user saw, isolated.** Two of his travels through the game's
> path ended with **both games closing** ~1.2 s after the curtain came down,
> 250 ms apart, two out of two (17:38 and 17:44). The control separates the
> cause:
>
> | path | legs | crashes | exceptions |
> | --- | --- | --- | --- |
> | `ir` on both (same transport, no vote and no menu) | 6 | 0 | 0 |
> | bonfire → list → vote | 2 | **2** | several |
>
> The six control legs were Majula ↔ Heide, there and back three times, with
> the guest **provably travelling** (six map loads in its log) and the session
> verified at the end. **The transport is the same in both cases** —
> `StartGo` on both machines. What differs is the interface surgery.
>
> **Ruled out:** the job lock dance when the bonfire menu closes. It ran the
> same way on the first `ir` leg (the host was sitting at the bonfire) —
> `trava do job de volta depois de 16 ms` — and that leg passed clean.
>
> **The best lead.** On the guest that crashed, the line `tela de carregamento:
> desceu` was **never written**, although `o mundo assentou` was, 1.8 s
> earlier. That line is the last instruction of `Curtain(false)`, after
> `s_hud_show`, `s_loading_close`, `s_hud_drop` and turning the world's drawing
> back on. That is: **the guest crashed inside the curtain coming down**. And
> it crashed in the renderer, on **two threads at once** (360 and 624), reading
> a pointer with the high half stamped (`00b010ffff483c10`).
>
> Put that together with what differs about the vote: the Yes/No box is opened
> and closed **on the same front-end object** where the curtain opens the
> loading screen. The hypothesis is that the box's `close`/`release` leaves a
> dead node in the front-end's list, and the renderer dies on the first frame
> it goes back to drawing. It is a hypothesis, not proof.
>
> **How to reproduce it without the user — and what got stuck.**
> `votar <mapa> <fogueira>` opens the vote, but answering requires pressing A
> on the guest's box, and on this boot **the pad does not reach instance 2**:
> neither the voting box, nor the warning box, nor the start menu responds,
> with `pad status` saying `pad 1: no ar` and `game focus` passing. Without
> that the vote times out in 30 s. The next cheap step is a request verb that
> answers an open vote, so the whole path becomes testable with nobody in front
> of the screen.

**Phase 3 — the guest in the world. This is where a point is at risk.**
With a baseline of both saves. **3a**: the host travels natively, the guest is
re-summoned. **3b**: the host travels natively, the guest loads natively and
the mod rebuilds the presence before `TravelRelease`. 3b's ruler: **one**
generation per peer, movement and action in both directions after the rebuild
(traffic on the channel does not count — it proves the mod is communicating,
not replication), unchanged counters, a legal exit.

## 6. Risks and what detects each one

- the host ends its own session on the warp → phase 1's reading; in a test,
  `ctrl+0x150` going to `0x12`/`0x13` and `0x15` at the peer
- the silence watchdog drops the guest while it loads → time it from the
  guest's warp to the `Leave*` on the server
- the 300 s watchdog → a reading detour recording the delta against 300.0
- the guest reappears in its own world → the host's world not applied, the
  other player not visible
- the guest's save contaminated → never call `FUN_14044fe30` on the guest
- penalty: any crash in phase 3 costs 10 points, and Chico is at 70
- presence duplication when recreating → `FUN_14051ce20` overwrites `E+0x40`
  without destroying the old one

## 7. What is reading and what needs measurement

**Read statically:** the travel chain and the request's fields; the reason
gate; the four components of the borrowed world; case 2 of `FUN_1402bd0d0`;
the two watchdogs; that `FUN_14044fe30` writes the local player's record; the
prologues above.

**Needs measurement, and cannot be presented as fact:** the value of `+0x30`
in a live session (prediction 7, but it is a prediction); whether suppressing
case 2 leaves the host's machine consistent, since it also writes
`+0x1b8 |= 2`; who on the host counts the ~23 s of silence and whether a load
fits inside it; whether the guest can load while preserving the join ctrl; and
the loading screen inside a real warp.

## 8. 17/09 — what kills the session on a group travel

Group travel through the vote already takes both to the same place without
anyone leaving the session. Three things used to bring it down, and two are
closed.

**The character table of a released map (closed).** The guest died at
`+0x517843`, 167-173 ms after the backread released a map, five times out of
five. `FUN_1405177c0` takes that table from the map's owner at two places:
`+0x160` with a null test, `+0x168` with no test at all. The owner survives the
map, only the table disappears, so the lookup finds the owner and reads null.
`DS2_NetSyncGuardHook` puts the missing test in 24 bytes, using the `ebx` that
has carried the index since `+0x5177d1`; the two `movzwl` the compiler emitted
were redundant and pay for the test and the branch. Returning false is the
game's own answer for "did not find it", and the caller at `+0x518d64` already
branches on it.

Before that I had written the `+0x18` of the object in slot 0x28 of
`0x141616cf8` thinking it was the map id `FUN_1405177c0` reads. **It is not**:
that id comes from the function's `param_1`, another record. Repointing it
changed nothing and was removed.

**The invented parts (closed).** When the other player's copy was not standing
on a known part, the request to hold the map came with no mask and turned into
**all** the parts: 128 bits across six blocks, against maps whose part index is
far smaller. The teardown walks the bits that are on. Releasing Brume Tower on
the guest failed twice in there (`+0x3f6476` and `+0x3ba0be`, each one over a
part pointer assembled out of rubbish), the safety net caught it, the map was
left half done, and the next map died in the wreckage. No mask now means no
part at all: the force byte on its own holds a map that is already in.

**The freed object left over in a list (open).** With the two above, the
four-bonfire campaign gave **3 of 4 clean**, with no caught failure. The one
that crashes is always the leg **right after Brume Tower**, and the guest dies
about half a second after arriving:

| where | what it does | what it had in hand |
| --- | --- | --- |
| `+0x3f3b20` | `call [rax+8]` walking a `[rcx+0x38]` list, node by node through `+8` | virtual table `bded80c840bde337` |
| `+0x3ce81c` | `[rcx+0x58]` over `[[rbx+0x10]+idx*8]+0x30`, index 321 of `[rbx+0x18]` | pointer `00002817f1b91828` |

Both are the same family: releasing a map leaves already freed objects attached
to the game's own lists, and the next load of that map walks over them.
Heide → Majula is clean; Brume → Majula killed two out of two.

**Holding every map does not work.** Tried and undone the same day: with Majula
and Heide held, the Iron Keep request stopped at state 0 and never loaded, and
Brume's did too; the host waited 30 s and gave up on both travels. Four maps
forced at the same time is more than the game loads, so holding everything
trades the crash for nobody going anywhere.

**How to score a leg.** It only passes if nothing goes up on either side: no
`excecao` in `DS2_Crash.log`, no `FALHA APARADA` in `DS2_Backread.log`, and the
session stays verified **and** the host actually arrived. The safety net turns
a crash into silent corruption, so the first caught failure is already the
failure; and a leg where nobody went anywhere is not a clean leg.

## 9. 18/09 — what the failing value was saying

The exception watcher now writes down who owns the objects in hand: a register
pointing at compromised memory whose first eight bytes are an address inside
the game is an object with a virtual table, and that table's offset names the
class in Ghidra with no guessing. With that, the reading changed.

**The failing value is not a corrupted pointer.** They are two floating-point
numbers, one in each half of the register: `4254670941466334` is 53.10 and
12.40; `4254a3a241338718` is 53.16 and 11.22. Nobody is writing over a pointer.
The block was **freed and handed to somebody else**, who keeps positions in it,
and whoever was still pointing there read the position as if it were a virtual
table.

This shows up in three different containers, all of the same family:

| where | the container |
| --- | --- |
| `+0x3f3b20` | a notification list at `obj+0x38`, node by node through `+8`, slot 1 of the virtual table |
| `+0x17b266` | a component list at `obj+0x18`, node by node through `+0x10`, slot 0 |
| `+0x3ce818` | a vector at `[obj+0x10]` with the count at `+0x18`, element 321 and element 392 |

Pruning the two lists before the game walks them (`DS2_PartNotifyGuardHook`)
cut nothing in any round: the nodes stayed healthy and the crash simply changed
container. That is, the damage is not in the list, it is in whoever was freed.

**The solo control is clean.** The same transport, one game only, with no
session, did ten legs in a row around the four bonfires without a single fault.
So loading and releasing a map do not corrupt anything by themselves: the
corruption needs the session, and what the session brings is the other player's
copy.

**The hypothesis left standing, and the fix it asks for.** The part bits were
only ever turned on, never taken back. A map was released with all of its parts
still requested, the owner's state machine tore it down without deactivating
any of them, and everything those parts had registered with went on pointing at
blocks the allocator had already reused. Releasing now has two stages: give the
bits back, let the game see the smaller mask for a few frames, and only then
drop the force byte. **That fix has not been measured yet** — the bench got
stuck first (see below).

**The bench got stuck, and it is not our code.** After about thirty-six hours
of Steam and Wine being up, the game started opening a white window and never
reaching the title: the loop runs (navigation publishes thousands of ticks),
the server sees the client connect and drop in the same moment, and both Steam
accounts are still logged in. The previous DLL, which had come up fine an hour
earlier, freezes the same way. A launch chain from the second Steam was left
hanging, one that `game stop` cannot see and that only dies with a `kill` by
pid.

## 10. 18/09 — what the remaining crash follows, and what does not fix it

**It follows the map where the session was formed.** That is the day's finding.
With the session formed in Majula, the guest died coming back to Majula; I
ended the session in Heide, let the party re-form there, and the death moved to
Heide. It is not Majula, it is not the zone with no summoning, it is not the
destination: it is the map of the meeting. The previous leg being Brume Tower
brings the crash forward, because Brume is the biggest map in the rotation and
it is its load that reuses the freed memory.

**The solo control is still clean.** Ten legs around the four bonfires, one
game only, with no session, without a single fault. The transport and the
backread do not corrupt anything on their own.

Everything below was tried and did **not** fix it:

| attempt | what happened |
| --- | --- |
| pruning the two linked lists before the game walks them | it never cut a single node; the crash changed container |
| stopping the writes into the two visibility blocks | it helped, but the crash continues |
| holding the character sync stopped throughout the teardown | no effect |
| handing the part to the streamer only when it belongs to the focused map | no effect; handing null breaks the floor and the travel fails |
| giving the part bits back before releasing the map | ran five times, no effect |
| rebuilding the presences on every arrival | no effect, and it crashed earlier |
| turning off the hold on the other player's map | 256 faults at once; the hold is necessary |
| holding the map of the meeting for as long as the session lasts | **it breaks the travel**: with two maps held, the third stops at state 0 and the host gives up in 30 s |

**Two limits that came out measured.** The game loads two maps at a time and
not three: anything that holds one map more makes the next request stop at
state 0. And the game's current map (`0x141616cf8 +0x20 → +0x5b8 → +0xc`)
**follows** the old transport, measured live: 0x0a040000 before, 0x0a1f0000
after. The theory that the game kept thinking the player was on the origin map
is wrong.

**Where to look next.** The freed object is found by the world update, always
in a parts or entities structure, and it only exists when there is a session.
What the session puts on that map and nobody takes away when it goes is what is
left to name. The exception watcher now writes down the virtual table of the
objects in hand, so the next crash that catches a heap object will already name
the class.

## 11. 18/09, afternoon — the writer found, and what is left after it

**The two-day guest crash was the object sync writing into freed memory.** On a
guest the sync binds once, at the join, to the object table of the session's
map (`FUN_140517880`: one record per 0xa0-byte object block, count at `+0xc`),
and only a real load binds it again. Travel here is not a real load. When the
backread released that map, the host's object packets kept arriving and
`FUN_140518920` wrote each one into a block that had since become a
`MapEntity`. Read live on the guest: the same 56 records, same pointers, same
map, before and after a travel and after the release; the first eight bytes of
several blocks were exactly the "vftables" of earlier crashes
(`0000002e00290c00`, `0000007e023dcd02`); and a crashed `MapEntity` had its own
`+0x1c` and `+0x2c` overwritten in the shape of that writer's stores.

Fixed in three steps, each measured:

| change | result |
| --- | --- |
| drop the records (count to 0) as the bound map's release begins | the state-0 write at the next travel end rebuilt all 56 against the freed table; died 0.7 s later |
| stop writing state 0 after travel and on release | the game went to state 0 by itself and rebuilt on the first return to Majula; died ~1 s after arriving |
| also close the guest's rebuild gate (`sync+0x198`) | **five clean legs**, including Brume → Majula; the sync stays empty |

The control was rerun on the current build first: twelve solo legs, four
returns to Majula, three straight after Brume Tower, all clean. So everything
here is the session's.

**What is left.** On the second return to Majula of that last run, **both
games** began faulting every frame within 17 ms of each other, host included:

| side | where | what |
| --- | --- | --- |
| guest | `+0x3f510f`, `mov 0x38(%rcx)` with `rcx = [MapModelComponent+0xc8]` | `000b0010e8364540` |
| host | `+0x1caa60` / `+0x1caaf0`, alternating | `000b001000000000` |

In both, a live `MapModelComponent` (`+0x10eb558`) has the pointer at `+0xc8`
with its top half replaced by `0x000b0010`, next to a `MapFlverModelCtrl`
(`+0x10e9488`). 17 ms is the gap between the guest arriving and the host
seeing the guest's copy appear, so the trigger looks like the other player's
copy arriving in the map the session began in. The object sync cannot be it on
the host, whose count is 0. The games survived that leg and one died on the
next. Not yet named: who writes `MapModelComponent+0xcc`.

## 12. 18/09, evening — the session's map stays loaded, and the travel holds

Six research passes over the binary, run in parallel (reports in
`docs/research/`), converged on the rule the mod had been breaking. In vanilla
the fog that rises when a phantom joins fences the session's area, so **the map
the session began in never unloads while the session lives**, and everything
the join binds counts on it: the enemy sync (`*(0x141616cf8+0x28)`, which
earlier sections called the object sync; it is `NetEnemyManager`) and the
per-map enemy generator table its records point into (`FUN_140419a70`; the
0xa0-byte blocks are `EnemyGeneratorAreaChrStatus` entries). Every guest crash
of 17 and 18/09 followed that map because the backread released it.

Two changes, both in `DS2_BackreadHook`:

- **The backread never releases the map the enemy sync is bound to**
  (`DS2_BonfireInSession_IsSessionMap`). It stays forced for the session. The
  enemy sync therefore never has to be dropped and keeps working.
- **The streaming cap goes from two maps to four.** The limit is a single
  compare in `FUN_1403cc450`'s state 0 (`cmp $0x1,%ebx` at `+0x3cc4f2`, on owners
  minus owners in state 0); the byte is raised to 3 after the whole compare is
  checked. Four is what a leg from Heide needs: the held session map, Heide, the
  No-man's Wharf the game streams in by itself, and the destination. The map
  heap (`MapSeamlessControl`, a fixed 13.5 MiB) read 57-60% throughout.

Result, on the same session, the four bonfires asked for (Majula, Heide's
first, Iron Keep's first, Brume Tower's first):

| run | legs | clean | session up |
| --- | --- | --- | --- |
| first | 16 | 16 | 16 |
| second, same session | 16 | 15 | 16 |

**32 consecutive group travels with the session verified the whole way**, ten
of them back into the map the session began in, several straight after Brume
Tower.

The one leg with a fault: a null write at `+0x1bee1c4` inside the owner cycle
of No-man's Wharf (`r15 = 0x0a1e0000`) while four maps coexisted; the trap
caught it, the game and the session carried on. The same address was seen on
the host on 17/09 before the cap was touched, so it is not new, but four maps is
where it lives. Not yet done: evicting the neighbour the game streams in before
forcing the destination (streaming-budget.md proposes a detour on
`FUN_1403dc930` zeroing `streamer+0x188[i]`), which would keep a leg at three.

What the other reports offer if the held map ever proves too costly: a native
host warp with bit 0x10 of the host controller's `+0x1b8` cleared, and a re-run
of the join's world load through host state `0xa` (rejoin-in-place.md,
warp-reasons.md), modelled on the arena duel, which already warps both players
inside one live session (alternative-travel.md).

## 13. 18/09, night — corrections to §12, and the last two killers

**Two corrections to §12.** The bonfire table was wrong: what §12 calls
"Brume Tower's first" was `140b0000/2d82`, the Tower of Prayer in Shrine of
Amana, and "Iron Keep's first" was `0a130000/4cc2`, Ironhearth Hall, the
second. The 32 legs therefore covered Amana and Ironhearth Hall, not Brume
Tower and Threshold Bridge. The right ids are Threshold Bridge `0a130000/4cc7`
and Brume Tower's Foyer `32240000/8f2f` (`docs/scenarios/travel-legs.sh`).
And the cap is not four: four maps exhaust a fixed pool (the null write at
`+0x1bee1c4`, r9 = `INAP_LD`, `+0x113a860`), so the byte is 2, three maps, and
the travel makes room by evicting the neighbours the game streamed in by itself
(`StreamerMasksHook`, the detour on `FUN_1403dc930`).

**The host expelled the guest 300 s after a cancelled death.** The accept
controller's `+0x1b8` gains bit 0x10 on a host death (case 0 of
`FUN_1402bd0d0`) or warp (case 4); nothing but the constructor clears it, and
the watchdog `FUN_1402be090` ends the session once the controller clock passes
`+0x1b4 + 300`. A travel landing whose fall the death hook cancelled set it at
20:03:45 and the host sent `RequestNotifyLeaveGuestPlayer` at 20:08:45. Proven
live: a harness kill turned 0x261 into 0x271 with `+0x1b4` = 101.5 s, and the
expulsion came at clock 401.5 s. `DS2_SeamlessSessionHook` now clears the bit on
every host tick while the controller is formed (state 0x10); the same bit also
made the re-entry handshake `FUN_1402bd720` refuse a guest with reason 8.

**Leaving Brume Tower killed the host about 0.4 s after its teardown**, one
exit in four, in the effects system: `FUN_140fd8570` resolves an
`FXEvaluatableReference` through the live node's parameter block (`node+0x50`,
handed in by the spawner, owned by neither the effects system nor anything that
outlives the map) and the entry was null; with that read guarded, the next call
went through the freed node's own vftable. The streaming teardown
(`FUN_1403cc3a0`) never touches live effects; the loading screen clears them all
first (`FUN_140bebe00` → SfxFxManagerBase slot +0x50 → `FUN_140a09a80`). Only
DLC maps (`0x32xxxxxx`) carry their own effect bank (sfx 5000 + area), and only
Brume Tower crashed, so `DS2_BackreadHook` now detours `FUN_1403cc3a0` and, for a
DLC map, makes that same clear call first. Every live effect goes at that
moment, as it does on a load.

| build | legs | clean | session up | note |
| --- | --- | --- | --- | --- |
| 4dd24149 | 6 | 5 | 5 | host died Brume → Heide, silently |
| f2ee8fcb | 16 | 9 | 9 | leg 10: the 300 s expulsion |
| 603d72a | 20 | 3 | 3 | host died Brume → Heide, effects |
| 0af479a (guard only) | 12 | 3 | 3 | guard fired, host died on the freed node |
| 6c419c7 (effects cleared) | 14 | 14 | 14 | seven exits from Brume Tower |
| 6c419c7 | 24 | 11 | 11 | leg 12: Threshold Bridge landing billed as a death |
| 6947c52 | 24 | 11 | 11 | leg 12: arrival not recognised after the fall |
| 4ec5743 | 24 | 24 | 24 | but three landings billed a death the score missed |
| 0817c13 | 24 | 24 | 24 | no billed death; three post-travel falls absorbed |
| 0817c13 | 24 | 19 | 19 | leg 20: the guest left on "duty fulfilled" |
| 9e9528b | 24 | 24 | 24 | then a legal `session end`, both games still up |

**"Duty fulfilled" sent the phantom home.** The player watching leg 20 saw the
game's message: the host's world has Iron Keep's bosses dead, and on arriving at
Threshold Bridge the guest's client took the phantom's "duty fulfilled" path
(reason 1 at the terminal `FUN_140190950`, which the respawn hook logged as
`morte de fantasma: motivo=1`) and left the session. The seamless brief keeps
the phantom past the boss, so `DS2_RespawnInSessionHook` now marks that record
done and does nothing else while the session is playing. The call reaching the
terminal comes through Arxan-obfuscated code with no static reference, so what
raises it on arrival is not read; it did not recur in the next 24 legs, so the
guard itself is not yet exercised.

**Threshold Bridge's landing.** Its bonfire sits on a bridge whose final
collision arrives after the first one the character touches, so a landing can
fall through. Three fixes in `DS2_DeathInterceptHook`, in the order they were
found: a travel to another map ends only on that map's ground (it used to end
one frame after the jump, on the contact still reporting the map left); the
ground is proven by the streamer's last part **or** the collision handle under
the feet (the first stayed null after a fall while the second named Iron
Keep); and a landing's death is never billed — the recovery waits out a
pending death or zero HP, and a fall within 3 s of a travel's end sends the
character back to the travel's target. `travel-legs.sh` now fails a leg that
billed a death, which is what the 4ec5743 run scored as clean.

## 14. 19/09 — a travel you cannot see, and the effects it no longer takes

**The loading screen was only letterbox bars.** Rapid captures of both
players during a travel showed the world still drawing while "the curtain" was
up: each camera flew across the map to the bonfire. The curtain copied the
game's loader (`FUN_140483250`) but not what the game's warp does first: a fade
to black on the fade object at `ctx+0x1160` (`FUN_14039a510`, `{alpha, target,
remaining}`), which the loader waits out, plus four frames, before raising its
curtain. `DS2_BonfireInSessionHook` now fades to black with the curtain, starts
the travel only once the fade is black (at most 1.5 s), and fades back in when
the curtain comes down. A guest goes black the moment it answers yes; before,
it watched the host's copy vanish for up to 1.8 s with its HUD on. Captured
afterwards: both screens black for ~3 s, then the destination with its area
name, no frame of the flight. The black quad is drawn over the front end, so
the loading screen's own art does not show during the wait.

**The bonfire flames.** Clearing every effect before a DLC teardown (§13) took
the arrival map's effects for good: a census at Heide counted 71 live effect
trees before the clear and 14 after — the bonfire flames (ids 3020/13020, one
per loaded bonfire), the torches (3019) and more, because their spawners never
spawn them again. The teardown itself soft-stops the map's effects and unlinks
their handles, and a stopped tree keeps ticking with nobody holding it. So the
clear is gone: the teardown runs first, and only the live top nodes it left
without a handle (`node+0xf8` empty now, not before) are killed, the way
`FUN_140a09d50` kills a tree. Every tree of the census was held by its entity,
so nothing of the other maps is touched; the flame stays lit.

| build | legs | clean | Brume exits | orphans killed |
| --- | --- | --- | --- | --- |
| a1feffc | 25 | 25 | 12 | host: id 8329 in 4 of 12; guest: none |

One exit in four leaving an orphan is the rate at which leaving Brume Tower
killed the host before any fix, which makes effect 8329 the likely culprit.

## 15. 19/09 — the real map budget is the TargetManager

The host died at `+0x1bee1c4` after a test that went Majula → Frozen Eleum
Loyce (`32250000`) → `0a170000`. That address is not a stray write: it is the
game's own fatal-assert trap (`movl $0xdeadba, 0`), reached here from
`DLFixedVector.inl` line 0x27e, "out of memory." (`rdi` = the message). The
vector is the **TargetManager** at `*(ctx+0x48)` (vftable `0x1410ed868`): 2048
16-byte entries inline in a 0x8028-byte object, count at `+0x8018`, checked at
`0x140248263`. Every enemy generator (`FUN_140411a40`), character and
targetable map object of every loaded map registers one (`FUN_1404208c0`), and
the per-frame sweep `FUN_140420b90` removes those a teardown flagged. It cannot
be enlarged by a byte patch (the layout offsets are baked into ~20 accessors).
A second inline vector, the chameleon areas at `*(mapmgr+0x208)` (count at
`+0x78`, capacity 3, `0x1401c5d5e`), is what the four-map guest crash of 18/09
overflowed. "INAP_LD" in `r9` was the assert's own buffer ("DL_PANI" backwards)
and never a pool. Of the seven faults logged at this address since 15/09, four
are the TargetManager, one the chameleon vector, and two a
`DLReferenceCountInvalid` in teardown.

`DS2_BackreadHook` now logs both counts on every owner state change and when
the target count settles (`budget:` lines). Measured on 19/09 from Majula, on
both machines, with identical figures:

| map | id | targets | chameleon |
| --- | --- | --- | --- |
| Majula | `0a040000` | 313 | 1 |
| Heide's Tower of Flame | `0a1f0000` | 274–281 | 1 |
| Iron Keep | `0a130000` | 451–464 | 1 |
| `0a110000` (streamed in beside Iron Keep) | `0a110000` | 601–875 | 1 |
| Brume Tower | `32240000` | 1126 | 1 |
| `0a220000` | `0a220000` | 226–232 | 1 |
| `0a170000` | `0a170000` | 794 | 1 |
| Shrine of Amana | `140b0000` | 441–446 | 1 |
| Frozen Eleum Loyce | `32250000` | 1389–1392 | 1 |

Every release gave its targets back (the count returned to 313 each time), so
there is no leak: the limit is the sum of what is loaded at the peak. The
crash's peak was Majula 313 + Eleum Loyce 1392 + `0a170000` 794 = 2499. The
closest a clean leg came was 1911 of 2048 (Majula, Iron Keep and Brume Tower
together, 19/09 11:53). A travel's peak is session map + map left + destination
(+ any neighbour the game streams in), so the budget has to be checked on that
sum, not on the number of maps.

## 16. 19/09, evening — making room: park, let the map go, then load

A travel's peak is session map + map left + destination, and the TargetManager
holds 2048 (§15). When the destination does not fit, the travel now makes room
the way a loading screen would, behind the black screen:

1. **Budget** (`DS2_Backread::Targets`, `TargetCost`): each map's cost is
   learned on load and kept in `DS2_TargetCosts.txt` (seeded with §15's
   figures; 1400 for a map never seen). If in use + destination > 2048 − 150,
   every 30 s hold goes and the map left is not held.
2. **Park** (new host event `TravelPark`): the guests, then the host, wait at a
   bonfire **of the session's map** (Majula) — teleport plus streamer focus.
   The game never takes down the map under a player, and a map another
   player's copy stands in is kept for it (letting it go under the copy killed
   the guest on 15/09), so everyone has to leave it. A parking bonfire of any
   other map is wrong: from Iron Keep the first one found was the neighbour
   `0a110000`, the game kept the player in Iron Keep and no room was made.
3. **Wait until the player is really off it**: streamer player map and current
   part (`streamer+0x20`, freed by the teardown, read by ~25 functions) must
   name another map for **6 s**. The map's objects keep their effects lit while
   the game still has the player near them; a teardown 0.7 s after parking
   orphaned four of them (8524, 218, 251 were Eleum Loyce's own object effects,
   not common ones) and killed the host every time.
4. **Lighting**: a character's lighting cube (render slot 9) is an entry of the
   bank of the map it stands in (`owner+0x1a0`), cached raw on the character
   (`+0x460/+0x468/+0x470`) and its model (`M=*(chr+0xf0)`: `+0x2a0` current,
   `+0x2a8` previous, `+0x228/+0x230` drawn, `+0x4a0` last pushed), refreshed
   only when the floor's region byte changes. Every character still holding an
   entry of the map being taken down has it cleared (the players at parking,
   anyone else — enemies, the guest's copy — on each update); otherwise the
   renderer bound the freed cube 300 ms after the destination was requested
   (`+0x833655`, AddRef on 7).
5. **Release** through the normal let-go (parts given back, force byte cleared,
   streamer allowed byte zeroed), then **2 s** before the destination is asked.
6. **Effects of a DLC map's own bank** are killed **before** its teardown: the
   ids its binders supplied (`SfxSystem+0x68`, path `sfx<5000+area>`, ids at
   `+0xf0`), every live tree whose top effect is one of them. Killing orphans
   after the teardown read freed memory and corrupted the heap; leaving them
   (8519, a per-map emitter) crashed the effects update. A detour on the handle
   unlink (`FUN_140a060f0`) during the teardown is the second net.
7. **World rebuild resets everything**: a new local controller clears the
   focus, the request, keeps, a pending unload and the parking. A guest parked
   when the host died went home to Heide with the streamer still focused on
   Majula: characters floating in a grey void.

| build | legs | clean | tight travels (room made) |
| --- | --- | --- | --- |
| a3ee517 | 14 | 13 | Iron Keep → Brume parked at `0a110000`, no room; guest's TargetManager reached 2040 (trapped) |
| c92eb71 | 20 | 20 | 10, every one parked in Majula |

Cost: a travel that has to make room takes about 15 s behind the black screen.

## 17. 19/09, night — the guest's own budget, and the list the game never purges

Testing the player's own route (Undead Refuge `0a170000/5c62`, `0a170000/5c67`,
Eleum Loyce, Majula, Brume Tower, Heide, Iron Keep) found four more:

1. **A destination already loaded needs no room.** Majula plus Eleum Loyce read
   1902, over the margin, so a travel to the already loaded Eleum Loyce waited
   30 s and took maps down around the other player. Room is made only for a
   destination that is not in yet.
2. **Room comes from the heaviest map that may go**, never the session's map,
   the destination, or the one the character waits on. Asking for "the map
   left" had a guest trying to take Majula down under both players, and the
   injector marked it unreachable for 32 s while the game kept it.
3. **The guest makes room too.** Parked in Majula it still held the map it had
   left, and the probe of `TravelGo` loaded the host's destination beside both
   (2496 targets). A guest already on the session's map now stays where it is,
   a guest that parks drops its holds, and when the destination does not fit
   its budget the probe is skipped so the travel makes room first. A map chosen
   to go also has the backread's own request released, or it stays forced.
4. **The sign areas the game never purges.** `*(ctx+0x90)` is the SignManager
   and `*(+0x80)` its area vector (begin `+0x10`, end `+0x18`); each entry
   keeps the event id at `+0x50`, a pointer into the map's data at `+0x58` and
   the map id at `+0x70` (read live). Its per-frame sweep reads byte `+0x18` of
   that pointer, and the game's own purge for a released map, `FUN_140210ad0`,
   is a bare `ret`: unmodded, maps only go through a loading screen. With the
   travel the entries stay, and the guest died every time on the same leg
   (`+0x3c3b7f`) the moment the map being built reused that memory. Every
   teardown now drops the entries of the map going down (2 to 10 per teardown
   in practice) and any whose object is no longer a location.

Also: the travel asks the game's own relocation lookup for the arrival spot
(`FUN_140451830`/`FUN_140451930`, region type 0x1a), which is the step the game
takes after "1.1 m behind the bonfire"; at Undead Refuge no region applies, and
the arrival there is in the open with the character free to walk (checked on
screen and with `goto`).

| build | legs | clean | route |
| --- | --- | --- | --- |
| 48ecc94 | 16 | 15 | one leg's guest hit the full TargetManager, trapped |
| e5b1adc | 20 | 20 | the same route, no fault, no "NO ROOM" |

**19/09, played by hand.** After the fixes of §16 and §17, a long session played
by the project's owner travelled the group across many bonfires with nothing
going wrong: no crash on either machine, no session drop, and no character left
stuck at an arrival. The scripted campaigns had reached 20 of 20 legs on the
same build (e5b1adc); this is the same road with a person at the controls.
