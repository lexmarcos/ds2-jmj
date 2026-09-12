# Dark Souls II PvP notes

Notes from investigating Dark Souls II PvP behaviour on a private DS3OS
server.

| Document | What it covers |
| --- | --- |
| [DS2_PHANTOM_TIMER_PATCH.md](DS2_PHANTOM_TIMER_PATCH.md) | The patch that removes the ~12 minute PvP session limit, and how to turn it on |
| [DS2_PVP_LEAVE_SESSIONS.md](DS2_PVP_LEAVE_SESSIONS.md) | How PvP sessions end, and how the timer leave differs from a normal one |
| [DS2_LEAVE_SESSION_BY_KILL.md](DS2_LEAVE_SESSION_BY_KILL.md) | The kill-based leave sequence, and why the leave message cannot simply be blocked |
| [DS2_REMATCH_AFTER_DEATH.md](DS2_REMATCH_AFTER_DEATH.md) | What stands between a death and the next invasion of the same pair, measured |
| [DS2_SESSION_END_CLIENT.md](DS2_SESSION_END_CLIENT.md) | The client's path from a death to `RequestNotifyLeaveSession`, traced at runtime |
| [DS2_CLIENT_NETSVR_API.md](DS2_CLIENT_NETSVR_API.md) | **The client's multiplayer classes carry RTTI names and full method signatures** |
| [DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md) | Seamless co-op: the one call every warp goes through, the request it carries, and the field that says "bonfire" instead of "home" |
| [DS2_AREA_RESTRICTION.md](DS2_AREA_RESTRICTION.md) | Why multiplayer items are refused in some areas; carries corrections where it was wrong |
| [DS2_MAJULA_MULTIPLAYER.md](DS2_MAJULA_MULTIPLAYER.md) | **Multiplayer in Majula and the other closed areas: the three things required, how to apply and verify them** |
| [DS2_SERVER_DEPLOY.md](DS2_SERVER_DEPLOY.md) | **Running the server on a small shared host: build here, seed the config, the service, the firewall, the key** |
| [DS2_STICKY_SIGNS.md](DS2_STICKY_SIGNS.md) | The record of getting there, including the approaches that failed and why |
| [DS2_TO_VALIDATE.md](DS2_TO_VALIDATE.md) | **What is untested: what "it works" does and does not cover yet** |
| [DS2_LIVE_MEMORY_ACCESS.md](DS2_LIVE_MEMORY_ACCESS.md) | Reading and writing the running game from Linux, which replaces the build-and-relaunch loop |
| [DS2_INVESTIGATION_TOOLS.md](DS2_INVESTIGATION_TOOLS.md) | The probe, the tracer and the oracle, and what they cost to learn |
| [DS2_SOUL_MEMORY_MATCHMAKING.md](DS2_SOUL_MEMORY_MATCHMAKING.md) | Soul Memory matchmaking tiers and how to open them up |
| [DS2_PVP_CODEMAP.md](DS2_PVP_CODEMAP.md) | Where the DS2 PvP flows live in the source tree |
| [DS2_FOG_GATES.md](DS2_FOG_GATES.md) | The area-transition fog walls: the class, the per-frame test, and what is still unknown |

The two leave-session documents are historical records: the tracing they
describe was scaffolding and is not part of this branch.
