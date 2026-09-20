# Dark Souls II reverse engineering notes — private server, seamless co-op, and a lot of Ghidra

A personal research repository where I pulled apart **Dark Souls II: Scholar of the First Sin** (`DarkSoulsII.exe`, version 1.03 / Calibrations 2.02, Steam appid 335300) to find out how the game actually works, and whether things the game never allowed could be made to work anyway.

The three questions that drove it:

- **Invading and summoning in Majula** and the other "safe" areas, where Dark Souls II refuses every multiplayer item — solved, and the reference is [DS2_MAJULA_MULTIPLAYER.md](docs/DS2_MAJULA_MULTIPLAYER.md).
- **Travelling between bonfires without breaking a co-op session**, so two players cross the game together instead of being thrown back to their own worlds — built, measured, and written up under seamless co-op below.
- **Learning the binary itself**: RTTI, vftables, the net subsystem, the map streamer, Havok bodies, the Arxan-obfuscated prologues, and how to read live memory from Linux while the game runs under Proton.

Everything here is **decompilation and reverse engineering notes**, done with **Ghidra**, `objdump`, a **Detours**-based DLL injector, and a two-account test harness. Every offset is hardcoded against one game version and moves the day the game is patched. Nothing in this repository helps with piracy, and the private server authenticates Steam tickets exactly as upstream does.

## This exists because of other people's work

This repo is a fork of **[TLeonardUK/ds3os](https://github.com/TLeonardUK/ds3os)** — the open source Dark Souls 3 / Dark Souls 2 server emulator — taught to run Dark Souls II SOTFS against a private server. None of what follows would have been reachable without it.

ds3os itself credits these, and so do I:

- [garyttierney/ds3-open-re](https://github.com/garyttierney/ds3-open-re)
- [Jellybaby34/DkS3-Server-Emulator-Rust-Edition](https://github.com/Jellybaby34/DkS3-Server-Emulator-Rust-Edition)
- [AmirBohd/ModEngine2](https://github.com/AmirBohd/ModEngine2)

And the wider **soulsmodding** community, whose [Paramdex / DSMapStudio](https://github.com/soulsmods/Paramdex) param definitions were an independent check on more than one reverse-engineered field name.

## Documentation

All of it is public and meant to be read. Each file says what was **measured** versus what was **inferred**, and several record where an earlier conclusion turned out to be wrong.

### The game's multiplayer, PvP and sessions

| Document | What it is |
| --- | --- |
| [DS2_MAJULA_MULTIPLAYER.md](docs/DS2_MAJULA_MULTIPLAYER.md) | How multiplayer was unlocked in Majula and the other closed areas. The reference for what the injector patches and why. |
| [DS2_AREA_RESTRICTION.md](docs/DS2_AREA_RESTRICTION.md) | Why a summon sign works in Heide and not in Majula: the area restriction on multiplayer items, ruled in and out step by step. |
| [DS2_STICKY_SIGNS.md](docs/DS2_STICKY_SIGNS.md) | The road to that result — the failed attempts and dead ends, kept so nobody walks them again. |
| [DS2_CLIENT_NETSVR_API.md](docs/DS2_CLIENT_NETSVR_API.md) | `DarkSoulsII.exe` carries full RTTI for the multiplayer subsystem. The binary documents itself, and this is the name list. |
| [DS2_PVP_CODEMAP.md](docs/DS2_PVP_CODEMAP.md) | Map of the project areas behind summon signs, invasions, sessions and the phantom timer. |
| [DS2_SESSION_END_CLIENT.md](docs/DS2_SESSION_END_CLIENT.md) | The client-side path between "the phantom died" and `RequestNotifyLeaveSession`, mapped with runtime breakpoints. |
| [DS2_PVP_LEAVE_SESSIONS.md](docs/DS2_PVP_LEAVE_SESSIONS.md) | Every way a PvP session ends, and the difference between a kill-based leave and the timer. |
| [DS2_LEAVE_SESSION_BY_KILL.md](docs/DS2_LEAVE_SESSION_BY_KILL.md) | How the game reports a session ending after a player kill, measured on the wire. |
| [DS2_PHANTOM_TIMER_PATCH.md](docs/DS2_PHANTOM_TIMER_PATCH.md) | The client-side countdown that ends PvP sessions, found at `R14 + 0xCFC`, and the patch that removes it. |
| [DS2_REMATCH_AFTER_DEATH.md](docs/DS2_REMATCH_AFTER_DEATH.md) | What happens between a PvP death and the same pair's next invasion, and how much of the way back can be skipped. |
| [DS2_SOUL_MEMORY_MATCHMAKING.md](docs/DS2_SOUL_MEMORY_MATCHMAKING.md) | How Soul Memory gates matchmaking, and what opens it up on a private server. |
| [DS2_FOG_GATES.md](docs/DS2_FOG_GATES.md) | The area-transition fog walls as the game sees them — and the game never calls them fog. |

### Seamless co-op

| Document | What it is |
| --- | --- |
| [DS2_SEAMLESS_COOP_DESIGN.md](docs/DS2_SEAMLESS_COOP_DESIGN.md) | The brief: the host owns the world, each player keeps their own save, and everything that follows from that. |
| [DS2_SEAMLESS_COOP.md](docs/DS2_SEAMLESS_COOP.md) | What was measured of the client — the warp, the death path, and what the game already solves for free. |
| [DS2_SEAMLESS_COOP_TASKS.md](docs/DS2_SEAMLESS_COOP_TASKS.md) | The work list in dependency order, milestone by milestone, edited as each one closed. The best single entry point. |
| [DS2_WORLD_STATE.md](docs/DS2_WORLD_STATE.md) | Event flags and per-object state: making a lever the host pulled read as pulled for the guest, without leaking into the guest's own world. |
| [DS2_SEAMLESS_TRAVEL_ARCHITECTURE.md](docs/DS2_SEAMLESS_TRAVEL_ARCHITECTURE.md) | The architecture options for travelling together, and the recommendation that came out of them. |
| [DS2_PRESENCE_REBUILD_PLAN.md](docs/DS2_PRESENCE_REBUILD_PLAN.md) | Rebuilding the players' presence on every trip — and the correction it forced on the architecture document above. |
| [DS2_PRESENCE_ASTRA_REVIEW.md](docs/DS2_PRESENCE_ASTRA_REVIEW.md) | An outside model's read-only review of that plan, kept as the third opinion in the chain. |
| [DS2_NATIVE_TRAVEL_PLAN.md](docs/DS2_NATIVE_TRAVEL_PLAN.md) | Travel by the game's own loading path, after three distinct families of crash killed the earlier approach. |

### Tools, harness and operations

| Document | What it is |
| --- | --- |
| [DS2_HARNESS.md](docs/DS2_HARNESS.md) | `ds2os-dev`: the harness that drives two Steam accounts, two game installations and a private server unattended, and records the evidence. |
| [DS2_INVESTIGATION_TOOLS.md](docs/DS2_INVESTIGATION_TOOLS.md) | How to work on the running game. The slow part was building these, not using them. |
| [DS2_LIVE_MEMORY_ACCESS.md](docs/DS2_LIVE_MEMORY_ACCESS.md) | Reading and writing the game's memory from Linux while it runs under Proton, with no injection and no rebuild. |
| [DS2_SERVER_DEPLOY.md](docs/DS2_SERVER_DEPLOY.md) | Running the server on a small shared Linux host that is already doing something else which must not go down. |
| [DS2_TO_VALIDATE.md](docs/DS2_TO_VALIDATE.md) | The standing list of what has **not** been tested. Kept honest on purpose. |
| [docs/README.md](docs/README.md) | The documentation index. |

### Research notes — static reading of `DarkSoulsII.exe`

Each of these is a read-only Ghidra session written up, with `[read]` and `[inferred]` marked line by line. Nothing in them was run against a live game.

| Document | What it is |
| --- | --- |
| [warp-reasons.md](docs/research/warp-reasons.md) | Every warp reason the game has, and which of them actually ends a multiplayer session. |
| [warp-teardown-order.md](docs/research/warp-teardown-order.md) | The 32-state teardown machine behind a warp, and why the vanilla order is notify, wait, then release. |
| [phantom-map-border.md](docs/research/phantom-map-border.md) | What the game does with a phantom that crosses a map border, and what group travel can borrow from it. |
| [rejoin-in-place.md](docs/research/rejoin-in-place.md) | Whether the join's world load can be run again inside a live session, without leaving it. |
| [alternative-travel.md](docs/research/alternative-travel.md) | Group travel by another route entirely, built from what the game already does in a live session. |
| [moving-the-session.md](docs/research/moving-the-session.md) | Can a live session be moved to another map? Four patches would be needed, and one already existed. |
| [session-map-rebind.md](docs/research/session-map-rebind.md) | Moving the enemy sync to another map so the session's map can finally be released. |
| [releasing-the-session-map.md](docs/research/releasing-the-session-map.md) | Ten parallel readings synthesised into one ordered plan, with its risks ranked. The densest file here. |
| [object-table-lifecycle.md](docs/research/object-table-lifecycle.md) | The per-map network object table: what it is, who owns it, and everything that points into it. |
| [enemy-table-races.md](docs/research/enemy-table-races.md) | How the enemy generator table is built and freed, its readiness test, and four ways a create is lost silently. |
| [target-manager-cap.md](docs/research/target-manager-cap.md) | The 2048-entry TargetManager cap: can it be raised, and would raising it help? |
| [streaming-budget.md](docs/research/streaming-budget.md) | The map streaming budget, and why a third loaded map quietly stays at state 0. |
| [net-map-id-inventory.md](docs/research/net-map-id-inventory.md) | A sweep of 3,976 net functions for every place a map id is produced or consumed, ranked by hazard. |
| [remote-copy-and-the-map.md](docs/research/remote-copy-and-the-map.md) | Everything that ties the other player's copy to the map underneath it. |
| [respawn-and-the-map.md](docs/research/respawn-and-the-map.md) | Whether respawning and bonfires need their map loaded at all. They do not. |
| [map-model-fault.md](docs/research/map-model-fault.md) | A crash on `MapModelComponent + 0xc8` in a live, intact object — one word written by somebody else. |
| [risk-4-six-readings.md](docs/research/risk-4-six-readings.md) | Six readings of that crash: what each one closed, what survived, and the stale-pointer mechanism that fits. |

## Status

The seamless co-op work is **paused, not finished**. A session survives death, a guest joins with no ritual, world state is shared, and two players travel between bonfires together — all measured, with the evidence recorded. Releasing the session's map mid-travel is seven of nine steps in. `DS2_TO_VALIDATE.md` says what has not been tested, including the honest note that three or more players is untestable on one machine.

## Licence and credit

MIT, as [ds3os](https://github.com/TLeonardUK/ds3os) is, and the upstream copyright notices are kept. Please support FromSoftware by buying their games.
