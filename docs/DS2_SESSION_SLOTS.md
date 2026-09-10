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

The peer array's constructor counts the slots down:

    14051aeb0  FUN_14051aeb0(array_base)
    14051aecd  mov $0x4,%ebp        five iterations: 4,3,2,1,0
    14051af78  lea 0x640(%rdi),%rdi

Each entry is zeroed from `+0x40` for `0x5f0` bytes and carries a state
int at `+0x80`, a word at `+0x630` and a flag at `+0x631`.

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

## What raising it to twelve would take

Both arrays are inline, and one of them sits in the middle of the
object, so growing them in place would move every field after `+0x5b8`.
The way that avoids moving anything is to leave `[0, 0x2500)` exactly as
it is and put both arrays past the old end:

| | now | after |
| --- | --- | --- |
| allocation | `0x2500` | `0x7300` |
| member array | `+0x1a8`, 5 × `0xd0` | `+0x2500`, 11 × `0xd0`, ends `0x2df0` |
| peer array | `+0x5c0`, 5 × `0x640` | `+0x2e00`, 11 × `0x640`, ends `0x7280` |
| `+0x5b8` | pointer | untouched |

The old array regions become dead padding. Field offsets shift by a
single constant per array, which keeps the patch mechanical.

Then: the constructor counts (`mov $0x4,%ebp` and the member array's
equivalent), every end bound — written both as `+0x2500` on the object
and as `lea 0x1f40(base)` on the array — every `cmp $0x5` index bound,
the `6 -` budget, the three session slot counts, and the second object
above.

### The question that decides whether this is possible

Everything mapped here is the **netcode** peer table. A seventh player
also has to exist in the world, as a character the game draws and
simulates, and the world manager has its own storage. Nobody has looked
at it yet.

If the world's player slots are another fixed inline array in a large
object, that is a second relocation project of unknown size. If they are
allocated per phantom, it is free. **This is unresolved, and it is the
item that decides whether twelve players is a patch or a rewrite.**

## What can and cannot be verified here

Two Steam accounts on one machine. Three or more players cannot be
tested at all, so "it works with twelve" is not a result this setup can
produce. What a patch could still be checked against:

- the object allocates at the new size, read live through the probe
- slots 6 through 11 come out constructed — the constructor zeroes
  `+0x40..+0x630` and clears `+0x5a8`, so entry 6 should read like
  entry 1
- `CreateLobby` receives 12, by breaking at `0x140a72d64` and logging
  `ebp`, or by capturing the 合計スロット数 log line
- a two-player sign in Majula and in Heide still works, as the control

Any patch ships behind its own config flag, off by default, with an
expected-bytes check per site, the way `DS2_UnblockMultiPlayHook` does.
