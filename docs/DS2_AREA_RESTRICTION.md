# DS2 area restriction on multiplayer items

Notes from investigating why a summon sign can be placed in Heide's Tower of
Flame and not in Majula. Nothing here is a fix yet; it is what has been ruled
in and out, so the next attempt does not repeat this ground.

## The behaviour

Using the Red Sign Soapstone in Majula does nothing at all: no animation, no
message, and the server never receives a request. Heide accepts the same item
from the same character on the same build. The refusal is a silent early return
on the client, before anything reaches the network.

## Where the current area lives

The area the player is standing in is a `uint32` at **`+0x1d0`** of a
heap-allocated structure. It holds the same ids the server uses, from
`Source/Server.DarkSouls2/Server/GameService/Utils/Ids/OnlineAreaId.inc`:

```text
Majula                  0x009932c0
Heide's Tower of Flame  0x009d5170
```

The structure's base has been at `0x00007ffff03a6dc0` in every run observed so
far, but it is heap memory and should be found rather than assumed.
`ds2os-dev game prepare --probe-area` locates it by scanning for values matching
a known area id and keeping the one that changes as the player travels; of
roughly 430 candidates, exactly one ever moves.

One confirmed reader:

```asm
1400f28f2:  test  BYTE PTR [rbx+0x1d9], 0x2    ; flags byte on the same struct
1400f28f9:  jne   0x1400f293e
1400f28fb:  mov   r8d, DWORD PTR [rbx+0x1d0]   ; the area id
1400f2902:  mov   edx, 0x5
1400f2907:  test  r8d, r8d
1400f290a:  js    0x1400f2922                  ; negative -> fall back
1400f2927:  mov   r8d, 0x98e4a0                ; Things Betwixt
```

## What has been ruled out

**There is no flag next to the area id.** Dumping 1024 bytes of the structure in
Heide and again in Majula shows **four** differing bytes, and all of them are the
location itself: the id at `+0x1d0` and one adjacent byte at `+0x1cc` that
tracks it. The flags byte at `+0x1d9` is identical in both. Whatever decides the
restriction is not a bit sitting beside the area id.

**The executable contains no table of area ids.** Searching all 27MB for the ids
of Majula, Heide and Forest of Fallen Giants returns nothing. Only Things
Betwixt appears, twice, as the fallback constant above. The restriction is
therefore param data the game loads, not a constant compiled into the code.

**Offset `0x1d0` is not a usable signature.** Ninety-two sites read a dword at
that offset from a pointer, and the ones that score highest on "reads the field,
then calls and branches" are all different structures: float maths, a list
length, vtable dispatch. Searching for code touching both `+0x1d0` and `+0x1d9`
off the same register finds two sites in the whole binary, and the second is
floating point on something unrelated.

**Hardware watchpoints do not work under Wine.** `SetThreadContext` with DR0 and
DR7 is accepted by all sixty threads and never fires: the handler ran, saw other
exceptions, and counted zero single steps. This is worth knowing before reaching
for the obvious tool again.

## What did work, and what it cost

A **guard page** on the address does fire, and the exception record carries the
exact address touched, which separates the four bytes we care about from the
rest of the page. It found the reader above.

It is also unusable as a continuous watch: the page is hot, and guarding it cost
**78,000 faults in thirty seconds**, each single-stepped, which took the game
down. A four second burst did too. If this is tried again, the handler must not
single-step or re-arm for faults that are not ours - the exact-address filter
already knows the difference - and even then the duty cycle needs care.

## What Ghidra found

Recovering functions and RTTI changes the picture completely. The binary keeps
its C++ class names, and two of them name the mechanism outright:

```text
MapAreaMultiPlayZoneCtrl
EventConditionMap_IsPlayerInsideMultiPlayZone
```

Dark Souls II divides the world into **multiplay zones**. Whether an area allows
summoning is not a property of the area id at all - it is whether the player
stands inside such a zone, and what that zone permits.

The chain, from the condition class down:

- `FUN_14046fb80` is `IsPlayerInsideMultiPlayZone::Evaluate`. It compares the
  zone the player is in against the one the event asks about.
- `FUN_1402aab40(zoneId)` looks a zone up by id and returns its record. Four
  functions call it.
- `FUN_140250dc0` is the one that matters. It reads the current zone id, looks
  the zone up, and takes the zone's permissions from it:

```c
iVar9 = 0;
uVar10 = *(uint *)(param_1 + 0x20);                  // flags currently in force
if ((0 < iVar8) && (piVar6 = FUN_1402aab40(iVar8), piVar6 != NULL)) {
    iVar9  = *piVar6;                                 // zone's first field
    uVar10 = (uint)*(byte *)(piVar6 + 3);             // zone's permission byte
}
if (uVar10 != *(uint *)(param_1 + 0x20)) {
    FUN_140250cc0(param_1, *(uint *)(param_1 + 0x20), uVar10);   // announce change
    *(uint *)(param_1 + 0x20) = uVar10;                          // and store it
}
```

So **`param_1 + 0x20` holds the multiplayer permissions for wherever the player
is standing**, copied from a byte at offset 12 of the zone record, and
`iVar8 <= 0` means no zone at all. That field is what the rest of the game
consults, and it is the natural place to intervene.

## What the game actually checks

Measured in a live session, with a breakpoint at `DarkSoulsII.exe+0x250e93`
reading the zone id from `ebx` and the permission byte from `edi`:

| Where | Zone id | Permission byte |
| --- | --- | --- |
| Majula, where a sign is refused | `-1` | `0x00` |
| Heide, where a sign is placed | `103110` | `0x00` |

The permission byte is the same in both, so it decides nothing. Majula is `-1`,
meaning the player is not inside any multiplay zone at all; Heide is inside one.

The zone id is read a few instructions earlier:

```asm
140250e50:  mov    rax,[rbx+0x30]
140250e54:  mov    rcx,[rax+0x70]             ; the map block record
140250e58:  mov    ebx,[rcx+0x20]             ; the zone the block is in
140250e5b:  cmp    ebx,[rsi+0x10]
...
140250e7f:  jle    0x140250e93                ; <= 0, no zone, skip the lookup
140250e83:  call   0x1402aab40                ; otherwise look the zone up
```

**`[block+0x20]` is the source; everything else is a derived copy.** The struct
at `rsi` is rebuilt from it every frame, so patching `rsi` fixes only what that
one struct's readers see.

## Forcing the zone is not enough

This was tested directly, by writing `103110` into `[block+0x20]` in the running
game (see [DS2_LIVE_MEMORY_ACCESS.md](DS2_LIVE_MEMORY_ACCESS.md)). The write
held, the game stayed up, and the whole derived state became what standing in
Heide produces:

```text
+0x10 zone id       = 103110      forced
+0x14 last valid    = 103110
+0x18 zone group    = 103100      the lookup SUCCEEDED
+0x20 permissions   = 0
```

That `+0x18` is the important one. The zone table is **global, not per map**:
looking up Heide's zone from inside Majula returns its record. An earlier guess
that the lookup would fail was wrong.

**The Red Sign Soapstone was still refused.** No animation, no sign, nothing
sent to the server, across four trials: at the original spot, on flat ground
elsewhere, and with `[block+0x12]` forced to `0` and to `1`.

So the earlier conclusion in this document - that the zone id is the whole
check - is wrong. Every value the zone system produces can be made identical to
Heide's and the item is still refused. **The gate that stops the soapstone does
not read the multiplay zone.**

Each trial was verified rather than eyeballed:

- the client was confirmed logged in (`4:Chico` on the server, one player)
- the gamepad was confirmed reaching the game (Start opened the menu; the right
  stick turned the camera)
- the refusal detector was calibrated on both outcomes: pressing Y to two-hand
  moves 7.0% of the character's pixels, a refused soapstone moves under 1.1%

## The control passed

The same automated trial was then run in Heide's Tower of Flame, on a character
standing at the bonfire with the same item equipped:

| Where | Pixels of the character that moved | Outcome |
| --- | --- | --- |
| Heide | 21.9% | sign placed, game printed **"Check your Summon Sign"** |
| Majula, zone forced | 1.1% | nothing |
| Majula, pressing Y to two-hand (calibration) | 7.0% | animation played |

So the trial does press the button, the item does work, and **Majula's refusal
is real**. Nothing about the measurement is in question any more.

One thing that is worth knowing before trusting it: **the server is not a
witness for this.** DS3OS logged no sign at all for the Heide placement that
plainly worked, so "the server logged nothing" says nothing about whether the
item fired. The animation is the signal; on success the game also prints
"Check your Summon Sign".

## The multiplay zone is not on this path at all

The cache theory was tested and is wrong, and so is everything else the zone
system holds. With one character travelling between the two areas by bonfire,
and with reads and writes into the running game from inside it
(see [DS2_LIVE_MEMORY_ACCESS.md](DS2_LIVE_MEMORY_ACCESS.md)):

- **Forced from map load.** The injector hook now writes the zone id into the
  block record itself, on the first frame after the map is built, and the log
  proves it lands: `subs=1 block_writes=1` over 30451 hits, one write that then
  holds by itself. Majula still refuses.
- **The whole object was compared.** Dumping the zone control in both places,
  with the zone forced, leaves exactly two bytes differing that are not the
  player's position: `+0x08` and the map id at `+0x0c`.
- **`+0x08` was equalised, both ways.** It is copied from `[block+0x12]`, and
  reads `2` in Majula and `1` in Heide. Setting Heide's to `2` does not stop
  the item working there.
- **The map id was equalised.** Majula is `100400` (m10_04) and Heide is
  `103100` (m10_31); an earlier attempt used `101000`, which is not a map at
  all, so that test did not count. Giving Heide Majula's real map id does not
  stop the item working there.
- **The gate itself was disabled.** `DarkSoulsII.exe+0x2509f0` is the function
  that answers "is multiplayer allowed here", and it reads the zone id:

  ```asm
  1402509f0:  cmpl $0x0,0x10(%rcx)     ; zone id
  1402509f4:  jl   0x1402509f9
  1402509f6:  mov  $0x1,%al ; ret      ; >= 0 -> allowed
  ```

  Patching it to `mov al,0; ret` so it always denies **does not stop the
  soapstone working in Heide.**

That last one settles it. **Whatever refuses the item in Majula does not read
the multiplay zone**, and neither does the code that places a sign. The zone
system governs something else - `FUN_140273840`, which decides whether another
player's sign is shown, does consult it - but not this.

The `DS2ForceMultiPlayZone` patch is therefore not the fix for Majula. It is
kept because it is correct about what it does and is likely still needed for
phantoms to see each other, but on its own it changes nothing here.

## The online area id: what that experiment really showed

Found by elimination, and confirmed in both directions on a character standing
at the Heide bonfire where the item works:

| What was done | Result |
| --- | --- |
| Every copy of the online area id changed from Heide's `0x009d5170` to Majula's `0x009932c0` | **refused** (0.8%) |
| The same copies changed back to `0x009d5170` | **works** (9.3%, sign placed) |

Nothing else about the game changed between those two runs.

**This was over-read at the time and the conclusion has since been corrected.**
Narrowing by halves found the copy that decides, and it turned out to be the row
key of `NETWORK_AREA_PARAM` - a param table, found by its own name in memory,
holding one 28-byte row per online area. Changing that key makes the lookup for
the current area **fail**, and a failed lookup refuses the item.

So what the experiment actually established is real but narrower: **the code
that places a sign consults NETWORK_AREA_PARAM for the current area, and
refuses if there is no row.** It does not follow that the area id or the row is
what separates Majula from Heide, and it is not.

### NETWORK_AREA_PARAM, and why it is not the answer either

The param is heap memory that moves between runs and is found by searching for
its own type name. Its header carries the row count at `+0x0a` and the name at
`+0x0c`; the row index starts at `+0x48` with 24-byte entries of
`{ dataOffset, nameOffset, rowId }`, and the row id is the online area id. Rows
are 28 bytes: three floats, twelve zero bytes, and a trailing bitmask.

Read out of a live game, that mask looked exactly like the answer:

```text
Things Betwixt   0    000000
Majula           4    000100
Heide            7    000111
most of the game 63   111111
```

Majula is the only area in the game below 7 apart from the tutorial. It was
not the answer. An injector hook now raises every mask to 63 before any area
loads - the log confirms `area=0x009932c0 antes=4 agora=63`, applied once and
never reverted - and Majula still refuses. Setting the one remaining differing
field, the second float, from 20 to 30 makes Majula's row **byte-identical to
Heide's**, and it still refuses.

`NETWORK_AREA_PARAM` is consulted, and it is not the discriminator. The hook is
kept because opening every area is wanted anyway and it is correct about what
it does.

### A warning about how to test this

Two game crashes came from writing to addresses a scan had reported earlier.
Four of thirty addresses no longer held the expected value by the time the write
went out, and one of those writes killed the process. **Read each address back
and confirm it still holds the old value immediately before writing it**, and
stay inside the game's own heap. Nothing was lost either time - the character
had been saved through the menu - but the loop costs several minutes each time.

## The item needs the player to be human

This cost several hours and invalidated a run of measurements, so it belongs
near the top of anything read next.

**The Red Sign Soapstone is refused while the character is hollow.** Confirmed
directly: the item stopped working in Heide, where it had worked repeatedly
minutes before, and stayed refused across a reverted injector, cleared
breakpoints and a reverted param. Using a Human Effigy - "Reverses hollowing" -
brought it straight back, 8.5% against 0.2%.

What made this hard to see is that it looks exactly like the Majula refusal:
no animation, no message, nothing sent. The character had died on a fall, and
every "refused" reading taken after that death says only that the character was
hollow.

**Every measurement in this document taken after that point is void**, which
includes the tests of the permission mask at 63 and at 7 in both areas. They
have to be run again on a human character. The findings above it - the zone
system, the map id, the equalised param row, `SignEventAreaManager` - were all
taken before the death and stand.

Before trusting any result here: confirm the item works in Heide first. That is
the baseline, it is cheap, and without it a refusal means nothing.

## Redone on a valid baseline

With the character human again, and confirming the item in Heide before each
reading:

| Where | Mask | Result |
| --- | --- | --- |
| Heide | its own 7 | **used**, 11.0% |
| Majula | its own 4 | refused, 1.4% |
| Majula | forced to 7 before the area loaded | refused, 1.5% |

So the restriction is real and reproducible, and **the permission mask is ruled
out for good**: Majula carrying Heide's exact value, applied before the map is
built, changes nothing.

## Where the two areas actually part

Comparing memory found nothing. Comparing **execution** did, and it is the
result to carry forward.

Arming one-shot breakpoints on every function in a range, letting the game idle
so the per-frame ones disarm themselves, then pressing the button, says which
code a press reached. Run in both areas on a human character:

| Where | Functions of the sign subsystem reached |
| --- | --- |
| Heide | 28 |
| Majula | **0** |

**The code that places a sign is never entered in Majula.** That is why no
amount of diffing found a flag: the flag being looked for sits inside a path
that is not walked.

Widening the range and taking the difference narrows it to fifteen functions
that run only in Heide, the earliest being `+0x1a6820`. Walking back from
there:

- `+0x1a64e0` looks something up by a 16-bit id and returns false when it is
  missing. It was a tempting answer and it is **not** the gate: it is called
  with identical arguments in both areas (`rcx=<same object>`, `rdx=0x52d`)
  and takes its **success** exit in both.
- Its success path tail-calls `0x1401a8b90`, which is where the interesting
  code is. From `+0x1a8bfb` it runs a chain of nine guards over the player's
  state, each bailing to the same address:

```asm
1401a8c00:  call 0x1401ab5e0     ; null -> bail
1401a8c16:  call 0x140500720     ; true -> bail
1401a8c26:  call 0x140500960     ; true -> bail
1401a8c32:  call 0x140500980     ; true -> bail
1401a8c3e:  call 0x1405009a0     ; true -> bail
1401a8c4a:  call 0x140500940     ; true -> bail
1401a8c56:  call 0x140500920     ; true -> bail
1401a8c6e:  call 0x1404517d0     ; false -> bail
1401a8c77:  call 0x1401ab660     ; null -> bail
1401a8c88:  call 0x140203db0     ; 0/1/2, anything else -> bail
1401a8ca7:  xor al,al ; ret      ; the bail
```

This chain runs **every frame**, not only on the press, so it can be read
without pressing anything. In Heide it runs to the end and never touches the
bail. Measured, with the fall-through address after each guard armed:

```text
reached in Heide: 1a8bfb 1a8c23 1a8c2f 1a8c3b 1a8c47 1a8c53 1a8c5f
                  1a8c77 1a8c81 1a8cc7 1a8cd5
never reached:    1a8ca7 (the bail)
```

**And it runs identically in Majula.** The same fourteen addresses were armed
standing in Majula: the same eleven are reached, the bail is never touched.
So this chain is not the gate either, and `0x1401a8b90` is not on the path
that diverges - it is per-frame work that happens in both places.

Which leaves `+0x1a6820` still the earliest known divergence. It is not
reached from the tail call out of `+0x1a64e0`, because that lands in the chain
above and the chain completes in both areas.

### Climbing from there

Recording the return address on each breakpoint hit names the caller directly,
which grep cannot do here - everything arrives through adjustor thunks and
vtables, so the static listing shows a thunk and stops. Two levels were
climbed this way:

```text
+0x1a6820   reached only in Heide, called with id 0x52d
  called from +0x32f5f6, inside the function at 0x14032f4e3
```

That call sits in a small branch:

```asm
14032f5d9:  test %rbx,%rbx
14032f5dc:  je   0x14032f5f8      ; null -> skip the call entirely
14032f5e1:  cmp  $0x1,%ebp
14032f5e4:  jne  0x14032f5ed
14032f5e6:  call 0x1401abb80      ; ebp == 1
14032f5ed:  movzwl %r15w,%edx
14032f5f1:  call 0x1401abe90      ; ebp != 1   <- reaches +0x1a6820
14032f5fc:  mov  %eax,0x700(%rsi) ; the result is stored here
```

In Heide the path is `32f5d9 -> 32f5ed -> 32f5fc -> 32f645`, the success
return. **In Majula none of those addresses is reached, and neither is the
function's own entry at `0x32f4e3`.** So the branch is not the gate either;
the whole function is skipped.

The gate is above `0x14032f4e3`, which has no static callers - it is virtual.

### The gate, found

Climbing further reached a state machine and then the decision itself.

`0x14032fa00` dispatches on a state at `[this+0x10]`:

```asm
14032fa37:  test %ecx,%ecx ; je 0x14032fc13   ; state 0
14032fa3f:  dec %ecx ; je 0x14032fbf7          ; state 1
14032fa47:  dec %ecx ; je 0x14032fa7b          ; state 2   <- Heide runs here
14032fa4b:  dec %ecx ; je 0x14032fa64          ; state 3
14032fa4f:  dec %ecx ; jne 0x14032fcc3         ; otherwise refuse
```

**Majula never leaves state 0.** Measured: pressing the item reaches
`+0x32fa00` and then `+0x32fc13`, the state-0 handler, and stops.

The state-0 handler decides what kind of sign this is, in `ebx`, and refuses if
it decides nothing:

```asm
14032fc2d:  call 0x140365ee0(rsi, 4)      ; -> ebx = 1
14032fc5c:  call 0x140365f10(rsi, 0xb)    ; -> ebx = 3
14032fc6b:  testl $0x800,0xfc(%r14)       ; set -> ebx = 2
14032fc8d:  je   0x14032fcc3              ; ebx == 0 -> REFUSE
14032fc9c:  call 0x14032f450              ; else carry on
14032fcc0:  mov  %esi,0x10(%rdi)          ; and advance the state to 2
```

In Majula all three fail. Measured by arming each branch: `+0x32fc54` (the
first test returned false), `+0x32fc6b`, then `+0x32fc8b` - which is the path
taken when **bit `0x800` of `[r14+0xfc]` is clear**. `ebx` stays 0 and the
function refuses.

Heide's state becomes 2, and the only route to 2 is that flag, so **the flag is
what separates the two areas.**

### Confirmed by patching it

Overwriting the branch at `0x14032fc7b` (`je 0x14032fc8b`, bytes `74 0e`) with
two nops forces the flag path. In Majula the flow then **passes the refusal**:
`+0x32fc8f` is reached, which never happens unpatched.

It stops one step later, at `0x14032f450` returning false - a second gate in
series. Patching that one out of the way too (the six-byte `je` at
`0x14032f4d6`) gets past it as well, and lands on the thing the whole
investigation was looking for.

### What actually stops it: the map has nothing to place a sign on

Inside `0x14032f450`, after a virtual call that fills three output slots:

```asm
14032f50b:  call *0x90(%rax)      ; ask for the placement
14032f511:  mov  0x38(%rsp),%r12
14032f516:  mov  0x40(%rsp),%r14
14032f51b:  test %r12,%r12
14032f51e:  je   0x14032f649      ; null -> refuse
14032f524:  test %r14,%r14        ; ... and two more like it
```

Measured in Majula with both gates above patched away: `+0x32f511` is reached
and `+0x32f649` follows immediately. **`+0x32f524` is never reached**, so the
very first output is null - the call produced nothing.

### What the call returns, and what it is not

The first reading of this was "Majula's map has nothing to place a sign on".
**That was wrong**, and the correction matters because it moves the search.

Capturing the outputs in Heide, where the call succeeds:

```text
+0x32f55d  rdx=0x7fffe9420a60   rcx=0x7fffeb718d10   r8=0x7fffe9422e24
```

Both outputs sit inside loaded **param** data, not map geometry. Searching the
region around them for a type name finds `ITEM_TYPE_PARAM` and
`ITEM_USAGE_PARAM` immediately after, and the rows have the shape of item
parameters - counts, distances, a couple of floats:

```text
0x7fffe9420a60  ... 0456 ... 03ea 1388 1.0f 0352 096a ...
0x7fffe9422e24  ... 1.0f 0096 0096 ... 0.5f 0064
```

So the call at `0x14032f50b` resolves **which item is being used**, not where a
sign may go. It returning null in Majula does not mean the map is missing
something; it means the game has already decided there is no item to use by the
time it is asked.

**The decision is therefore still upstream**, and the useful thing this
established is narrower but solid: the refusal is reached through a specific,
short path, and two of the checks along it can be patched away without
changing the outcome.

## The item can be made to fire in Majula

Two byte patches, applied live, get the Red Sign Soapstone to play its
placement animation in Majula - the kneeling gesture that had never once
appeared there across dozens of trials.

```text
0x14032fc7b   74 0e            -> 90 90              take the flag path always
0x14032fc7d   41 0f b7 6e 40   -> bd 2d 05 00 00     mov ebp, 0x52d
```

The second one is the interesting half. `0x14032fc7d` reads the id of the item
being used out of `[r14+0x40]`. Measured at `0x14032fc9c`, where the value has
reached a register:

| Where | type (`rdx`) | item id (`r8`) |
| --- | --- | --- |
| Heide | 2 | `0x52d` |
| Majula, first patch only | 2 | **`0xffff`** |

`0xffff` is "none". **In Majula the game never registers an item as being
used**, so everything downstream - the sign type staying 0, the item param
lookup coming back null - follows from that one empty field. Writing the
soapstone's own id into it makes the whole chain run.

Measured: 6.4% sustained against a 1.0-2.5% refusal, and the animation is
plainly visible in the captures.

**The sign itself does not stick.** Checked afterwards the same way Heide was
checked: pressing the interact button offers "Cancel summon sign?" in Heide and
offers nothing in Majula, and no sign remains on the ground. So the item is
now *accepted* - the game runs the placement and plays the animation - but the
sign is not registered.

That is a real step and an incomplete one, and it says where the rest of the
work is. Forcing a constant into `[r14+0x40]` gets the flow moving without
putting the game into the state that flow expects; the field is meant to be
filled by whatever registers an item as being used, and in Majula that never
runs. Finding *that* is the remaining question, and it is now a narrow one:
something decides, before any of this, that the soapstone is not a usable item
here.

### The object, and the last open thread

`r14` resolves as `[[this+0x8]+0xc8]`, and it can be read live. In Majula,
standing there with the soapstone equipped:

```text
+0x40  item id     = 0xffff   (none)
+0x42              = 0x0000
+0xfc  flags       = 0x00000000   bit 0x800 clear
```

Writing the item id in **holds** - `+0x40` stays `0x52d` across frames. Writing
the flag does not: `+0xfc` is back to zero on the next read, so it is
recomputed every frame from somewhere else, and with it clear the sign type
stays 0 and the refusal stands.

**So the last question is what computes `[r14+0xfc]` bit `0x800`.** It is not
stored state that can be poked; it is derived. Everything in front of it is
now mapped, measured and reproducible, and the two byte patches above show
that forcing past it moves the whole chain.

### What is nailed down

- Majula never leaves state 0 of the state machine at `0x14032fa00`.
- In the state-0 handler the sign type stays 0, because all three routes fail,
  including the `0x800` flag test at `0x14032fc6b`.
- Patching that branch (`0x14032fc7b`, `74 0e` to two nops) gets past the
  refusal at `0x14032fc8d`, which never happens otherwise.
- Patching the next one (`0x14032f4d6`, the six-byte `je`) gets past that too.
- What is left refusing is the item resolution at `0x14032f50b` coming back
  empty, and that is a consequence rather than the cause.

### How to continue, concretely

The climb is mechanical now and does not need a travel per level, which is what
made it slow: **stay in Heide, where the path runs**, and repeat this loop as
many times as needed in one sitting.

1. `bp <offset of the function>` , press the item, read `de=+0x...` from the
   log. That is the caller.
2. Take the caller's own function entry from `.pdata` and repeat.
3. When a level looks like a plausible decision point, go to Majula **once**
   and arm every level captured so far. The highest one that still fires in
   Majula is where the two paths part, and the gate is inside it.

Each Heide level costs about a minute. The expensive step is the single trip to
Majula, so batch it.


## Still open

Nothing found so far separates Majula from Heide. Ruled out by direct
measurement, in both directions where possible: the multiplay zone and every
value derived from it, the map id, the whole of `NETWORK_AREA_PARAM`, and
`SignEventAreaManager`'s list test at `+0x20c820`, which changes nothing when
forced to either answer.

What is known about the gate: it runs on the client before anything reaches the
network, it consults `NETWORK_AREA_PARAM` on the way, and it produces no
animation and no message.

The tooling to keep going is in place and is the part worth keeping: reads,
writes, scans and code patches into the running game from inside it, an oracle
that separates a used item from a refused one by 7% against 1% of the
character's pixels, and menu automation that can quit, relaunch, load a
character and travel between areas.

`+0xf28fb`, which the guard page found, turned out to be a red herring: Ghidra
shows `FUN_1400f2690` builds display text, formatting the area name for the
interface. It reads the area id every frame, which is exactly why a watch on
that address found it first.

## Tooling built along the way

`Source/Injector/Hooks/DarkSouls2/DS2_AreaProbeHook.cpp`, enabled with
`ds2os-dev game prepare --probe-area`:

- scans for the area id and reports the address that follows the player
- accepts a known address with `--area-address`, skipping the scan
- dumps the structure on request, so two areas can be compared

Reads are guarded with structured exception handling and copied a megabyte at a
time. Both were learned the hard way: the first version crashed the game by
reading memory another thread had freed, and the fix for that then hung it by
allocating whole regions at once.
