# The client's network API has names

`DarkSoulsII.exe` carries full RTTI for the multiplayer subsystem, with the
method signatures preserved inside the lambdas' namespaces. In practice the
binary documents itself — found on 12/09, after months of treating these
functions as `FUN_1402a14c0`.

This matters for whoever carries on the reverse engineering here: **before
tracing anything in multiplayer, look for the name**.

## How to read the names

The label Ghidra keeps is only `vftable`; the class name is in the symbol's
*namespace*, so you have to ask for the qualified name
(`Symbol.getName(true)`). Two scripts in `/home/suel/tools/scripts` do that:

```
analyzeHeadless ... -postScript LabelAt.java   <saida> 0x1410d61f8 ...
analyzeHeadless ... -postScript ClassList.java <saida> NetSvr
```

`ClassList.java` sweeps every symbol and prints the ones ending in
`::vftable`. With `NetSvr` there are 148, of which 84 are the game's own
classes and the rest is template plumbing.

The signatures show up because a job created inside a method becomes a local
class, and the namespace preserves the whole declaration:

    NetSvrBreakInInterface::GetBreakInTargetList(
        unsigned char, unsigned int, unsigned int, unsigned int,
        Frpg2Sv::MatchingParameter const&,
        Frpg2ClientLib::Frpg2Vector<Frpg2ClientLib::BreakInTargetData>*)
    NetSvrBreakInInterface::BreakIn(unsigned char, unsigned int, unsigned int, unsigned int)
    NetSvrSummonSignInterface::SummonSummonSign(unsigned int, Frpg2Sv::CellAddress const&, ...)

Types that turn up and are worth knowing: `SignHandle`,
`Frpg2Sv::CellAddress`, `Frpg2Sv::MatchingParameter`, `Frpg2Sv::SignInfo`,
`NetSvrJobResult`, `App_BloodMessageData`, `NetSvrBreakInSessionInfo`.

## The pattern: Manager, Interface, Job

Each subsystem shows up three times:

| layer | what it does | example |
| --- | --- | --- |
| `...Manager` | the facade the game calls | `NetSvrSummonSignManager` |
| `...Interface` | builds the protocol request | `NetSvrSummonSignInterface` |
| `...Job` | the task that runs and sends | `NetSvrSummonSignSummonJob` |
| `...PushNotifyBuffer` | receives the server's push | `NetSvrSummonSignPushNotifyBuffer` |

That is how the summon path became readable. The `FUN_1402a14c0` that the
breakpoint sweep found is **`NetSvrSummonSignManager::<invocar>`**: the `rcx`
captured at runtime (`0x7ffffe591000`) has the vftables
`0x1410d61f8`/`0x1410d6270`, which are the manager's, and `rdx` is a pointer
to a 32-bit `SignHandle`.

## The vftable addresses that matter here

    1410d61f8  NetSvrSummonSignManager
    1410d5db8  NetSvrSummonSignInterface
    1410d63a8  NetSvrSummonSignSummonJob
    1410d6380  NetSvrSummonSignPushNotifyBuffer
    1410d6028  NetSvrSummonSummonSignJob
    1410d5df8  NetSvrCreateSummonSignJob
    1410d5e68  NetSvrUpdateSummonSignJob
    1410d5ed8  NetSvrRemoveSummonSignJob
    1410d5f48  NetSvrRejectSummonSignJob
    1410d5fb8  NetSvrGetSummonSignListJob

    1410d2928  NetSvrBreakInManager
    1410d2680  NetSvrBreakInInterface
    1410d2728  NetSvrBreakInJob
    1410d2b18  NetSvrBreakInSummonJob
    1410d2af0  NetSvrBreakInPushNotifyBuffer
    1410d2798  NetSvrAllowBreakInJob
    1410d2808  NetSvrRejectBreakInJob
    1410d26b8  NetSvrGetBreakInTargetListJob

The full list of 84 comes out in seconds with `ClassList.java`; it is not
worth freezing it here, because the script is more reliable than a copy.

## The client's sign registry, and how to reach it

`NetSvrSummonSignManager` does not hold the signs; the game side does.
`FUN_1402a41b0` gets the registry like this:

```asm
1402a41c1:  call   0x1402128d0        ; devolve o registro
1402a41c6:  mov    (%r14),%r8d        ; o SignHandle
1402a41ce:  mov    %rax,%rcx
1402a41d6:  mov    (%rax),%r8         ; a vftable
1402a41dc:  call   *0x98(%r8)         ; registro->slot 0x98(handle) -> placa
```

And `FUN_1402128d0` is a pointer chain starting from the game's global,
`DAT_1416148f0`, verified in live memory:

| step | class (RTTI) | vftable |
| --- | --- | --- |
| `*(global) + 0x90` | `SignManager` | `0x1410cb668` |
| `... + 0x68` | `SignSetCtrlManager` | `0x1410cb4a0` |
| `... + 0x20` | `SummonSignSetCtrl` | `0x1410cb698` |
| `... + 0x28` | `SummonSignSetCtrl`, second base | `0x1410cb6e8` |

It is the second base that answers slot `+0x98`. It is `FUN_140213460`:

```c
void FUN_140213460(longlong this, undefined4 *handle)
{
    uint h = *handle;
    if (FUN_14020e6f0(*(this - 0x10), &h) == 0) {   // procura na coleção A
        FUN_14020e6f0(*(this - 8), &h);             // e depois na B
    }
}
```

**Two collections**, at `this-0x10` and `this-0x8`, with `FUN_14020e6f0` doing
the lookup by handle. The neighbouring slots `+0xa0` and `+0xa8` call the same
`FUN_14020f660(this - 0x28, x, 2|3, ...)` with different discriminators, which
smells like "by sign type".

### The container's format, and the prediction that closed the model

`FUN_14020e6f0(colecao, &handle)` is the lookup, and it reveals the interface:

```c
count = colecao->vftable[0x18]();                 // quantos
for (i = 0; i < count; i++) {
    item = colecao->vftable[0x10](colecao, i);    // o i-esimo
    if (item[5] < 0 && item[0] == *handle) return item;
}
```

The two collections are `TSignSet<SummonSignParam>`, and reading live memory
gives the rest:

| field | contents |
| --- | --- |
| `+0x14` | capacity (20 in the collection measured) |
| `+0x18` | **count** — went from 5 to 6 the instant a new sign arrived |
| `+0x30` | pointer to the storage |
| `+0x38` | count, again |

And each item takes **0x88 bytes**:

| field | contents |
| --- | --- |
| `+0x00` | the `SignHandle` |
| `+0x04`, `+0x08`, `+0x0c` | world x, y, z, as floats |
| `+0x14` | high bit set = valid item (that is the lookup's `item[5] < 0`) |

What closes the argument: with a freshly placed sign, item 5 of the storage
carried handle `0x80000055` and position (6.15, -18.5, 209.1) — the Heide
bonfire where it had been placed. **I predicted the summon would use
`0x80000055` and the breakpoint captured exactly that.** Summon signs carry
the tag `0x80000000`; the other collection has items with `0xc0000000`.

## With that, the rematch has a complete design

Nothing else is left to find out to write the host-side hook:

1. walk from the global to `SummonSignSetCtrl` (four pointers, above);
2. walk the collection with the `count`/`at` interface, taking the item whose
   position matches the previous duel's — or, more simply, the only item with
   the `0x80000000` tag when there is a single pair on the server;
3. call `FUN_1402a14c0(NetSvrSummonSignManager*, &handle)`.

The manager pointer is stable and has already been captured (`0x7ffffe591000`
in two different sessions, including after closing the game), but the hook
should resolve it through the chain itself instead of hardcoding the address.

What has not been read yet is **which field of the item identifies the owner**
of the sign. For two players that is not needed; for more than two, it is.
