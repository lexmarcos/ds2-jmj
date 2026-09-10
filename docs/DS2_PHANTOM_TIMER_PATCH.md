# DS2 Phantom Timer Patch

Removes the client-side timer that ends Dark Souls II PvP sessions.

## What the timer actually is

Confirmed by watching a live session, not inferred:

- The float at `R14 + 0xCFC` is **time remaining**, and it counts **down**.
- A session starts it at **900.0** seconds, so an untouched session ends after
  15 minutes.
- It runs on the **phantom's** side only. The host's log shows the hook
  installing and never firing; every patch event comes from the guest.
- The breakpoint is hit roughly once a second while a session is live.

That makes the patch a refill rather than a one-time write. The hook keeps the
value at or above the target:

```text
old_seconds=900.000   new_seconds=4000.000    first write, session start
old_seconds=3998.998  new_seconds=4000.000
old_seconds=3998.985  new_seconds=4000.000    ~1 refill per second
```

**A target below 900 does nothing.** The hook only writes when the current
value is under the target, so setting, say, 60 to make a session end quickly
produces `already_high_enough` forever and the session runs its normal 900
seconds. That reads as a broken patch and is not one.

## How to enable it

In the Loader, open **Settings** and tick **Remove the PvP session time
limit?**. The **Session length (seconds)** box next to it sets the value
written to the timer; the default is `4000`.

The setting is Dark Souls II only. The Loader forces it off for any other
game before writing the injector config, so it can never reach a DS3
session.

Under the hood this writes two fields into the injector config:

```json
{
  "DS2PatchPhantomTimers": true,
  "DS2PhantomTimerSeconds": 4000
}
```

Those two fields are the only way to control the patch. There are no
environment variable overrides.

## Confirming it works

The hook writes `DS2_TimerParamPatch.log` next to `Injector.dll`. Count the
results in it; a working PvP session on the phantom's side looks like this:

```text
   106  result=patched
  6069  result=already_high_enough
     1  result=installed
```

`installed` alone means the hook loaded but no session ever reached it. On the
host that is expected. On the phantom it means no PvP session happened.

The console also shows, once per session and then at most once a minute:

```text
[DS2TimerParamPatch] patched active session timer old=<value> target=4000.000
```

`result=already_high_enough` means the timer was already at or above the
target, which is the normal steady state after the first patch.

Full validation:

1. Launch with the setting enabled and enter a PvP session.
2. Confirm `DS2_TimerParamPatch.log` shows `result=patched`.
3. Stay in the session past the old ~12 minute window.
4. Confirm the session does not end on its own.
5. Confirm a normal kill still ends the session.

## What the patch does

The active session timer is a float at:

```text
R14 + 0xCFC
```

The hook installs a software breakpoint (`0xCC`) at:

```text
DarkSoulsII.exe + 0x2c9844
```

On each hit the handler:

1. Validates the expected instruction signature before touching anything.
2. Restores the original byte.
3. Reads the register context and computes `R14 + 0xCFC`.
4. Reads the current float and range-checks it.
5. Writes `DS2PhantomTimerSeconds` if it is lower than the target.
6. Sets the trap flag and reinstalls the breakpoint after the single step.

Both the code address and the struct offset belong to the game binary, not
to DS3OS, so updating DS3OS does not invalidate them. A game patch would.

## Why not block the leave message

The client sends its timer-based leave after roughly 763 to 766 seconds.
That final message is byte-identical to the one sent after a legitimate
kill:

```text
raw_hex=08 01 10 05 18 00 20 00
```

Kill cleanup and the phantom timer share the same leave path, so blocking
`RequestNotifyLeaveSession` would also break normal session cleanup. Keeping
the timer from ever reaching that path is the safe option.

## How the address was found

Cheat Engine located a live float timer at `7FF447B49A9C` while `R14` held
`0x00007ff447b48da0`, which is where the `R14 + 0xCFC` relationship comes
from. The original investigation timed the leave at 763 to 766 seconds; a live
session later showed the counter starting at 900, so those earlier numbers were
measured from somewhere after the session began rather than from its start. See [DS2_PVP_LEAVE_SESSIONS.md](DS2_PVP_LEAVE_SESSIONS.md) for the
full investigation and [DS2_LEAVE_SESSION_BY_KILL.md](DS2_LEAVE_SESSION_BY_KILL.md)
for the kill-leave comparison.

## Files

- `Source/Injector/Hooks/DarkSouls2/DS2_PhantomTimerParamPatchHook.cpp`
- `Source/Injector/Hooks/DarkSouls2/DS2_PhantomTimerParamPatchHook.h`
- `Source/Injector/Config/RuntimeConfig.h` / `.cpp`
- `Source/Injector/Injector/Injector.cpp`
- `Source/Loader/Config/InjectionConfig.cs`
- `Source/Loader/Forms/SettingsForm.cs` / `.Designer.cs`
- `Source/Loader/Forms/MainForm.cs`

## Safety notes

- The hook only installs for Dark Souls II on Windows x64.
- It refuses to patch if the target instruction signature does not match.
- It writes one float, and only when the current value is below the target.
- Values outside a sane range are ignored rather than overwritten.
