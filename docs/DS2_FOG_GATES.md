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
| 2 | `FUN_1401d1330` | init: allocates two objects into `+0x68` and `+0x70`, links itself into the map's list, caches a flag at `+0x85` |
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

## What is not known yet

- **What that byte drives.** It is written every frame while the player is
  inside, so writing 0 over it from outside would only fight the writer. The
  readers have not been found, and until they are, nothing here says whether
  the byte is the prompt, the transition, or only a hint for the HUD.
- **Where the blocking lives.** A fog wall stops the player physically, and
  nothing above touches collision. The two objects init allocates at `+0x68`
  and `+0x70` are the obvious suspects and have not been looked at.
- **What `MapObjectWhiteDoorParam` holds.** The table is registered but no
  column has been read yet. If the behaviour is a column, the patch is the same
  shape as `DS2_UnblockMultiPlayHook` — rewrite rows, never `.text`.
- **The server half.** Nothing has been checked about what a session does when
  the host crosses an area boundary.

## How to carry on

The cheapest next measurement is live, not static: with `ds2os-dev up` putting
a character in the world in a minute, a `DS2_MemProbe` chain on
`[[0x1416148f0 + 0x22f0] + 0x3c0] + 0x1e` reads that byte while someone walks
into a fog wall, and `chain` on `this + 0x58` reads the door kind. That turns
both guesses above into measurements.

Run the control: a fog wall that already behaves the way we want, if one
exists, before believing anything about one that does not.
