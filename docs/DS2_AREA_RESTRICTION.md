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
