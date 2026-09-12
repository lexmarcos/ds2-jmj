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

## The fog a phantom sees is not a white door

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

## How to carry on

Take a character to a fog wall, then measure three things while standing in it:

1. `abs` on `[[0x1416148f0 + 0x22f0] + 0x3c0] + 0x1e` — the byte, to see it go
   to 1 and to see what changes on screen when it does.
2. `scan` for `MAP_OBJE` again — the white door rows should be resident there,
   and the header's row count and layout can be read the way
   `DS2_UnblockMultiPlayHook` reads `NETWORK_AREA_PARAM`.
3. The door kind at `this + 0x58`, which decides whether the box test runs at
   all.

Only then is there enough to say what a patch would change.

Run the control: a fog wall that already behaves the way we want, if one
exists, before believing anything about one that does not.
