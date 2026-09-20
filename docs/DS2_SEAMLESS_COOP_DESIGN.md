# Seamless Co-op in DS2: the design

This document is the **brief**, not the report. It says what the mod should
be; what has already been measured from the game is in
[DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md), and what is left to build, in
order, is in [DS2_SEAMLESS_COOP_TASKS.md](DS2_SEAMLESS_COOP_TASKS.md).

The decisions below are the project owner's, recorded on 12/09. Where I added
something, it is marked as an **engineering note** — that is my own
observation about cost or risk, and it does not change the decision.

## The principle

> **The host defines the session's world state, but each player keeps their
> own character and their own save.**

There is no shared save. The guest enters the host's world with his own
character — same weapons, spells, stats, items, souls, level, Soul Memory —
and the only thing that changes is **which world he is playing in at that
moment**.

That is the difference from the original multiplayer: the second player stops
being a temporary phantom and becomes a persistent character taking part in
the same campaign.

    before: summon → area → boss → phantom vanishes → effigy → new summon
    after:  enters once → Forest → Bastille → Iron Keep → ... → Nashandra

## What belongs to the host and what belongs to each player

| belongs to the **host's world** | belongs to **each player** |
| --- | --- |
| bosses already killed before the session | character, level, stats |
| doors, levers, elevators, shortcuts | inventory, weapons, rings, spells |
| illusory walls, Pharros mechanisms | souls and bloodstain |
| NPC and quest state | hollowing and human form |
| bonfire intensity (Bonfire Ascetic) | covenant |
| enemy despawn | Soul Memory |
| Company of Champions (difficulty) | loot picked up |

## Progress: what crosses into the joining player's save

**The host's earlier progress is not copied.** Entering the world of someone
further along cannot retroactively complete the joining player's game.

    A: Pursuer dead         B enters A's world
    B: Pursuer alive        → B does not meet the Pursuer in the session
                            → B's save still has the Pursuer alive
                            → back in his own world, the Pursuer is there

**What you do together is saved for everyone.** If the two of you kill the
Lost Sinner in the session, both saves end up with the Lost Sinner dead, and
she stays dead when the guest goes back to his own world. That is what makes
it possible to play the whole campaign without repeating every boss in each
player's world.

The same goes for quests done together. For statues and secondary mechanisms,
it syncs when both were present; for important quests, it syncs.

**Loot is individual.** A chest with an Estus Flask Shard hands the item to
each player who opens it. That goes for titanite, weapons, rings, Estus
Shards, Sublime Bone Dust, Fragrant Branches and Pharros Lockstones — without
it the co-op campaign breaks.

**Boss souls belong to everyone.** Each participant gets the souls, the boss
soul and the item. There is none of the original multiplayer's reduced reward.

**Bonfire Ascetic does not sync.** The intensity belongs to the host's world.
The guest plays at the host's intensity during the session, but his own world
does not go up in intensity because of it.

**Enemy despawn follows the host** during the session, and does not record the
kills in the joining player's save.

## Death

Outside a boss fight:

    player dies → loses the souls carried → bloodstain where he died
                → respawns at the last bonfire → stays in the session
                → walks back to the group

The host keeps playing normally. No soapstone, Name-engraved Ring or new
summon is needed.

Hollowing still applies: dying cuts maximum HP down to DS2's normal limits,
the Ring of Binding still softens it, and the Human Effigy still restores
humanity and HP — **without disconnecting from the session**. What the effigy
stops doing is unlocking multiplayer; that becomes always allowed.

In a boss fight:

    a player dies → spectator mode, watching whoever is still alive
    everyone dies → party wipe: the boss returns to its initial state,
                    everyone reappears, the session continues
    someone kills → the win counts for everyone, including whoever died first

The host dying does **not** end the session: he becomes a spectator too and
the guest can finish the fight.

## World and travel

Resting at a bonfire **resets the world for everyone**: enemies come back,
breakable objects come back, NPC invasions can restart, combat state is
cleared. **No on-screen warning**: a modal box in the middle of a fight was
worse than no warning at all (decided 15/09).

Any player rests, not just the host, and anyone's rest resets the world for
everyone. The healing belongs to whoever sat down: the host's rest does
**not** heal the guest, and a guest who wants the healing sits at the bonfire
himself (decided 15/09).

Fast travel moves the whole group, by vote, and any player can propose it. The
question says where to, with bonfire and area:

    The host wants to travel to The Far Fire (Majula). Travel together?
    A ✓  B ✓  C ✓   → the whole party travels

A proposal for a bonfire the host has not lit is cancelled, with a warning to
whoever proposed it.

Doors, levers, elevators, shortcuts, illusory walls and Pharros mechanisms are
synchronised: the session's world has one authoritative state. Whoever uses
the Pharros Lockstone spends his own; nobody else loses one. If the guest goes
back to his own world, the mechanism there is still closed — Pharros counts as
world interaction, not as boss progress.

## Matchmaking, covenants and invasions

**Soul Memory stops limiting who plays with whom.** It still exists in the
save, but it does not decide the connection: entry is by password.

The covenant is individual and does not change on joining a session; the
rewards stay individual too. If the host is in Company of Champions, the world
takes his difficulty for everyone, without changing anybody's covenant.

Invasions are optional (`allow_invasions`). With them on, a Dark Spirit
invades the session and faces the group — 3v1, or balanced for more than one
invader.

## Session lifecycle

    A opens the game, creates the session
    B joins by password
    the world becomes A's state
    they play
    B leaves → goes back to his own world with everything he earned
    A leaves → the party is dissolved and everyone returns to their own world

**There is no host migration.** If the host leaves, the session ends: another
player's world state is different, and promoting someone to host would produce
inconsistency. For the same reason, a campaign should keep **always the same
host** — switching hosts between sessions makes NPC flags run backwards.

## Engineering notes

These do not change the design; they say what it costs.

- **The architectural hole is the session, not the warp.** Today a death tears
  the session down and the game never loads any area for a guest inside the
  host's world. Almost everything else on this list depends on solving that
  first — respawn, spectator, party wipe, group fast travel and bonfire reset
  only make sense once the session survives.
- **Three players are not testable on this machine.** There are two Steam
  accounts, and the session is peer to peer keyed on Steam id. Anything about
  3+ players (spectator with two alive, a three-way party wipe, 3v1) goes
  unverified locally until a third account exists.
- **The save is the second half of the problem.** "What we did together goes
  into both saves" requires the guest's client to write into its own save
  flags from a world that is not its own. That has not been investigated yet,
  and it is probably the biggest job after the session.
- **NPC quests are the most expensive item on the list** and the easiest way
  to corrupt a save. Better to leave them for last, behind bosses and world
  flags, which are simpler and easier to verify.
