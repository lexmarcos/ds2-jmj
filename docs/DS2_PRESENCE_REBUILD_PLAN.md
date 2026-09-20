# Rebuilding presence: what exists, what does not, and the plan

Survey made 16/09 by a Fable agent, in Ghidra `-readOnly` and `objdump`,
without running anything in the games. Version 1.03 Calibrations 2.02.

This document is the answer to `DS2_SEAMLESS_TRAVEL_ARCHITECTURE.md`, and it
corrects that document's central premise. Read the two together.

## What changes with respect to the review

The review said that **there is no proven primitive for removing a player's
presence without leaving the session**, and called that the first piece of
reverse engineering to do. It **does** exist, it is native, and it is per
player.

What does **not** exist is the **rebuild** through the native path outside the
entry states: the player data packet (`0xd`) is only accepted by the host in
state `0xe` and by the guest in `0xc`; in `0x10`/`7` the session vetoes it. So
the "re-entry coordinated by the native loading" **is not available in its pure
form** without redoing the handshake that M2 banged on nine times.

What is available is a **hybrid**: native removal plus a rebuild done by the
mod, with the player blob captured at the join.

That inverts the review's ranking: **alternative 2 first** (remove the copies
before the crossing, keep the current transport that has already done 40 clean
legs, recreate them when the barrier is released), and alternative 1 (native
warps) only as an escalation.

## The remote presence registry

`R = *(*0x141616cf8 + 0x20)`, 0x2500 bytes, built by `FUN_14051ad90`.

| field | what it is |
| --- | --- |
| `R+0x08` | how many live active entries; with zero, `FUN_14051d9b0` turns the sync off |
| `R+0x174` | this player's net id |
| `R+0x1a8 .. +0x5b8` | 5 **active entries** of 0xd0 bytes |
| `R+0x5c0 .. +0x2500` | 5 **pending slots** of 0x640 bytes |

Active entry `E`: `+0x00` Steam member, `+0x40` the copy's `PlayerCtrl`,
`+0x48` state (0 free, 2 alive, 3 leaving), `+0x4c` role, `+0x6a` net id,
`+0x78` the leaving timer, `+0x8c` name.

Pending slot `S`: `+0x00` Steam member, `+0x40` the player's **0x5f0-byte
blob**, `+0x630` flag, `+0x631` released to be born.

## The primitives

| step | function | the precondition that matters |
| --- | --- | --- |
| register | `FUN_14051b0e0(R, membro, blob, flag)` | the `membro` has to come from the **live** list (`FUN_140520040`), not from a 0x40-byte copy |
| release | `FUN_14051c4d0(R, steamid)` | only role `0xe` needs this in order to be born |
| be born | `FUN_14051dbb0(R)` → `FUN_14051ce20(R, S)` | **if an active entry already exists for that id, it overwrites `E+0x40` without destroying the old one**: that is the default failure mode, duplication and an orphan in the `CharacterManager` |
| **remove** | **`FUN_14051c820(E)`** | starts the 0.5 s fade and sets `E+0x48 = 3`; it touches no session and sends nothing to the server |
| destroy | `FUN_14051c940` state 3 → `FUN_14051d2a0(R, E)` | calls `FUN_140359890` in the `CharacterManager`; the real destruction is **deferred** through a list, so "entry in state 0" is not "character destroyed" |
| full reset | `FUN_140513340` → `FUN_14051bff0(R)` | this is what the **warp** calls: it destroys every presence without ending any session |

Prologues for the expected bytes:

    +0x51c820  40 53 48 83 ec 20 8b 41 48 48 8b d9
    +0x51b0e0  48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18
    +0x51ce20  40 55 53 56 57
    +0x51d2a0  48 89 6c 24 20 57 48 83 ec 20 48 8b e9
    +0x51c4d0  48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20
    +0x51bff0  48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 60
    +0x51c940  40 53 48 81 ec 80 00 00 00 48 8b 1d a8 a3 0f 01
    +0x51dbb0  48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18
    +0x520810  48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18
    +0x2be090  40 53 48 83 ec 30 f6 81 b8 01 00 00 20 48 8b d9
    +0x2c2820  48 89 5c 24 18 48 89 7c 24 20 55 48 8d 6c 24 a9
    +0x359890  48 85 d2 0f 84 43 02 00 00 55 41 56
    +0x513340  40 53 48 83 ec 20 48 8b 05 ab 39 10 01 48 8b d9

## Why the removal looks neutral to the session

Neither session machine reads the registry: the guest's state 7 handler
(`FUN_1402c3830`) only looks at `+0x120` and `+0x1cc`; the host's `0x10`
(`FUN_1402bed10`) describes the entry once and tries again on the next frame
if it is gone. **This is reading, not measurement** — the experiment below is
what decides.

## Named risks

- **300 s watchdog on the host** (`FUN_1402be090`, `DAT_1410d7b40 = 300.0f`):
  the warp notice sets a bit without renewing the timer, so a host warp long
  after the start can end the session. It is the likely wall for alternative 1.
  It can be measured at no cost through today's legal path.
- **Duplication when recreating**, above.
- **Who sends the `0xd`** was not found statically; it can be measured with
  `bp 520810` on a normal summon.

## The plan, in phases

**A — instrument, at no cost.** A `DS2_PresenceHook` that only observes
register, be born, remove, destroy and reset, plus a `status` of the 5 entries
and the 5 slots. Run a normal summon, a travel through the legal path and a
`session end`, and read the positive signal of each step.

**B — the experiment that decides.** With a baseline of both saves: the host
removes the guest's copy, holds for 60 s (the session has to stay verified,
with no `Leave*` on the server and no penalty point), recreates it, and the
copy has to **move when its owner walks**. Repeat it the other way round, then
both at the same time, and only then a leg with the copies absent during the
crossing, reading the failure counters before and after.

**C — alternative 2.** Vote approved, each machine removes the copies, travels
through the current transport, and recreates them when the barrier is
released, before the curtain comes down. The bar: 40 legs with both arriving,
copies recreated and moving, **failure counters unchanged** (any trip is an
architectural failure), equal points, the session verified the whole time, a
legal exit at the end.

**D — alternative 1, only if C still trips failures with the copies absent.**
Native warps on both sides, with the watchdog neutralised and the guest's warp
built by hand. The whole transport goes; the barrier and the contract stay.

## What to preserve in any path

The `Idle/Moving/Arrived/Failed` contract, with `Arrived` written only when the
map reached is the destination one and never by time (the phrase "only by
physical contact" was too strong: what decides is the map of the part the
streamer registered under the player, and the contact goes into diagnostics and
the hold — see the 16/09 correction in `DS2_SEAMLESS_COOP_TASKS.md`); the
barrier and the `TravelRelease`; host first; the `DropDeadRigidBody` (which
becomes a **meter**: it should stop firing for copies); every `__try` guard as
a net, with the rule that a trip is an architectural failure; the type proofs;
the `keep` that adds up and never shrinks.

## What would refute the direction

The session dropping after the removal; the recreated copy not moving; a crash
inside the removal or right after it; two presences of the same player; and the
most important one for us: **failures in the guards even with the copies
absent**, which would prove that the problem is the local player carried
between maps, and not the copy.
