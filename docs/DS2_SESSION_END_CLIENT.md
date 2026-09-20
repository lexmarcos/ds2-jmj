# How the client ends a session

The path, inside `DarkSoulsII.exe`, between "the phantom died" and
`RequestNotifyLeaveSession` reaching the server. Mapped on 12/09 with
breakpoints at runtime, because the static call graph leads nowhere here: all
of these functions are called through a table or virtually, and
`getCallingFunctions` comes back empty for every one of them.

Version 1.03 Calibrations 2.02, base `0x140000000`.

## The sends, found by the protocol ids

Searching for each message's id as an immediate (`FindImmediate.java`):

| function | immediate | message |
| --- | --- | --- |
| `FUN_1406aad80` | `MOV EDX,0x3f1` | `RequestNotifyDeath` |
| `FUN_1406ab230` | `MOV EDX,0x3ea` | `RequestNotifyJoinSession` |
| `FUN_1406ab650` | `MOV EDX,0x3eb` | `RequestNotifyLeaveSession` |
| `FUN_1406a2610` | `MOV EDX,0x398` | `RequestSummonSign` |
| `FUN_1406a24f0` | `MOV EDX,0x396` | `RequestRemoveSign` |
| `FUN_1406a1170`, `FUN_1406a1de0` | `MOV EDX,0x394` | `RequestCreateSign` |
| `FUN_1406a0910` | `MOV EDX,0x39b` | `PushRequestSummonSign` |
| `FUN_1406a6300` | `MOV EDX,0x3d2` | `RequestGetBreakInTargetList` |
| `FUN_1406a6fb0` | `MOV EDX,0x3d3` | `RequestBreakInTarget` |

All neighbours, at `0x1406a....`: this is the network layer, not the
gameplay.

## The death, and then the leave

A phantom dying in the host's world, with both breakpoints armed on the
**phantom's** client (`DS2_Trace.req`):

    bp 6aad80     RequestNotifyDeath
    bp 6ab650     RequestNotifyLeaveSession

What came out:

    alcancado +0x6aad80 de=+0x2ab3de  rdx=0x009d5170 r8=0x00ffffff
      pilha: +0x2ab3de +0x203bb4 +0x26b047 +0x18fc2d +0x18f690 +0x470b9 +0x46e44

    alcancado +0x6ab650 de=+0x293153  rdx=1 r8=5
      pilha: +0x293153 +0x2900f8 +0x25c400 +0x2bbd33 +0x2c3c19 +0x2581ff

The twelve seconds between the two were already measured in
[DS2_LEAVE_SESSION_BY_KILL.md](DS2_LEAVE_SESSION_BY_KILL.md); here they show
up as two distinct points in the code, not as a network delay.

## The leave chain, bottom up

    FUN_1406ab650                    sends RequestNotifyLeaveSession
      ^ FUN_140293110  (+0x43)       calls virtual slot +0x30 of the net service
      ^ FUN_1402900b0  (+0x48)       session event dispatcher
      ^ FUN_14025c3d0  (+0x30)
      ^ FUN_1402bbcf0  (+0x43)       wraps the event code and dispatches
      ^ FUN_1402c3900  (+0x319)      the routine that ends the session

### The dispatcher decides by the event code

```c
void FUN_1402900b0(longlong param_1, undefined1 *param_2, int *param_3)
{
    uVar1 = FUN_1402aacb0(*param_2);
    if (*param_3 == 5) {
        FUN_140292fa0(..., param_3[1], uVar1, param_2[0x18] == 2, param_3[2]);
    }
    else if (*param_3 == 6) {
        FUN_140293110(..., param_3[1]);      // ← the leave
    }
    ...
}
```

`*param_3` is the event code. **6 is leaving the session**; 5 is the other
side of the pair. The rest of the function passes the same event on to four
optional subscribers, kept at `param_1 + 0x78`, `+0xa0`, `+0xa8` and `+0xb0`,
each behind a bit of the mask at `param_2[0x18]`.

### Who raises the event

```c
LAB_1402c3bb7:                                    // inside FUN_1402c3900
    uVar7 = FUN_14028f270(local_58, *(undefined4 *)(param_1 + 0x198));
    FUN_1402900b0(*(undefined8 *)(param_1 + 0x110), param_1 + 0xd8, uVar7);
    FUN_1402bbcf0(param_1, param_1 + 0xd8, *(undefined4 *)(param_1 + 0x198));
    *(undefined4 *)(param_1 + 0xf8) = 9;
```

Fields of the session object that show up here:

| field | what it is |
| --- | --- |
| `+0x198` | the event code, 6 in the case of death |
| `+0xf8` | the state, written 9 when ending |
| `+0xd8` | the session record passed on |
| `+0x110` | the dispatcher |

Whoever writes `+0x198` decides **why** the session ends, and that is where
the difference lives between dying, the timer running out and using a leave
item. That point has not been located yet.

## The reason field, and its vocabulary

`+0x198` is not a "left" boolean: it is the **reason**, and the game has a
vocabulary for it. Searching for the immediate writes at that displacement
(AT&T syntax, `movl $0x?,0x198(%reg)`, range 0x140200000–0x140400000):

    1402bd018   movl $0xd,0x198(%rdi)
    1402bd622   movl $0x2,0x198(%rdi)
    1402bd781   movl $0x1,0x198(%rdi)
    1402bdcda   movl $0x1,0x198(%rbx)
    1402be5a2   movl $0x4,0x198(%rsi)
    1402be7a9   movl $0x4,0x198(%rsi)
    1402beeb2   movl $0x7,0x198(%rdi)
    1402bf4f7   movl $0x6,0x198(%r14)      <-- the only one that writes 6
    1402bf63a   movl $0x1,0x198(%r14)
    1402c04bb   movl $0x1,0x198(%rdi)
    1403f3914   movl $0x1,0x198(%rbx)

A single site writes 6, inside `FUN_1402bf440`, and it has the shape of a
**default**, not of a decision:

```c
if (*(int *)(param_1 + 0x198) == 0) {
    *(undefined4 *)(param_1 + 0x198) = 6;     // the other branch puts 1
}
if (*(int *)(param_1 + 0x150) != 0) {
    FUN_1402ce5f0(*(undefined4 *)(param_1 + 0x198), param_1 + 0xb0);
}
```

Whoever wrote first wins: the reason is only filled in if it is still zero.
So whoever knows *why* the session ended writes earlier, and `FUN_1402bf440`
only fills in what was left blank. Changing the 6 to another value here
changes the label, not the behaviour.

These two have a static caller, unlike the rest of the path:

    FUN_1402bddb0  ->  FUN_1402bf440     (the routine that sets the reason)
    FUN_1402c3630  ->  FUN_1402c3900     (the routine that ends it)

## The session state machine

`FUN_1402c3630` is the session object's per-frame step. It dispatches on the
state kept at `+0xf8`:

| state | handler | what it is |
| --- | --- | --- |
| 0 | `FUN_1402c37a0` | |
| 1 | `FUN_1402c4450` | |
| 4 | inline | counts time and, when it runs out, calls virtual slot `+0x30` with reason `0xf` |
| 5 | `FUN_1402c3c80` | |
| 6 | `FUN_1402c45b0` | |
| 7 | `FUN_1402c3830` | **in session** — this is where the leave is decided |
| 8 | `FUN_1402c3900` | tears down: dispatches the event, sends the `LeaveSession`, puts the state at 9 |
| 10 | `FUN_1402c4240` | |
| 0xb | inline | state := 0xc |

A single instruction in the whole binary writes 8 to that field —
`1402c386a`, inside state 7's handler — and its condition is surprisingly
simple:

```c
void FUN_1402c3830(longlong *param_1, float param_2)
{
    ...
    if (*(int *)((longlong)param_1 + 0x1cc) != 0) {
        *(undefined4 *)(param_1 + 0x1f) = 8;      // +0xf8 := 8, end it
    }
    ...
}
```

`+0x1cc` is not a boolean: it is **the reason somebody asked for the end**,
and `FUN_1402c2f20` is the door the request comes in through:

```c
void FUN_1402c2f20(longlong *param_1, undefined4 param_2)   // virtual slot +0x30
{
    if ((**(code **)(*param_1 + 0xa8))() != '\0') {
        *(undefined4 *)((longlong)param_1 + 0x1cc) = param_2;
        ...
    }
}
```

The reasons the binary passes to that slot, found by call pattern: `0xe`,
`0xf`, `0x10`, `0x12`, `0x13`, `0x18`. They are on a different scale from
`+0x198`'s, so at some point one is translated into the other.

## What death does, measured

Breakpoint on `FUN_1402c2f20` on the phantom's client, an orb invasion
staged, the phantom killed by a fall:

    alcancado +0x2c2f20 de=+0x2c9246 rdx=2 r8=0x1410c0050
      pilha: +0x2c9246 +0x190989 +0x18f773 +0x470b9 ...

**Death's reason is 2.** Walking up the stack:

    FUN_1402c9220(manager, motivo)     walks the session list (+0x48..+0x50)
                                       and calls slot +0xe8 of each one
    FUN_140190950(obj, motivo)         "tell every session to end"
    FUN_14018f760(tarefa)              task invoker: +0x18 is the function
                                       pointer, +0x20 and +0x28 are the
                                       captured arguments

In other words, death **queues** the leave as a task; by the time it runs,
the decision has already been taken and is no longer on the stack.

## The table that decides whether a kind of death undoes the session

The direct caller of `FUN_140190950` is a jump guarded by a table lookup:

```asm
14018ffcf:  test   %rdx,%rdx              ; no reason, returns
14018ffd2:  je     0x140190006
14018ffd4:  movzbl 0xe0(%rcx),%r8d        ; the "type", a byte of the object
14018ffdf:  cmp    $0x14,%r8b             ; 20 types
14018ffe3:  cmovb  %r8d,%r9d
14018ffe7:  lea    0xf30062(%rip),%r8     ; the table: 0x1410c0050
14018fff2:  add    %rax,%rax
14018fff5:  cmpb   $0x0,0x1(%r8,%rax,8)   ; entry[type].byte1
14018fffb:  je     0x140190920            ; zero: does NOT end the sessions
140190001:  jmp    0x140190950            ; non-zero: ends them all
```

16-byte entries, read from live memory with `mod 10c0050 140`:

    +0x000  00 00 00 00  07 17 00 00     type 0  -> byte1 = 0, does not end
    +0x010  02 01 01 02  03 02 01 02     type 1  -> byte1 = 1, ends
    +0x020  02 01 02 02  03 02 00 02     type 2  -> ends
    +0x030  02 01 01 03  03 02 01 02     type 3  -> ends
    +0x040  02 01 02 03  03 02 00 02     type 4  -> ends
    +0x050  02 02 01 07  01 03 00 06     type 5  -> ends
    +0x060  01 02 01 08  01 05 00 07     type 6  -> ends
    +0x070  01 01 01 04  01 07 00 03     type 7  -> ends
    +0x080  01 02 01 04  01 07 00 04     type 8  -> ends

**This is where seamless co-op can be born.** Zeroing the `+1` byte of the
right entry makes a death of that type stop tearing the sessions down — and
it is a *data* patch, not a `.text` one, of the same family as
`DS2_TimerParamPatch`.

### Zeroing the whole table does not deliver co-op, and teaches why

First attempt, on 12/09: zeroing the `+1` byte of the seventeen entries that
had it non-zero, **on the phantom's client only**, with `pokemod 10c00?1 00`.
All of them were accepted (`antes=01`/`03`, `ok`).

After that, an invasion staged and the phantom thrown off the cliff:

- the session **stayed up** — the host's health bar stayed on the phantom's
  screen, and the server kept receiving its position;
- but the phantom **did not die**. It kept falling through the void for over
  two minutes, with no death screen, no respawn, no return to its own world.
  The process was still running at 80% CPU and the image kept changing, so it
  was not a freeze: it was a death flow that never completes;
- **restoring the bytes unblocked it**: seconds later the phantom really
  died, went back to its own world and reappeared at the bonfire, normal.
  That closes the argument: the byte was holding the death flow, not just the
  session.

The reading: that byte is not "keep the session". It is part of the **death**
path, and taking it out leaves the player in limbo. Seamless co-op needs more
than suppressing the end of the session — it needs to redirect the respawn to
the bonfire inside the host's world. Suppressing without redirecting is
exactly the mistake CLAUDE.md describes as "patching the value instead of the
source".

### The phantom's index is 7

Measured on 12/09, after the tracer got its `deref` (the object is transient
and a late probe reads memory that has already been recycled — which is what
happened on the first attempt):

    bp 190950 deref rcx+e0 1

With an invasion staged and the phantom killed by a fall:

    alcancado +0x190950 de=+0x18f773 rdx=2 r8=0x1410c0050 [rcx+e0]=07

In other words: **the role is 7** and the reason is 2. The table's entry 7 is

    +0x070  04 01 01 01  03 00 07 01      byte1 = 1, ends

So seamless co-op's target is a single byte: `0x1410c00c1`. Zeroing the whole
table jammed the death; zeroing only that entry is the test still missing,
and it is the difference between switching a behaviour off and switching it
off for the right role.

## The byte does not send the player home — it only undoes the session

Tested on 12/09, once the index was known: zeroing **only** entry 7's byte
(`0x1410c00c1`, `antes=01`), on the phantom's client, then invading and
dying.

What happened:

- `FUN_140190950` **was not called** — the breakpoint armed on it did not
  fire, which is the proof that the branch changed: with the byte zeroed the
  code takes the `je 0x140190920`;
- and even so the phantom saw **"You have been vanquished. Returning to your
  world…"** and went back.

Both branches end in the same place:

```c
void FUN_140190920(longlong param_1)          // the "do not end sessions" branch
{
    FUN_14044fde0(*(undefined8 *)(DAT_1416148f0 + 0x70));
    *(undefined1 *)(param_1 + 0xce) = 1;
}

void FUN_14044fde0(undefined8 param_1)        // and this one is the "go back to your world"
{
    undefined8 pedido[4] = { -1, -1, 0, 0 };
    FUN_14044ed40(param_1, pedido);
    (**(code **)(*DAT_1416148f0 + 0x40))(DAT_1416148f0, pedido, 0);
}
```

`FUN_14044fde0` builds a request with an **empty** destination (two `-1`s)
and hands it to virtual slot `+0x40` of the game's global context. It is the
same call that shows up inside `FUN_1402c3900`, the routine that tears the
session down.

**So the split is this:** the table byte decides whether the *sessions* are
undone; the return to one's own world is a **warp**, asked for separately. A
seamless co-op has to touch the warp — suppress it, or swap the empty
destination for the bonfire of the world the player is already in. Touching
only the table leaves the player going home with the session left hanging,
which is worse than the original behaviour.

## Why this matters

Two requested features depend on this path:

- **Rematch by red sign.** If the session did not end on death, there would
  be nothing to reconnect. See
  [DS2_REMATCH_AFTER_DEATH.md](DS2_REMATCH_AFTER_DEATH.md) for the conclusion
  that the server on its own cannot rebuild the session.
- **Seamless co-op.** The goal is for death to return the player to the last
  bonfire **inside the same session**, instead of sending them to their own
  world.

A warning that was already recorded and still holds:
`RequestNotifyLeaveSession` **cannot** be blocked blindly, because it also
does the legitimate cleanup of a death. Not sending the message does not stop
the client from ejecting the player — the same event does that, locally. The
right target is the event code, not the send.

## What is still missing

- Who writes `+0x198` **before** `FUN_1402bf440`, which is what really
  decides the reason. The site with the 6 is only the default.
- What codes 5 and 6 are exactly (5 shows up in the leave's pair).
- Whether `FUN_1402c3900` is called once per session or per frame.
