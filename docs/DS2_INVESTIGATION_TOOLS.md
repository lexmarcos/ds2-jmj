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

Three things learned the hard way:

- **Lengths are decimal.** `1024`, not `400`.
- **A scan finds its own needle.** Every scan answers with one hit in the
  probe's own memory, where the value it was asked for is kept (`0x9c5fb08`,
  `0x9d5fb08`, `0x9fffb08`... - low, and the same for any value in one boot).
  A vftable that is not in the process still returns that one hit.
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

## Where the player is, and how to take them somewhere

`DS2_NavHook` publishes the local player's position in `DS2_Nav.txt`, next to
the DLL, rewritten whole every 50 ms:

    <x> <y> <z> <facing x> <facing z> <pointer> <sample>

The chain is the same one the game itself uses when a guest enters the host's
world and has to say where it is (`FUN_1402c2a80`):

    player    = *(*(*(0x1416148f0) + 0xa8) + 0xc0)
    position  = player + 0xa8      three floats, x y z
    facing    = player + 0xbc and + 0xc4, normalised

**There are two more position triples just before `+0xa8`** and they do not
follow the character — you can pick the wrong one and end up with a value
that never changes. The right one was found by walking and re-reading; it is
the only test that tells the three apart.

The **sample counter** at the end is not decoration. A reader cannot tell a
character standing still from a file that stopped being written, and that
difference is exactly "the walk arrived" against "the walk got stuck". It
cost one wrong diagnosis: the first attempt reported `travou depois de 6
passos, ainda a 1,8 m` with the character **on top of the target**, reading a
position from three steps earlier. The publisher writes through `rename`,
`rename` sometimes fails under Wine while the reader has the file open, and
swallowing that error leaves the old sample in place looking current.

The **pointer is no use for telling the instances apart**: with no ASLR, the
game's two copies land at the same heap address and publish the same value.
Two identical readings in the two files are a plausible result, not a bug —
that is how it was discovered that the walk had arrived.

With that the harness walks on its own:

```
ds2os-dev where
ds2os-dev goto --instance 1 --to-instance 2
```

`--to-instance` is "go to where the other one is", which in practice is "step
on its sign": the guest places the sign where it is standing, and the two
worlds use the same coordinates, so the sign's position never has to be found
anywhere.

The stick is **relative to the camera**, and the camera turns with the
character, so there is no fixed mapping to learn once. Each step measures the
displacement it produced: the angle between what was asked and what happened
is the camera's yaw, smoothed into the next step. Falling and getting stuck
are reported, not fought — a test that depended on the walk fails saying what
happened instead of timing out.

Three things that were expensive to find out, and that hold for anything
driving the game:

- **Focus has to be reasserted every time.** It is not enough for the window
  to already be active: the game stops accepting the virtual pad if focus is
  not claimed again. A shortcut that returned early when `_NET_ACTIVE_WINDOW`
  already pointed at the window made every walk move on the first step and
  freeze afterwards — and that looks exactly like blocked terrain. Hours were
  spent blaming the bonfire.
- **The pad's Y axis reaches the world inverted.** The mapping is a rotation
  **composed with a mirror**, and a mirror cannot be absorbed by an estimate
  that only knows how to rotate: getting it wrong makes the character walk
  steadily away from the target while the estimate chases its own tail.
  Measured, not guessed: stick right gave an angle of −164.8° in the world
  and stick forward −82.2°, and forward is only +90° from right under that
  reading.
- **The character turns before walking.** A burst that ends during the turn
  covers no ground at all — one of 700 ms after ninety degrees measured zero
  displacement. That is why steps are 1.2 s, grow when they yield little, and
  only steps that covered more than half a metre are allowed to teach the
  estimate.

What it does **not** do: avoid obstacles. It sweeps the eight directions when
it stops making headway, which solves snagging, but it does not go around
geometry. In practice it gets within 2 m of the target on terrain with steps,
which is plenty of slack for stepping on a sign, and that is why the default
radius is 2 m.

## `DS2_Bonfire.req`: travel with no session

`ir <map hex> <bonfire hex>` takes the local player to that bonfire the way
the group travel does — the map held beside the current one, focus on the
bonfire's cell, teleport, no warp and without touching the session. It exists
to measure the travel **alone**, which is the control: with a single client,
no fall costs an illegal disconnect point.

```
ir 0a1f0000 7ba7      # Heide's Ruin
ir 0a040000 122a      # The Far Fire
```

The log is `DS2_Bonfire.log`, and `DS2_Death.log` shows the load and the
landing ("viagem: o mapa ... carregou em N quadros").

## The breakpoint that follows a pointer

`DS2_Trace.req` accepts, since 12/09:

```
bp <hex offset>
bp <hex offset> deref <register>[+<hex>] <bytes>
```

Registers: `rcx rdx r8 r9 rax rbx rsi rdi`; at most 64 bytes.

This exists because **half of what is interesting in this binary is behind a
pointer**, and the pointer dies before any probe can answer. The summon's
entry point receives a `SignHandle` by address; the object that decides
whether a death undoes the session keeps the type at `+0xe0` and is recycled
within seconds. Trying to read those addresses afterwards, with
`DS2_MemProbe.req`, returns memory that has already been reused — it was
measured, and the late reading gave a heap pointer where a type byte should
have been.

Real examples:

```
bp 2a14c0 deref rdx 4        # the SignHandle the host summoned
bp 190950 deref rcx+e0 1     # the type that decides if death ends sessions
```

The read is guarded: a register can point anywhere, and a fault inside a
vectored handler takes the game with it. An unreadable address comes out as
`[rcx=... ilegivel]` instead of turning into a crash.

### Rearming the same address needs `clear`

The tracer keeps every address it has ever armed, and `Arm` gives up silently
if the address is already in the map. Sending `bp 2a14c0` a second time
**looks like it works** — the log answers `=== armados 1 enderecos ===` —
and arms nothing. It cost a whole duel round until the hit that never came
explained it.

To measure the same function twice:

```
clear
bp 2a14c0 deref rdx 4
```

### The address has to be the start of an instruction

`bp` writes `0xCC` at the byte asked for, without checking whether it is the
start of an instruction. Armed in the middle of one (`bp 26bdab`, inside a
5-byte `movsd`, on 14/09), it only failed to bring the game down because the
previous jump branched away first; the first flow to pass through there would
have executed a cut instruction. Check the address in an objdump of the range
before arming, and send `clear` if you got it wrong.

### The time of each hit

The hit line has no time. To measure how long separates two points (the
online bloodstain: the call on the death's frame, the job five seconds
later), stamp the log from outside while it grows:

```
tail -n0 -F DS2_Trace.log | while IFS= read -r l; do echo "$(date +%T.%3N) $l"; done
```

## The write watch: who writes this address

`DS2_Trace.req` accepts, since 13/09:

```
wp <hex absolute address> <bytes> [seconds, default 3, max 20]
wpclear
```

and answers, when the deadline runs out, with **each distinct instruction**
that wrote in that interval, how many times, the registers of the first hit
and the stack:

```
=== vigia de escrita em 00007fffe3649ad0 encerrada (prazo): 5040 faltas na pagina, 2 instrucoes ===
  escreveu em +0x314df1 (180x) rax=... rbx=... rcx=... rdi=00007fffe3651600 ... pilha: +0x322827 ...
  escreveu em +0x36df42 (180x) rax=00007fffe3649a40 ... rsi=00007fffe3650fc0 ... pilha: +0x370c0e ...
```

A write once per frame shows up at ~60 hits per second, and the register
pointing at the value's source is usually among the top ones — that is how
the character's position was followed, in four steps, to the Havok body.

**It is not a hardware watchpoint, and that is on purpose.** Under Wine,
another thread's debug registers are written by the wineserver through
`ptrace`, and with `/proc/sys/kernel/yama/ptrace_scope = 1` (this machine's
value) that attach is refused and the write is lost with no error. The watch
uses page protection: the target's page is made read-only, every write to it
faults with the instruction's exact address, and the page is released for one
instruction with the trap flag before being protected again.

The cost is that **every** write to the 4 KB page faults, not only the
target's — on a heap page next to `PlayerCtrl`, 5,000 to 15,000 faults in 3
seconds. The game survives that, but it is why every watch has a deadline. It
refuses pages that are not writable data and coexists with
`DS2_ForceMultiPlayZoneHook`'s single-step.

**Until 13/09 it could bring the game down when it was lifted.** A write that
faults while the page is still read-only can reach the handler after the
request thread has disarmed; the handler saw the watch off, passed the fault
on, and the process died with `0xC0000005`. It happened on an
`IngameCameraOperator` page at 24,000 faults per second, at the exact instant
the watch ended. Now the last watch's page is kept, and a write that faults on
it with the page already writable is simply retried. The DLL from before the
commit `trace: a vigia de escrita nao derruba mais o jogo` still has the race.

### Who **reads** this address: `wpr`

```
wpr <hex absolute address> <bytes> [seconds, default 3, max 20]
```

The same watch with the page **inaccessible** instead of read-only: reads and
writes of the target both show up, `leu em` or `escreveu em`. It is for
finding who consults a flag when everybody goes through a getter the listing
does not show. Read faults on other pages still go to the game before the
lock, like the ones from the stack walk.

It costs more: on a heap page next to the character, 850,000 faults in 20 s,
and the game survived. **Zero instructions proves nothing without the
control**: on 14/09 the hollow state had no reader at all in 20 s, and the
control — HP (`chr+0x168`), 9 instructions in 5 s — showed the watch was
seeing. What was reading it was the level, at `PlayerParam+0x1ac`.

## The position telemetry does not follow a teleport

`DS2_Nav.txt` (and therefore `where` and `goto`) reads the position from
`*(*(*(ctx)+0xa8)+0xc0)+0xa8`, a data block with no vtable. It follows the
walk, but it does **not** follow a position written into the physical body:
after a 69 m teleport it kept showing the bonfire the character had left,
while the server and the screen already showed the destination. The live
position is the `PlayerCtrl`'s translation (`ctx+0xd0`, field `+0x90`), which
is what the game's virtual getter `+0x148` returns.

## The co-op channel: `DS2_Channel.req`

`DS2_CoopChannelHook` (step 7, [DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md),
"The host's bonfire") answers `status` in `DS2_Channel.log`:

```text
=== canal 7: polls=4657 estranhos=0 enviados=22 falhas=0 recebidos=0 recusados=0 eu=011000010afd1a3a ===
    sessao 00007FFFFE5BFA00 vista ha 3 ms: 2 membros: 011000010afd1a3a (host) 0110000140d6d6d1
    local ha 7 ms: papel 0, registro mapa 0a1f0000 tipo 0 id 00007ba7
    do host: nada recebido
```

- `polls` counts the calls to the session poll; stuck while in a session
  means the game is not consulting the session. `estranhos` is the detour
  called with an object that is not a `SteamSessionLight`.
- `enviados`/`falhas` are the host's `SendP2PPacket`; `recebidos`/`recusados`,
  the packets read on channel 7. A host only sends, a guest only receives.
- `sessao` lists the members as the game keeps them, with `(host)` where the
  `+0xad` mark is set. Out of a session there is no object and the line
  disappears.
- `local` is what the game thread published on the last frame; a "ha" above
  2 s means loading, and the host stops announcing.
- `do host` is the last announcement kept; the respawn only uses it under
  30 s old and from someone who is still the host.

Outside `status`, the log writes a line when the members change, when the
announcement changes (host) and when the received announcement changes
(guest).

## Loading by parts: `DS2_Backread.req`

`DS2_BackreadHook` (step 8, [DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md), "A
bonfire on another map") loads a map beside the current one without a warp,
and accepts one command per line:

```
load <map hex> [<mask hex> x4]       forces the map with those parts (all, by default); one map at a time
focus <map hex> <x> <y> <z>          the streamer fetches the parts from that position's cell
unfocus                              back to the player's cell
clear                                lets the requested map go
keep <index> <ms> [<mask hex> x4]    what another player's copy does: forces that index's map for a while
status                               the maps loaded or forced: state, force byte and the seven masks
```

The answers go to `DS2_Backread.log`, and the respawn writes there too:

```text
13:31:43.984  === pedido: mapa 0a040000 partes ffffffffffffffffffffffffffffffff ===
13:31:44.490  mapa 0a040000: estado 4 -> 5, 504 ms depois do pedido
13:31:44.988  foco no mapa 0a040000 em (10.526, 5.916, -16.255): celula 19137027 (tentativa 1)
13:33:03.727  mapa de indice 1 mantido: um jogador esta nele, partes 00000000000000200000000000000000
13:33:08.011  mapa 0a040000 solto, mas segue mantido por outro jogador
13:33:08.712  mapa 0a1f0000 nao e mais de ninguem; solto
```

The maps measured: Majula is `0a040000`, index 1; Heide is `0a1f0000`, index
12. A cell of -2 in the focus is an exception inside the game's search; -1 is
no navigation map yet, or no cell within 10 units of the point. While there
is no cell, the focus tries again every 15 frames and the streamer carries on
with the player's.

**Taking a character to another map with no warp**, in the order that worked:

1. `load <map>` and wait for `estado 4 -> 5`;
2. `focus <map> x y z` at the arrival point, and wait for the cell's line;
3. teleport to the point (the "Teleport without a warp" recipe in
   DS2_SEAMLESS_COOP.md), a little above the ground;
4. `unfocus` and `clear` only after the character is standing there. Letting
   go earlier left Samuel on a Majula rock at the point of Heide's bonfire.

The force byte and the masks are live memory of objects rebuilt on every
load: a warp wipes everything, and the request is only good until then.

## Where the game breaks: `DS2_Crash.log`

`DS2_CrashHook` is always on in DS2. A vectored handler records an access
violation, an illegal instruction, a privileged instruction or a stack
overflow **with the instruction inside the game's image**: the offset, the
address read or written, the registers and up to 24 of the game's return
addresses found in the top 256 words of the stack. Then it lets the exception
carry on. It writes without the heap, at most 32 per boot, and every boot
opens with

```text
=== ds2os: vigia de excecoes no jogo ===
```

A log with only that line, on a game that closed, means the fault was not on
a game instruction — or that the process died without an exception. The
hooks' guarded reads never show up there: their instruction belongs to the
DLL.

## The EzState queries: `esd` in `DS2_Trace.req`

```
esd <ms> [label]
```

Turns on, for `<ms>` (up to 20000), a record of every EzState environment
query the game evaluates, in both evaluators: the dispatcher `FUN_140456a90`
(marked `e`) and the inner `FUN_14045c6a0`, which the map event scripts call
directly (marked `i`). At the end of the window `DS2_Trace.log` gets one line
per id:

    esd sozinho e 0001fcfd (130301) x150: 000011e4/2

the id in hex and decimal, how many times it was queried and up to four
distinct answers (`valor/tipo`; type 2 is a number). It is for finding the
query a script uses to decide something: run the same action with and without
the condition, with different labels, and compare. The detours stay installed
all the time; outside the window they cost one atomic read per query.

Measured 15/09: resting at the bonfire with and without a session gave the
same four queries, so that lock does not go through here.

## The breakpoint sweep, with the list coming from Ghidra

The `pdata.py` mentioned above no longer exists. `Entries.java`, in
`/home/suel/tools/scripts`, does the same job by reading Ghidra's own
function list and printing the **module offsets**, one per line:

```
analyzeHeadless ... -postScript Entries.java <output> 0x140270000 0x1402a0000
```

The method, confirmed on 12/09 by finding what the A button on a summon sign
reaches:

1. `head -520 <output> | sed 's/^/bp /' > <install>/DS2_Trace.req`
   (above ~600 the game gets unstable, and 3229 has killed the process)
2. leave the game idle for about fifteen seconds: what runs per frame fires
   and disarms itself
3. **wipe `DS2_Trace.log`** — that is what separates the noise from what you
   want
4. press the button
5. whatever shows up in the new log is what the action reached

In practice the first touch (opening the "Summon this dark spirit?" dialog)
left two functions: one is a pool allocator, which on its own says the touch
**allocated** something. The confirm left eleven, with their full stacks.

It is worth knowing what the noise looks like: `FUN_140279d50` is free-list
initialisation, and `FUN_140276050`/`FUN_140276110` are request-send wrappers
— they take a subsystem, ask the object for an id through virtual slot
`+0x58` and pass it on. A real target has the request's fields in hand.

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
- **A teleport keeps the character's facing, and a bonfire only offers "Rest"
  to someone facing it.** Standing on the exact spawn point after a teleport
  gave no prompt. The stick moves the camera too, so "down" changes meaning
  between presses: measure what a short press did to the position, convert the
  world direction to a stick direction, and take the last step *toward* the
  bonfire. The prompt came up 0.16 m from the spawn point.
- On the character list, **X is Delete**. Only ever press A there.
