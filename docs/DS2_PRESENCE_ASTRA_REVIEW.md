# gpt-6-astra's review of the presence rebuild

Consulted on 16/09, with `codex exec` in a read-only sandbox over this
repository, model `gpt-6-astra`, effort `low`. Thread to resume:
`codex resume 01a0abfe-7d86-7d71-8888-1a40a0b85a6d`.

It is the third opinion in the chain: the outside assessment
(`DS2_SEAMLESS_TRAVEL_ARCHITECTURE.md`) proposed rebuilding the presence; a
Fable agent checked it against the binary and found the central premise wrong
(`DS2_PRESENCE_REBUILD_PLAN.md`); this review checks the Fable one and lays out
the implementation paths.

**What it changed, in one line:** all 13 of Fable's prologues match, but our
remaining crash was attributed to the wrong function — see "the second
dependency that escaped the guard" in `DS2_SEAMLESS_COOP_TASKS.md`.

The report below is the model's own output, translated from the Portuguese
it was written in and otherwise unedited.

---

## 1. Verification

**I recommend bringing the crossing test with the copies absent forward and keeping the hybrid rebuild as the first option, conditional on the result.** The analysis did find, however, three relevant divergences between documentation and implementation: the address of the remaining crash belongs to another function, arrival does not yet directly require the physical contact, and the published counters needed to run the proposed gate rigorously are missing.

No file was created or edited. I did not run the harness, the games, the server or Steam; I did not access the network. `git status --porcelain=v1` was empty at the start and stayed empty at the end, on the `feature/seamless-coop` branch.

### Checking the prologues

I used `x86_64-w64-mingw32-objdump -d`, with ranges bounded by `--start-address` and `--stop-address`, over the indicated executable. **All 13 prologues in the table match completely**, including the bytes of the instructions objdump prints on continuation lines.

| Offset relative to the module | Bytes checked | Result |
|---|---:|---|
| `+0x51c820` | 12 | Equal |
| `+0x51b0e0` | 15 | Equal |
| `+0x51ce20` | 5 | Equal |
| `+0x51d2a0` | 13 | Equal |
| `+0x51c4d0` | 15 | Equal |
| `+0x51bff0` | 15 | Equal |
| `+0x51c940` | 16 | Equal |
| `+0x51dbb0` | 15 | Equal |
| `+0x520810` | 15 | Equal |
| `+0x2be090` | 16 | Equal |
| `+0x2c2820` | 15 | Equal |
| `+0x359890` | 12 | Equal |
| `+0x513340` | 16 | Equal |

The file is **28,200,992 bytes** and `ImageBase = 0x140000000`.

One factual caveat: the PE header contains `DYNAMIC_BASE` and `HIGH_ENTROPY_VA`. So **"no ASLR" is not a conclusion the header supports**; it may describe the behaviour measured in this environment. The hooks should keep resolving addresses from the base they were actually loaded at.

The prologues matching confirms the documented entry points, but it does not certify ABI, calling context or concurrency safety. In particular, the five bytes at `+0x51ce20` are a rather short signature for a future version check.

### What I checked beyond the prologues

I read the documents in the order asked, the relevant parts of the history and the hooks' implementation excerpts. I also consulted decompilations that **already existed** in `/tmp/ds2-presence-review/`, without running Ghidra again, and checked the decisive points against fresh disassembly of the executable.

**Important confirmations:**

- `FUN_14051ce20` looks for an existing entry and writes the result of the creation straight into `E+0x40`. The instruction at `0x14051d006` confirms the overwrite; that path does not remove the old character first.
- The constructor copies **`blob+0x22e → E+0x6a`**, establishing the presence's net id.
- `FUN_14051ac20` looks up an entry by net id, and requires state `2` and a valid identity.
- `FUN_14051b3c0` resolves a net id to the `PlayerCtrl`; `FUN_14051a9c0` also resolves the entry and uses `FUN_14051d360` to produce a description containing its character.
- `FUN_14051d2a0` calls `FUN_140359890`, zeroes `E+0x40`, decrements `R+8` and frees the entry.
- `FUN_14035b9c0` processes the deferred list at `CharacterManager+0x30..+0x38`, including steps before the final free. That confirms a free entry is insufficient evidence of destruction.
- The watchdog checks bit `0x10` at `sessão+0x1b8` and compares `sessão+8 − sessão+0x1b4` against `DAT_1410d7b40`. That constant's bytes are `00 00 96 43`: **300.0 as a float**.

**Important correction about the remaining crash:** `0x1403f4f2b` belongs to **`FUN_1403f4f10`**, not to `FUN_1403f4f60`. On that path:

1. The argument points to the component.
2. The game loads `componente+0x40`.
3. At `+0x3f4f2b`, it reads that object's vftable.
4. Then it calls its virtual slot `+0x08`.

That pins down the consumer of the invalid pointer better. It still does not identify who freed the object, nor whether the component belongs to a remote character, a local one or another entity.

### Divergences in the current code

- **Physical arrival:** in [ContinueSettle](/home/suel/projects/ds2-jmj/Source/Injector/Hooks/DarkSouls2/DS2_DeathInterceptHook.cpp:1780), `Arrived` depends on `CurrentMap() == destino`. [CurrentMap](/home/suel/projects/ds2-jmj/Source/Injector/Hooks/DarkSouls2/DS2_DeathInterceptHook.cpp:701) walks the streamer down to the part where the player has been. The physical contact is read for holding and for diagnostics, but it **takes no part in that decision**. The written contract is stronger than the implementation.
- **Counters:** the accumulators exist, but `Backread::WriteStatus` does not publish `s_caught_update`; `Death::status` does not publish `s_bodies_dropped`; `TravelWatch`'s interface does not offer a complete snapshot of its counters. Counting a bounded number of log lines is no substitute.
- **Freezing:** `HoldCopies` appears in the document, but I did not find its declaration or implementation in `Source`.
- **Barrier:** it already tells `Arrived` from `Failed`, but it still releases on the transport's result, with no rebuild step. The code also records the limitation of a single inbox per event type, which can lose receipts with three or more players.

**I did not check completely:** every field of the blob, every consumer of replication, the `0xd` vetoes in the two session machines, nor all of the removal's transitive effects. There was no live measurement in this consultation.

## 2. The options

### Option A — Native removal, the current transport and a rebuild through the registry

**What it is:** implement the plan's hybrid, with one barrier before the travel and another before handing control back.

The sequence would be:

**Prepare → remove on every machine → confirm destruction → host travels → guests travel → rebuild on every machine → confirm replication → `TravelRelease`.**

The central functions are:

| Responsibility | Functions/structures |
|---|---|
| Individual removal | `FUN_14051c820(E)` |
| Progression of the removal | `FUN_14051c940` → `FUN_14051d2a0` |
| Removal from the CharacterManager | `FUN_140359890`; queue processed by `FUN_14035b9c0` |
| Pending registration | `FUN_14051b0e0(R, membro, blob, flag)` |
| Materialisation | `FUN_14051dbb0` → `FUN_14051ce20` |
| Clearance specific to role `0xe` | `FUN_14051c4d0` |
| Identity resolution | `FUN_140520040`, `E+0x6a`, `FUN_14051ac20`, `FUN_14051b3c0` |

**I would rather let `FUN_14051c940` process the pending entries the way it normally does**, after the mod has registered the request in the right context. Calling `FUN_14051dbb0` directly adds the risk of reentrancy and of running the creation in an unsuitable phase of the frame.

There are two concrete ways to supply the data:

#### A1. The blob captured on entry, updated only where there is a known contract

It is the smallest prototype. Capture the blob `FUN_14051b0e0` receives, keep the identity and the flag separately, and reuse the creation representation.

For a rebuild on the same map, that cuts the work down a lot. For travelling, **reproducing the blob untouched is not enough as a final design**: it contains the character's position and state at the instant of entry. It may recreate at the origin, with old equipment or HP.

**Cost:** medium to demonstrate remove/recreate; medium to high to turn into robust travel.

**Main risk:** a stale snapshot, replication that does not resume, destruction still pending, and duplication from repeating the registration.

**Positive signal:** one new character generation per peer, resolved by the same net id, receiving the owner's current native movement and actions in both directions.

#### A2. A fresh snapshot produced by the owner on every travel

Keep the initial capture as a reference, but produce current data for the rebuild. There is a concrete path to investigate: **`FUN_14051ba70` and `FUN_14051d5a0`**, which export character data; the first also handles identity, role and net id.

The owner would produce the snapshot in a safe game context, and channel 7 would carry the data attached to the transaction. The receiver would call the registration with a member taken from the current list.

**That avoids depending on resending the native `0xd` packet or reopening its session gate.** It does not, however, authorise calling those exporters arbitrarily: their preconditions and the complete buffer still have to be checked.

**Cost:** higher than A1; it requires a validated export, transport of the snapshot, and handling of version, size, repetition and time validity.

**Main risk:** an incomplete or inconsistent export; the right identity attached to old data; treating an internal structure as a network format without validating its representation.

**Positive signal:** change equipment/state before the travel, rebuild with that current state and watch replication working afterwards. The initial copy is there to compare the fields, not as authorisation to restore old state.

**My assessment:** A1 is the better prototype; A2 is the better candidate for continuous use. Neither of them automatically solves a dependency belonging exclusively to the map or to the local player.

### Option B — Fix the second dependency through the model's life cycle

**What it is:** identify the owner of `componente+0x40` in the `FUN_1403f4f10` crash and fix its removal/recreation through the owning mechanism.

There are concrete native doors to study:

- `FUN_1403f4f10`: the exact consumer of the reported crash.
- `FUN_1403f4f60`: another model update path.
- `FUN_1403f6300`: release of the component's resource.
- `FUN_1403f4c20`: reaction to a backread change, with a shutdown and virtual release/load calls.
- `FUN_1403f4ce0`: shutdown of registration/proxy.
- `FUN_1403f4410` and `FUN_1403f4500`: construction and destruction of the component.

The last relationships come from the existing decompilations; **they do not amount to an already validated reload API**.

This option has two possible implementations: invalidate the dependency correctly when its resource dies, or run the native shutdown/reload cycle of the model while the owning component stays alive.

**Cost:** low if a single link to a safe native operation turns up; high if scattered dependencies have to be discovered.

**Risk:** turning an invalid pointer into a leak, a permanently missing model or an incomplete update. **Zeroing `+0x40` by analogy with `DropDeadRigidBody` is not justified.** That consumer's null test does not prove the others' contract.

**Positive signal:** watching the old resource shut down and destroyed, the new one attached to the right component, and both update paths running normally after the origin is unloaded.

**My assessment:** still viable, because this is the second dependency identified. I would give the investigation a bounded question: *who owns the component and which native operation swaps the resource?* I would not open another series of one-off guards. A third independent dependency triggers the rebuild rule.

### Option C — Freeze the copies while keeping their resources alive

**What it is:** keep the `PlayerCtrl` and its network identity, temporarily stop it crossing, and keep the parts of the origin the copy needs. Then resettle/reattach the copy to the destination before releasing the origin.

It is different from simply hiding the character:

| Intervention | What it solves | What remains |
|---|---|---|
| Hide the drawing | Appearance during the transition | Physics, animation, tasks and old references |
| Skip only the pre-draw | One consumer | The other consumers and the freeing of the resources |
| Freeze the position | The copy's movement | Resources can die underneath it |
| Suspend consumers and hold resources | May preserve the whole thing's life | Still has to solve the transfer to the destination |

The existing pieces include `DS2_Backread::KeepIndex`, the transport and the task points `TravelWatch` observes. The suspension would have to happen at a safe scheduling point; abandoning tasks without seeing them through would not be enough.

**Cost:** low for a limited trial; medium to high to guarantee safety across every consumer.

**Risk:** merely postponing the defect until the thaw or until the `keep` expires. It can also hold on to maps and resources progressively.

**Positive signal:** after the thaw, the copy resumes replication and normal execution, and the origin is effectively torn down with no surviving reference to the old resources.

**My assessment:** it is cheaper as an experiment, **it has not been shown to be cheaper as a solution**. The documentation already shows why "holding for longer" is not the same as establishing correct ownership. There is still no proof of a native state in which the live copy stops touching any map.

### Option D — Coordinated native loading, followed by a hybrid rebuild

**What it is:** replace the forced backread and the teleport with native loading, but keep rebuilding the presences from the mod. Do not take up again, as a premise, the pure re-entry that the session states already block.

Relevant pieces:

- Host: `FUN_1401843b0`, `FUN_140184830`, `FUN_14044fe30`.
- Guest: building the request so that it preserves the destination and the host's world, taking `FUN_1402c2a80` as a reference.
- Presence reset: `FUN_140513340` → `FUN_14051bff0`.
- Rebuild: the same primitives as option A.
- Watchdog: `FUN_1402be090`.
- Preservation of the guest's original return and of the borrowed-world state.

**Cost:** high; it is a change of transport and a revalidation of the world, not just swapping one call.

**Risk:** the guest reappearing in its own world, the wrong snapshot, loss of the replication link, a timeout, and improper alteration of the return record.

I would **not neutralise the watchdog globally**. First I would watch it arm; then I would define a treatment limited to the travel transaction, with its own deadline and recovery. The end of the transaction has to restore normal behaviour.

**Positive signal:** both materialise the destination in the host's world, replicate actions, preserve character/save and, on a legal exit, the guest gets its original world back. It must also pass with the travel started after more than 300 seconds of session.

**My assessment:** the appropriate escalation if the current transport keeps producing crashes after the copies' removal and destruction have been proven.

## 3. Recommendation

**Adopt option A, but invest first in the experiment that can refute it.** The order I propose is:

1. **Close the minimum observability.** Publish complete counters, identify the characters' generations and observe the deferred destruction. Record the `FUN_1403f4f10` crash correctly. The experiment's proof of arrival must include the current contact, the destination and the physics advancing.
2. **Watch one normal entry and one normal exit in Heide.** That calibrates the registration, materialisation, removal and destruction signals. Passive instrumentation lowers the risk, but it is not literally "free": an incorrect detour can bring the client down too.
3. **Run the causal gate before implementing any rebuild.** With consistent fixtures for both accounts, remove the copies on both sides, confirm destruction and do one leg through the current transport, host first.
4. **Keep the copies absent until the origin is actually unloaded.** Going past 30 seconds is necessary, but the clock on its own does not prove the resources were torn down.
5. **If there is a relevant crash in that window, stop implementing option A as a sufficient solution.** Investigate the component's ownership and choose B or D according to the evidence. The removal may still be part of the solution, but it has lost its justification as a solution on its own.
6. **If the gate does not refute it, test a rebuild on the same map.** First one side, then the other, then both. This is where the prolonged-absence trial, the return of replication and the duplication control come in.
7. **Integrate preparation and rebuild into the barrier.** The rebuild must happen **before** `TravelRelease`, with receipts of its own. Do not make the message that releases control start the creation as well.
8. **Validate a short sequence and then at least 40 real legs**, with intervals that expose the origin's teardown, host inversion, a complete vote, two-way movement/actions and a final legal exit.

During a deliberate absence, `R+8` can reach zero and the native sync gate can go false. So the criteria have to tell **a session preserved during the absence** from **working replication after the rebuild**. Traffic on channel 7 proves the mod is communicating, not that the characters are moving.

One successful leg lets the investigation continue; it does not approve the architecture against an intermittent crash.

## 4. Direct answers

### 1. Do the bytes match?

**Yes: all 13 prologues in the table match completely.**

I also confirmed the overwrite of `E+0x40`, the net id being carried, an important part of the identity→character resolution, the deferred queue and the watchdog's constant.

The main correction is the crash site: **`+0x3f4f2b` is in `FUN_1403f4f10`**, reading the object at `componente+0x40`.

I did not certify the plan's whole semantics from those bytes alone.

### 2. Is the order of the phases right? What is the smallest experiment?

**I agree with bringing the leg with the copies absent forward, after the instrumentation needed to prove they really were destroyed.** I would not require the recreation to be implemented in order to ask that question.

The smallest interpretable trial has:

1. A control with the copies present, on the same route and under the same conditions, to establish exposure to the defect.
2. A trial from an equivalent initial state, removing both copies while still at the origin.
3. A positive destruction receipt for each old generation.
4. Serial crossing of the local players.
5. Physical confirmation of arrival and of the origin actually being torn down.
6. Snapshots of the counters before the removal, after the removal, after arrival and after the teardown.

If the control does not reproduce the crash, a clean pass without copies has little discriminating power. If the crash happens **after destruction has been proven**, removal on its own is not enough.

Two causal cautions:

- A crash during the removal has to be classified separately from a crash during the crossing.
- Crashing without copies **does not prove the local player is the cause**. There may be residue from the earlier presence, shared resources, pending tasks or a streamer defect.

The trial's cleanup has to be planned for the case where the legal exit does not work without presences. The experiment must not depend on a recreation that does not exist yet in order to protect the saves.

### 3. How to capture the blob and get the copy moving again?

**I would capture on entry to `FUN_14051b0e0`, before calling the original.**

At that point the four useful elements are available: registry, member, blob and flag. The procedure would be:

- Copy the `0x5f0` bytes immediately into memory owned by the mod.
- Record the normalised identity, the session generation, the side, the flag, the size and where the call came from.
- After the original, confirm which pending slot was actually filled.
- At creation, correlate slot, active entry and the new `PlayerCtrl` generation.
- Invalidate the cache on leave/re-enter, on a peer change or on a change of session generation.
- Resolve the member again from the current list before registering. **Do not keep a raw copy of the 0x40-byte wrapper as a reusable member.**

The game uses `FUN_14051b050` to copy the payload into the slot. Comparisons must respect the fields that path copies; padding must not turn into a false divergence.

Concrete relevant fields, relative to the **blob**, not the slot:

| Field | Use observed statically |
|---|---|
| `+0x00..+0x3f` | Transform data; initial position read from `+0x30..+0x3f` |
| `+0x40` | Role used at creation |
| `+0x5c` | Data handed to initialisation by `FUN_140338a50` |
| `+0x18c` | Block handed to a character subsystem |
| `+0x22e` | Net id copied to `E+0x6a` |
| `+0x230` onwards | Selections and equipment data consumed at creation |
| `+0x298` | Value used to initialise HP, clamped by the created character's values |
| `+0x29c` | Name |
| `+0x2e0`, count at `+0x5e0` | Block applied by `FUN_140228dc0` |

This is **not a complete specification of the blob**.

The link that must be preserved is:

**current member ↔ entry `E` ↔ net id at `E+0x6a` ↔ new `PlayerCtrl` at `E+0x40`.**

The binary confirms the resolvers of that association. The creation also restores the entry's state, increments counters and calls `FUN_1405206a0`, which marks the member's corresponding record. That is why calling only the character constructor would not be enough.

**I did not close the whole chain from the position packet through to its application to the new character.** Having the right net id is necessary, but it does not show that buffers, sync gates and receive state resumed correctly.

The positive test has to show a distinctive sequence — walk, stop, change direction and perform an action — reaching the copy's new generation in both directions. A copy that is visible or positioned at the destination is not enough.

I would also avoid indiscriminately replaying packets accumulated during the absence. Replaceable state has to be told apart from events, the transport kept active, and a way established to discard state older than the rebuild without breaking the native protocol.

### 4. How to avoid duplication and confirm destruction?

I would use a mod state machine per identity and generation, without tampering with the entries' native states.

The correct sequence is:

1. **Reserve the operation.** Stop two requests from the same transaction registering the same peer.
2. **Capture the old generation.** Record `E`, `PlayerCtrl`, identity and net id while they are valid.
3. **Request the removal through `FUN_14051c820`.**
4. **Let the game make progress.** Watch for state `3`, a pass through `FUN_14051d2a0` and entry into the `CharacterManager` path.
5. **Confirm removal from the indices and completion of the deferred cycle.** The `+0x30..+0x38` queue is processed by `FUN_14035b9c0`; the path involves `FUN_14035b920`, `FUN_14035b380` and the release of references.
6. **Confirm the old generation's destruction.** In the `PlayerCtrl` vftable at `0x1410e4bb8`, I checked the targets `FUN_14037ec60` in slot `0` and `FUN_14037f240` in slot `+0x10`. They are concrete doors for observing destruction and cleanup.
7. **Revalidate member, session and the absence of a concurrent registration.**
8. **Register exactly once**, let it materialise and check cardinality and the network link.

Strong confirmation combines positive events for removal, cleanup/destruction and a safe point after the tasks have been processed. If the goal includes proving the memory was reclaimed, add a receipt of the allocator's free, filtered by the objects being tracked.

**Do not use as proof on their own:** `E.estado=0`, `E+0x40=0`, unreadable memory, a different address or waiting a few frames.

The allocator can hand the same address back for the new character. So the tracking identity has to be **address + generation**, and the instrumentation must not hold a strong reference that prevents exactly the destruction it is trying to measure.

Finally, `FUN_14051b0e0` returning success means **a pending request was accepted**. Repeating it because the copy has not appeared yet can create several pending ones for the same player.

### 5. Is there a cheaper alternative?

**Yes: fixing the life cycle of the `componente+0x40` resource may be cheaper**, if the investigation finds a native shutdown-and-reload operation with a defined scope. That target is now more precise than "the map's pre-draw".

Besides, `FUN_1403f4c20` suggests a native path that reacts to the backread and swaps the resource without rebuilding the whole presence. It is a concrete lead, not a demonstrated solution.

Hiding the drawing is cheap, but it does not solve ownership. Freezing the update may be a good experimental control, as long as it includes pending tasks and holding the resources. Without a safe resume operation, it only moves when the crash happens.

I would not recommend:

- Calling `FUN_1403f6300` on its own and assuming everything was shut down.
- Zeroing fields by analogy with the Havok fix.
- Generically reopening acceptance of `0xd` in an established session: that solves neither duplication, nor deferred destruction, nor the entry's other effects.
- Keeping old maps indefinitely as a final solution.

### 6. What is the minimum instrumentation for phase A?

I would separate the core needed for the first gate from the hooks needed to explain the rebuild and the replication.

| Question | Observation door | Positive signal |
|---|---|---|
| What data arrived? | `FUN_14051b0e0`, entry/exit | The blob captured and the pending slot associated with the member |
| Who was born? | `FUN_14051ce20`, entry/exit | A new generation at `E+0x40`, state `2`, the right net id and inclusion in the manager |
| Who started to leave? | `FUN_14051c820` | The tracked generation transitioning to state `3` |
| Who left the registry? | `FUN_14051d2a0` | The pointer removed and the count adjusted |
| Who was queued? | `FUN_140359890` | The old generation accepted onto the removal path |
| Who completed the deferred destruction? | `FUN_14035b9c0` and the cleanup/destruction doors | The old generation processed, the indices detached and the destructor finished |
| Was there a global reset? | `FUN_14051bff0`; caller recorded | A reset observed and its reason told apart from an individual removal |
| Is the new object resolvable? | `FUN_14051b3c0` or `FUN_14051a9c0` | A lookup by net id returns the new generation |
| Did replication resume? | The movement applier, still to be identified, plus character samples | The owner's current movement/action applied to the copy |
| Who sends the `0xd`? | `FUN_140520810`, filtered by type | A send observed with caller, thread, size and context |
| Who owns the model that fails? | `FUN_1403f4f10` and the existing instrumentation | A component→entity→generation association, captured while valid |

For the **first gate**, capture, removal, queue/destruction and counters are enough; finding who emits the `0xd` does not have to block that trial.

To keep the hook small and minimally invasive:

- Record binary events in a bounded buffer; format outside the hot paths.
- Include boot, session, travel, peer, generation, thread and sequence.
- Publish coherent snapshots of the five entries, five slots and tracked characters.
- Count dropped events: a saturated buffer makes the proof incomplete.
- Use the existing task/map observers, adding correlation, without another executor.
- Do not persist pointers to game objects in order to dereference them later on a logging thread.

The counters must cover at least map exceptions, tasks, model/post-physics, release/deregistration, skipped nodes and discarded Havok bodies. **An unchanged counter is a necessary condition; the positive signal of success comes from the life cycle, arrival, unload and replication events.**

## 5. What I do not know

### What was verified statically in this consultation

- The 13 prologues, the executable's size and the PE header.
- The exact consumer at `+0x3f4f2b`.
- The overwrite of `E+0x40`.
- The transfer of the net id and the resolvers cited.
- The existence and the processing of the deferred queue.
- The watchdog's comparison against 300 seconds.
- The divergences between the documented contract, the implemented arrival and the published counters.

### What is earlier measurement reported by the project

- The 40 legs, the 16–18 ms difference between curtains and the guards firing.
- The drop from 39 exceptions to zero on the host after `DropDeadRigidBody`.
- The reuse of the Havok block observed by the watchpoint.
- The limitations of the hardware watchpoints and the results of the re-entry attempts.

I did not reproduce those measurements nor audit all of their artifacts.

### What remains hypothesis or pending work

- Whether removing the copies eliminates the second dependency.
- Who owns and who frees the resource accessed in `FUN_1403f4f10`.
- Whether the removal stays neutral for the session under every relevant condition.
- Whether the captured blob is enough for a working rebuild in an established session.
- The final position applier, the handling of messages during the absence and the resumption of sync.
- The safety of using the exporters for fresh snapshots.
- The possibility of reloading only the model.
- The concurrency safety of the point chosen for removing and registering.
- The behaviour with three or more players.

**The most justifiable initial investment is proving complete destruction and crossing without the copies.** That test answers the causal caveat before the project takes on the cost of the rebuild.
