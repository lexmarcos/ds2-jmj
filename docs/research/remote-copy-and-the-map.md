# The other player's copy, and the map under it

Research made on 20/09 in Ghidra (`-noanalysis -readOnly`) and `objdump`,
against `DarkSoulsII.exe` 1.03 / Calibrations 2.02. Nothing was run in either
game. Addresses are at image base `0x140000000`. **[read]** / **[inferred]**.

For M8 item 6b: releasing the session's map means releasing a map the other
player's copy may be standing in.

## A correction to fix first

**`chr+0xf0` is the model, not the physics component.** `chr+0x100` is the
`ChrPhysicsCtrl` — the contact handle and the Havok body hang off it, and the
repo's own `kChrPhysics` already says `0x100`. `chr+0xf0` carries the
lighting snapshot at `+0x2a0`/`+0x2a8`/`+0x228`/`+0x230`/`+0x4a0` and is what
`FUN_14031a7f0(*(chr+0xf0), 0.5f, 1)` fades **[read]**. Any recipe that
treats `+0xf0` as physics touches the wrong object.

## The answers

1. **A character has no map id.** It is anchored by a 32-bit *handle* its
   physics contact holds — `*(chr+0x100)+0x10` field `+0xe0` (and `+0xdc` for
   lighting) — packed `{kind: bits 0-3, map index: bits 4-9, element: 10+}`,
   and **every** use re-resolves it through the map owner with a null and
   bounds guard **[read]**.
2. **The CharacterManager never walks maps.** Its per-frame passes drive each
   character's own components and touch no owner **[read]**.
3. **The streamer does not keep a map loaded for a remote copy.**
   `FUN_1403dc930` consults only the local player **[read of its own body]** —
   which is exactly why the mod had to invent `keep <map index> <ms>`.
4. **Every owner-mediated lookup fails safe**, because the teardown frees
   **and then nulls** each field **[read]**.
5. **A remote copy's memory is not map memory.** `FUN_1403572e0` allocates it
   from one of six CharacterManager slot heaps (`*(ctx+0x650) + 0x5d0 +
   i*0xa90`), and names it `NetworkPlayer_%06u` **[read]**. A map release
   cannot free the copy.
6. **Vanilla has no "this map went away, deal with the copies" path.** The
   only native removal is the whole-world shutdown **[read]**.

## The one cached map-owned pointer on a character

The **lighting cube**. `FUN_140312920` caches `chr+0x460` (entry),
`chr+0x468` (part), `chr+0x470` (region), and refreshes only when the region
byte or the part changes — **never on a map change** **[read]**. The entry it
caches is a raw pointer out of `owner+0x1a0`, which the teardown frees and
nulls at step `0xd`.

That is the measured 19/09 crash: the renderer bound a freed cube 300 ms
after a release and killed the host at `+0x833655`. **It applies to every
character** — players, enemies and the remote copies.

## What holds what, across a teardown

| site | dereferences | after teardown |
| --- | --- | --- |
| `FUN_140312ba0` (the part under a character) | `owner+0x160`, `+0x168`, guarded by `owner+0x1e0 > 0xb` | returns 0, "no ground" — **safe** |
| `FUN_140312be0`, `FUN_1401da970`, `FUN_1403bcfa0` | via the owner, null-guarded | **safe** |
| **`chr+0x460`** the lighting cube | a raw entry of the freed bank | **use after free**, measured |
| **`streamer+0x20`/`+0x28`** | the raw part of `owner+0x148` | **use after free** |
| **enemy sync records** | the map's 0xa0 blocks | **use after free** |
| **SignManager areas** | a pointer into map data | **use after free** — the game's purge is a bare `ret` |

**Not established**: whether the character's Havok body (`chr+0x100 +0x40`,
`+0x110`) references the map's collision world. That is the one binding left
open, and the repo's 16/09 use-after-free was on `hkpRigidBody+0x18`.

## A second correction

The crash recorded as "inside the CharacterManager" when a map was released
under a copy — `+0x3f4fac` / `+0x3f510f` — is inside `FUN_1403f4f60` and
`FUN_1403f4f10`, which are **`MapModelComponent` methods** **[read]**, as the
repo itself corrected on 16/09. So the "a character was already released"
reading of that crash does not hold, and no character-side dereference was
found that would produce it. See [map-model-fault.md](map-model-fault.md).

## The order a mod has to follow

On the machine that releases, **with the map still loaded**:

1. **Unbind the enemy sync**, `FUN_140517080(*(0x141616cf8 + 0x28))`. Must
   run while the map is loaded, on the host.
2. **Drop the SignManager's areas of that map by hand** — the game's own
   purge is a bare `ret`. The mod already does this.
3. **Clear the lighting cache of every character** in the manager's list
   whose entry belongs to this map's bank: `chr+0x460 = 0`, `chr+0x468 = 0`,
   `chr+0x470 = 0xff`, `*(chr+0xb8)+0x250 = 0xff`, and on the **model**
   `M = *(chr+0xf0)`: `+0x2a0 = +0x2a8 = +0x228 = +0x230 = 0`, `+0x4a0 = 1`,
   `+0x2b0 = 0.0f`, `+0x238 = 0`. **No native call does this**, and it is the
   step the remote copy needs and would otherwise die of.
4. **Get the local player's streamer part off the map** before
   `owner+0x148` is freed. The mod's parking already does this.
5. **Either leave the copy where it is** — which the guards suggest is safe
   once step 3 is done **[inferred]** — **or take it out** with
   `FUN_14051c820(E)` per entry and wait for the manager's deferred list to
   drain before releasing anything. A copy that is *moved* rather than
   removed keeps its pose from the net, so any teleport of it is overwritten
   by the next position packet **[inferred]**.
6. **Only then** let the owner go. The whole teardown is one frame.

Expect the enemy-generator pass `FUN_14041eee0` to call `FUN_140312ba0` on
the copy during and after the release: it is guarded, so the copy contributes
no cell mask and map-spawned enemies near it will not activate **[read]**.

**"Leave the copy" is untested**, and one fact argues against it: the
`MapModelComponent` corruption of 18/09 hit **both** machines 17 ms apart,
and nothing read here accounts for that.

## What this could not establish

- Whether the copy's Havok body references the map's collision world.
- What ticks `chr+0x390` (`ChrEventTriggerCtrl`) and whether it caches a map.
- The **pose-replication** path of a remote copy — the last place a vanilla
  "its map is not loaded, hide it" gate could hide.
- Whether the streamer's mask callees read a presence; only `FUN_1403dc930`'s
  own body was read.
