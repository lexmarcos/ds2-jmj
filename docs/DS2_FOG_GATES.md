# The fog walls, as the game sees them

Working notes on the area-transition fog. Everything here is read out of
`DarkSoulsII.exe` version 1.03, Calibrations 2.02, module base `0x140000000`,
and moves the day the game is patched.

**The game does not call them fog.** Every "fog" string in the executable
belongs to the weather — `MapVolumeFogParam`, `Fog_Ps.fpo`, `AppVolumeFogFilter`
— and none of it has anything to do with walking through a wall. The wall is a
**white door**, and that is the word to search for.

## Where its data lives

`FUN_1403bf0c0` registers every map-object param table into one block, at fixed
offsets. The block is `manager + 0x28` (`FUN_1403bd510` passes `param_1 + 5`),
so each handle below is at that offset inside the block:

| offset | table |
| --- | --- |
| `+0x08` | `MapObjectParam` |
| `+0x18` | `MapObjectGDParam` |
| `+0x28` | `MapObjectInstanceTypeParam` |
| `+0x38` | `MapObjectDoorParam` |
| `+0x48` | `MapObjectDamageMomentumScaleParam` |
| `+0x58` | `MapObjectChrSpeedScaleParam` |
| `+0x68` | `MapObjectWeightParam` |
| `+0x78` | `MapObjectBreakActionParam` |
| `+0x88` | `MapObjectBonfireParam` |
| **`+0x98`** | **`MapObjectWhiteDoorParam`** |
| `+0xa8` | `MapObjectTorchParam` |
| `+0xb8` | `MapObjectDefaultInstanceParam` |
| `+0xc8` | `MapObjectPlayGoParam` |
| `+0xd8` | `AreaParam` |
| `+0xe8` | `SlideFloorParam` |
| `+0xf8` | `MapVolumeFogParam` |
| `+0x108` | `PlayAreaParam` |
| `+0x118` | `MapStateActParam` |
| `+0x128` | `ChunkPhaseParam` |

## The class

`MapObjWhiteDoorComponent`, one instance per fog wall, held in a
`MapGimmickListCtrl<MapObjWhiteDoorComponent>`.

| what | address |
| --- | --- |
| vtable | `0x1410c6458` (a second base's vtable at `0x1410c64b0`) |
| RTTI type descriptor | `0x14156c9d8`, name at `0x14156c9e8` |
| runtime class singleton | `DAT_14160eaf8`, filled by `FUN_1410724c0` |
| constructor | `FUN_1401d0c00` (a second one at `FUN_1401d0cf0`) |
| name strings | `0x1410c64c8` ASCII, `0x1410c64e8` UTF-16 |

Virtual methods that matter so far:

| slot | address | what it does |
| --- | --- | --- |
| 2 | `FUN_1401d1330` | init: allocates two `EventKeyGuideCtrl` into `+0x68` and `+0x70`, links itself into the map's list, caches a flag at `+0x85` |
| 3 | `FUN_1401d1040` | teardown: releases both, unlinks |
| 6 | `FUN_1401d18c0` | the per-frame act — see below |
| 7 | `FUN_1403f2500` | an "is this thing allowed to act" check shared with other gimmicks |

## What happens when you walk into one

`FUN_1401d18c0`, the per-frame act, is three steps:

```c
if ((*DAT_1416148f0)->vtable[0x50]()) {        // the world is in a state that acts
    FUN_1403f2bd0(this, arg);
    if ((&DAT_14156c9cc)[**(byte **)(this + 0x58)]) {   // door kind → does it test the player?
        FUN_1401d2600(this);
    }
}
```

`this + 0x58` points at a record whose **first byte is the door's kind**, and
`DAT_14156c9cc` is a small table of flags indexed by that kind: `01 01 01 01
01` for the first five kinds and zero after. So some kinds run the test below
and some do not — which is the first place a patch could live, and the first
thing worth reading out of a running game.

`FUN_1401d2600` builds the door's oriented box from `FUN_1401d1ca0`, takes the
player from `[DAT_1416148f0 + 0xd0]`, transforms the player's position into the
box's space and compares it against the half extents on all three axes at once
(`movmskps == 0xf` is "inside on every axis"). When the player is inside:

```c
FUN_1405137e0(*(DAT_1416148f0 + 0x22f0));   // the FeManager
```

and that function is two lines:

```c
if (*(FeManager + 0x3c0) == 0) return;
*(*(FeManager + 0x3c0) + 0x1e) = 1;
```

So **standing inside a fog wall sets one byte in the frontend**, every frame,
and that is the only thing this path does. `DAT_1416148f0 + 0x22f0` is the same
FeManager the title-screen work uses.

Two things sharpen what that means:

- `FUN_1405137e0` has exactly **one caller**, the test above. The byte means
  "the player is standing in a white door" and nothing else can set it.
- The object it writes into lives at `FeManager + 0x3c0`, is `0x100` bytes,
  built by `FUN_140250940` inside `FUN_140513be0` and released in
  `FUN_1405138c0`.

## It is the prompt half, not the wall

The two objects init allocates are `EventKeyGuideCtrl` — the on-screen button
guide (`FUN_1404533d0` writes `EventKeyGuideCtrl::vftable`, and `FUN_140453a10`
takes it off the HUD list at `[DAT_1416148f0 + 0x70]`). So a fog wall carries
two button prompts and a test that says when the player is inside it.

That is worth stating plainly because it decides where a patch can go:
`MapObjWhiteDoorComponent` **does not block anyone**. Nothing in it touches
collision, and its whole per-frame job is

```c
FUN_1403f3000(this + 0x48, arg);   // the act every gimmick runs
if (kind_is_tested) set_the_frontend_byte_when_the_player_is_inside();
```

Whatever stops the player, and whatever moves them to the next area, is
somewhere else: the map's own collision, the generic gimmick act through
`this + 0x48`, or the event scripts, which are not in the executable at all.

## What is not known yet

- **What that byte drives.** Its writer is known and its readers are not. The
  likeliest reader is the code that puts the key guide on screen, but that is
  a guess, and writing 0 over the byte from outside would only fight the
  writer anyway.
- **Where the blocking lives.** Not in this component. The candidates are the
  map's collision, the shared gimmick act reached through `this + 0x48`
  (`FUN_1403f3000`), and the event scripts.
- **What `MapObjectWhiteDoorParam` holds.** The table is registered but no
  column has been read yet. If the behaviour is a column, the patch is the same
  shape as `DS2_UnblockMultiPlayHook` — rewrite rows, never `.text`.
- **The server half.** Nothing has been checked about what a session does when
  the host crosses an area boundary.

## Read from a running game

The handle chain resolves live. `[DAT_1416148f0 + 0x38]` is the map object
manager, so the white door param handle is

```
chain wd 16148f0 38,c0 40      # → an object { vtable 0x1410d9aa8,
                               #   L"param:/MapObjectWhiteDoorParam.param", … }
```

and the pointer inside it leads to a container of 0x20 byte entries. Reading
rows that way means walking someone else's container, which is why the
existing `DS2_UnblockMultiPlayHook` does not: it scans memory for the param's
name and parses the header at a fixed layout instead.

**The table is not resident in Majula.** Scanning for `MAP_OBJE` finds six
loaded params — `MAP_OBJECT_BREAK_ACTION_PARAM`, `MAP_OBJECT_CHRSPEED_SCALE_PARAM`,
`MAP_OBJECT_INSTANCE_PARAM`, `MAP_OBJECT_GD_PARAM`,
`MAP_OBJECT_PLAY_GO_DOOR_PARAM`, `MAP_OBJECT_WEIGHT_PARAM` — and no white door
among them. Neither `WHITE_DO`, `WHITEDOO`, `ITE_DOOR` nor the UTF-16 form
appears anywhere in a gigabyte of committed memory. Majula has no fog walls,
and the rows evidently arrive with the map that does.

So the measurements below have to be taken **standing somewhere that has a fog
wall**, not at the bonfire where every other test has started.

One trap worth writing down: a `scan` always reports one hit at a low address
(`0x0954fb08` here), because the probe's own needle is in memory too. Ignore
the hit that is not in the game's heap.

## Correction: white doors are there, the param is not

The section below concluded from two readings — the param not resident, the
byte never set — that the phantom's barrier was not a white door. The first
reading does not support that conclusion, and the conclusion was wrong.

Scanning the guest's memory for the class's own vtable pointer, `0x1410c6458`,
finds **five live `MapObjWhiteDoorComponent` objects** in Heide while the
barrier is up. Each holds at `+0x58` a pointer into a table of `0x20` byte
records:

| offset | meaning |
| --- | --- |
| `+0x00` | kind: `0`, `1` or `2` among these five |
| `+0x04` | a float, `1.5` on the kind `1` records and `0` on the rest |
| `+0x08`, `+0x0c` | two ids, `0x118`…`0x11e` here |
| `+0x10` | `0x009d5300` / `0x009d5301` on the kind `1` records, zero elsewhere |
| `+0x18` | `0x07cee711` / `0x07cee716` |

So the param table being absent means only that: these doors do not need a row
from it. Scanning for a class's vtable is the cheap way to ask whether its
objects exist at all, and it should have been the first question.

What remains true from the measurement is narrower: the byte at
`[[FeManager + 0x3c0] + 0x1e]` stayed 0 while the guest stood **in front of**
the barrier, which says the guest was not inside any door's box, not that no
door was there.

## The reading that was wrong

Measured in Heide, with a guest in a host's world and the fog on screen in
front of the guest, both clients probed at the same moment:

| reading | guest | host |
| --- | --- | --- |
| `[[FeManager + 0x3c0] + 0x1e]`, polled for 24s | 0 | 0 |
| `MAP_OBJECT_*` params resident | 6 | 6 |

The six are the same six that are resident in Majula, and none of them is the
white door table. So while a fog wall was being rendered a few metres away,
**no white door data was loaded and the white door test never fired**.

That is a negative result worth keeping: the fog that appears because someone
is a guest in another player's world is drawn by something other than
`MapObjWhiteDoorComponent`, and the whole class above — vtable, box test,
frontend byte — is about the *placed* fog walls of a map, not about the
boundary a phantom runs into.

## What the boundary is not, and where it probably is

The fog that appears when a phantom joins, walls off the area the session may
use, and stops host and guest alike, is a different thing again. Two systems
have been ruled out by measurement rather than by reading:

- **`MapObjWhiteDoorComponent`**, above: its table is not loaded and its test
  never fires while that fog is on screen.
- **`MapObjPlayGoDoorComponent`** (vtable `0x1410c5c88`, constructor
  `FUN_1401cd500`), whose param *is* resident everywhere. Its init
  (`FUN_1401cd7c0`) copies the door's transform and then sets its one flag from
  `[[DAT_1416751f8 + 0x368] + 0x38] < row[0]` — a comparison against an install
  chunk index. It is the streaming-install door, and it has nothing to do with
  sessions. Its per-frame slot is the generic gimmick act, so it does nothing
  else.

What is left, by name, is the multiplay zone:

| what | where |
| --- | --- |
| `MapAreaMultiPlayZoneCtrl` | vtable `0x1410c7c40`, constructor `FUN_1401e8d90` |
| `EventConditionMap_IsPlayerInsideMultiPlayZone` | an event-script condition, so part of this logic lives in the game's data, not the executable |

That is the same zone the injector already lies about: `DS2ForceMultiPlayZone`
makes every area claim to be in Heide's zone `103110`. If the boundary is built
out of the zone the player is in, the patch should change where the fog appears
— and the session that produced the measurements above ran with the patch
**off**, so nobody has seen the two together yet.

That is the next test, and it is cheap: relaunch with the patch on, summon
again, and look.

## What the session changes in a door

Two snapshots of the same five doors, matched by the record each points at —
the records keep their addresses across a map reload and the objects do not —
one with the guest alone in his own world and one with him standing in the
host's world with the barrier up. Ignoring every field that is a pointer:

| field | alone | as a phantom |
| --- | --- | --- |
| `+0x80` | `0` | `5`, on all five doors |
| `+0x85` | `0x14` | `0x0a`, on three of them |
| `+0x86` | `0` | `4` or `5` |

`+0x80` comes from the init, which copies it out of the frontend:

```c
*(int *)(this + 0x80) = *(*( *(FeManager + 0x3b8) + 0x10) + 0x68);
```

That chain reads 0 for the owner of a world and 5 for a red phantom, which is
what the guest was. **Every white door is stamped with the local player's
phantom type when it is created.**

### Poking it does nothing, and that is informative

Writing 0 over `+0x80` in all five doors, with the barrier on screen, changed
nothing: the fog stayed. The field is read at init and the door's state is
already built by then, so a later write has nothing left to affect — and no
method of the class reads the field at all, which fits.

So the test that would settle it has to change the value **while the door is
being created**: a hook on the init (`FUN_1401d1330`) that stamps 0 instead,
so every door comes up as if the local player owned the world. That is the
same shape as the patches this project already ships, and it is the next thing
to build.

### The hook, and why it failed

`DS2_PhantomFogHook` replaces the call that fetches the phantom type with
`xor eax,eax`, so every door is built stamped 0. Verified in the running game:
`+0x1d136f` reads `31 c0 90 90 90`, both clients relaunched, session formed.

**The barrier appeared exactly as before, and still could not be crossed.**

So the stamp is a symptom. And there was a reading already on the table that
said so, which should have been noticed before building anything: the **host**
owns his world, his doors carry 0 in every state, and he sees the same barrier
and is stopped by it too. A field that is 0 for someone the barrier blocks
cannot be what raises it.

The hook stays in the tree, off by default, because the measurement it makes
is worth keeping: it proves the stamp is not the lever.

### The mode, and why that failed too

The state that really tracks a session is the mode byte at `this + 0x85`:
`0x14` with the player alone, `0x0a` with a phantom in the world, on the host's
client as much as the guest's. It is written twice, in the door's init and in
its per-frame update (`FUN_1401d1920`), and the update is why patching the init
alone could never have worked.

Both writes were pinned to `0x14` — verified in memory, both clients, both
sites reading `b0 14 90 90 90`. **The barrier appeared again and still could
not be crossed.** The invasion also took noticeably longer to start, which is
either coincidence or a side effect worth remembering.

So `MapObjWhiteDoorComponent` is not the barrier at all. Its state follows a
session the way a thermometer follows a fever.

### What it actually was

`FUN_1401d24a0` is the door's decision. It takes the door's kind, from the
first byte of the record at `this + 0x58`, and the field at `this + 0x80`:

```c
case 0:  return (this->0x80 != 0) ? 4 : 0;
case 3:  return (this->0x80 != 0) ? 5 : 0;
case 2:  /* the closed state only while it is 0 */
```

and hands the answer to `FUN_1401d1f20`, which is what puts the wall up. So
`+0x80` was the lever after all — the very first field measured. The first
attempt failed because it patched the write in the door's **init** and left the
one in the per-frame **update**, which wrote the value back on the next frame.
A right hypothesis applied in the wrong place looks exactly like a wrong one.

**Patching both writes removes the barrier.** Confirmed in a session: the guest
invades, no fog wall appears at the boundary, and both players move freely.

The mode byte at `this + 0x85` moves with a session too and is a red herring:
pinning it changed nothing.

### The census, which found nothing, and was still worth having

Counting the live objects of all 43 map object component classes, with and
without a session, gives identical numbers: nothing is created or destroyed
when a phantom arrives. That ruled out a whole family of explanations in one
measurement, and it is cheap to repeat — `censo.sh` in the scratchpad scans for
each class's vtable pointer.


`.?AVMapObj*Component` and `.?AVMapArea*` give 74 classes, and their vtables
are recoverable from RTTI in one pass. Scanning the running game for each
vtable pointer counts the live objects of every class at once, and the class
whose count changes when a session forms is the one that builds the barrier.

## What is still unknown

The patch makes every fog wall behave as it does when nobody is visiting. That
is the right answer for the barrier at an area boundary, and it is untested for
everything else a fog wall does:

- **Boss fog during a session.** A boss gate is a white door too. Whether a
  phantom can still be brought through one, and whether the gate still works at
  all, has not been tried.
- **Any area but Heide**, and any door kind but the five measured there.
- **The host's side over a long session**, and what happens when a guest walks
  somewhere the game never expected a guest to be: the server's own area
  filtering is a separate mechanism, and `DS2_InvadeAnywhere` is what governs
  it.

`DS2RemovePhantomFog` is off by default and off in the loader until those are
answered.
