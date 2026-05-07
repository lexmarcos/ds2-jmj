# DS2 PvP Codemap

This document maps the project areas involved in Dark Souls II PvP behavior and records the current understanding of summon signs, invasions, sessions, and the phantom timer.

## Scope

This codemap is for the private DS2 server and injector workflow.

Out of scope:

- official FromSoftware servers
- retail save modifications
- unstable magic-offset patches without validation
- bypassing the private server architecture

## Build overview

The project uses the generated Visual Studio 2022 build tree.

Common generation path:

```bat
Tools\generate_vs2022.bat
```

Common build targets:

```bat
cmake --build intermediate\vs2022 --config Debug --target Injector
cmake --build intermediate\vs2022 --config Release --target Injector
cmake --build intermediate\vs2022 --config Debug --target Loader
cmake --build intermediate\vs2022 --config Release --target Loader
```

Expected runtime output directories include:

- `bin\x64_debug\loader`
- `bin\x64_release\loader`

## Repository structure

Important project areas:

- `Source/Server`: shared server infrastructure.
- `Source/Server.DarkSouls2`: DS2-specific server logic.
- `Source/Injector`: injected client-side hooks.
- `Source/Loader`: loader and generated config.
- `Source/Shared`: shared utilities and protocol helpers.
- `Source/MasterServer`: master server entry points.
- `Protobuf`: protocol definitions and generated protobuf code.
- `Tools`: local utility scripts and build helpers.
- `docs`: project documentation.

## DS2 game selection

The loader and injector select DS2-specific logic based on the detected game.

DS2-specific hooks and config should remain gated to Dark Souls II so they do not affect other supported games.

## Summon signs

Relevant systems:

- `DS2_SignManager`
- Red Soapstone matching parameters
- White Soapstone matching parameters
- Dragon Soapstone matching parameters

Important Red Soapstone path:

- sign creation request
- sign list request
- server-side matching
- response sign count
- join session request

Soul Memory filtering is enforced in the sign manager matching path unless disabled by config.

## Invasions and break-in

Relevant system:

- `DS2_BreakInManager`

This handles invasion-style matchmaking and has separate matching parameters from soapstone signs.

Do not assume that changing Red Soapstone matching also changes invasions.

## Visitor and covenant flows

Relevant system:

- `DS2_VisitorManager`

This path covers visitor and covenant-style auto-summon behavior.

It should be treated separately from direct Red Soapstone summon signs.

## Quick match and arena

Relevant system:

- `DS2_QuickMatchManager`

Arena and quick match behavior have their own matching logic and should not be mixed with summon-sign debugging unless the change is intentionally global.

## Soul Memory and matching parameters

DS2 matchmaking uses runtime config parameter blocks.

Important fields:

- `DisableSoulMemoryMatching`
- `Tiers`
- `TiersBelow`
- `TiersAbove`
- password-specific tier settings

Relevant parameter groups include:

- Red Soapstone
- White Soapstone
- Small White Soapstone
- Dragon Soapstone
- invasion / break-in
- visitor / covenant
- quick match / arena

For targeted changes, prefer editing one parameter group at a time.

## Network services and handlers

Important server flows:

- login / auth
- game server handshake
- heartbeat
- profile state
- session state
- summon sign creation and listing
- invasion request
- visitor request
- quick match request
- disconnect and leave-session cleanup

PvP state is distributed across profile state, session state, and request-specific managers.

## Approximate loader / injector flow

Approximate runtime flow:

1. Loader starts the game process.
2. Loader injects `Injector.dll`.
3. Injector loads config.
4. Injector detects the game.
5. DS2 hooks install when enabled.
6. Hook logs and patches are emitted to the loader output directory.

Config should have safe defaults. Experimental hooks should remain opt-in unless they are known stable patches.

## Approximate server PvP flow

Approximate Red Soapstone flow:

1. Player creates a sign.
2. Server stores sign metadata.
3. Other player requests area sign list.
4. Server filters signs using matching parameters.
5. Client receives visible signs.
6. Player requests summon / join session.
7. Server creates or joins a PvP session.
8. Session state updates continue until leave, death, disconnect, or timeout.

## Phantom timer hypotheses

Current understanding:

1. There is no known direct server-side config that controls the client phantom timer.
2. `phantom_leave_at` is likely a client-observed or client-driven behavior rather than a server timer.
3. The server mostly brokers matching and session lifecycle.
4. `DS2_LoggingManager` and protobuf logging were useful initial observation points.
5. A correct patch needs session state before it touches timer behavior.

## Server-side versus client-side split

Server-side changes are appropriate for:

- matchmaking rules
- Soul Memory filtering
- sign visibility
- session diagnostics
- cleanup classification

Client-side injector changes are appropriate for:

- observing raw messages
- tracing call paths
- patching client-only timers
- identifying client-side UI or item-use restrictions

The phantom timer ended up requiring a client-side patch because the final leave message was indistinguishable from valid leave cleanup.

## Known risks

- Heavy protobuf tracing can cause lag and large logs.
- Callstack capture must remain opt-in.
- Blocking final leave messages can break kill cleanup.
- Broad Soul Memory bypasses can affect more PvP systems than intended.
- Offsets and signatures can change between builds or game variants.

## Recommended debugging approach

Use the least invasive tool that answers the current question:

1. Prefer server config for matchmaking scope tests.
2. Prefer lightweight injector state for runtime client decisions.
3. Use heavy tracing only for short discovery windows.
4. Avoid per-frame probes unless absolutely necessary.
5. Once a patch is understood, keep only the stable patch and remove discovery probes.

## Related docs

- `docs/DS2_SOUL_MEMORY_MATCHMAKING.md`
- `docs/DS2_PVP_LEAVE_SESSIONS.md`
- `docs/DS2_LEAVE_SESSION_BY_KILL.md`
- `docs/DS2_PHANTOM_TIMER_PATCH.md`
