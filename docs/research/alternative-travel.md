# Group travel by another route: what the game already does in a live session

Research note, 18/09. Read-only work in Ghidra (copy `/home/suel/tools/proj-a6`)
and `objdump` against `DarkSoulsII.exe` 1.03 / Calibrations 2.02, preferred
base `0x140000000`. Nothing was run in either game. Addresses are Ghidra's;
the code offset is `address - 0x140000000`. Globals written as absolute
addresses (`0x1416148f0` = the game context, `0x141616cf8` = the network root)
are image-relative and must be resolved from the module base in a hook.

Every finding is marked **[read]** (the decompiled line or the instruction is
quoted) or **[inferred]** (a conclusion drawn from reads, not measured).

This note deliberately stays off the five subjects other agents are covering
(map-border streaming, warp reasons as such, re-joining in place, the network
object table, the streaming budget). Where a finding lands on one of them it
is stated once and handed over.

## The answer first

| rank | direction | chance it works | code it needs | keeps the session | partial map load |
| --- | --- | --- | --- | --- | --- |
| 1 | **The arena choreography**: re-run the join's own phases on the live summon session, around a native host warp | medium: the phase veto is satisfied by design, but the map-layer crash of 16/09 (`+0x3f39b3`) is not explained by anything here | medium: 4 data writes, 3 game calls, a switch on the snapshot detour | yes, on the Steam and ctrl levels | none: both sides do real loads |
| 2 | **An invisible re-summon**: end the session legally, but skip the guest's trip home and the sign | high | medium-large (server + client) | **no** (new session, invisibly) | none |
| 3 | Suppress only the warp-triggered teardown on the host, keep everything else | low on its own | small | yes | none, but the dangling-consumer crashes stay |
| 4 | Travel as "host dies and respawns at another map's bonfire" | none beyond today | — | — | it *is* today's transport |
| 5 | Elevators, fog walls, cutscene/event warps as the carrier | none | — | — | same entry as every warp |

The one genuinely new fact behind rank 1: **the game already has a flow in
which the host warps to another map while a Steam session and a multiplayer
controller are live, and the guest follows with its own join warp** — the
arena duel (`NetDuelAcceptMultiplayCtrl` / `NetDuelJoinMultiplayCtrl`, jobs
`GoDuelField`, `WaitGuestWarpFinished`, `ReturnMyWorld`). It works because the
host warps *before* anything is materialised in its world: presences, the
world snapshot and the "I'm in" handshake all come *after* the warp. The co-op
summon controller has exactly the same later phases. So the recipe is: take a
live co-op session back to the shape the arena has at the moment of its warp
(using the game's own shutdown calls, not writes), warp both sides natively,
and let the game's own phases 0xb→0x10 (host) and 2→7 (guest) rebuild the
rest — including the presences the mod could not recreate on the guest.

Four corrections to the existing record came out on the way (section 2): a
warp reaches the ctrls as code 4, never as its reason; code 0 is the local
player's death, not an arrival; the warp's "multiplay gate" is just "alive";
and bit 0x10 of `ctrl+0x1b8`, which nothing ever clears, makes the host refuse
a re-joining guest with reason 8. That last one would kill rank 1, and any
re-join in place, on its first run.

## 1. The arena duel: a native in-session warp of both players

### 1.1 The host's job chain [read]

`FUN_1402b72c0` is state 8 of the duel host ctrl (`NetDuelAcceptMultiplayCtrl`,
vftable `0x1410d6f18`, dispatcher `FUN_1402b4640` on `+0x148`):

    if (*(char *)(param_1 + 0x170) != '\0') {        // arena, not an in-world duel
      *(undefined4 *)(param_1 + 0x148) = 10;
      uVar2 = FUN_1402b4940();                        // _CreateMultplayFlowJob
      FUN_140285630(param_1 + 0x1c8,uVar2);
      return;
    }

`FUN_1402b4940` builds the chain, in this order (each is a `new` of the
named vftable followed by `FUN_140286670(seq, job)`):

    WaitPhase(0xc)            -> waits for ctrl+0x148 == 0xc
    GoDuelField               -> map index ctrl+0x1c0, spawn 0
    WaitGuestWarpFinished     -> polls ctrl+0x195
    SetPhase(0xd)
    WaitPhase(0x10)
    ... ShowDuelStartDialogJob, ConsumeStartItem, ..., WaitDuelEnd
    WaitGuestDisconnect
    LeaveSession              -> FUN_1405205d0(*root)
    SetPhase(0x11)
    MethodCallJob(FUN_1402b51e0)
    ReturnMyWorld             -> FUN_1402bc020(req, 3), warp flag 0

`GoDuelField`'s run slot is `FUN_1402b4310`:

    FUN_1402bc1c0(*(undefined8 *)(param_1 + 0x20));          // snapshot "where I stood"
    FUN_14027be20(param_1 + 0x18,local_48,0);                 // build the warp request
    cVar1 = (**(code **)(*DAT_1416148f0 + 0x40))(DAT_1416148f0,local_48,0);  // FUN_1401c2a80, flag 0
    if (cVar1 == '\0') return;
    FUN_140500fd0(DAT_1416148f0[0x45c]);                      // ctx+0x22e0, same "prepare" the join uses

and `FUN_14027be20` fills the request as **kind 4, reason 4**, flavour byte 3,
map from a six-entry table at `0x141613b20` (stride 0xc, in `.bss`, filled at
runtime), spawn point `param_3`:

    *param_2 = 4;  param_2[1] = 4;  *(undefined1 *)(param_2 + 5) = 3;  param_2[6] = uVar1;

### 1.2 How the host's phases move [read]

| phase | who moves it | what it does |
| --- | --- | --- |
| 10 → 0xb | `FUN_1402b5410` (from slot 0x90 `FUN_1402b45f0`): every guest slot's `+0x70` query is 0 | — |
| 0xb | `FUN_1402b6340` | computes the arena spawn **1** (`FUN_14027be20(param_1 + 0x1c0,&local_d0,1)` + `FUN_1404799f0`), sends the guest a destination payload (`FUN_1402cf020(&local_98, …)`: map, position, rotation, net-id short), sets `+0x148 = 0xc`, calls `FUN_1402bbf20` |
| 0xc | the job chain | `GoDuelField` warps the host |
| — | slot 0xc8 `FUN_1402b40e0` (a message from the guest) | `if (+0x195 == 0) +0x195 = 1` → releases `WaitGuestWarpFinished` |
| 0xd → 0xe | `FUN_1402b5490`: every guest slot's `+0x78` query is 0 | — |
| 0xe | `FUN_1402b6880` | exports the world snapshot (event flags `FUN_1401f3b70`, bonfires `FUN_14017e910`, map objects, presence records via `FUN_14051ba70`; sent by `FUN_1402cf1d0`), sets `0xf` |
| 0xf → 0x10 | slot 0xd0 `FUN_1402b3e80` (the guest's "I'm in") | checks `(+0x1d8 & 0x10) == 0`, else `FUN_1402b5180(param_1,8)` (end); sets `0x10` |

So the arena host warps at phase 0xc: **after** the guest has been told where
to go and **before** the snapshot export (0xe) and the guest's "I'm in"
(0xf→0x10). Nothing of the guest exists in the host's world yet.

### 1.3 The guest side [read]

`NetDuelJoinMultiplayCtrl` (vftable `0x1410d7678`) dispatches on `+0xf0` in
`FUN_1402ba160`; its state 2 handler, slot 0x28 `FUN_1402b9750`, is the same
code as the summon join's `FUN_1402c2a80`: a request with `local_b8 =
0x400000000` (kind 0, reason 4), map `*param_2`, position `param_2[1..3]`, and

    cVar2 = (**(code **)(*DAT_1416148f0 + 0x40))(DAT_1416148f0,&local_b8,1);   // flag 1
    ...
    FUN_1402bbf20(param_1);
    FUN_140500fd0(DAT_1416148f0[0x45c]);
    *(undefined4 *)(param_1 + 0xf0) = 3;

The duel join flow (`FUN_1402ba360`) has no `GoDuelField` of its own: the
guest reaches the arena **through its ordinary join warp**, driven by the
host's 0xb payload. [inferred] The map in that payload is
`*(*(root[4]+0x5b8)+0xc)` read at 0xb, before the host's own warp; I could not
tell from the reading alone whether the field already holds the arena map at
that moment (see Unknowns).

### 1.4 How the duel ctrl treats the warp and the death [read]

The ctrl's slot 0xe0 is `FUN_1402b3aa0`:

    if (param_2 == 0) {                      // the local player died (see 2.2)
      +0x1d8 |= 0x10; if (+0x190 == 0) +0x190 = 8; ... ; +0x148 = 0x11;   // duel over
    } else if (param_2 == 5) { ... }
    FUN_1402855a0(param_1 + 0x39, param_2);  // forward to the job chain at +0x1c8, slot 0x38

A warp (code 4) is **only forwarded to the job chain**; it does not end or
arm anything on the ctrl.

### 1.5 The two teardown orders the game uses [read]

- Arena end (job chain): `WaitGuestDisconnect` → `LeaveSession`
  (`FUN_1402b4380`: `FUN_1405205d0(*DAT_141616cf8)`) → `SetPhase(0x11)` →
  `ReturnMyWorld` (`FUN_1402b4490`: `FUN_1402bc020(…,3)`, `FUN_140500fd0`,
  warp flag 0).
- Summon guest going home, state 8 `FUN_1402c3900` (and the duel joiner's
  `FUN_1402ba8d0`, identical in shape): warp home (flag 0) → state 9 →
  **`FUN_140517080(DAT_141616cf8[5])`** → `FUN_1405205d0(*DAT_141616cf8)`.

That second one names the game's own shutdown for the enemy sync (1.6).

### 1.6 The enemy sync has a native stop and a native re-arm [read]

`root[5]` is `NetEnemyManager` (constructor `FUN_140515860`: `*param_1 =
NetEnemyManager::vftable`; it registers packet types 0x14–0x18 in
`FUN_140515eb0`). This is the "object sync" of `DS2_NATIVE_TRAVEL_PLAN.md`
§8–§11. Its three entry points:

`FUN_140517080` — **stop**:

    if (*(int *)(param_1 + 8) == 1) FUN_140517e70(param_1);       // host unbind
    else if (*(int *)(param_1 + 8) == 2) FUN_140517a80(param_1);  // guest unbind
    *(undefined4 *)(param_1 + 8) = 0;
    *(undefined1 *)(param_1 + 0x74) = 0;     // disarm
    *(undefined1 *)(param_1 + 0x198) = 0;    // guest rebuild gate

Callers: guest state 8 `FUN_1402c3900`, duel joiner `FUN_1402ba8d0`, the
multiplayer manager's tick `FUN_1402c9540` (event type 1) and
`FUN_1402c9bd0` (last ctrl gone).

`FUN_140517040` — **arm**: `+0x74 = 1; +0x198 = 0`. Its only caller is the
session manager's event handler `FUN_14051f5a0`, on Steam session **create**
(case 7, `root[0]+0xa4 = 2`) and **join** (case 8, `+0xa4 = 4`).

`FUN_1405170e0` — **tick**, binds only from state 0:

    if (+0x74 != 0 && (&DAT_14157c3b0)[*(int *)(root[3] + 0x68) * 8] != 0 && *(uint *)(root[0] + 0xb4) > 1) {
      if (*(int *)(root[0] + 0xa4) == 2) { FUN_140517bf0(p); +8 = 1; }            // host
      else if (ctx slot 0x58 () && +0x198 != 0) { FUN_140517880(p); +8 = 2; }    // guest
    }

The per-mode table `0x14157c3b0` (stride 8, first byte): mode 0 → 0, modes
1–5 → 1, mode 6 → 0. `root[3]+0x68` is recomputed every tick by
`FUN_1402cb8f0` from the ctrl list; I did not decode which number co-op gets.

**Why this matters.** The night of 16/09 recorded that writing the sync's
state to 0 "did not stick: the game's state machine restored it". The reading
says why: state 0 with `+0x74` still 1 is the "bind now" condition. The
game's own stop clears `+0x74`, and nothing but a Steam create/join re-arms
it, so after `FUN_140517080` the sync stays idle across a warp **by the game's
own rules**, and `FUN_140517040` re-arms it on the new map. It is the
enumeration-by-subsystem the plan asked for, at least for this subsystem.
(Handed to the network-object-table work: whether `FUN_140517e70` /
`FUN_140517a80` leave the records clean.)

## 2. Corrections to the record

### 2.1 A warp always reaches the ctrls as code 4, never as its reason [read]

`FUN_1401c2a80` calls `FUN_1402c7ec0(mgr, param_2[1])`, and that function is:

    1402c7ee0:  mov    (%rbx),%rcx
    1402c7ee3:  mov    $0x4,%edx          <- constant
    1402c7ee8:  mov    (%rcx),%rax
    1402c7eeb:  call   *0xe0(%rax)        <- each ctrl, slot 0xe0, code 4
    ...
    1402c7f23:  mov    %esi,%edx          <- the reason
    1402c7f28:  call   *0x38(%rax)        <- each listener at root+0x50, slot 0x38

So the summon host ctrl (`FUN_1402bd0d0`) runs **case 4** on every warp, not
case 2. `DS2_NATIVE_TRAVEL_PLAN.md` §1 and phase 1 frame the risk as case 2;
the measured behaviour (`+0x1b8` 0x261 → 0x271, `+0x1b4` staying 0.0) is case
4's, exactly:

    case 4:
      *(uint *)(param_1 + 0x37) |= 0x10;                 // +0x1b8 bit 0x10
      iVar1 = (**(code **)(*param_1 + 0x88))(param_1);   // FUN_1402bcb60: +0x150 == 0x10
      if (iVar1 == 0) { FUN_1402be1e0(param_1, 6); +0x150 = 0x11; ... }

The conclusion of phase 1 survives (a host at 0x10 is not ended by the warp),
but for a different reason, and with a new condition: **the host must be at
exactly 0x10 at the instant of the warp**. The mod's `snapshot` trick leaves
the host at 0xe until something writes 0x10 back; a warp in that window ends
the session with reason 6.

### 2.2 Code 0 means "the local player died", not "arrived" [read]

`FUN_1402c7b00` sends code 0 to every ctrl. Its only caller is `FUN_140251040`,
which fires it on the local player's `slot 0x1b0` true→false edge. That slot
of `PlayerCtrl` (vftable `0x1410e4bb8`) is `0x140314440`:

    140314440:  xor    %eax,%eax
    140314442:  cmp    %eax,0x168(%rcx)     <- HP
    140314448:  setg   %al                  <- HP > 0
    14031444b:  ret

So case 0 of `FUN_1402bd0d0` (arm bit 0x10 **and** refresh `+0x1b4` to the
ctrl clock) is the host's **death**, and case 0 of the duel ctrl is "duel
over". There is **no arrival notification** to the ctrls at all.

Also in `FUN_1402c7b00` [read]: if no ctrl reports an active phase
(`FUN_1402c6e40` = max of slot 0xf0 is 0) and `root[0]+0xa4 != 0`, it calls
`FUN_14051f560(root[0])`, which is the same body as `FUN_1405205d0`
(`LeaveSession`). [inferred] A death while a Steam session exists with no
live ctrl leaves the session.

### 2.3 Bit 0x10 of `+0x1b8` is a one-way switch with two consequences [read]

Writes to `ctrl+0x1b8` in `0x1402b0000–0x1402da000`: only ORs, plus the
constructor's `andl $0xfffff800,0x1b8(%rsi)` at `0x1402bc4ca`. Nothing clears
bit 0x10 once a warp (case 4) or a death (case 0) has set it. Two readers:

1. **The 300 s watchdog**, `FUN_1402be090`, every tick from the dispatcher:

       if ((+0x1b8 & 0x10) && *(float*)(+8) - *(float*)(+0x1b4) > 300.0f /* 0x1410d7b40 */) {
         FUN_1405205d0(*(undefined8 *)(param_1 + 0x180));   // leave the Steam session
         ...; *(undefined4 *)(param_1 + 0x150) = 0x13;
       }

   `+0x1b4` is only ever refreshed by case 0 (death). [inferred] A host that
   warps natively in a session **older than 300 s of ctrl clock** and has not
   died since is ended on the next tick. That fits the 17/09 line "host travels
   alone → the session drops in 10 s, the guest goes home" better than any
   guest-side cause; phase 2 on 16/09 did not see it because the clock was at
   57 s. (`DS2_PRESENCE_REBUILD_PLAN.md` already named this watchdog "the
   likely wall for alternative 1"; what is new is that its only refresh is a
   death, and the second reader below.)

2. **The re-entry refusal**, slot 0xd0 `FUN_1402bd720`, which handles the
   guest's "I'm in" at 0xe:

       uVar1 = *(uint *)(param_1 + 0x1b8);
       if ((uVar1 & 0x10) == 0) { ... bits 4, 8, 2 checked ...; +0x150 = 0xf; return 1; }
       FUN_1402be1e0(param_1,8);                       // end, reason 8

   [inferred] This is the vanilla rule "a host that has warped or died since
   the ctrl was made does not accept a (re)joining guest". **Any plan that
   re-runs the join handshake after a native host warp is refused here unless
   bit 0x10 is cleared first.** It applies to re-joining in place as much as
   to rank 1.

### 2.4 The warp reason reaches almost nobody [read]

Listeners are added to `root+0x50` by `FUN_140512fe0`. Its callers and their
slot 0x38 (the warp reason):

| registered by | object | slot 0x38 |
| --- | --- | --- |
| `FUN_1402bc3f0` / `FUN_1402c1620` | the summon host / join ctrls (second vftables `0x1410d7ae8`, `0x1410d7cf8`) | `FUN_140069120`: `return;` |
| `FUN_140191bb0` | the object that owns `EventPhantomReturn` (`+0x18`); [inferred] `EventResultManager` (`0x1410c3718`), whose listener slots point at the same empty stubs | `FUN_140069120`: `return;` |
| `FUN_140068af0` | [inferred] a `NetEventListener` (`0x1410b1038`) or `FeSceneRepatriationWindow` (`0x1410b1138`); both vftables have the empty stub at 0x38 | `return;` |
| `FUN_14027a930` | `NetSvrSummonJobBase` (`0x1410d34b8`) | `FUN_14027ab00`: `return;` |
| `FUN_140279db0` | `NetSvrMonitorJobBase` (`0x1410d3248`) | `FUN_140279f90`: `if (param_2 != 4) { +0x10 = 4; +0xc = result 6; }`, aborts the job |
| `FUN_140252f70` | `NetNpcPhantomManager` (`0x1410d1308`) | `FUN_140253e30`: every NPC-phantom slot to state 5 |

[inferred] For player co-op, **the reason (2 travel vs 4 return) makes no
difference to the session objects**; it only aborts pending server monitor
jobs (reason ≠ 4) and resets NPC summons. Handed to the warp-reasons work.

### 2.5 The warp's "multiplay gate" is "the local player is alive" [read]

`FUN_1401c2a80` refuses reasons 2 and 4-with-flag-1 when `FUN_140248940()`
is true. That function is:

    if (*(longlong **)(DAT_1416148f0 + 0xd0) == 0) return false;
    cVar1 = (**(code **)(**(longlong **)(DAT_1416148f0 + 0xd0) + 0x1b0))();   // HP > 0
    return cVar1 == '\0';

`DS2_NATIVE_TRAVEL_PLAN.md` §1 calls it "`*(mgr+0x168) > 0` — the multiplay
time counter, positive in a live session", and `DS2_RespawnInSessionHook.cpp`
lifts `ctx+0xd0 +0x168` around the replay. `+0x168` of `PlayerCtrl` is the
current HP (M2 step 4 measured it), and slot 0x1b0 is `cmp %eax,0x168(%rcx);
setg %al`. So the gate is simply **"the local player is not dead"**: a
travel or a join warp is refused while HP ≤ 0. Nothing in it depends on the
session. (That is also why the gate was 0 "at a guest's death".)

## 3. Rank 1 — the arena choreography on the summon session

### 3.1 The idea

The summon host ctrl (`NetSummonAcceptMultiplayCtrl`, dispatcher
`FUN_1402bddb0` on `+0x150`) already has the arena's post-warp phases under
other numbers [read]:

| step | arena host | summon host |
| --- | --- | --- |
| send the guest its destination | 0xb `FUN_1402b6340` | 10 `FUN_1402bf440` (payload built the same way, `FUN_1402cf020`) → 0xb |
| guest reports its warp done | slot 0xc8 `FUN_1402b40e0` (`+0x195 = 1`) | slot 0xc8 `FUN_1402bd9c0`: `if (+0x150 == 0xb) +0x150 = 0xc` |
| wait for every slot | `FUN_1402b5490` (0xd → 0xe) | `FUN_1402be490` (from slot 0x90 `FUN_1402bdd60`): every slot's `+0x78` query 0 → `0xc → 0xd` |
| export world snapshot | 0xe `FUN_1402b6880` | 0xd `FUN_1402bf8f0` → 0xe |
| guest's "I'm in" | slot 0xd0 `FUN_1402b3e80`, refuses on `+0x1d8 & 0x10` | slot 0xd0 `FUN_1402bd720`, refuses on `+0x1b8 & 0x10` |
| finalise | → 0x10 | 0xf `FUN_1402c03e0` → 0x10 |

The only thing the arena does that co-op never does is warp the host between
"destination sent" and "snapshot exported". The co-op session has the same
Steam lobby and the same ctrl classes; what it has in addition, at travel
time, is everything phases 0xd–0x10 and guest states 4–7 built: presences,
a bound enemy sync, an imported snapshot, and bit 0x10 unset. Rank 1 undoes
those with the game's own calls, warps both natively, and lets the game redo
them.

What is different from what the mod tried on 16–17/09:

- the enemy sync is stopped by its own `FUN_140517080` (which disarms
  `+0x74`), not by silencing the whole network tick or writing its state;
- the guest's presences are rebuilt by its **own state 5** from the host's
  snapshot, which the mod skipped (it detoured the import to jump from 4 to
  7) — the mod's own blob replay was the part that "does not work" on the
  guest (17/09, "none of the five entries is born");
- bit 0x10 is cleared, so the native "I'm in" is accepted instead of ending
  the session with reason 8, and the 300 s watchdog does not fire;
- the host is at 0x10 at the instant it warps.

### 3.2 The sequence

Channel 7 carries the orchestration, as today. `H` = host, `G` = guest.

1. **Quiesce (both).** The vote passes; the curtain comes down.
   - H and G: remove presences with `FUN_14051c820` per live entry (measured
     16/09 to keep the session).
   - H and G: `FUN_140517080(*(0x141616cf8 + 0x28))` on the game thread.
     Check `+0x08 == 0` and `+0x74 == 0` afterwards.
2. **Host warp.** H confirms `ctrl+0x150 == 0x10`, then the native bonfire
   chain (`FUN_1401843b0` + `FUN_140184830` + `FUN_14044fe30`, as M8 does).
   Immediately after `FUN_1401c2a80` returns 1: `ctrl+0x1b8 &= ~0x10`
   (the game never clears it; the value before the warp is known, 0x261 in the
   phase 1 sample).
3. **Host waits for its world**: `ctx+0x24ac` back to 0x1e, `ctx+0xd0`
   non-null, **and** the two things `FUN_1402bf440` tests before it sends
   anything [read]: `*(char *)(*(ctrl+0x188) + 9) == 0` and
   `*(*(ctx+0xd0) + 0x490) != 0`. If either fails, state 10 ends the session
   (reasons 6 or 1).
4. **Rewind the host to "destination".** Write `ctrl+0x150 = 10`. On the
   next tick `FUN_1402bf440` builds the payload from the host's **new** map
   (`*(*(root[4]+0x5b8)+0xc)`) and position (`FUN_1402bdf10`), sends it with
   `FUN_1402cf020`, sets `0xb` and ORs `0x40` into `+0x1b8`.
   Alternative that avoids the rewind: the mod builds the payload itself and
   G replays it, as `DS2_RespawnInSessionHook` already does; then H must be
   moved to 0xb by hand so that step 6's slot 0xc8 fires.
   **Never move H past 0xb by hand.** `FUN_1402c2fa0` imports only at guest
   state 4; in any other state it writes `+0x120 = 1` and the session ends on
   the next frame (17/09). A snapshot exported while G is still at 3 kills
   the session, so 0xb → 0xc must come from G's own "warp finished" and
   0xc → 0xd from `FUN_1402be490`.
5. **Guest to state 2.** G writes its join ctrl `+0xf8 = 1`; state 1
   (`FUN_1402c4450`) sees the peer link (`root[0]+0xa4 == 4`), sends its block
   (`FUN_1402ceb30`) and sets 2 (measured in the M2 work). G saves the go-home
   block `+0x1a0..+0x1c8` first and puts it back after the warp (existing
   code). When the host's payload arrives, `FUN_1402c2a80` does the flag-1
   warp into the host's new world and moves G to 3. Two guards in that
   handler to read live beforehand (both hit the M2 replay): the arrival
   byte `*(char *)(*(join+0x108) + 8)` must be 0, and `FUN_1402c6570` (a
   role/mode table `FUN_14014ed40` plus `FUN_1402ca190`) must say yes.
6. **Guest arrival.** G's slot 0x40 `FUN_1402c1f60` [read] moves 3 → 4 and
   calls `FUN_1402cf7f0` ([inferred] the "warp finished" send, packet 0xb).
   Packet 0xb reaches H through the router `FUN_1402cde30` case 0xb →
   `FUN_1402c87d0` → the sender's ctrl slot 0xc8 [read], which moves H
   0xb → 0xc. H moves itself 0xc → 0xd in `FUN_1402be490` [read], then 0xd
   exports the snapshot and sets 0xe. G, at state 4, imports it **natively**
   (the mod's `FUN_1402c2fa0` detour must be off for this path; the 30 s
   accumulator `+0x1dc` was zeroed by the last import and only state 4 adds to
   it), goes to 5 (presences from the blob), 6 (permission, 20 s limit at
   `0x14157c304`), sends its player data, and sets 7. H's slot 0xd0 sees bit
   0x10 clear → 0xf → `FUN_1402c03e0` → 0x10.
7. **Re-arm the enemy sync (both).** `FUN_140517040(root[5])` once both are at
   0x10 / 7; the tick binds it to the new map (H state 1, G state 2 once
   `+0x198` opens).
8. Curtain up; `TravelRelease`.

About the network silence (`FUN_140514020` skipped during the host's load):
it was built for the enemy sync's crash at `+0x5180a8` and bought the
~20 % → ~70 % improvement. With the sync stopped by `FUN_140517080` it is
not needed for that consumer, but the other subsystems it also froze
(presences, root[3]) were never isolated. Keep it on for the first legs and
take it off as a separate experiment.

### 3.3 The documented veto, and why rank 1 is the case it allows

`DS2_PRESENCE_REBUILD_PLAN.md`: "the player data packet (`0xd`) is only
accepted by the host in state `0xe` and by the guest in `0xc`; in `0x10`/`7`
the session vetoes it." The code behind it [read]: packet 0xd → router case
0xd → `FUN_1402cf910` → `FUN_1402c8330`, which finds the sender's ctrl and
calls its slot 0xd0. On the summon host that is `FUN_1402bd720`, whose first
line is `if (*(int *)(param_1 + 0x150) != 0xe) return 0;` — and a 0 return
skips the registration of the presence. On the guest the registration is
allowed when the join ctrl's slot 0xc8 (`FUN_1402c1ce0`: 1 for states 0–2,
2 for 4–5, 3 for 6–8) is 0 or `FUN_1402d4fc0(packet)` accepts it.

So the veto is a **phase** veto on the ctrl, not a Steam-level or registry
one. Rank 1 does not fight it: it moves the host back through 0xe, which is
exactly when the guest's state-6 packet arrives. What the veto doc did not
have is the second condition in the same function, bit 0x10 (2.3): with it
set, the packet arrives in the right phase and is refused anyway, with reason
8. Every earlier attempt to re-run the handshake after a host warp would have
hit that.

**The closest existing test** is the M2 replay of 12/09: the guest alone was
taken back through the join (state 1 → 2 → the flag-1 warp) and reached
state 7, the host stayed at 0x10 the whole time, and the host sent
`RequestNotifyLeaveGuestPlayer` 23 s later. [inferred] That is what the guest
half does without the host half: the host never saw a join, never exported,
never accepted player data. The delta rank 1 adds is precisely the host's
participation (10 → 0xb … 0xe → 0xf → 0x10).

### 3.4 What it costs

Four data writes (`+0x1b8` bit, `+0x150 = 10`, `+0xf8 = 1`, the go-home block
restore), three game calls (`FUN_14051c820`, `FUN_140517080`,
`FUN_140517040`) and one switch that turns the snapshot detour off for this
path. All the transport pieces exist (native bonfire chain, guest phantom
warp). The curtain is the game's own loading screen on both sides.

### 3.5 Why it might still fail

- [inferred] The five dangling-consumer crashes of 16/09 happened with the
  presences removed and the network tick silenced, but with the enemy sync
  **held** at state 1 by the machine. Step 1 removes that consumer at the
  source. The map-layer crash at `+0x3f39b3` (`MapModelComponent`
  family), seen inside the silence window, is not explained by anything here;
  if it comes back, rank 1 has the same problem as every native host warp.
- The 17/09 recipe also lost the session ~190 s after arrival, from the
  guest's side (`RequestNotifyLeaveSession`, the guest's member list losing
  the host). That recipe skipped guest states 5 and 6 and left the host at 0xe
  or with a hand-written 0x10; whether the native 4→5→6→7 path avoids it is
  unknown.
- Guest state 4 gives the host **30 s** (`0x14157c300`, file value 30.0) to
  export, else `slot 0x30(0xf)` ends the join. The host's load plus the export
  must fit.
- Writing `+0x150 = 10` on a ctrl that is past 10 is untested; other code may
  read "10" as "summon not finished" (the multiplay mode in `FUN_1402cb8f0`,
  the ctrl's slot 0xf0 returning 1 instead of 3).

## 4. Rank 2 — an invisible re-summon

If the session must not be preserved at all costs, the fallback's 75 s can
shrink to "two loads in parallel". Where it goes today [inferred from the
docs]: guest trip home (a load), the party mod's sign placement, the server's
sign poll, the host's summon, the guest's join load.

What the binary says can be cut [read]:

- the guest's trip home is one warp in state 8 (`FUN_1402c3900`: builds the
  request from `+0x1a0..`, `FUN_1401c2a80(ctx,&local_d8,0)`), followed by
  `FUN_140517080` and `FUN_1405205d0`. The mod already knows how to rewrite
  that warp's destination (M1/M2). Suppressing it instead leaves the guest
  standing, alone, in the host's old map with its session gone.
- a guest enters a host world with **one** flag-1 warp from wherever it
  stands (`FUN_1402c2a80`, and the arena joiner `FUN_1402b9750` enters from
  its own world). It does not need to be home first.

So: host and guest end the session legally in the arena's order
(`WaitGuestDisconnect` → `LeaveSession`), the guest's go-home warp is held,
the host travels natively (clean, measured twice), and the party re-forms at
once with a server-side direct pairing instead of a sign poll. Cost: server
work plus a held warp on the guest. It is not seamless by the brief's
definition — it is a new session, `RequestNotifyLeaveSession` and
`JoinSession` reach the server — and the brief's M2 criterion explicitly
rejects a re-summon. It is ranked second because nothing in it is unproven by
the game itself.

## 5. The directions that do not hold

**Suppress only the warp teardown (rank 3).** Now that the ctrl-side teardown
is known to be case 4 (2.1) and bit 0x10 (2.3), suppressing it is two lines:
be at 0x10, clear the bit. That removes the session end. It does nothing for
the consumers that walk the old world — the 16/09 series already had the
session surviving and the host crashing at `+0x5180a8`, `+0x3ce81c`,
`+0x1e5c30`, `+0x3f39b3`. On its own it is a sub-step of rank 1.

**Travel as a death (rank 4).** M2 step 8 already respawns at a bonfire on
another map, and it does it with the backread transport
(`DS2_SEAMLESS_COOP_TASKS.md`, M2 step 8: "`DS2_BackreadHook` forces the owner
of the bonfire's map … with no warp"). That is the transport whose corruption
we are escaping. A *real* death instead (HP to 0, reason 1 warp) is a host
native warp plus case 0 (2.2), which arms bit 0x10 and, in vanilla, ends the
co-op anyway. Nothing to gain.

**Elevators, fog walls, cutscene and event warps (rank 5).** Every warp in
the game goes through `FUN_1401c2a80` (the project's own finding, confirmed
by the callers above: `GoDuelField`, `ReturnMyWorld`, the join, the go-home
all call ctx slot 0x40). Event-driven transitions are either same-map
streaming or that entry. The only in-session carrier the game has is the
arena flow of section 1; `EventPhantomReturn` (constructed by
`FUN_14018d660`, owned by `EventResultManager`) is the event that sends
phantoms home, the opposite of a carrier.

**Running the travel through the duel ctrl literally.** Tempting because the
chain exists, but the duel ctrl ends on death (case 0 → 0x11), shows the duel
dialog and consumes the start item, and its end is `LeaveSession` +
`ReturnMyWorld`. Porting the *order* (rank 1) is the useful part.

## 6. Unknowns

- Which multiplay mode (`root[3]+0x68`) co-op gets, and whether duel gets 0
  or 6 (enemy sync never bound in the arena) — the arena being safe may owe
  something to that.
- What map the arena host puts in the 0xb payload: `*(*(root[4]+0x5b8)+0xc)`
  is read **before** `GoDuelField`. Either the field already names the arena,
  or the guest's join warp map is corrected later; I did not find where.
- Who opens the guest's rebuild gate `NetEnemyManager+0x198`; no byte write
  to `+0x198` other than the constructor, `FUN_140517040` and `FUN_140517080`
  was found by a whole-`.text` scan of `movb/mov r8,0x198(reg)` forms. It may
  be written through a pointer: the best lead is `lea 0x198(%rbx),%rcx` at
  `0x1405061ca`, which hands the address of a `+0x198` field to a callee.
- Whether `FUN_1402cf7f0` is the packet-0xb send and who calls the join
  ctrl's slot 0x40 (inferred: load completion).
- Whether `FUN_140517e70` (host unbind) leaves no stale records — the
  object-table work owns this.
- What drops the guest ~190 s after a flag-1 warp in the 17/09 recipe.
- Whether a host ctrl written back to 10 behaves (3.5).
- The timeouts `0x14157c300` (30.0) and `0x14157c304` (20.0) are the file's
  initial values; a param may overwrite them at runtime.

## 7. Measurements that would confirm it

In order of cost; the first three need no travel.

1. **Bit 0x10 and the 300 s watchdog.** In a live co-op, read
   `ctrl+0x1b8`, `+0x1b4`, `+0x08`. Native-warp the host in a session whose
   clock is past 300 s: prediction — `+0x150` goes to 0x13 on the next tick
   and `RequestNotifyLeave*` follows in seconds. Same warp with bit 0x10
   cleared right after the call: prediction — no drop from the host side.
2. **Case 0 is death.** Kill the host with the M2 hook in `observe` (a real
   death): `+0x1b8` gains 0x10 and `+0x1b4` jumps to the clock. With `cancel`
   / `respawn` (HP never reaches 0): no change.
3. **The enemy sync's own stop.** Call `FUN_140517080(root[5])` in a live
   session without travelling: `+0x08` 0 and `+0x74` 0, and — unlike the 16/09
   write — still 0 thirteen seconds later. Then `FUN_140517040`: it rebinds
   within a tick (`+0x08` back to 1 on the host, count at `+0x0c` > 0).
   Also read `root[3]+0x68` here: that is the co-op mode.
4. **Re-join in place, native.** Same map, no warp: presences out, sync
   stopped, host `+0x150 = 10`, guest `+0xf8 = 1`. Pass: host back at 0x10
   through 0xb/0xc/0xd/0xe/0xf **without** the snapshot detour, guest at 7
   through 4/5/6, **one** presence per side created by the game, each seeing
   the other move. Then the same with bit 0x10 set by hand before the "I'm
   in": prediction — end with reason 8. That pair proves 2.3.
5. **One leg of rank 1**, Majula → Heide, with the full ruler of
   `DS2_NATIVE_TRAVEL_PLAN.md` §8 (no `excecao`, no caught failure, verified
   session, both arrived) and a watch of several minutes, since the 16/09
   crashes were sometimes late. Then the leg that returns to the session's
   first map, which is where today's transport dies.

Baseline both saves before 4 and 5: they can crash, and a crash in a live
session costs a strike.
