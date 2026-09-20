# Architecture for travelling together

## Recommendation

My recommendation is to **rebuild the players' presence on every trip, using
the game's own loading and entry**. I would investigate two architectures, in
this order: a full coordinated re-entry; rebuilding only the remote copies,
keeping the current transport.

I do not consider it demonstrated that the current architecture is beyond
saving. What is demonstrated is that **the guards did not establish a safe
lifetime**. The corruption's signature still allows a write out of bounds, a
use after free and a race between tasks. Two changed bytes in a pointer do not
prove the writing instruction is two bytes wide.

This assessment was made from the project's documents, the four travel hooks
and a read-only check, in Ghidra, of the entry functions. No trip was run
during the analysis.

Three findings weigh on the decision:

- The reload destroys the players' presence, while parts of the session
  survive. Earlier experience already showed the host at `0x10` and the guest
  at `7`, with no interaction between them. **Preserving the session neither
  preserves nor rebuilds the characters.** See
  [DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md), section "Every warp reloads,
  and that is what takes the phantom away".
- The entry supplies more than coordinates: the host exports a snapshot, the
  guest imports the world and receives the data needed to register the remote
  players. That gives an existing mechanism for rebuilding. See
  [DS2_WORLD_STATE.md](DS2_WORLD_STATE.md), section "Where the copy on entry
  comes from".
- The map-cycle guard allows the teardown to be left incomplete. Running a
  task's completion after an exception preserves its bookkeeping; it does not
  undo the work's partial changes. So the current cost may include retained
  resources and inconsistent structures, on top of the lost frame. See
  `DS2_BackreadHook.cpp`, in `GuardedOwnerUpdate`.

## 1. Re-entry coordinated by the game's own loading

This is the first choice.

### Mechanism

Separate staying in the party from the characters existing on that map. Behind
the curtain, the party stays; the old presences are removed and the new ones
are built by the game.

The proposed sequence would be:

1. An approved vote starts an identified transaction, with the destination and
   the participants fixed.
2. Both sides suspend actions and the application of updates to the characters
   that are going to be removed. The transport and the control messages keep
   working.
3. Each machine finishes the pending tasks and properly removes its remote
   copies from the managers.
4. The host does the game's own load of the destination, now with no old
   copies left hanging in the torn-down world.
5. The host supplies a **new** invitation and snapshot of that world; the guest
   loads that destination directly through the game's own entry.
6. Both rebuild the presences and confirm interaction before handing control
   back.

The concrete pieces are:

| Piece | Existing path |
| --- | --- |
| The host's own travel | `FUN_1401843b0`, `FUN_140184830`, record through `FUN_14044fe30` |
| The guest's entry | `FUN_1402c2a80`, a request with reason 4 and flag 1 |
| The host's snapshot | `FUN_1402bf8f0` |
| Import on the guest | `FUN_1402c2fa0`, when it is in state 4 |
| Registering the presences on the guest | state 5, `FUN_1402c3c80` → `FUN_14051b0e0` |
| The host's finalisation | sequence `0xd → 0xe → 0xf → 0x10`; the `0xf` handler: `FUN_1402c03e0` |

The check in Ghidra showed that the import also fills the player list that
state 5 consumes. `FUN_14051b0e0` prepares a pending entry in the manager: a
positive return from it **does not mean a character has been spawned**.

The difference from what has already failed is substantial: it would not be
repeating the warp, writing `estado=2`, or replaying the old invitation on its
own. It would be running the materialisation protocol again **at both ends**,
including the snapshot, the registration of the remotes and the confirmations.

The guest does not pass through his own world: during the transition he may be
without a playable world, loading, but the next world materialised has to be
the host's. The original return record `+0x1a0..+0x1c8` and the separation
between his own state and borrowed state have to be preserved. A new entry
must not record the host's world as if it were the guest's home.

### Why it respects the lifetime

It lets the loader destroy and build characters and resources in the order the
game expects. It removes the need to carry the live graph of animation, model
and physics between maps.

The hypothesis still not demonstrated is that this rebuild can happen while
keeping the network link usable, without triggering the return home. The
original summon shows that the pieces exist; it does not show them being
reused in a session that is already established.

### The cheapest experiment that refutes it

First, instrument a normal summon in Heide and its legal exit, with no
experimental travel: identify the registration, the removal, the completion of
the tasks and the association between peer and character.

Then do a **coordinated re-entry on the same map, with the guest alive**. The
host has to take part in the rebuild; do not repeat the old one-sided
experiment. The attempt fails if:

- the guest's own world has to be materialised;
- the states end correctly but the characters do not interact again;
- the presence cannot be removed and recreated without losing the link the
  entry needs.

That test already needs two accounts. Alone, only the loader and the
instrumentation can be checked.

### Positive acceptance criterion

Record the removal of the old presences, the building of the new ones, the
correct snapshot applied, and movement or actions actually reproduced in both
directions. Then repeat across maps. Different addresses are not a
requirement: the allocator may reuse them; object generations have to be
tracked.

### Risks and regressions

More loading than the current roughly 2.5 seconds per player; the game's own
timeouts; late messages referring to old characters; duplicated registrations;
overwriting the return home; interference with M7's shared progress.

The biggest gap is removing the presence safely **without leaving the
session**. There is no proven ready-made primitive for that. That is the first
piece of reverse engineering, not a detail to fill in later.

## 2. Destroy and recreate only the remote copies

This is the second choice.

### Mechanism

Keep `StartTravel`, backread, focus and the local player's teleport, but
temporarily remove the remote copies **before any machine starts travelling**.

The transaction would be: both remove their copies, confirm the tasks have
completed, travel, confirm contact at the destination and register the remotes
again with current data.

The relevant structures are the remote `PlayerCtrl`, identified by
`chr+0x54 == 2`, its components, the `CharacterManager` and the manager
reached through `FUN_14051b0e0`. Besides the state 5 path, Ghidra showed
`FUN_1402c8330` calling that registration from `FUN_1402cf910`: another
concrete place to investigate the reception that delivers the player's data.

Calling `FUN_1403f6300` to free the model would not be enough. The presence
has to be removed through the owning path: network, physics, animation and
task registrations all have to agree. An isolated destructor can manufacture
exactly another dangling reference.

The existing control channel uses Steam P2P on channel 7. It can coordinate
the operation, but going on exchanging those announcements does not prove the
game's own character replication came back.

### Why it can live with the current travel

It attacks the strongest difference between the controls: alone, the local
player crosses; in a session, there is also a remote copy crossing. The
architecture would have each machine carry only its local player and create
the partner's representation once at the destination.

That is a plausible causal hypothesis, not a generalisation from the 14 solo
legs. Session subsystems stay active even when the copy does not exist.

### The cheapest experiment that refutes it

In Heide, without changing map, remove and rebuild **one** remote copy,
keeping its owner connected. Confirm that:

- the old presence really was removed;
- the new one receives position, animation and actions;
- the partner is not dropped after the interval in which the old attempt lost
  communication.

Repeat on the other machine. If that cycle requires leaving for one's own
world, leaves dangling pointers or produces only a visual phantom, the
proposal loses its premise.

If it passes, do a single leg between maps with the copies absent during the
crossing. Corruption even in that interval refutes the idea that removing only
the copies is enough.

### Positive acceptance criterion

One presence per peer after each rebuild, interaction both ways, proven
removal of the old resources and no intervention from the guards. The local
player has to keep HP, inventory, souls, role and world state correct.

### Risks and regressions

It keeps the streamer manipulation and its limits. A partial rebuild may be
harder than letting the loader rebuild everything. It needs explicit handling
of the updates aimed at the absent presence; simply queueing and replaying old
packets is dangerous.

The potential advantage is preserving much of the current speed. That is why
it stays as the second option and as a useful experiment for the first.

## The travel contract has to change in both alternatives

Today the host takes `!Moving()` as settled, but the interface itself defines
that as "arrived **or gave up**". After the timeout, the host can even call
the guests without having arrived. That ambiguity takes part in the
orchestration, as well as hurting the tests. See
`DS2_BonfireInSessionHook.cpp`, in the `s_call.Active` logic.

The implicit completion should be replaced by distinct outcomes: preparing,
loading, settling, arrived and failed. The group's completion requires
receipts from every participant for the same trip.

Arrival confirmation has to combine:

- each player's current physics contact, converted into the destination map;
- proximity to the bonfire and physics frames advancing;
- a working remote presence on both machines;
- the host's authoritative world applied.

`ContinueSettle` compares `CurrentMap()`, taken from the streamer, and prints
the contact as a diagnostic. For the proposed test, the contact has to be
compared directly as well; the log line must not turn into an independent
physics assertion. See `DS2_DeathInterceptHook.cpp`, in `ContinueSettle`.

## When to keep the current architecture

It is worth capturing the write before investing in an extensive rebuild. It
may reveal a localised bug and change the ranking.

There is a documented obstacle: Dr0–Dr7 have been accepted without firing in
this environment. `DS2_INVESTIGATION_TOOLS.md` attributes that to the
Wine/wineserver/`ptrace` path under Yama. So "never tried on this corruption"
is not the same as "instrumentation available".

The procedure would be:

1. **A positive control outside a session:** observe a deliberate write into
   memory belonging to the injector, including from a thread other than the
   one arming the breakpoint. Require the correct exception, address and
   thread.
2. Identify an **equivalent healthy instance** of the field that usually
   corrupts. For example, the vftable embedded at `MapModelComponent+0x50`,
   after confirming the component and the owner.
3. Arm the watchdog before the trip, covering the threads that can write and
   recording gaps in coverage. For `SetThreadContext`, suspend the thread
   while modifying its context; use a suitable size and alignment.
4. Capture registers, stack, bytes and the object's generation into a bounded
   buffer, without stopping the session for long to inspect.
5. Correlate the hit with destruction or freeing of the object and the heap. A
   legitimate write into an already freed block points to a stale consumer; an
   improper write into an object still alive points to the writer.

Arming on the address **after** the corruption does not recover the earlier
write. Nor should the raw address be carried between boots.

If the hardware control fails, it is not worth spending a session waiting for
silence. The existing page watchdog is an alternative for short windows, with
its cost and its gaps; not an automatically equivalent proof.

The current architecture deserves to be kept if that capture identifies a
bounded defect, its fix establishes the correct lifetime order for the
resources, and the tests prove the maps are fully torn down. If it shows stale
dependencies spread across several subsystems, that argues for rebuilding the
presence.

## Final criterion and the cost of the experiments

The guards can stay as protection during the tests, but **any trigger counts
as an architectural failure**, even if the trip arrives. Full counters have to
be read: several failure messages are limited in number, so the log can go
quiet while the counter keeps growing.

The requirement should be a short sequence at first and then at least the same
40 real legs as the reference, with:

- physical arrival of both on each leg;
- movement and actions replicated in both directions after arrival;
- going through the actual unloading of the origin, past the 30 seconds of
  retention;
- old resources removed and occupancy stable on returns to the same
  destination;
- no penalty point added;
- a final legal exit, confirming the guest gets his own world back and keeps
  the character.

Swapping host and guest and exercising the full vote are also part of passing.
Two players passing does not authorise a conclusion about three.

The first trials can be observational and with no expected disconnect cost.
The rebuild experiments carry no such guarantee: they should use a scenario
with a baseline of both accounts and a verified restore. That avoids keeping
the penalty in the original save; it does not turn a crash into a
penalty-free test.

The recommended start is the watchpoint control and the "remove/recreate one
presence on the same map" cycle. Those are the two experiments that cut the
most uncertainty: the first tells a localised defect apart from a structural
problem; the second tests the central capability of both proposed
architectures. The preferred implementation direction remains re-entry
coordinated by the game's own loader.
