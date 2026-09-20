# The TargetManager cap: can 2048 be raised, and would it help?

Research made on 20/09 in Ghidra (`-noanalysis -readOnly`) and a raw byte
scan of the code sections, against `DarkSoulsII.exe` 1.03 / Calibrations 2.02.
Nothing was run in either game. Addresses are at image base `0x140000000`.

Each finding is **[read]** or **[inferred]**.

The question behind it: M8's item 6b wants the session's map released so a
heavy pinned map stops blocking heavy destinations. If the 2048-entry
`TargetManager` could simply be made bigger, most of that work would be
unnecessary.

## The answer

**Raising it is mechanically realistic — and it does not remove the need to
release the map.** The **chameleon vector caps loaded maps at three**
regardless of the TargetManager **[read]**, so the fourth map carrying
chameleon data trips a different assert. Budgeting stays the route that
ships; the cap raise is worth having only as a separately gated experiment
that buys headroom for phantom and bullet spikes.

## What a target is

RTTI confirms the class: vftable `0x1410ed868`, type descriptor
`.?AVTargetManager@@` **[read]**. Layout, from the ctor `FUN_140420850` and
the vector ctor `FUN_1402480f0` **[read]**:

```
+0x0000  vftable
+0x0008  owner / allocator
+0x0010  Entry data[2048]       0x8000 bytes
+0x8018  size_t count
+0x8020  uint16 serial
         0x8028 bytes in all
```

An entry is 16 bytes, `{u16 category; u16 serial; u32 pad; TargetCtrl* obj;}`,
kept sorted by `(category << 16) | serial`. **The handle is
`(serial << 16) | category` — not an index** **[read]**, which is why nothing
outside the manager knows the capacity.

Five categories, from slot `+0x28` of each adapter's vftable **[read]**:
`TargetCharacterCtrl` 0, `TargetMapCtrl` 1,
`TargetMapGeneralLocationCtrl` 2, `TargetGeneratorCtrl` 3,
`TargetBulletCtrl` 4. The entries are a handle table for `TargetProxy`, used
by lock-on, AI, camera and homing — 31 call sites **[read]**.

## Lifetime: the entity, not the map

Unregistration is two-phase **[read]**. `FUN_140420b80` only sets bit 0 of
`obj+0x10`; `FUN_140420b90(mgr, dt)` then sweeps, releases and compacts. The
six flag sites are the teardowns of exactly the entities that register.

So entries of an unloaded map **are** released and there is no leak path —
the session's map is "permanently spent" because pinning keeps its entities
alive, not because anything leaks **[inferred]**.

But **release is deferred by up to one tick**, and whether the sweep runs
during a loading screen was **not established**. If it does not, peak
occupancy can exceed the steady-state sum the mod's budget is computed from —
a hazard whether the cap is 2048 or 4096.

## The change set, verified complete: 31 instructions

Two numbers **[read]**:

```
140248263  49 81 f8 00 08 00 00    cmp %r8,$0x800     ; the capacity
1401bd9cd  b9 28 80 00 00          mov $0x8028,%ecx   ; the one allocation
```

`FUN_1401bd7f0` is the **only** caller of the ctor, so there is exactly one
allocation **[read]**. And 29 more instructions bake the offset of `count`,
because the array is inline and `count` sits behind it: a program-wide scan
of all 4,258,167 decoded instructions for `0x8008`/`0x8010`/`0x8018`/`0x8020`/
`0x8028` found **30** hits **[read]**.

The completeness of that was cross-checked without sharing its assumptions: a
**raw byte scan** of all three code sections for the little-endian sequences
found 84 occurrences, of which 30 are the known instructions, 51 lie inside
already-decoded instructions as unrelated immediates, and 3 were read by hand
**[read]**. Two are `cmpl $0x80,0x10(%rax)` coincidences. The third, at
`0x14056853c`, is **dead linker padding in an alignment gap** that decodes as
a folded duplicate of the allocation idiom — no function, no reference. **It
is a trap for a byte-pattern patcher**, so a patcher must match the full
`ba 08 00 00 00 b9 28 80 00 00 e8` including the call.

All 31 are `disp32`/`imm32` encodings with room for any value up to
`0x7fffffff` **[read]**.

## Why it is still not enough

**The chameleon registry caps at three** — `FUN_1401c5ce0`, pushing into the
vector at `registry+0x58` with its count at `+0x78` **[read]**:

```c
uVar7 = *(longlong *)(param_1 + 0x78) + 1;
if (3 < uVar7) { assert("DLFixedVector.inl", 0x24c, "out of memory."); }
```

One entry per map that has chameleon data, released per map id by
`FUN_1401c5dd0`. Every map measured so far contributes one, so the wall is the
**fourth** such map — and raising the TargetManager cap does nothing for it.
The mod already reads and logs this limit without enforcing it.

A third limit, per character: **10 targets** (`FUN_140315930`, the push into
`chr+0x270`) **[read]**. Per character, so it never meets the global cap —
but it is why a phantom costs up to ten entries, and why the mod's 150-entry
margin has to absorb phantoms and bullets on top of the per-map costs.

## The risks of raising it

- **The failure mode of a partial patch is heap corruption, not the assert.**
  If the compare and the allocation ever disagree, or one of the 29
  displacements is missed, the game writes past the buffer and the crash
  lands somewhere unrelated. Strictly worse than today's honest assert.
- **Timing.** The manager is built once, in `FUN_1401bd7f0`, at startup. The
  size patch must be in place **before** that call; the capacity patch is
  harmless late — exactly the asymmetry that produces a too-small buffer with
  a too-large cap. The hook must apply all 31 atomically and refuse on any
  mismatch.
  - A fallback that removes the atomicity risk: every reader reloads
    `*(ctx+0x48)` from the global and none caches the pointer **[inferred]**,
    so a hook could allocate a larger block at the title screen, copy, swap
    the pointer, then patch. Offered as something to **check**, not a plan.
- **Five of the 30 offset sites are in control-flow-flattened chunks**, four
  of them in the second `.text` beside Steam's CEG `.bind` stub. Their bytes
  are plain and patchable, but a runtime integrity check over that section is
  **not ruled out**.

Reimplementing the manager behind its six entry points is **worse** here,
precisely because of those five chunks: under a byte patch they are five more
displacements to fix; under a detour they are five places that might still
reach the old storage through an entry point nobody resolved **[inferred]**.

## What this could not establish

1. Whether the sweep runs during loading screens — the "every frame" claim is
   inferred from the caller's shape and the mod's own measurements.
2. The full body of the find-by-handle path (obfuscated, split across
   `0x141afe13a` and friends).
3. Whether the Steam CEG `.bind` stub integrity-checks the second `.text`.
4. The capacity of the chameleon registry's sibling vector at `+0x18`.
