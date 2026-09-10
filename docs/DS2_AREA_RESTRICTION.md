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

## Where the next attempt should start

The next question is what the permission byte actually holds. Log it in Heide,
where a sign can be placed, and in Majula, where it cannot. Two values, and the
mod is forcing one into the other.

`FUN_140250dc0` is where to read it, and the interesting values are `iVar8` (the
zone id, non-positive outside any zone) and the byte the lookup returns. From
there the patch has the same shape as
[DS2_PHANTOM_TIMER_PATCH.md](DS2_PHANTOM_TIMER_PATCH.md): a breakpoint at a
known offset, a value read, a value written.

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
