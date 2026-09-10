# Where the six-player limit lives

The brief asks for twelve players in a session instead of six. This
document records where that six is written, because the answer changes
what the work is: it is not a constant to raise, it is the size of two
fixed arrays inside a heap object the client allocates once.

Nothing here is patched yet. Everything below was read out of
`DarkSoulsII.exe` version 1.03, Calibrations 2.02, with Ghidra and
`objdump`. Addresses are absolute, with the module at `0x140000000`
and no ASLR.

## The short version

A session is a Steam lobby. The host asks Steam for a lobby with a slot
count, and that count is `6`, written as a literal in the game's own
matching layer. Behind it, the client keeps **five** peer records and
**five** member records, both as fixed inline arrays in a single object
of `0x2500` bytes. Five remote players plus the host is the six.

Raising the literal alone would let a seventh player into the lobby and
give them nowhere to be stored. That is worse than the limit: a
rejection becomes a write past the end of an array.

## The object

    [0x141616cf8 + 0x20]

Allocated once, `0x2500` bytes, 16-byte aligned:

    140513e17  mov  $0x10,%edx        alignment
    140513e1c  mov  $0x2500,%ecx      size
    140513e24  call 0x140833320       allocator
    140513e34  call 0x14051ad90       constructor
    140513e4a  mov  %rax,0x20(%rcx)   stored on the global

Its layout, as far as the player limit is concerned:

| offset | what | evidence |
| --- | --- | --- |
| `+0x1a8` | 5 member records of `0xd0` | `imul $0xd0` with `cmp $0x5` at `14051b13b`, `14051c03a` |
| `+0x5b8` | pointer to the multiplay state | already known: `[[0x141616cf8+0x20]+0x5b8]` |
| `+0x5c0` | 5 peer records of `0x640` | loop `add $0x640` bounded by `+0x2500` |
| `+0x2500` | end of the object | equals the allocation size |

The arithmetic closes on itself, which is the strongest evidence that
the reading is right:

    0x1a8 + 5 * 0xd0  = 0x5b8    the member array ends exactly at the pointer
    0x5c0 + 5 * 0x640 = 0x2500   the peer array ends exactly at the object

The object's constructor, `FUN_14051ad90`, builds both arrays and settles
the layout beyond argument. It counts each one down from four, which is
five iterations, and zeroes the pointer between them:

    14051ae0b  lea 0x4(%rbp),%esi     rbp = 0, so esi = 4
    14051ae14  lea 0x1fc(%rdi),%rbx   0x1a8 + 0x54, a field of entry 0
    14051ae61  dec %esi
    14051ae67  lea 0xd0(%rbx),%rbx
    14051ae6e  jns 0x14051ae20        five member records

    14051ae77  mov %rbp,0x5b8(%rdi)   the pointer, cleared
    14051ae70  lea 0x5c0(%rdi),%rcx
    14051ae7e  call 0x14051aeb0       the peer array's own constructor

    14051aecd  mov $0x4,%ebp          five iterations again
    14051af78  lea 0x640(%rdi),%rdi

Each peer entry is zeroed from `+0x40` for `0x5f0` bytes and carries a
state int at `+0x80`, a word at `+0x630` and a flag at `+0x631`.

## The literals

### The admission budget

`FUN_14051dbb0` decides how many more players may be let in:

    14051dbe3  mov $0x6,%esi
    14051dbeb  movsbl 0x301(%rdx),%eax     [[0x1416148f0+0x18]+0x301]
    14051dbf5  sub %eax,%esi               esi = 6 - option

then walks the five peer slots from `+0x5c0` to `+0x2500`, admitting up
to `esi` of them. The `6` here counts the host.

### The session's slot count

`FUN_14051f340` creates the session. Two sixes:

    14051f3bc  mov $0x6,%r8d          factory->vt[0x18](factory, &out, 6)
    14051f402  lea 0x6(%r9),%r8d      r9 = 0, so 6
    14051f406  mov $0x80000001,%edx   property "total slot count"

`FUN_140520450` does the same for the join path:

    1405204e2  mov $0x6,%r8d

### Steam

`SteamSessionLight` (the middleware, `..\..\source\matching\steam\SteamSessionLight.cpp`)
reads property `0x80000001` off the session and hands it straight to
Steam:

    140a72d2b  call FUN_140a475d0        property key
    140a72d30  cmp  $0x80000001,%eax     "total slot count"
    140a72d49  call FUN_140a472b0        property value
    140a72d64  mov  %ebp,%r8d            cMaxMembers
    140a72d67  mov  $0x2,%edx            k_ELobbyTypePublic
    140a72d72  call *0x68(%r9)           ISteamMatchmaking::CreateLobby

The log line beside it reads `プロパティ(合計スロット数) : %d` —
"property (total slot count): %d" — which is what named the key. The
vtable slot was checked against the bundled SDK header rather than
recalled: `0x68` is index 13, `CreateLobby`, and the neighbours agree —
`0x70` `JoinLobby`, `0x78` `LeaveLobby`, `0x88` `GetNumLobbyMembers`,
`0x90` `GetLobbyMemberByIndex`, `0x118` `GetLobbyOwner`.

The session property keys seen so far: `0x80000001` total slots,
`0x80000002`, `0x80000006`, `0x80000009`, `0x8000000c`.

### A second object

Around `14051fef0` a different object carries the same six:

    14051fef7  cmp $0x6,%rax           capacity of a vector of 0x40-byte elements
    14051fefd  mov $0x6,%edx           reserve(6)
    14051ff0a  lea 0xb8(%rsi),%rdi     fixed array, stride 0x48
    14051ff11  lea 0x268(%rsi),%rbp    0x268 - 0xb8 = 6 * 0x48

Not yet identified. It has to be inventoried the same way before
anything is written.

## What was done

Both allocations grow and both arrays move past the old end of their
object. Everything already in the object stays at the offset it has, the
multiplay-state pointer at `+0x5b8` included; only the arrays themselves
move.

| | before | after |
| --- | --- | --- |
| session control size | `0x2500` | `0x7500` |
| member array | `+0x1a8`, 5 x `0xd0` | `+0x2600`, 11 x `0xd0`, ends `0x2ef0` |
| peer array | `+0x5c0`, 5 x `0x640` | `+0x3000`, 11 x `0x640`, ends `0x74c0` |
| multiplay manager size | `0x2d0` | `0x620` |
| slot array | `+0xb8`, 6 x `0x48` | `+0x300`, 11 x `0x48`, ends `0x618` |

The new ranges deliberately do not overlap any old base or bound. A site
missed by the inventory therefore reads or writes the region the arrays
used to occupy, which is now dead padding inside a larger allocation:
the failure is a phantom that does not appear, not a corrupted heap.

141 sites, all in one module plus the two allocation sizes:

| count | what |
| --- | --- |
| 47 | member field, displacement moved by `+0x2458` |
| 16 | member array end |
| 18 | member index bound, `cmp $0x5` becomes `cmp $0xb` |
| 4 | member base reached as object + index * stride |
| 9 | peer field |
| 6 | peer array end |
| 4 | peer array span, `0x1f40` becomes `0x44c0` |
| 18 | slot field on the multiplay manager |
| 10 | slot array end |
| 3 | constructor counts |
| 4 | the plain sixes: the admission budget and three session slot counts |
| 2 | the two allocation sizes |

Every one rewrites a displacement or an immediate in place, so no
instruction changes length.

### How the sites were found

Not by hand. `Source/Injector/Tools/ds2_session_slots/` holds the two
scripts that produce `patches.json`, and the table header is generated
from it.

The hard part is telling an offset measured from the object from one
measured from an entry: both fall in the same range, because an entry is
smaller than the array. Straight-line taint on the disassembly is unsound
here, and quietly so, because the compiler leaves epilogues in the middle
of these functions and a linear walk applies their pops to code that is
only reached by a jump. That silently lost the object in
`FUN_14051ba70` and dropped 29 real sites.

What is stable instead is that the object is `this`: it arrives in RCX
and stays in one register for the whole function. So the classifier works
out which registers those are from the prologue, and every site it does
**not** claim was then read individually against a scalar scan of the
same ranges, to confirm it is an entry offset, a loop stride, or another
object. Four false positives came out of that review and are named in the
script: two reached a different global, one arrived through
`obj->0x88`, and one was the stride of an unrelated container.

### Applying it

`DS2ExpandSessionSlots` in `Injector.config`, off by default. It is
deliberately not folded into `DS2ForceMultiPlayZone`: that opens the
closed areas, this changes the shape of two game objects, and a stale
table here is worse than no multiplayer at all.

`DS2_SessionSlotsHook` verifies all 141 sites before writing any and
writes nothing if one disagrees, because a half-applied layout change
would leave the game reading its players out of two different places. If
a write fails partway it rolls back what it already did.

## What can and cannot be verified here

Two Steam accounts on one machine. Three or more players cannot be
tested at all, so **"it works with twelve" is not a result this setup can
produce, and nobody has produced it.** What can still be checked:

- the hook installs without reporting a mismatch, which proves all 141
  sites still hold the bytes the table was generated against
- the objects allocate at the new sizes, read live through the probe:
  `chain mgr 1616cf8 20` and the size at `0x140513e1c`
- slots 6 through 11 come out constructed. The peer constructor zeroes
  `+0x40..+0x630` and clears `+0x5a8`, so entry 6 at `+0x3000 + 6*0x640`
  should read like entry 1
- `CreateLobby` receives 12, by breaking at `0x140a72d64` and logging
  `ebp`, or by capturing the 合計スロット数 log line
- a two-player sign in Majula and in Heide still works, as the control.
  This is the one that matters: if the relocation is wrong anywhere on
  the path a single guest takes, it will fail here.

## Still unknown

- **Whether the protocol can carry more than six.** Nothing has checked
  the width of the player index in the messages the clients exchange. If
  it is three bits somewhere, twelve will not fit and no amount of room
  in the arrays will help.
- **The server.** DS3OS imposes no cap of its own - there is no
  max-player check anywhere in the DS2 server - but it has never been
  asked to broker a session larger than two.
- **What twelve phantoms do to the game.** Rendering, animation budget,
  the peer-to-peer traffic between twelve Steam clients, and the areas
  themselves, which were built for six.
