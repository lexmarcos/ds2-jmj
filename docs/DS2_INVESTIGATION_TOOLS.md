# Working on the running game

Everything below exists and works. It is written down because the slow part of
this investigation was building it, not using it.

## Before trusting any reading

**Confirm the item works in Heide first.** The Red Sign Soapstone is refused
while the character is hollow, and a hollow refusal looks exactly like the
Majula one - no animation, no message, nothing sent. A death costs hours if it
goes unnoticed. Use a Human Effigy ("Reverses hollowing") and re-check.

The server is **not** a witness. DS3OS logs nothing for a sign that is plainly
placed.

## The oracle

`trial.sh` presses the item and answers used-or-refused by watching the
character. It captures a reference frame, presses X, captures five more, and
compares a crop of the torso only - above the interaction banner, below the
HUD, away from the bonfire flame, all three of which produced false positives
before the crop was tightened.

The verdict is **sustained** change across the last three frames, not a peak. A
real animation holds the character in another pose for seconds; a flickering
prompt or a fire is a one-frame spike. Calibrated in both directions:

```text
item used            7-11%
two-handing with Y   9.9%
item refused         0.2-2.5%
```

## Reading and writing the game

`DS2_MemProbeHook` answers requests dropped in `DS2_MemProbe.req` beside the
injector and writes to `DS2_MemProbe.log`. It runs inside the process, which
matters: `/proc/<pid>/mem` works too, but only from an ancestor of the game,
and Steam reparents it out of reach.

```text
abs   <label> <hex address> <decimal length>
mod   <label> <hex offset from module base> <decimal length>
chain <label> <hex offset> <off,off,...> <decimal length>
pokeabs/pokemod/pokechain <label> <where> [<offsets>] <hex bytes> [<expected>]
scan  <label> <hex value> <width 1|2|4|8> [max hits]
```

Two things learned the hard way:

- **Lengths are decimal.** `1024`, not `400`.
- **Always pass the expected bytes on a poke.** Addresses a scan reported go
  stale, and writing to one that has been reused killed the game twice. With an
  expectation the write is refused instead. It also caught a wrong instruction
  encoding once: `xor al,al` is `32 c0`, not `30 c0`.

Write the request file **atomically** - build it elsewhere and `mv` it into
place. The probe polls twice a second and will happily read a half-written
file.

## Tracing what actually ran

`DS2_TraceHook` answers `DS2_Trace.req`:

```text
bp <hex offset from module base>
clear
report
```

Breakpoints are one-shot: the first hit records the address with `rcx/rdx/r8/r9`
and restores the byte for good. A hot function costs one exception instead of
thousands.

The method that worked:

1. Arm a range of function entries. `.pdata` lists every function in the
   binary - 95,446 of them - with exact bounds:
   `python3 pdata.py <lo> <hi>` prints entry and size.
2. Let the game idle ten seconds. Per-frame functions fire and disarm
   themselves, leaving a clean set.
3. Press the button. What fires now is what the press reached.
4. Do it in both areas and take the difference.

Limits found by hitting them: about 600 breakpoints is comfortable, 3229 killed
the game when the character moved. A second thread can reach an address between
the first restoring the byte and the handler running, so the handler owns an
address whether or not it is still armed - without that it died immediately.

## Reading the binary

`objdump` reads the PE directly and dumps all 27MB in about three seconds:

```bash
x86_64-w64-mingw32-objdump -d --no-show-raw-insn DarkSoulsII.exe > ds2.asm
```

That is a grep-able 5.6M line listing and it answered "who calls this" faster
than Ghidra opens the project. The image has two `.text` sections and a `.bind`
section, so linear disassembly has misaligned stretches; a reference that greps
to nothing may still exist inside one.

`rtti.py <ClassName>` maps a class name to its vtable through the RTTI
descriptors, and `vt.py <address>` finds the vtable a function sits in.

## Driving the game

`ds2os-dev pad` and `game shot` cover it. Things that cost time to learn:

- **Several interaction prompts can overlap and `Y` cycles between them.** A
  bonfire will not respond to `A` while "Touch your bloodstain" is the active
  prompt. This is the single biggest source of lost minutes.
- Standing **on** a bonfire gives no prompt; stand beside it.
- The d-pad works in menus and is how to move a selection; the left stick does
  not move menu selections.
- Quit cleanly through the menu: Start, RB five times to the gear, down twice to
  Quit Game, A, then **left** to YES. The confirm defaults to NO.
- Loading a save reliably leaves the character at the bonfire with a working
  prompt, which is often faster than walking back to one.
- On the character list, **X is Delete**. Only ever press A there.
