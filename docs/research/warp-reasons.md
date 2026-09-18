# Warp reasons, and what actually ends a session on a warp

Read-only survey of `DarkSoulsII.exe` 1.03 / Calibrations 2.02, done on
18/09 in a private Ghidra copy (`-noanalysis -readOnly`) plus `objdump` and a
byte scanner over the executable. Nothing was run against the games. Every
address below is a runtime address with the image at `0x140000000`; subtract
the base for a hook offset.

Two kinds of statement are kept apart all the way through:

- **read**: a decompiled line or an instruction, quoted with its address;
- **inferred**: my reading of what the code is for, or of how two read facts
  combine. Those are marked *(inferred)*.

## The short version

1. **The warp's reason does not decide whether the host's session survives.**
   The warp tells every host-side session controller the same constant, `4`,
   whatever the reason was. A host controller that is playing (`+0x150 ==
   0x10`) is **not** ended by any warp. The reason reaches only a list of
   network listeners (summon/monitor jobs, NPC phantoms), and only one of them
   reads it.
2. What a host warp does to the session is **set one bit**, `ctrl+0x1b8 |=
   0x10`, and never clear it. That bit has two consumers, and both are
   session killers: the 300 s watchdog (`FUN_1402be090`), which fires at once
   if the controller is older than 300 s because `+0x1b4` is never refreshed,
   and the host's re-entry handshake (`FUN_1402bd720`), which ends the session
   with code 8 if the guest ever tries to join again.
3. **The guest's session controller is never told about a warp at all.** Its
   only warp-time exposure is the loader's end-of-load hooks, and those only
   act in join states 3, 5, or on a load timeout.
4. `docs/DS2_NATIVE_TRAVEL_PLAN.md` §"The conclusion" point 1 is **wrong about
   which branch a warp takes**: the warp calls case **4** of `FUN_1402bd0d0`,
   not case 2. Case 2 comes from a character event (`FUN_1401d1540`). The
   16/09 phase 1 measurement (`ctrl+0x30 == 1`) tested a branch the warp
   never runs. Its conclusion (the host's warp does not immediately end the
   session) is still right, for a different reason.
5. The game already contains a **session that survives a host warp**: the
   Brotherhood-of-Blood style arena duel (`NetDuelAcceptMultiplayCtrl`,
   jobs `GoDuelField` → `WaitGuestWarpFinished` → … → `ReturnMyWorld`). And it
   contains the **clean unbind** for the object sync that crashed the host on
   16/09: `FUN_140517080`, which the game calls right after every
   session-ending warp.
6. **Proposal:** the host travels natively (reason 2, unchanged), and three
   game-native levers replace the transport and the forced writes: clear bit
   `0x10` after the warp, unbind/re-arm the object sync with the game's own
   pair (`FUN_140517080` / `FUN_140517040`), and **re-run the game's own join
   from host state `0xa` / guest state `2`** so that the invitation, the
   snapshot export, the import, the presence registration and the "I am in"
   handshake all come from the game. Details and order of operations in the
   last sections.

## 1. Every caller of the warp

### How the inventory was made

The warp is `FUN_1401c2a80`, slot `+0x40` of the global context's vtable. The
vtable is at `0x1410c4c68`; the file has exactly one pointer to
`0x1401c2a80` (at `0x1410c4ca8`), and **no direct `call rel32`** to it. So
every caller is an indirect `call [rax+0x40]` on the context.

Two independent inventories were made, and they agree:

- **byte scan**: every RIP-relative `mov r64, [0x1416148f0]` in `.text`
  (3,958 of them) followed within 80 bytes by `ff 5x 40` (`call [reg+0x40]`);
- **decompiler sweep**: every function containing one of those 3,958
  references was decompiled (a few thousand functions) and grepped for
  `(*DAT_1416148f0 + 0x40))(DAT_1416148f0`.

Both found the same 11 call sites (the byte scan's extra hits were other
objects' slot `+0x40`, rejected by reading them). **Their shared blind spot:**
a caller that reaches the context through a parameter or a struct field
instead of the global. That blind spot is real — reading the loader by hand
turned up a 12th site, at `0x1401bf885`, inside the in-game state of the
loader, where the context is `rsi`. There may be more of that shape. The
runtime log (`DS2_Seamless.log`, `de=+0x...`) remains the ground truth for
completeness.

### The reasons exist, 0 to 6

The warp indexes three 7-entry tables by the reason when `reason < 7`
(`0x1401c2a80`, decompiled):

```c
if (((*(byte *)(ctx + 0x24b2) & 8) == 0) && ((uint)req[1] < 7)) {
    uVar8 = *(undefined4 *)(&DAT_1410c4ee8 + reason * 4);   // fade out, float
    uVar7 = (&DAT_1410c4f24)[reason];                        // byte
    *(undefined4 *)(ctx + 0x24b4) = *(undefined4 *)(&DAT_1410c4f08 + reason * 4);
}
```

Read from the file: `0x1410c4ee8` = `0, 3.0, 2.0, 2.0, 2.0, 2.0, 0`;
`0x1410c4f08` = `0, 6.0, 2.0, 2.0, 2.0, 2.0, 0`; `0x1410c4f24` =
`1, 0, 1, 1, 1, 1, 1`. Reason 1 (death) gets the long fade; 0 and 6 get none.

### The table

| # | call site (the `call [rax+0x40]`) | function | gameplay event | reason (`+0x04`) | flag (3rd arg) | how the request is built |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | `0x14044fe1f` | `FUN_14044fde0` | death → last rested bonfire (called by `FUN_140190920`; `FUN_140190950` takes it only when there is no session manager) | **1** | 0 | `FUN_14044ed40`: `param_2[1] = 1` always; family 3/4/2 from the respawn record `+0x168` |
| 2 | `0x1401bf885` | in-game loader state (`0x1e`), context in `rsi` | a respawn issued by the loader itself; the branch leading to it is obfuscated | **1** | 0 | `FUN_14044ed40(*(ctx+0x70), &req)` then `call [rax+0x40]` with `xor r8d,r8d` (read at `0x1401bf84a..0x1401bf885`) |
| 3 | `0x140184af6` | `FUN_140184a10` (travel phase machine) | whatever the travel object carries (rows 3a–3d) | from the builder | 0 | `FUN_140184830` copies a prepared request into the travel object |
| 3a | | `FUN_14017fdb0` → `FUN_1401843b0(&req, id, 2)` | **bonfire menu travel** | **2** | 0 | also writes the local respawn record (`FUN_14044fe30`) |
| 3b | | `FUN_140461f20` case `0x2041d` → `FUN_140184450` | script-driven warp to a map and position (`local_58 = 4; local_54 = 3`) | **3** | 0 | map, position and angle from script arguments *(inferred: an ESD/talk-script command; the eagle and scripted falls are candidates, not confirmed)* |
| 3c | | `FUN_140452140` → `FUN_1401843b0(..., 5)`; `FUN_1404521b0` → `FUN_140184610(..., 5)` | script warp to a bonfire id / to a map point | **5** | 0 | reached only from the obfuscated block at `0x14014c3da..0x14014c406` |
| 3d | | `FUN_140452200` → `FUN_140184660` | "go back to where you rested", with fallbacks to Majula `0x122a` / Things Betwixt | **5** (family 4 reason 5 for `m20_21` point 1700000) | 0 | *(inferred: Homeward Bone / Aged Feather style item)* |
| 4 | `0x140184c30` | `FUN_140184bd0` (same travel object, phase 4 set directly) | same as row 3 | from the builder | garbage (`r8` not set in the decompile) | |
| 5 | `0x1402c2e45` | `FUN_1402c2a80`, `NetSummonJoinMultiplayCtrl` slot `+0x28` (join state 2) | **co-op guest enters the host's world** | **4** | **1** | from the host's invitation (P2P message 10) |
| 6 | `0x1402c3bdb` | `FUN_1402c3900`, join state 8 | **co-op guest sent home** (session end) | **4** | 0 | go-home record `+0x1a0..+0x1c8`, form picked by `FUN_1402d47a0` |
| 7 | `0x1402b9958` | `FUN_1402b9750`, `NetDuelJoinMultiplayCtrl` slot `+0x28` | **duel/invader guest enters** the host's (or arena's) world | **4** | **1** | `local_b8 = 0x400000000` = family 0, reason 4 |
| 8 | `0x1402ba940` | `FUN_1402ba8d0` (duel join teardown) | duel guest sent home | **4** | 0 | `FUN_1402bc020(saved, &req, code, role)` |
| 9 | `0x1402b4345` | `FUN_1402b4310`, job `GoDuelField` (RTTI `GoDuelField@?A0x89915c51`) | **arena host goes to the duel field, with the P2P link up** | **4** | 0 | `FUN_14027be20`: `*req = 4; req[1] = 4`, map from a 6-entry table at `0x141613b20` |
| 10 | `0x1402b44d6` | `FUN_1402b4490`, job `ReturnMyWorld` | arena host goes back | **4** | 0 | `FUN_1402bc020(..., 3)` |
| 11 | `0x140040178` | `FUN_140040060` | debug / new-game start: Things Betwixt (`m10_02`) or Majula bonfire `0x122a` | **6** | 0 | literal |

Also on the context's vtable, but **not** a warp: slot `+0x38`,
`FUN_1401c2cf0`, is **quit to title** (sets `0x24b1 |= 4`, clears bits `0x28`,
and notifies the session manager with reason `-1`; callers `FUN_140052fd0`,
`FUN_14006ec90`, `FUN_140502370`).

**Elevators, area transitions and the Dark Chasm have no caller of their own
in this list.** Elevators and ordinary area transitions move the player inside
the loaded maps and are not warps *(inferred from their absence; the one
unresolved family is rows 3b/3c, whose callers are script commands)*. The
Dark Chasm / Pursuer's eagle / "sent home as a phantom" cases must fall into
rows 3b, 3c, 6 or 8; which one is a runtime question (§7).

### The gate at the top of the warp

`0x1401c2ab6..0x1401c2acf` (read, raw instructions):

```
mov  ecx, [rdx+4]      ; reason
cmp  ecx, 1  ; je accept
cmp  ecx, 4  ; jne gate
test r8b, r8b ; je accept
gate: call 0x140248940 ; test al,al ; jne refuse
```

`FUN_140248940` (read):

```c
if (*(longlong **)(DAT_1416148f0 + 0xd0) == 0) return false;
return (*(code **)(**(ctx+0xd0) + 0x1b0))() == '\0';
```

So reasons 2, 3, 5, 6 and 4-with-flag-1 are refused when the local
character's virtual `+0x1b0` returns 0. *(Inferred: that virtual is "alive";
attempt 1 in `DS2_SEAMLESS_COOP.md` — reason 4 flag 1 on a dead guest — was
refused by this gate, and the case-0 session notification below fires on its
true→false edge.)* This corrects `DS2_NATIVE_TRAVEL_PLAN.md` §1, which
describes the gate as a multiplay time counter.

(Minor: the prologue quoted for `+0x1c2a80` in that section,
`40 55 41 54 41 56 ...`, is not what the file holds, which is
`48 89 5c 24 08 48 89 74 24 10 57 48 81 ec 90 00`.)

## 2. Where a warp tears a session down

### What the warp calls, in order

`FUN_1401c2a80` (read, `0x1401c2ad5..0x1401c2afd`):

```
mov  rcx, [rbx+0x22f0] ; test ; je skip
call 0x1405132e0        ; the session manager (raiz[3] of 0x141616cf8)
mov  edx, [rdi+4]       ; the reason
call 0x1402c7ec0
```

`FUN_1402c7ec0(manager, reason)` (read, `0x1402c7ecf..0x1402c7f28`):

```
loop over manager+0x48 .. manager+0x50:     ; the accept controllers (host side)
    mov rcx,[rbx] ; mov edx, 4 ; mov rax,[rcx] ; call [rax+0xe0]   ; <- constant 4
loop over *(0x141616cf8)+0x50 .. +0x58:     ; NetEventListener list
    mov rcx,[rbx] ; mov edx, esi ; mov rax,[rcx] ; call [rax+0x38] ; <- the reason
```

The same manager has sibling notifiers for the other cases, each passing its
own constant to slot `+0xe0` (read by byte scan for `ba 0N 00 00 00` before
`ff 90 e0 00 00 00`):

| case | notifier | its caller | event *(inferred)* |
| --- | --- | --- | --- |
| 0 | `FUN_1402c7b00` (`xor edx,edx`) | `FUN_140251040` | the local character's `+0x1b0` virtual goes true→false |
| 1 | `FUN_1402c7d40` | `FUN_140181490`, phase 3 | the area's boss fight ends (the manager with the 3 boss groups, message `0x12`) |
| 2 | `FUN_1402c7dc0` | `FUN_1401d1540`, `+0x86 == 2` | a character event |
| 3 | `FUN_1402c7e40` | `FUN_1401d1540`, `+0x86 == 5` | a character event |
| **4** | `FUN_1402c7ec0` | **the warp** (and quit-to-title with `-1`) | **any warp** |
| 5 | `FUN_1402c8d30` | `FUN_140191df0` | another player died |

The join controller (the guest) is **not** in `manager+0x48`: it lives at
`manager+0x40` (read: `FUN_1402c8600`, `FUN_1402c8f30`, `FUN_1402c9220` all
dereference `*(manager+0x40)` for it). On a guest the accept list is empty,
so **a warp on the guest's machine touches no session controller at all**,
which matches the measurement "guest travels alone → session survives 5 min".

### Host: case 4 of `FUN_1402bd0d0`

`NetSummonAcceptMultiplayCtrl` (vftable `0x1410d7998`), slot `+0xe0`
(read):

```c
case 4:
    *(uint *)(ctrl + 0x1b8) |= 0x10;
    iVar1 = (**(code **)(*ctrl + 0x88))(ctrl);      // FUN_1402bcb60: +0x150 == 0x10
    if (iVar1 == 0) { FUN_1402be1e0(ctrl, 6); +0x150 = 0x11; +0x1b0 = clock; }
    break;
```

- **In state `0x10` the warp does not end the session**, whatever the
  reason. Outside `0x10` (i.e. mid-join) it ends it with code 6.
- It arms bit `0x10` and, unlike case 0, does **not** refresh `+0x1b4`.
  That is exactly what 16/09 measured (`0x261 → 0x271`, `+0x1b4 = 0.0`).
- Bit `0x10` is cleared in exactly one place: the constructor
  (`FUN_1402bc3f0`: `+0x1b8 &= 0xfffff800`). A host that warps once carries
  it for the rest of the controller's life.

The other cases, for the record: case 1 sets bit 8 and ends with `0xb` unless
the map id is one of two globals; case 2 sets bit 2 and, when the guest's role
index `ctrl+0x30` is outside 1..4, sends `0x15` to the peer and ends with 9 —
that is the branch `DS2_NATIVE_TRAVEL_PLAN.md` attributed to the warp; case 3
sets bit 4, sends `0x17` and ends with 10; case 5 ends with `0xc` for role 5.

`NetDuelAcceptMultiplayCtrl` (the arena host) slot `+0xe0` is `FUN_1402b3aa0`
(read): it only reacts to cases 0 and 5. **Case 4 falls straight through**
(`if (param_2 != 5) goto LAB_1402b3b12;`) — the arena host warps with no
effect on its controller at all.

### Host: the two consumers of bit `0x10`

**The 300 s watchdog**, `FUN_1402be090`, run first thing every frame by the
host dispatcher `FUN_1402bddb0` (read):

```c
if ((*(byte *)(ctrl + 0x1b8) & 0x10) == 0) return;
if (*(float *)(ctrl + 8) - *(float *)(ctrl + 0x1b4) <= 300.0f) return;   // DAT_1410d7b40
FUN_1405205d0(*(ctrl + 0x180));        // the P2P link object
... notify ...
*(ctrl + 0x150) = 0x13;                 // ended
```

`ctrl+8` is the controller's own clock (`FUN_1402b17f0` adds the frame delta);
`+0x1b4` is only written by case 0. So after a host warp **the session ends
on the first frame after the controller turns 300 s old — or immediately, if
it already is**. *(Inferred: this is the most likely cause of the 17/09
measurement "host travels alone → session drops in 10 s, guest goes home",
if that session was older than five minutes; the phase 2 run was at 57 s and
did not drop. Not measured — see §7.)*

The first branch of the same function is the phantom time limit
(`clock > FUN_1402d8600()`, sends `0x2f`, state `0x13`), unrelated to warps.

**The re-entry handshake**, `FUN_1402bd720`, slot `+0xd0`, which the host
runs when a guest reports "I am in" while the host sits at `0xe` (read,
abridged):

```c
if (*(int *)(ctrl + 0x150) != 0xe) return 0;
...
uVar1 = *(uint *)(ctrl + 0x1b8);
if ((uVar1 & 0x10) == 0) {
    if (uVar1 & 4) { end(10); return 0; }
    if ((uVar1 & 8) == 0) {
        if ((uVar1 & 2) && (3 < ctrl->role - 1U)) { end(9); return 0; }
        ... role 5 check ...
        *(ctrl + 0x150) = 0xf;  return 1;       // accepted
    }
    end(0xb); return 0;
}
FUN_1402be1e0(ctrl, 8);                          // bit 0x10 set: refused
```

So **once a host has warped, no guest can ever complete a join with that
controller again** until bit `0x10` is cleared. This is the host-side reason
a native re-entry after a host warp would fail even if everything else were
right.

### Guest: the join controller and the loader

`NetSummonJoinMultiplayCtrl` (vftable `0x1410d7bd8`, state `+0xf8`,
dispatcher `FUN_1402c3630`). The only exits from state 7 are
`+0x120 != 0` → `End(3)` and `+0x1cc != 0` → state 8 (`FUN_1402c3830`,
read). `+0x120` is written by slot `+0x30`, `FUN_1402c2820(ctrl, code)`:
in state 7 any code outside `8..0xb` ends the session (`0xd, 0x10, 0x11,
0x14` at once through `vf+0xa0(ctrl, 3)`, the rest through `+0x120`).

Who calls slot `+0x30` on the join controller, and would a warp trigger it:

| caller | code | fires on a warp? |
| --- | --- | --- |
| join slot `+0x60`, `FUN_1402c1fd0` ← `FUN_140513720` ← loader state `0x1c` | 1 | **only on the load-timeout branch** (`ctx+0x24b2 & 8`, set when `ctx+0x24b8` passes `DAT_1410bdc88`) |
| join slot `+0x38`, `FUN_1402c2a70` ← `FUN_1402c8f30` ← `FUN_140290630` | `0x11` | no (called from the P2P/`raiz[6]` side, not the loader) |
| manager event type 1 in `FUN_1402c9540` | `0xf` | no *(inferred: link loss)* |
| P2P message 5 in `FUN_1402cde30` → `FUN_1402c8600` | the host's code | only if the host sends one |
| join state 4 timer, state 6 timeout (`_DAT_14157c304`) | `0xf` | only mid-join |

The loader's end-of-load hooks that reach the join controller (read in the
state handlers; the state table is `0x1410c49e0`, dispatched at `0x1401c3310`
with `lea r14, [0x1410c49e0]; mov r8, [r14+rax*8]`):

| loader state | handler | calls | join controller effect |
| --- | --- | --- | --- |
| `0x16` | `FUN_1401bef10` | `FUN_140513680` → join slot `+0x40` (`FUN_1402c1f60`) | **state 3 → 4** and sends message `0xb` (`FUN_1402cf7f0`), if `+0x120 == 0` |
| `0x1c`, normal | `FUN_1401bf200` | `FUN_140513740` → join slot `+0x58` (`FUN_1402c1fe0`) | acts only in **state 5**: registers the presences from the snapshot |
| `0x1c`, timeout | same | `FUN_140513720` → join slot `+0x60` | `End(1)` |

A guest at 7 that warps is therefore untouched by the game — which is why
the mod's flag-1 guest warp is session-safe and also why nothing in the game
rebuilds anything for it.

### What destroys the controllers, and what decides it

The warp sets `ctx+0x24b1 |= 0x0a` and copies the flag into bit `0x20`
(read, `FUN_1401c2a80`). The loader's state `0x15` handler decides (read,
`0x1401bee7d..0x1401beed7`):

```
test $0x8, 0x24b1 ; je → state 0x11
state 0x12 ; if bit 0x20: 0x24b1 = (|0x40 &~0x20)  else 0x24b1 &= ~0x40
```

- with bit 8 (any warp) the loader goes to `0x12` and rebuilds the world;
  the "in someone else's world" bit `0x40` becomes whatever the flag was.
  **A guest that warps with flag 0 loses bit `0x40`**, which is why the guest
  must always warp with flag 1;
- without it (quit to title clears bits `0x28`) it goes to `0x11`,
  `FUN_1401be560` → `FUN_140513830` → `FUN_1402c9400`, which **deletes every
  accept controller and the join controller**.

So controller destruction is decided by that bit, not by a reason, and no
warp reaches it.

### The listeners: the only place the reason matters

`NetEventListener` subclasses (found through RTTI class hierarchies) that
override slot `+0x38`:

| class | slot `+0x38` | effect (read) |
| --- | --- | --- |
| `NetSvrMonitorJobBase` | `FUN_140279f90` | `if (reason != 4) { job->state = 4; job->result = FUN_14028f410(6); }` — **aborts on every reason except 4** |
| `NetNpcPhantomManager` | `FUN_140253e30` | resets all NPC phantom slots (`FUN_1402521e0(slot, 5)`) on every warp |
| `NetSvrSummonJobBase`, `NetSvrBreakInSummonJob`, the sign/visitor summon jobs | `FUN_14027ab00` | `return;` |
| `NetSummonAcceptMultiplayCtrl`, `NetSummonJoinMultiplayCtrl` (listener sub-objects at `+0x10`) | base `0x140069120` | nothing |

**So the only "exempt" reason is 4, and what it exempts is monitor jobs**, not
the session.

## 3. Would the guest stay in the session through a host warp?

At the controller level, yes: nothing on either machine ends the session at
the moment of a host warp in state `0x10`, with any reason. What follows is
what goes wrong afterwards, and each item is already measured or read:

1. the 300 s watchdog, immediately if the session is older than 300 s (read);
2. the guest's world is still the host's **old** map, with no host presence
   and a snapshot of the old map (read: nothing re-sends the snapshot;
   measured 17/09 as "the guest does not see the host");
3. the object sync on both machines stays bound to the old map's object
   table, which crashed the host on 16/09 and the guest on 18/09 (measured);
4. any attempt to rejoin is refused at the host's `0xe` handshake by bit
   `0x10` (read).

### The game's own precedent: the arena

The arena duel does, in vanilla, what the brief wants: a host warps to
another map while a P2P session with a guest exists, and the guest then loads
into the host's new world. The job classes read themselves (RTTI,
anonymous namespace `0x89915c51`): `GoDuelField`, `WaitGuestWarpFinished`,
`WaitDuelEnd`, `WaitGuestDisconnect`, `LeaveSession`, `ReturnMyWorld`.

- `GoDuelField` (`FUN_1402b4310`): saves a return record (`FUN_1402bc1c0`),
  builds a reason-4 request (`FUN_14027be20`), warps with flag 0.
- the arena host's controller ignores case 4 entirely (above);
- `FUN_1402b6340` (arena host state `0xb`) sends the guest an invitation with
  the destination map and position (`FUN_1402cf020`, P2P message 10) —
  the **same sender** the co-op host uses in its state `0xa`;
- the guest's `FUN_1402b9750` warps with reason 4 flag 1 into that world,
  as `FUN_1402c2a80` does for co-op.

*(Inferred)* The order in the arena is host warp → invitation → guest warp,
with the guest never having had a presence in the host's world before the
host moved. That is exactly the configuration 16/09 found safe ("without
presences it arrives clean").

### The game's own sync unbind

`FUN_140517080(sync)` where `sync = *(0x141616cf8 + 0x28)` (read):

```c
lock(sync+0x78);
if (state == 1) FUN_140517e70(sync);      // host binding: marks each bound object
                                           // (+0x3c |= 1<<48), clears "in use", the
                                           // pointer, the count and the three trees
else if (state == 2) FUN_140517a80(sync);  // guest binding: same, without the marks
sync+8 = 0; sync+0x74 = 0; sync+0x198 = 0;
unlock;
```

Callers: `FUN_1402c3900` and `FUN_1402ba8d0` right after their go-home warp,
`FUN_1402c9540`, `FUN_1402c9bd0`. The game **always unbinds the sync after
queuing a session warp** — the mod's 16/09 host warp never did, and 18/09's
fix did it by hand for the guest.

Re-arming (read): `FUN_140517040` sets `+0x74 = 1, +0x198 = 0` under the
same lock (called from `FUN_14051f5a0`, when the P2P session forms);
`FUN_140516370` sets `+0x198 = 1` (called by the snapshot import
`FUN_1402c2fa0` and the arena's `FUN_1402b9ad0`). The tick `FUN_1405170e0`
then rebinds from state 0: the host (`raiz0+0xa4 == 2`) through
`FUN_140517bf0`, the guest (context virtual `+0x58` true and `+0x198`) through
`FUN_140517880`. *(Inferred: `+0x74` is "a session exists", `+0x198` is "the
guest has a snapshot to bind to".)*

### What the guest's machine needs to load into the host's new map

The co-op join, as read and as measured on 12/09
(`4→5→7→8→0xa→0xb→0xd→0xe→0xf→0x10` on the host):

| step | host (`+0x150`) | message | guest (`+0xf8`) |
| --- | --- | --- | --- |
| invitation: map, position, angle, net id | `0xa` `FUN_1402bf440` → `0xb` | P2P 10 (`FUN_1402cf020`, 0x24 bytes) | state **2** `FUN_1402c2a80`: warp reason 4 flag 1 → 3 |
| guest's world is up | | `0xb` from loader `0x16` → join `+0x40` | 3 → 4 |
| | `0xb` → `0xc` (`FUN_1402bd9c0`) → `0xd` (`FUN_1402be490`) | | |
| snapshot export | `0xd` `FUN_1402bf8f0` → `0xe` | `0xc` | 4: import `FUN_1402c2fa0` (sets `+0x19c`, `sync+0x198`) → 5 |
| presences | | | 5: at loader `0x1c`, join `+0x58` `FUN_1402c1fe0` → 6 |
| "I am in" | `0xe` `FUN_1402bd720` → `0xf` (**bit `0x10` must be clear**) | | 6 `FUN_1402c45b0` → 7 |
| close | `0xf` `FUN_1402c03e0` → `0x10` | | 7 |

The invitation is only accepted in state 2 (`FUN_1402c2a80` opens with
`if (+0xf8 != 2) { +0xf8 = 0xb; ... }`), and its first test is the arrival
guard `*(*(ctrl+0x108)+8)` (see `DS2_SEAMLESS_COOP.md`, "The arrival guard"),
then `FUN_1402c6570` (transient, measured on 12/09 to need ~96 frames after
the player is up).

## 4. Proposal

### What changes, on which machine

| lever | machine | kind | replaces |
| --- | --- | --- | --- |
| **A.** after the host's warp returns 1: `ctrl+0x1b8 &= ~0x10`, `ctrl+0x1b4 = ctrl+8` | host | two data writes on the accept controller | nothing today; removes the 300 s drop and the `0xe` refusal |
| **B.** `FUN_140517080(sync)` right after the warp is queued; `FUN_140517040(sync)` once the world is back | both | calls into the game | the 18/09 hand-written sync count/gate writes and the 16/09 state-0 writes |
| **C.** re-run the join from host `0xa` / guest `2` | both | two state writes plus the saves around them | the snapshot request, the forced `+0xf8 = 4/7` around `FUN_1402c2fa0`, the host `0xe → 0x10` write, the presence recreation from a captured blob |

Levers A and B are independent and cheap; each can land on the current
transport first. C is the one that turns travel into the game's own join.

A is best done as a data write from the existing warp detour
(`DS2_SeamlessCoopHook` sits on `FUN_1401c2a80`), not as a `.text` patch of
`FUN_1402bd0d0`: the game cannot re-arm the bit behind our back (only warps
and case 0 set it), and a patch that skipped case 4 would also have to worry
about the `+0x88` check that protects a mid-join controller. Only clear the
bit on a controller at `0x10`; if the warp arrived with the controller
elsewhere, case 4 already ended it and there is nothing to save.

### Order of operations

Starting point: host controller at `0x10`, guest at 7, both alive, the mod's
channel up, destination agreed by the vote.

**Host**

1. Remove the guest's presence (`FUN_14051c820`, as today — measured safe for
   the session).
2. Native travel: `FUN_1401843b0(&req, bonfire, 2)` + `FUN_140184830` (the
   bonfire menu's own chain; reason 2, flag 0). Keep the network tick silence
   during the load — it is what fixed the network-layer crash family.
3. In the warp detour, when the call returns 1: `FUN_140517080(sync)`
   (lever B), and lever A on the accept controller.
4. When the world is back (`ctx+0x24ac == 0x1e`, `ctx+0xd0 != 0`) and the tick
   is released: `FUN_140517040(sync)`.
5. Only after the guest reports "at state 2, ready": check
   `ctrl+0x80` (if non-zero, `FUN_1402bdf10` sends the stored `+0x84/+0x90..`
   position instead of the live one — clear it or write the new spawn there),
   then set `ctrl+0x150 = 0xa`. The host's own dispatcher sends the
   invitation with the **new** map and position and walks
   `0xb → 0xc → 0xd → 0xe → 0xf → 0x10` by itself.

**Guest** (in parallel with host steps 2–4; it stays where it is, in the host's
old map, alive)

6. Remove the host's presence from its own registry, and
   `FUN_140517080(sync)` on its own sync.
7. Save the go-home block `ctrl+0x1a0..+0x1c8` (the state 2 handler rebuilds
   it from the live player, which is now standing in the host's world; see
   §5).
8. Wait for the arrival guard `*(*(ctrl+0x108)+8) == 0` and
   `FUN_1402c6570` to answer yes, then set `+0xf8 = 2` with `+0x120 == 0` and
   `+0x1cc == 0`. Report "ready" to the host.
9. The invitation arrives; the game's `FUN_1402c2a80` warps with reason 4
   flag 1 → state 3. Restore the go-home block saved in step 7 right after it
   returns.
10. Everything else is the game: loader `0x16` → 3→4 and message `0xb`; host
    exports; guest imports at 4 (which also sets `sync+0x198`); loader `0x1c`
    → presences at 5; 6 → "I am in"; host `0xe → 0xf → 0x10`; guest 7.

**Why it should hold** *(inferred)*: every structure the session hangs on the
world is either unbound by the game's own call (sync), removed by a measured
primitive (presences), or rebuilt by the game's own join (snapshot, presence
registration, handshake). No map is forced, no position is teleported, and
each machine only ever has its own current map loaded — so the two-map
streaming limit and the "map where the session formed" corruption have
nothing to act on.

### Fall-backs, in order

- If step 5's re-invitation turns out to be refused or double-notifies the
  server (see §5), keep levers A and B and the current guest path (flag-1
  warp + `SnapshotPlease` + forced import) — A and B still remove two known
  killers from it.
- If the host's native warp keeps crashing in the map layer
  (`+0x3f39b3`, 16/09 attempt 5), that is a consumer outside this survey, and
  it was measured with the old sync still bound; re-measure it with lever B
  in place before concluding anything.

## 5. What is still unknown

- **Whether host state `0xa` can be re-entered.** `FUN_1402bf440` requires
  `*(*(ctrl+0x188)+9) == 0` and a local character with `+0x490 != 0`, and it
  calls `FUN_1402901b0(ctrl+0x190, ..., FUN_14028f300(...))`, which looks like
  a server-side notification *(inferred)*; the server may log a second
  join-type `RequestNotify*` or reject it.
- **The go-home block.** `FUN_1402c2a80` rewrites `+0x1a0..+0x1c8` from the
  live player and `ctx+0x70`; for a white phantom the go-home form is the
  position form (measured 12/09), so without the save/restore a later legal
  exit would put the guest at host-world coordinates in his own map.
  `ctx+0x70` on the guest is his own record and is only read here, never
  written.
- **The guest's presence of the host.** Guest state 2 calls
  `FUN_14051c6a0(presences, netid, role)` from payload `[6]`; whether that
  duplicates an entry the mod did not remove is untested.
- **The 12th warp site** (`0x1401bf885`) and the reason-3/5 script paths:
  their triggering branches are obfuscated in this build, so which gameplay
  events use them (eagle, Dark Chasm, scripted falls) is not read.
- **The `NetSvrMonitorJobBase` users.** Which live jobs a non-4 warp aborts is
  not identified.
- **Case 0.** Its source is the true→false edge of the character's `+0x1b0`
  virtual; if that is "alive", a host death re-arms bit `0x10` and refreshes
  `+0x1b4` — relevant to M2, not measured.
- **Three or more players**: untestable on this machine.

## 6. Doc corrections this survey makes

- `DS2_NATIVE_TRAVEL_PLAN.md` §"The conclusion" point 1 and §1: the warp
  calls case **4**, with a constant, not case 2 with the reason. Phase 1's
  reading of `ctrl+0x30` is correct but concerns case 2, which is a character
  event.
- Same doc, §1: the gate `FUN_140248940` is the local character's virtual
  `+0x1b0`, not `*(mgr+0x168) > 0`; the listed prologue for `+0x1c2a80` does
  not match the file.
- Same doc, §3: the 300 s watchdog fires **immediately** after a host warp on
  a controller older than 300 s — `+0x1b4` is only written by case 0.

(These are recorded here; the docs themselves were not edited.)

## 7. Measurements that would confirm it

All on the live game with the harness, cheapest first. None needs the
proposal to be implemented.

1. **The watchdog explains the 10 s drop.** Form a session, wait until the
   host controller's `+0x08` passes 300, let the host travel natively alone.
   Expect `+0x150` to go `0x10 → 0x13` on the first frame after the warp
   with `+0x1b8 & 0x10` set. Control: the same at clock < 300 (phase 2: no
   drop). Then repeat at > 300 with lever A: expect no drop.
2. **Bit `0x10` blocks re-entry.** `bp 2bd720` on the host, force a guest
   re-join after a host warp; expect the `(uVar1 & 0x10)` branch and
   `+0x198 = 8`.
3. **Every warp passes 4.** `bp 2bd0d0 deref rdx` on the host during a reason
   2 travel and during a death: `edx == 4` both times.
4. **Lever B.** Arm the page watchpoint on a record of the host's sync before
   a native host travel, with and without `FUN_140517080` called after the
   warp; the 16/09 `+0x5180a8` family should disappear with it.
5. **Reasons in the wild.** With `DS2_Seamless.log` on, ride the Pursuer's
   eagle, fall into the Dark Chasm, use a Homeward Bone and let a phantom be
   sent home; record `motivo` and `de=+0x...` for each, which fills the
   unconfirmed rows of the table and says whether any path besides row 2
   reaches the warp through a non-global context.
6. **Lever C, one leg.** Same map first (Majula → Majula bonfire), the
   control for everything else; the ruler is the host trail
   `0xa → 0xb → 0xd → 0xe → 0xf → 0x10`, the guest trail `2 → 3 → 4 → 5 → 6 →
   7`, `p2pSessionVerified`, each seeing the other, and the server's
   `RequestNotify*` lines. Then Majula → Heide, then Heide → Iron Keep.
