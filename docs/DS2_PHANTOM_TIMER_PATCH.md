# DS2 Phantom Timer Patch

This document describes the current fix for the Dark Souls II PvP phantom timer.

## Version

Visible injector version:

```text
ds2-jmj injector pvp-timer v2026-05-07.1
```

## Goal

Prevent the client-side phantom timer from ending a PvP session after roughly 12 minutes.

The active timer is stored at:

```text
R14 + 0xCFC
```

The default patched value is:

```text
4000.0f
```

## Expected logs

When the patch applies, the injector should emit logs similar to:

```text
[DS2TimerParamPatch] patched active session timer old=<value> new=4000.000 source=r14_plus_0xcfc
event=DS2ActiveTimerPatch result=patched source=r14_plus_0xcfc
```

The main log file is:

```text
DS2_TimerParamPatch.log
```

`DS2_LeaveSessionTrace.log` is written only when `DS2TraceLeaveSession=true`.

## Config

Recommended config:

```json
{
  "DS2TraceLeaveSession": false,
  "DS2PreventPvpTimerLeave": false,
  "DS2PatchPhantomTimers": true,
  "DS2PhantomTimerSeconds": 4000
}
```

Environment variables:

```text
DS2_PATCH_PHANTOM_TIMERS=1
DS2_PHANTOM_TIMER_SECONDS=4000
```

`DS2PreventPvpTimerLeave` should remain disabled when `DS2PatchPhantomTimers` is enabled.

## Trace decoupling

`DS2PatchPhantomTimers=true` requires lightweight session state updates from the protobuf hook.

It does not require expensive leave-session tracing.

With the current implementation:

- `DS2TraceLeaveSession=true` captures callstacks and writes `DS2_LeaveSessionTrace.log`.
- `DS2PatchPhantomTimers=true` updates lightweight session state without callstack capture and without writing the leave-session trace file.

This avoids enabling heavy tracing in normal use while still giving the timer patch enough state to operate.

## Main files

- `Source/Injector/Injector/GameConfig.cpp`
- `Source/Injector/Injector/GameConfig.h`
- `Source/Injector/Injector/Injector.cpp`
- `Source/Injector/Hooks/DarkSouls2/DS2_LogProtobufsHook.cpp`
- `Source/Injector/Hooks/DarkSouls2/DS2_PhantomTimerParamPatchHook.cpp`
- `Source/Injector/Hooks/DarkSouls2/DS2_PhantomTimerParamPatchHook.h`
- `Source/Injector/Hooks/DarkSouls2/DS2_SessionTraceState.cpp`
- `Source/Injector/Hooks/DarkSouls2/DS2_SessionTraceState.h`
- `Source/Loader/Loader/GameConfig.cpp`

## Discovery summary

The client sends the timer-based leave after roughly 763 to 766 seconds in the observed PvP sessions.

The final leave message looked the same as a legitimate kill leave:

```text
raw_hex=08 01 10 05 18 00 20 00
```

Because kill leave and timer leave share the same final message path, blocking `RequestNotifyLeaveSession` directly is unsafe.

Cheat Engine confirmed a live float timer address:

```text
7FF447B49A9C
```

Observed register base:

```text
R14 = 0x00007ff447b48da0
```

`R14 + 0xCFC` matched the live timer address.

## Hook mechanics

The hook uses a software breakpoint at:

```text
DarkSoulsII.exe + 0x2c9844
```

The handler:

1. Validates the expected instruction signature.
2. Restores the original byte.
3. Reads the current register context.
4. Computes `R14 + 0xCFC`.
5. Reads the current float timer.
6. Writes `DS2PhantomTimerSeconds` when needed.
7. Sets the trap flag.
8. Reinstalls the breakpoint after single-step.

## Why not block leave

The same final leave path is used by:

- legitimate kill cleanup
- the client-side phantom timer

Blocking the leave message directly risks breaking normal PvP session cleanup.

The safer patch is to keep the timer from reaching the leave path.

## Validation steps

1. Start the loader and injector with `DS2PatchPhantomTimers=true`.
2. Enter a PvP session.
3. Confirm `DS2_TimerParamPatch.log` shows `result=patched`.
4. Stay in the session past the old timer window.
5. Confirm the session does not end at the old timer time.
6. Confirm normal kill-based leave still works.

## Safety notes

- The hook only installs for DS2.
- The hook validates the target signature before patching.
- The patch writes one float value at the observed active timer slot.
- Heavy leave-session tracing remains opt-in.
