# Rejoin in place: running the join's world load again inside a live session

Static reading on 18/09, Ghidra headless (`-readOnly`, project copy `proj-a3`)
and `objdump` on `DarkSoulsII.exe` 1.03 / Calibrations 2.02. Nothing was run
against the games. Every claim about the binary names its address. Lines
marked **[read]** are decompiled or disassembled code. Lines marked
**[inferred]** are my reading of what that code is for, and they need a
measurement before anything is built on them.

Read this with `DS2_NATIVE_TRAVEL_PLAN.md` (sections 2 and 11),
`DS2_SEAMLESS_COOP.md` ("The session is a state machine" through "The host's
timeline") and `DS2_WORLD_STATE.md`.

## The short answer

- **From state 7 the guest's join machine has no way back to the warp.** Its
  only exits are 8, when `+0x1cc` is set, and "end" through `+0x120`. The state 2
  handler and the import both *end the session* if they run in any state other
  than their own.
- **But the whole join is driven by messages and loader callbacks, not by the
  guest's per-frame dispatcher**, and every step checks one state value. So
  the join can run a second time if two states are written: the guest to 2 at
  the instant the invitation arrives, and the host to `0xa`.
- **The host can produce a fresh invitation *and* a fresh export at its
  current map on demand.** State `0xa` (`FUN_1402bf440`) builds the invitation
  from the host's current map and position. State `0xd` (`FUN_1402bf8f0`)
  exports the current map. Both read the map live from `*(*(0x141616cf8+0x20)+0x5b8)+0xc`.
  The invitation also hands the guest a **new net id**.
- Apart from the constructor (`FUN_1402c1620`, at `0x1402c16de` and
  `0x1402c18cc`), the guest's state 2 handler is the **only** writer of the join
  controller's `+0x19c` in `0x1402a0000–0x1402e0000` (`0x1402c2bb2`; the other
  two hits in that range, `FUN_1402bc3f0` and `FUN_1402bdc10`, belong to the
  host's class). The mod's `snapshot` detour writes it by hand as well; what is
  new here is the native path writing it. That field is the map the guest's
  object sync binds to (`FUN_140517880` → `FUN_1402c6de0` → join ctrl slot `+0xf0` = `+0x19c`), so a
  rejoin fixes at the source the bug in section 10 of the plan, where the crash
  followed the map the session was formed on.
- Five things break it. Each is avoidable. Details are in section 5: the
  guest's go-home record gets overwritten; the host allocates a *new* member
  slot for the guest; a host warp notice or armed flag bits during the window
  end the session; any failure in guest states 2–6 leaves the P2P session
  at once (`FUN_1405205d0`), through a path the M1 hook does not cover; and the
  guest's net id changes, which stales anything the mod keyed on the old one.

## 1. The guest: `NetSummonJoinMultiplayCtrl`, vftable `0x1410d7bd8`, state `+0xf8`

### 1.1 The vtable slots that matter

| slot | offset | function | what it does **[read]** |
| --- | --- | --- | --- |
| 3 | `+0x18` | `FUN_1402c3600` | `slot30(0x12)`, then `FUN_1405205d0(+0x100)`: fail and leave the P2P session |
| 4 | `+0x20` | `FUN_1402c3630` | per-frame dispatcher |
| 5 | `+0x28` | `FUN_1402c2a80` | **state 2 handler: the invitation.** Reached from message 10 (see 1.3) |
| 6 | `+0x30` | `FUN_1402c2820` | "fail with reason" (see 1.4) |
| 8 | `+0x40` | `FUN_1402c1f60` | **3 → 4**: `if +0xf8==3 && +0x120==0 { FUN_1402cf7f0(); +0xf8=4 }`, else 5 |
| 9 | `+0x48` | `FUN_1402c1ee0` | `+0x120 != 0 \|\| +0xf8 > 4` |
| 10 | `+0x50` | `FUN_1402c2fa0` | **import** of the host's world, only when `+0xf8 == 4`; else `+0xf8=5, +0x120=1` |
| 11 | `+0x58` | `FUN_1402c1fe0` | **5 → 6**: builds own player data, `FUN_1402ce4e0` sends message `0xd` to each member; also 9 → 10 |
| 12 | `+0x60` | `FUN_1402c1fd0` | `slot30(1)`: the loader's "load failed" callback |
| 14 | `+0x70` | `FUN_1402c1ad0` | message `0xe` pre-check (looks up the sender in the presence registry) |
| 15 | `+0x78` | `FUN_1402c2420` | message `0xe`: if the record names the **local** player, `+0x1d0 = 1` (unlocks 6 → 7); otherwise it registers another player |
| 16 | `+0x80` | `FUN_1402c22f0` | message `0xf`: sets `+0x1d4/+0x1d5` |
| 20 | `+0xa0` | `FUN_1402c2f20` | EndSession(reason), writes `+0x1cc` (the M1 hook's target) |
| 21 | `+0xa8` | `FUN_1402c1cd0` | `+0xf8 == 7` |
| 25 | `+0xc8` | `FUN_1402c1ce0` | phase: 0–2 → 1; 3 → 1/2; 4–5 → 2; 6–8 → 3 |
| 26 | `+0xd0` | `FUN_1402c1ac0` | `+0xf8 == 0xc` (the manager deletes the ctrl when true) |
| 30 | `+0xf0` | `FUN_1402c1db0` | returns `+0x19c` (the session map) |

### 1.2 Every state

Dispatcher `FUN_1402c3630` (slot 4). Its switch covers 0, 1, 4, 5, 6, 7, 8, 10
and `0xb`. States 2, 3 and 9 have no case: they are advanced only by messages
or loader callbacks.

| state | who acts | what happens **[read]** | session objects touched |
| --- | --- | --- | --- |
| 0 | `FUN_1402c37a0` | arrival guard `*(+0x108)+8` set → `slot30(0x13)`; `FUN_140520450(P2P, +0x20)` joins the P2P session → 1, else `slot30(0xe)`, `0xb` | **creates** the P2P membership |
| 1 | `FUN_1402c4450` | P2P object `+0xa4 == 4` and `FUN_14051fdc0` → `FUN_1402ceb30` (join request, message 4) → **2**; else fail | sends message 4 |
| 2 | slot 5 `FUN_1402c2a80` (message 10) | see 1.5. **Warps** (reason 4, flag 1) → **3** | presence registry `FUN_14051c6a0` (own net id), `FUN_140500fd0(ctx+0x22e0)` |
| 3 | slot 8 (loader callback, see 1.3) | `FUN_1402cf7f0`: `root[0]->slot 0xa0(0xb, …)`, broadcasts message `0xb` → **4** | message `0xb` to the host |
| 4 | dispatcher | `+0x1dc += dt`; at **30 s** (`0x14157c300` = 30.0) `slot30(0xf)` | — |
| 4 | slot 10 (message `0xc`) | import (flags, event values, bonfires, object state, dead counters, member records `+0x1050`, `FUN_1404434c0`); then `+0x1dc=0`, **`+0xf8=5`**, `FUN_140520650(P2P)` (P2P `+0xf8=2`), **`FUN_140516370(*(root+0x28))` = sync `+0x198 = 1`** | object sync gate reopened |
| 5 | dispatcher `FUN_1402c3c80` | if `+0x120==0` and `ctx+0xd0`: registers each imported member as a presence, `FUN_14051b0e0(presreg, …)` | presence registry (pending entries) |
| 5 | slot 11 (loader callback) | `FUN_14051ba70` own data; `FUN_1402ce4e0` sends message `0xd` to each member; `FUN_1405207a0` (P2P `+0xf8=3`) → **6** | message `0xd` |
| 6 | dispatcher `FUN_1402c45b0` | `+0x1e0 += dt`; role check or **20 s** (`0x14157c304`) → `+0x120=0xf`, `FUN_1405205d0` (**leave P2P**). With `+0x1d0` set: `FUN_1402d6810`, member records, `FUN_1402bbd40` → **7** | notification to listeners |
| 7 | dispatcher `FUN_1402c3830` | `+0x120 != 0` → `slot 0xa0(3)`; **`+0x1cc != 0` → 8**; nothing else | — |
| 8 | `FUN_1402c3900` | go-home warp built from `+0x1a0..+0x1c8`, → 9, `FUN_140517080(sync)`, `FUN_1405205d0(root[0])` | leaves the P2P session |
| 9 | slot 11 | → 10 | — |
| 10 | `FUN_1402c4240` | leave notifications, `FUN_14051c6d0(presreg)`, `+0x1cc=0`, → `0xc` | presence registry cleared |
| `0xb` | dispatcher | → `0xc` | — |
| `0xc` | manager tick `FUN_1402c9540` | slot `0xd0` true → ctrl deleted, `FUN_1405205d0(root[0])` | ctrl destroyed |

**Which states warp:** 2 (`FUN_1402c2a80`, `call *0x40(%rax)` at `0x1402c2e45`,
into the host's world with reason 4 and flag 1) and 8 (`FUN_1402c3900`, the
go-home warp with flag 0). No other state warps.

### 1.3 How messages and the loader reach the ctrl

Messages arrive in `FUN_1402cde30` (switch on the type) and are routed to the
manager `*(0x141616cf8+0x18)`. **[read]** Its `+0x40` is the local join ctrl,
and `+0x48..+0x50` is the list of accept ctrls (one per guest, on the host):

| msg | parser | manager | ctrl slot hit |
| --- | --- | --- | --- |
| 4 (0x1c B) | `FUN_1402cff30` | `FUN_1402c7f50` | host list, by sender id: `+0xb8` `FUN_1402bd560` |
| 5 (4 B) | — | `FUN_1402c8600` | `+0x40` ctrl `+0x30` (fail with that reason) |
| **10** (0x24 B) | `FUN_1402d04f0` (payload floats validated) | `FUN_1402c87b0` | **`+0x40` ctrl `+0x28` = guest state 2 handler** |
| **0xb** | — | `FUN_1402c87d0` | **host list, by sender: `+0xc8` = `FUN_1402bd9c0` (`0xb` → `0xc`)** |
| **0xc** (export) | `FUN_1402d09c0` | `FUN_1402c8d10` | **`+0x40` ctrl `+0x50` = import** |
| **0xd** (player data) | `FUN_1402cf910` | `FUN_1402c8330` | host list `+0xd0` = `FUN_1402bd720`; **only if accepted**, then `FUN_14051b0e0(presreg, sender, data, 1)` |
| **0xe** (100 B) | `FUN_1402cce80` | `FUN_1402c82b0` | `+0x40` ctrl `+0x70`/`+0x78`, then `FUN_14051c4d0(presreg, name)`: sets the spawn flag `+0x631` on the pending entry with **that name** |
| 0xf | `FUN_1402ccda0` | `FUN_1402c8290` | `+0x40` ctrl `+0x80` |

The loader calls the join ctrl twice per load, **[read]** whatever state it is
in:

- `FUN_1401bef10` (the loader step that sets `ctx+0x24ac = 0x18`) →
  `FUN_140513680` → thunk `0x1402c7550` → `mgr+0x40` **slot `+0x40`** (3 → 4).
- `FUN_1401bf200` (the step that ends at `ctx+0x24ac = 0x1e`) → on success
  `FUN_140513740` → `FUN_1402c75a0` → `mgr+0x40` **slot `+0x58`** (5 → 6). On
  failure (a 30 s budget, `0x1410bdc88`), `FUN_140513720` → thunk
  `0x1402c7580` → **slot `+0x60`** = `slot30(1)`.

Both are no-ops in state 7, which is why the mod's guest warp (flag 1) can run
without disturbing the machine.

### 1.4 The failure path the M1 hook does not see

`FUN_1402c2820` (slot `+0x30`) **[read]**:

- if `+0x120` is already set, it does nothing;
- in states 0–2 it records the reason, sends leave events, → `0xb`, and calls
  `FUN_1405205d0(+0x100)`;
- in states 3–6 it records the reason and calls **`FUN_1405205d0` at once**,
  unless the reason is `0x10`/`0x11`;
- in state 7, for a reason outside 8–11, it notifies, then either calls
  `slot 0xa0(3)` (for reasons `0xd, 0x10, 0x11, 0x14`) or sets `+0x120`, which
  the state 7 handler turns into `slot 0xa0(3)` on the next frame;
- at the end: `if +0xf8 == 4 → 5`.

`FUN_1405205d0` is the P2P session leave (`FUN_140a40c40` on the Steam
session). **[inferred]** Every timeout in states 3–6 therefore drops the
session without passing through `FUN_1402c2f20`, where
`DS2_SeamlessSessionHook` sits.

### 1.5 The state 2 handler, as far as it matters here

`FUN_1402c2a80` **[read]**:

```
if (+0xf8 != 2)              { +0xf8 = 0xb; +0x120 = 1; return; }   // 0x1402c2aaf / 0x1402c2ab9
if (*(+0x108)+8 != 0)        { slot30(0x13); return; }              // arrival guard
if (!FUN_1402c6570(role))    { notify; +0xf8 = 0xb; +0x120 = 0x12; return; }
FUN_14051c6a0(presreg, payload[6] /*net id*/, …);
+0x19c = payload[0];  +0x198 = payload[7];
if (ctx[7] && player && ctx+0xd0 && ctx+0x70) {                     // the go-home record
    +0x1a0 = current map (*(presreg+0x5b8)+0xc)
    +0x1a4..+0x1ac = the live player's position, +0x1b4 = its yaw
    +0x1b8..+0x1c0 = *(ctx+0x70)+0x164/0x168/0x16c   (bonfire record)
    +0x1c4 = *(ctx+0xd0)+0x168
}
warp {type 0, reason 4, map payload[0], pos payload[1..3], yaw payload[5]}, flag 1
    fail → slot30(0x13)
FUN_1402bbf20(this)        // may raise a save request: FUN_1402e7410(ctx+0xb8, 5)
FUN_140500fd0(ctx+0x22e0)
local event kind 2 (+0x198, id)
+0xf8 = 3                   // 0x1402c2edb
+0x1c9 = payload[8]
```

**Consequence for a rejoin [read]:** if message 10 arrives while the guest is
at 7, the first line writes `0xb` and `+0x120 = 1`, and the session is over.
And if the handler does run, it **overwrites the go-home record** with the
host world's map and position. State 8 builds the way home from that record
(`FUN_1402c3900`, the `FUN_1402d47a0` branch 0 reads `+0x1a0/+0x1a4..+0x1b4`;
branch 1 reads `+0x1b8..+0x1c0`).

### 1.6 Transitions possible from 7 **[read]**

- `+0x1cc != 0` → 8 (the ordinary end; blocked by M1).
- `+0x120 != 0` → `slot 0xa0(3)` → 8.
- Slot 5 or slot 10 called in 7 → an end, as shown above (the mod's `snapshot`
  detour avoids this by writing 4 and then 7).
- Slots 8 and 11 do nothing in 7.

**There is no native edge from 7 back to 2.** The only way is a write, and
section 4 shows where to make it so that the window is zero frames wide.

## 2. The host: `NetSummonAcceptMultiplayCtrl`, vftable `0x1410d7998`, state `+0x150`

Dispatcher `FUN_1402bddb0` (slot 5, `+0x28`). It first calls
`FUN_1402be090`, the watchdog: if `!(+0x1b8 & 0x20)` and the clock is past
`FUN_1402d8600()`, the session ends; if `+0x1b8 & 0x10` and
`+0x08 - +0x1b4 > 300.0` (`0x1410d7b40`), `FUN_1405205d0` and `0x13`.

A second per-frame hook, slot 18 (`+0x90`) `FUN_1402bdd60`, runs five
"advance when no other accept ctrl is busy" steps **[read]**: 1→2 (others'
`+0x58`), 3→4, 6→7, 9→10 (others' `+0x70`), **`0xc→0xd`** (others' `+0x78`,
i.e. no other ctrl in `0xd/0xe`) — `FUN_1402be310/290/390/410/490`.

| state | handler | what happens **[read]** |
| --- | --- | --- |
| 1 → 2 | `FUN_1402be310` | stepped |
| 2 | `FUN_1402bedf0` | needs `+0x1b8 & 1` (join request received) → 3, or end |
| 4 | `FUN_1402be510` | validates the peer (`FUN_14051fdc0`), builds lists → 5 |
| 5 | `FUN_1402c02d0` | pending list empty and `+0x178` → 6 |
| 7 | `FUN_1402bf1c0` | → 8 |
| 8 | `FUN_1402c0330` | timer + list → 9 |
| 9 → 10 | `FUN_1402be410` | stepped |
| **10** | **`FUN_1402bf440`** | needs `*(+0x188)+9 == 0`, `ctx+0xd0 != 0` and `*(ctx+0xd0)+0x490 != 0`. **Map = current map**; `FUN_14051c5a0(presreg, &id, +0xb0)` **allocates a member slot** and returns the net id; `FUN_1402bdf10` picks the position (override at `+0x80/+0x84/+0x90..+0x98`, else `FUN_140451890(map, …, role)`, else the host's own position and yaw); **→ `0xb`**; local event kind 1; `+0x1b8 \|= 0x40`; **`FUN_1402cf020` sends message 10 (0x24 B)** |
| `0xb` | slot 25 `FUN_1402bd9c0` (message `0xb`) | → `0xc` |
| `0xc` | slot 18, `FUN_1402be490` | → `0xd` |
| **`0xd`** | **`FUN_1402bf8f0`** | waits while `FUN_14051c1b0` says a listed member's registry entry is pending (state 1) or its character is not ready. Then exports **the current map**, `FUN_1402cf1d0` sends message `0xc`, **→ `0xe`** (`0x1402c00a4`) |
| `0xe` | slot 26 `FUN_1402bd720` (message `0xd`) | copies the guest's name; validates `FUN_1402d4fc0`; then the **flag checks**: `0x10` → `FUN_1402be1e0(8)` (end), `4` → end(10), `8` → end(`0xb`), `2` with role outside 1..4 → end(9), role 5 special; otherwise **→ `0xf`** and returns 1, which lets the manager register the guest's presence |
| **`0xf`** | **`FUN_1402c03e0`** | `FUN_1402c08c0`; `FUN_1402ce3e0` **broadcasts message `0xe`** (role, `+0x19c..`, name); events; `FUN_1402b1810` → `FUN_14051c4d0` (the guest's copy spawns); **→ `0x10`**; `FUN_1402bbf20` |
| `0x10` | `FUN_1402bed10` | once (`+0x1b8 & 0x200`): `FUN_1402ce320` with the guest's character |
| `0x11/0x12` | `FUN_1402c0240` | teardown timer → `0x13` |

The warp notice, slot 28 (`+0xe0`) `FUN_1402bd0d0` **[read]**:

- case 0: `|= 0x10`, refreshes `+0x1b4`, and if `slot 0x88` (`== 0x10`) is
  false it ends the session with reason 8;
- case 1: needs a specific `*(ctx+0x70)+0x88+0x14` and `== 0x10`, else it ends
  with `0xb`;
- case 2: `|= 2`, and the role check;
- case 3: `|= 4`, ends with 10;
- case 4: `|= 0x10` (no refresh), and ends with 6 if the state is not `0x10`;
- case 5: role 5 only.

**Correction to `DS2_NATIVE_TRAVEL_PLAN.md` §1** **[read]**: the warp
(`FUN_1401c2a80` → `FUN_1402c7ec0`) passes the constant **4**, not the reason
(`mov $0x4,%edx` at `0x1402c7ee3`). Case 2 comes from `FUN_1402c7dc0`, called
by `FUN_1401d1540`. That fits the measured `0x261 → 0x271` on the host's
native warp: bit `0x10`, which is case 4.

### 2.1 Can the host re-send an export at its current map on demand?

Yes, in two forms **[read]**:

- **Export only:** calling `FUN_1402bf8f0` from `0x10` (what the `snapshot`
  remedy already does) exports the current map and leaves `0xe`. That is the
  import half, without a load on the guest.
- **Invitation + export:** writing `+0x150 = 0xa` makes the next dispatcher
  frame run `FUN_1402bf440`. It builds an invitation for the current map, sends
  message 10, and the rest of the chain (`0xb → 0xc → 0xd` export → `0xe` →
  `0xf` → `0x10`) runs on the guest's messages exactly as in a first join.

## 3. The join, message by message

```
host 0xa  FUN_1402bf440  --msg 10 (map,pos,yaw,netid)-->  guest 2  FUN_1402c2a80: warp flag 1 -> 3
host 0xb                                                   loader FUN_1401bef10 -> slot 0x40
host 0xb  <--msg 0xb--------------------------------------  guest 3 -> 4
host 0xc  -> 0xd (next tick)
host 0xd  FUN_1402bf8f0 --msg 0xc (export, current map)-->  guest 4  import -> 5, sync gate = 1
host 0xe                                                   loader FUN_1401bf200 -> slot 0x58
host 0xe  <--msg 0xd (player data)------------------------  guest 5 -> 6
host 0xe -> 0xf, registers the guest's presence
host 0xf  FUN_1402c03e0 --msg 0xe (the guest's record)-->  guest 6: +0x1d0 = 1
host 0x10                                                  guest 6 -> 7
```

On the guest, the host's copy is born from state 5's imported records
(`FUN_14051b0e0`) and the registry tick, not from message `0xe`. On the host,
the guest's copy is born from message `0xd` (`FUN_14051b0e0`) and the spawn
flag `FUN_1402b1810` → `FUN_14051c4d0` sets at `0xf`. **[read]** for the calls;
**[inferred]** for the split between "record" and "spawn".

This is the same order the measured normal join showed
(`… 0xb → 0xd → 0xe → 0xf → 0x10` on the host, `DS2_SEAMLESS_COOP.md`).

## 4. Proposal: rejoin in place

Preconditions: a verified session (host `0x10`, guest 7), both players alive,
standing, and out of menus; guest `+0x120 == 0`, `+0x1cc == 0`, arrival guard
`*(+0x108)+8 == 0`; and the host has arrived, with `ctx+0x24ac == 0x1e` and
`ctx+0xd0 != 0`.

**Step 0 — the host travels.** Use the current transport, or a native warp
once the other agent's work makes that survivable. Wait until the host is
settled at the destination, and do not start the rejoin while any host warp,
rest or death can still happen (see risk 2).

**Step 1 — host: clean the controller.**

1. Read `ctrl+0x1b8`. Clear bits `0x10 | 0x8 | 0x4 | 0x2` and set
   `ctrl+0x1b4 = ctrl+0x08`. Bit `0x10` is set by any host warp (case 4), and
   at `0xe` it ends the session (`0x1402bd7c0`).
2. Read `ctrl+0x80`. If it is non-zero, the invitation would use the stored
   `+0x90..+0x98/+0x84` override (`FUN_1402bdf10`), which belongs to the old
   map. Either clear it, or write the destination bonfire's spawn point there.
   **Unknown who sets it**; read it live first.

**Step 2 — both: remove the old presences.**

1. On the host: `FUN_14051c820(entry)` on the guest's member entry
   (`presreg+0x1a8 + i*0xd0`). This is already measured as safe for the
   session. Then **wait until that slot is free** (its id at `+0x00` null,
   i.e. `FUN_140a3e270` false). Otherwise `FUN_14051c5a0` at `0xa` allocates a
   *second* slot for the same Steam id.
2. On the guest: the same for the host's entry, so that state 5 does not add a
   second pending copy of the host.
3. Turn off the mod's M8 closure of the guest sync gate (`sync+0x198`), its
   presence re-creation, its network silence and its `snapshot` request for
   this transaction. The native path does all four jobs.

**Step 3 — guest: arm the entry.** `FUN_1402c2a80` (slot 5) is **already
detoured** by `DS2_RespawnInSessionHook` (`ArriveHook`, `kArriveOffset =
0x2c2a80`, prologue checked). The arm belongs in that detour, not in a second
attach on the same entry. When the rejoin is armed and `this+0xf8 == 7`:

```
// the two tests the handler makes after the state check; each failure there
// ends in FUN_1402c2820 states 0-2 -> FUN_1405205d0 (P2P leave, past M1)
if (*(*(this+0x108)+8) != 0)                    refuse: log, stay at 7, disarm
if (!FUN_1402c6570(*(0x141616cf8+0x18), &role))  refuse (it is transient:
                                                 measured no at frame 0 and yes
                                                 96 frames later on 12/09)
saved = copy of this+0x1a0 .. this+0x1c8  (0x28 bytes, the go-home record)
this+0xf8 = 2
call original(this, payload)
if (this+0xf8 == 3) restore saved into this+0x1a0 .. +0x1c8
log payload[0..8], the resulting state, +0x120
disarm
```

Writing 2 inside the detour means no frame ever sees state 2, so no role or
phase getter reads the wrong value, and a message 10 cannot find the guest
at 7. Do **not** restore `+0x19c` or `+0x198`: the new values are the point.

A refusal still leaves the host needing an answer: the invitation has already
been sent, and the host sits at `0xb`. Put it back to `0x10` by hand, and
refuse only *before* the host writes `0xa` wherever possible. The guest can run
the same two tests when it answers "armed".

**Step 4 — host: invite.** Once the guest confirms over the mod's channel that
it is armed: write `host ctrl+0x150 = 0xa`. From here on it is the native
chain in section 3. Nothing else is written.

**Step 5 — watch, and step in only where the game can stall.**

- If the guest sits in **5** after the loader is idle (`ctx+0x24ac == 0x1e`),
  the loader's `+0x58` callback fired before the import arrived. There is no
  timer in 5, so the machine would hang. Call slot `+0x58` on the join ctrl
  once (or `FUN_1402c75a0(mgr)`).
- Log every call to `FUN_1402c2820` (slot `+0x30`) and `FUN_1405205d0` while
  the rejoin is armed. They are the failure signal.

**Step 6 — the ruler.** The rejoin passes only if all of these hold:

- host `0x10`, guest 7, `p2pSessionVerified: true`;
- one live presence on each side;
- guest `+0x19c` equal to the destination map;
- the object sync on the guest bound to the new map (sync `+0x18` and a
  non-zero count);
- flags equal on both sides;
- the go-home record byte-identical to before;
- the guest's bonfire prompt working;
- no `excecao` on either side for minutes afterwards;
- and nothing unusual in the server's `Notify` lines.

## 5. What could break

1. **Message 10 reaching a guest at 7** ends the session (`0x1402c2aaf`). The
   detour must be armed before the host writes `0xa`. And if the detour is
   missing, the host must not write `0xa`.
2. **A host warp notice during `0xa..0xf`** ends it. Cases 0, 1 and 4 all end
   the session when the state is not `0x10`. No warp, bonfire rest, death or
   travel on the host while the rejoin runs.
3. **Host `+0x1b8` bits** `0x10/0x8/0x4/0x2` end it at `0xe`. Clear them.
   `0x20` must stay set, or the first watchdog branch applies.
4. **Member slot duplication.** `FUN_14051c5a0` always takes a new slot and
   bumps the generation at `presreg+0x10`. The table has 5 slots. An uncleared
   old slot leaves two `+0x1a8` entries with the same Steam id, and
   `FUN_14051d4b0` (lookup by id) will find the first. The export's own wait
   (`FUN_14051c1b0` at `0xd`) checks the ids in `+0x130` plus the host's own,
   not the joining guest's slot, so it does not stall on this.
5. **Timers, and where their failure lands.** Guest 4 has 30 s before `0xf`.
   Guest 6 has 20 s. The loader has 30 s before slot `+0x60`. All of them go
   through `FUN_1402c2820` or `FUN_1405205d0` and leave the P2P session. M1
   does not block that. **[inferred]** A clean leave is probably not an illegal
   disconnect, but that is unmeasured, so run it on a save baseline.
6. **Loader/import race.** See step 5. The normal join has the same race and
   evidently wins it (the export goes out when the host reads message `0xb`,
   mid-load), but a slow host could lose it.
7. **The go-home record.** Without the restore, state 8 would send the guest
   to the host world's coordinates on the host's map, in its own world. The
   restore fixes this. `+0x1b8..+0x1c0` are copied from `*(ctx+0x70)+0x164..`,
   which a phantom rest may have changed, so compare before and after.
8. **Duplicate server notifications [inferred].** Host `0xf` raises a kind-4
   event (`FUN_14028f380` → `FUN_1402901b0` → `FUN_140291fd0`). The guest's
   state 2 raises kind 2, and state 6 calls `FUN_1402bbd40`/`FUN_1402d6810`.
   These may re-announce the join to the server. The server log answers this.
9. **Bit `0x200` on the host** keeps `FUN_1402ce320` from running a second
   time at `0x10`. What that call does is unknown; if the host's copy of the
   guest lacks something after the rejoin, clear `0x200` before `0xa`.
10. **A save request on the guest.** `FUN_1402bbf20` in the state 2 handler
    calls `FUN_1402e7410(ctx+0xb8, 5)` (`+0x1a2 = 1`, level 5). **[inferred]**
    That is the save system, and it happens on every normal join too. A guest
    saves its stored copy, not the host's world (`DS2_WORLD_STATE.md`).
11. **The guest's load is a real load with the session live.** The mod's
    measured phantom warp is the same warp (reason 4, flag 1) and survives.
    Its worker-thread crash in `MapModelComponent` teardown (plan §5,
    17/09 17:06) can still happen here.

12. **The guest's net id changes.** `FUN_14051c5a0` returns
    `generation*0x100 + 1 + index`, and the guest adopts it through
    `FUN_14051c6a0` → `FUN_14051c700(presreg+0x174, id, …)`. The measured
    baseline had the guest at 513 and the host at 32512. Anything keyed on the
    old id goes stale: the object-sync records, the mod's captured presence
    blob, anything cached on the channel. After a rejoin, capture again rather
    than reusing.

## 6. Still unknown, and the measurement for each

| unknown | measurement |
| --- | --- |
| Does `FUN_14051c820` + the registry tick free the member slot (id cleared), and how soon? | after `presenca retira` on the host, read `presreg+0x1a8+i*0xd0` `+0x00` (id pointer) and `+0x48` every 100 ms |
| Host `ctrl+0x80` and `+0x84/+0x90..+0x98` in a live session | one `chain` read of the host ctrl |
| Host `*(ctrl+0x188)+9`, the outer gate of `0xa` (`+0x188` is probably the manager: `FUN_1402c6d70(*(+0x188), …)` returns a role list) | the same read |
| The guest's net id before and after | guest `presreg+0x174` (short), and the host's slot index and generation |
| Does the loader's `+0x58` callback land after the import on the guest? | `bp 2c1fe0 deref rcx+f8 4` together with `bp 2c2fa0`; order and states |
| Does the whole chain run from a write of `0xa`? | a detour log on `FUN_1402bddb0` (already exists) plus the guest's slot-5 detour; expected host `0xa→0xb→0xc→0xd→0xe→0xf→0x10`, guest `7→(2)→3→4→5→6→7` |
| Server reaction to a second join | `timeline --run` for `RequestNotifyJoinGuestPlayer/JoinSession` duplicates |
| What `FUN_1402ce320` (host `0x10`, bit `0x200`) adds | compare the host's copy of the guest with and without clearing `0x200` |
| The object sync after the rejoin | read `*(0x141616cf8+0x28)` `+0x08/+0x0c/+0x18` on the guest: the state leaves 0 via the gate, and the map matches `+0x19c` |

**The cheapest first test, as a control and on a save baseline:** a rejoin on
the **same map**, with neither player travelling. This separates "the machine
can run twice" from anything about travel. If it passes, repeat it after a
host move on the current transport (Majula → Heide). Only after that, try it
with a native host warp.
