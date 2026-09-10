# Dark Souls II PvP notes

Notes from investigating Dark Souls II PvP behaviour on a private DS3OS
server.

| Document | What it covers |
| --- | --- |
| [DS2_PHANTOM_TIMER_PATCH.md](DS2_PHANTOM_TIMER_PATCH.md) | The patch that removes the ~12 minute PvP session limit, and how to turn it on |
| [DS2_PVP_LEAVE_SESSIONS.md](DS2_PVP_LEAVE_SESSIONS.md) | How PvP sessions end, and how the timer leave differs from a normal one |
| [DS2_LEAVE_SESSION_BY_KILL.md](DS2_LEAVE_SESSION_BY_KILL.md) | The kill-based leave sequence, and why the leave message cannot simply be blocked |
| [DS2_AREA_RESTRICTION.md](DS2_AREA_RESTRICTION.md) | Why multiplayer items are refused in some areas, and what has been ruled out |
| [DS2_STICKY_SIGNS.md](DS2_STICKY_SIGNS.md) | How multiplayer was opened in Majula, and the two dead ends that led there |
| [DS2_LIVE_MEMORY_ACCESS.md](DS2_LIVE_MEMORY_ACCESS.md) | Reading and writing the running game from Linux, which replaces the build-and-relaunch loop |
| [DS2_INVESTIGATION_TOOLS.md](DS2_INVESTIGATION_TOOLS.md) | The probe, the tracer and the oracle, and what they cost to learn |
| [DS2_SOUL_MEMORY_MATCHMAKING.md](DS2_SOUL_MEMORY_MATCHMAKING.md) | Soul Memory matchmaking tiers and how to open them up |
| [DS2_PVP_CODEMAP.md](DS2_PVP_CODEMAP.md) | Where the DS2 PvP flows live in the source tree |

The two leave-session documents are historical records: the tracing they
describe was scaffolding and is not part of this branch.
