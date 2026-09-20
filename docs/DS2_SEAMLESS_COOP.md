# Seamless co-op: the warp, and what it already solves

The goal: two players crossing the game together from end to end, and a
death — of either of the two — sending the player to the **last bonfire**
instead of undoing everything and sending him back to his own world.

This document started on 12/09 as the list of what was known; today it
records the whole path of the warp, because that is what opened up. For the
session end chain, see [DS2_SESSION_END_CLIENT.md](DS2_SESSION_END_CLIENT.md);
for the red sign rematch, [DS2_REMATCH_AFTER_DEATH.md](DS2_REMATCH_AFTER_DEATH.md).

## The three things a phantom's death does

Confusing them cost an all-nighter:

1. **Ending the sessions.** A table at `0x1410c0050`, twenty 16-byte entries
   indexed by a role read from `objeto+0xe0`, and the `+1` byte of each entry
   decides whether that role tears the sessions down on death. The phantom is
   **index 7**; its byte lives at `0x1410c00c1`.
2. **Demolishing the session.** `FUN_1402c3900`, the handler for state 8 of
   the session state machine. It sends `RequestNotifyLeaveSession` and puts
   the state at 9.
3. **Sending the player home.** A **warp** — and this is the one that gives
   co-op.

Measured: zeroing the table byte for index 7 does **not** stop the return.
The phantom sees "You have been vanquished. Returning to your world…" all the
same; the only thing that changes is whether the sessions are torn down with
it. Suppressing the table would leave the player going home with the session
left hanging — worse than the original. The warp is the piece.

## Every warp in the game goes through a single function

`0x1401c2a80` (module `+0x1c2a80`), reached through virtual slot `+0x40` of
the game's global context, `*(0x1416148f0)`:

```
mov  rcx, [0x1416148f0]
lea  rdx, <pedido>
xor  r8d, r8d
mov  rax, [rcx]
call [rax+0x40]
```

Signature: `void Warp(void* Contexto, WarpRequest* Pedido, uint8_t Flag)`.

The first thing it does is read the **second dword of the request**, at
`0x1401c2ab6`:

```
mov  ecx, [rdx+4]
cmp  ecx, 1        ; 1 passa direto
je   aceita
cmp  ecx, 4
jne  portao
test r8b, r8b      ; 4 passa se o terceiro argumento for zero
je   aceita
portao:
call 0x140248940   ; qualquer outro motivo precisa da permissão daqui
test al, al
jne  recusa
```

After that it copies the request's `0x38` bytes to `contexto+0x24c8` and sets
bits in `contexto+0x24b1` and `+0x24b2`. The warp is **queued**, not executed
there — which explains why a `bp` at the end of the chain never showed the
destination.

## The request, 0x38 bytes

The game has a constructor for it, `FUN_14044ed40(registro, saida)`, and that
is what gave the names:

| field | what writes it | what it is |
| --- | --- | --- |
| `+0x00` | `3`, `4` or `2` depending on `registro+0x168` | destination family |
| `+0x04` | always `1` in the constructor | **reason** — this is what the entry point reads |
| `+0x08` | `registro+0x164` | map id (`0x0a1f0000` = Heide's Tower of Flame) |
| `+0x0c` | left at `-1` from initialisation | — |
| `+0x10` | left at `0` | — |
| `+0x14` | byte `3` | — |
| `+0x18` | `registro+0x16c` | spawn point inside the map |
| `+0x1c`…`+0x34` | only the copy path | — |

When the constructor does not recognise the record it copies the whole `0x38`
bytes of the default destination, which comes from `FUN_14039a9a0`.

### The table of reasons, and the trap it hides

What has been measured so far, each row coming from a request captured at
runtime:

| reason | third argument | who asks | what it is |
| --- | --- | --- | --- |
| `1` | `0` | `FUN_140190920` → `FUN_14044fde0` | ordinary death: the last bonfire |
| `4` | `0` | `FUN_1402c3900`, `0x1402c3bdb` | **forced return to your own world** |
| `4` | `1` | `0x1402c2e45` | the guest's entry **into the host's world** |

**Reason 4 on its own does not mean "home".** The first version of the hook
swapped every reason 4 and broke summoning: the guest saw "Summoning
canceled.", the host saw "Summoning failed. The sign has disappeared.", and
the session was never born. Both things go through the same door with the
same reason.

What separates them is the **third argument** — and that is not a guess, it
is what the entry point itself tests: reason 4 only skips the permission gate
when the argument is zero. The teardown sends `xor r8d,r8d` (`0x1402c3bd8`);
the entry into the host's world sends `mov r8b,1` (`0x1402c2e42`). The hook
tests exactly that.

Any other reason has to go through `0x140248940`; no other has been seen yet.
`DS2_Seamless.log` writes one line per warp, with the reason, the argument and
the return address of whoever asked, and that is how this table grows.

### The two requests, byte by byte

An ordinary death (Samuel fell into the water in Heide's Tower of Flame),
captured with `bp 1c2a80 deref rdx 56`:

    +0x00  03 00 00 00   família 3
    +0x04  01 00 00 00   motivo 1 - última fogueira
    +0x08  00 00 1f 0a   mapa 0x0a1f0000, Heide's Tower of Flame
    +0x0c  ff ff ff ff
    +0x10  00 00 00 00
    +0x14  03 eb b0 c1   o byte 3; o resto é lixo de pilha
    +0x18  a7 7b 00 00   ponto 0x7ba7
    +0x1c  48 6e 3d 41   lixo de pilha daqui para baixo

A guest's forced return, captured with `bp 2c3bdb deref rdx 32` on a phantom
death in the host's world:

    +0x00  03 00 00 00
    +0x04  04 00 00 00   motivo 4 - de volta ao próprio mundo
    +0x08  00 00 1f 0a   mesmo mapa: o duelo foi ali
    +0x0c  ff ff ff ff
    +0x10  00 00 00 00
    +0x14  03 7f 00 00
    +0x18  a7 7b 00 00

Two measurements of the same ordinary death, from different positions,
changed only `+0x15..+0x17` and `+0x1c` — which is exactly what the
constructor does **not** write. That is the proof that the rest of the struct
is stack garbage and not fields.

## The respawn record hangs off the same context

What makes the substitution cheap:

```c
void FUN_140190920(longlong param_1)
{
  FUN_14044fde0(*(undefined8 *)(DAT_1416148f0 + 0x70));
  *(undefined1 *)(param_1 + 0xce) = 1;
}
```

`FUN_14044fde0` is "send the player where he last rested": it builds the
request through the constructor and calls the warp. Its only argument is the
respawn record, and it lives at `*(contexto + 0x70)` — the same context that
arrives as the warp's first argument. Which means **from inside the hook you
can ask for the game's own respawn**, with no struct built by hand.

## The hook

`Source/Injector/Hooks/DarkSouls2/DS2_SeamlessCoopHook.{h,cpp}`, turned on by
`DS2SeamlessCoop` in `Injector.config` (`ds2os-dev up --seamless`, or
`game prepare --seamless`). A single detour, at `+0x1c2a80`, with the bytes at
`+0x1c2a80` and `+0x44fde0` checked before anything is written:

```
se o pedido tem motivo 4 e o terceiro argumento é 0:
    FUN_14044fde0(*(contexto + 0x70))   // última fogueira, do jeito do jogo
senão:
    passa adiante
```

There is a reentrancy lock because the replacement warp goes through the same
entry point. Writing `0` to `DS2_Seamless.req` turns the swap off and leaves
only the logging; anything else turns it back on. The log sits in
`DS2_Seamless.log`, next to the DLL, and one line comes out per warp with the
reason, the map, the point and the return address of whoever asked — the
`de=+0x...` is what identifies the path, and the `cru=` carries the whole
`0x38` bytes, because half of them still have no name and a line that prints
only the named half cannot answer a question nobody has asked yet.

### How to turn it on, and how to play a co-op today

```
ds2os-dev up --auto-rematch --seamless
```

`--seamless` turns this hook on; `--auto-rematch` turns on the
[DS2_RematchHook](DS2_REMATCH_AFTER_DEATH.md), and the two together are the
loop that exists today:

1. the guest uses the **Small White Sign Soapstone** or the
   **White Sign Soapstone** (inventory → consumables category → 7 down,
   2 right → Use; it works while hollow);
2. the host summons **on its own**, with nothing pressed, as soon as the sign
   arrives — that is the rematch hook, which does not look at the sign's
   type;
3. they play together;
4. whoever dies goes to the last bonfire and the session ends;
5. the guest puts the sign down again, and step 2 repeats.

Step 5 is the only one that still needs a hand, and it is the next obvious
piece.

### Where step 5 begins

The sign's path is already located, by a breakpoint on a real white sign
placement. `FindImmediate.java 0x394` (the id of `RequestCreateSign`) gives
five candidates; placing the sign lit **two**:

    alcancado +0x6a1de0 de=+0x29ec4a ...
    alcancado +0x6a1170 de=+0x284f52 ...

and the server logged `Sign 1000 created: type 1` right after. The
interesting part is the caller of the first one:

```c
void FUN_14029ec00(longlong trabalho, longlong *dono, undefined8 p3)
{
  interface = (**(code **)(*dono + 0xe8))(dono);
  (**(code **)(*interface + 8))
      (interface, p3, trabalho[0x28], trabalho[0x2c], trabalho+0x30,
       trabalho[0x5c], trabalho+0x60);
}
```

It is the same Manager / Interface / Job design as the rest of the subsystem
([DS2_CLIENT_NETSVR_API.md](DS2_CLIENT_NETSVR_API.md)): the **job** in
`param_1` already carries everything the sign needs, and what builds it is
further up, at `+0x286239` in the captured stack. Putting the sign back down
by itself means finding that constructor and calling it — the same move the
`DS2_RematchHook` already makes on the host's side.

## The result that turns the brief inside out

With the hook installed and the redirection turned on, a phantom died in the
host's world. The guest's log, top to bottom:

    warp motivo=4 forca=1 tipo=0 mapa=0a1f0000 ponto=c26abe77 de=+0x2c2e48   <- entrada, passou
    warp motivo=4 forca=0 tipo=3 mapa=0a1f0000 ponto=00007ba7 de=+0x2c3bde   <- volta forçada
    co-op: em vez de voltar para o proprio mundo, ultima fogueira
    (reentrada) motivo=1 forca=0 tipo=3 mapa=0a1f0000 ponto=00007ba7 de=+0x44fe22

It worked: only the forced return was swapped, the entry went through intact
and the session was born (`RequestNotifyJoinGuestPlayer` followed by
`RequestNotifyJoinSession`). But **the two requests point to the same place**
— `tipo=3`, same map, same point `0x7ba7`. And `0x7ba7` is the point of the
guest's last bonfire: that is measured separately, on an ordinary death of
his in his own world, which produced
`motivo=1 tipo=3 mapa=0a1f0000 ponto=00007ba7`.

In other words: for a **red sign invader**, Dark Souls II already sends
whoever dies to the last bonfire. The brief "instead of going back to the
world, go back to the last bonfire" is already the stock behaviour in that
case — and for a while that looked like the end of the story. It is not:
**it depends on who died.**

The code confirms it without depending on the measurement. `FUN_1402c3900`
builds **two** destinations and picks one:

```c
cVar3 = FUN_1402d47a0(sessao+0xd8 /* papel */, sessao+0x1cc /* por que acabou */);
local_d4 = 4;                       // motivo, sempre
if (cVar3 == 1) {                   // forma fogueira
    local_d0 = sessao+0x1b8;        // mapa   ) tirados do registro de
    local_c0 = sessao+0x1c0;        // ponto  ) renascimento do convidado
    local_d8 = 3;                   // tipo
} else if (cVar3 == 0) {            // forma posição
    local_d8 = 0;
    local_c0 = sessao+0x1a4;        // x, y, z de onde ele estava quando entrou
}
```

Both snapshots are taken **on entry**, in the state 2 handler: the respawn
record (`*(contexto+0xe)` → `+0x164/+0x168/+0x16c`) and the player's position
in his own world.

And what decides is a **param row**, not code:

```c
undefined1 FUN_1402d47a0(papel, porQueAcabou)
{
  linha = FUN_14016f540(papel);          // *(contexto+0x18) é o gerenciador de params
  if (linha) {
    if (porQueAcabou == 1) return linha[0x2c];
    if (porQueAcabou == 2) return linha[0x2e];
    if (porQueAcabou == 3) return linha[0x2d];
    if (porQueAcabou == 4) return 2;
  }
  return 1;                               // o padrão é a fogueira
}
```

Three bytes per role — `+0x2c`, `+0x2d`, `+0x2e` — say, for each session end
reason, whether the player goes back to the bonfire or to where he was. It is
a data switch, and touching it needs no detour at all.

### Co-op picks the other form, and there the hook makes a difference

The same measurement, done again with a **white co-op phantom** — white sign,
`Sign ... type 1`, summoned on its own by the rematch — gives the opposite.

With the redirection **off**:

    warp motivo=4 forca=0 tipo=0 mapa=0a1f0000 ponto=40c5efa4 de=+0x2c3bde
    cru=00000000 04000000 00001f0a ffffffff 00000000 03000000
        a4efc540 002294c1 9a0d5143 0000803f ...

`tipo=0` is the **position form**, and the three floats are
`6.1855, -18.516, 209.05` — exactly where the guest was in his own world when
he was summoned, the same the server had logged (`position 6.2 -18.5 209.1`).

With the redirection **on**, the same death:

    warp motivo=4 forca=0 tipo=0 mapa=0a1f0000 ponto=40c5efa4 de=+0x2c3bde
    co-op: em vez de voltar para o proprio mundo, ultima fogueira
    (reentrada) motivo=1 forca=0 tipo=3 mapa=0a1f0000 ponto=00007ba7 de=+0x44fe22

The position form became the bonfire form. **For co-op, which is the case in
the brief, the hook really does change the destination.** That is why it
stays on; for the red invader it remains a no-op, which is correct.

A summary of what the three param bytes decide, measured:

| who dies | form the game picks | does the hook change it? |
| --- | --- | --- |
| red sign invader | bonfire (`tipo 3`, point from the record) | no, it already was |
| white co-op phantom | position (`tipo 0`, where he was) | **yes** |

### A host who dies takes the guest down the same path

The third measurement, the one that was missing: the **host** died with the
co-op phantom inside. The two logs, side by side:

    host     warp motivo=1 forca=0 tipo=3 ponto=00007ba7 de=+0x44fe22
    convidado warp motivo=4 forca=0 tipo=0 ponto=40c5efa4 de=+0x2c3bde
              co-op: em vez de voltar para o proprio mundo, ultima fogueira
              (reentrada) motivo=1 forca=0 tipo=3 ponto=00007ba7 de=+0x44fe22

The host has an ordinary death and goes to his own bonfire — the hook does
not even touch it, because the reason is 1. And the guest goes down
**exactly the same path** as when it is he who dies: same function, same
reason, same position form, same swap. Which means one hook covers both cases
in the brief, "if the phantom or the host dies", with no code specific to
either.

**But what a death costs a co-op is still not the place you arrive at. It is
the session** — and that still ends.

## What this does **not** do

Being honest here matters more than the feature:

- **The session still ends.** `RequestNotifyLeaveSession` goes out before the
  warp, and the demolition in `FUN_1402c3900` is not touched.
- For a red invader **the destination does not change**: the game already
  picks the bonfire on its own. For the co-op phantom it does change, and
  that is the case in the brief.
- So this is still not the seamless co-op in the brief. To "finish the game
  end to end together" two things are missing, and only one of them is our
  code:
  1. the session surviving a death, which the game never does — a phantom
     never loads any area inside the host's world;
  2. or, accepting that it ends, the pair finding each other again on their
     own — which is exactly what the red sign rematch already does.
- With two players on one machine you cannot test three; see
  [DS2_TO_VALIDATE.md](DS2_TO_VALIDATE.md).

## The session is a state machine, and the warp is what it uses to move people

The session object keeps its state at `objeto+0xf8`, and each state has its
own handler. Two matter:

| state | handler | what it does |
| --- | --- | --- |
| `2` | `FUN_1402c2a80` | takes the guest **into** the host's world |
| `8` | `FUN_1402c3900` | tears the session down and sends the guest home |

The state 2 handler builds the request like this — and this is the template
for "put the player **here**, on this map":

    +0x00  0            tipo 0
    +0x04  4            motivo
    +0x08  mapa         vem do pedido que chegou pela rede
    +0x14  byte         de FUN_1402d4830
    +0x18  x, y, z      posição
    +0x24  1.0f
    +0x28  ...          orientação, normalizada ali mesmo

and calls the warp with third argument **1**. The return value matters: if
the warp refuses, the handler throws the machine to state `0x13`
(`0x1402c2ee6`). Which means **the warp returns a byte**, and a detour
declared `void` hands the caller whatever was left in `al` — that was a real
bug in this hook, since fixed.

### State 7 only leaves through one field

And the teardown does not start on its own. The state 7 handler —
`FUN_1402c3830`, the state the session sits in while you play — has just one
line that matters:

```c
if (*(int *)(sessao + 0x1cc) != 0) {
    sessao[0x1f] = 8;          // +0xf8, o estado: vai para o desmonte
}
```

`+0x1cc` is the **reason for the end**, and what writes it is
`FUN_1402c2f20`, the session's **`+0xa0`** slot — "end it, and this is the
reason". (This paragraph said `+0x30` until 13/09; slot `+0x30` is
`FUN_1402c2820`, and the call measured at `de=+0x2c9246` is
`call *0xa0(%rax)`. The hook always worked because it detours the function,
not the slot.) It checks a `+0xa8` virtual first, stores
`FUN_1402d4750(papel, motivo)` at `+0x1c8` and tells the rest of the game.

In other words: **keeping `+0x1cc` at zero keeps the session in state 7**,
and refusing that function is enough for it — that is not a guess, it is the
only path from 7 to 8. That is what `DS2_SeamlessSessionHook` rests on.

This is also the design of the next step. If on a guest's death the machine
were taken back to state 2 with a new destination, instead of to state 8, the
guest would respawn **inside the host's world**. It is not tested, and there
are bills the game may have to settle (the host has to agree, the phantom's
body has to disappear, the bloodstain ends up somewhere). But it is the first
hypothesis with an address.

## The sites that call the warp do not come out of Ghidra

`Xrefs.java` on `0x1416148f0` returns 40 reads and **none** of them is one of
the three in the table above. The list is not a bad sample, it is incomplete:
the known sites (`0x1402c3bca`, `0x1402c2e2f`, `0x14044fe0d`) simply do not
appear. It is the same wall the static graph put up on the invasion sends,
and the same conclusion holds: here the honest inventory is the runtime log,
with each warp's `de=+0x...`, and not the analysis.

## The session can survive a death

Measured on 12/09, and it is the result that changes the rest of the plan.
The whole teardown path hangs off a single request, `FUN_1402c2f20`, and
refusing it **on the side of whoever died** is enough:

- the state never leaves 7, because `+0x1cc` never becomes anything but zero;
- no warp is issued — the guest's warp log stays empty;
- the host does not take part: it asks for no session end at all when the
  guest dies, so there is no need to touch its client;
- both HUDs keep listing the other player.

The guest stays **dead where he fell**. Refusing the end prevents the
teardown, it does not make him respawn — that is the next piece.

## What is still not known

- What reasons exist besides 1 and 4, and what the gate at `0x140248940`
  charges.
- What `+0x00` (family 3/4/2) and `+0x14` mean.
- Whether a guest's bonfire is reachable from the host's world — that is,
  whether the map and the point the record holds are still his while he is a
  phantom. It is the first thing the log answers.
- Whether a host who dies with a phantom inside issues reason 4 to anyone,
  and by which path.

## Five attempts to make the guest respawn inside the session

Refusing the session end kept the guest inside it — dead. Making him respawn
**in the host's world** took five measurements, and the first four failed the
same way underneath.

| # | what was done | what happened |
| --- | --- | --- |
| 1 | warp reason 4, flag 1, gate as it was | refused by the gate |
| 2 | warp reason 4, flag 0 | accepted, no end request, guest alive — **sent home**, map and position ignored |
| 3 | flag 1 on the **host's** death, gate at 811 | accepted, and the **host froze** |
| 4 | flag 1 on the guest's death, gate raised by hand | accepted, guest alive and on his feet, session at 7 on both sides, host still listing him — **in his own world** |
| 5 | session state put back to 1 | no warp, no session end, guest dead in the water; the machine walked on its own to **state 2** and stopped |

The fifth is the one that explains the others. With `bp 2c3630 deref rcx+f8 8`
the dispatcher showed `[rcx+f8]=02`: the state 1 handler ran, saw the link to
the peer standing, wrote 2 and left. And **state 2 is not in the dispatcher's
switch** — nor is 3. They only run when a message arrives.

### The state 2 handler answers the first four at once

`FUN_1402c2a80`, the session's `+0x28` slot. Decompiled, it:

```c
if (sessao[0x1f] != 2)                    { sessao[0x1f] = 0xb; ... return; }
if (*(char*)(sessao[0x21] + 8) != 0)      { slot30(sessao, 0x13);     return; }  /* slot30 = FUN_1402c2820, não o EndSession */
if (!FUN_1402c6570(..., papel))           { sessao[0x1f] = 0xb; ... return; }

sessao+0x19c = param_2[0];                // mapa
sessao+0x198 = param_2[7];

/* o bloco "de onde ele veio": lido do jogador VIVO e dos contadores VIVOS */
sessao+0x1a0 .. +0x1c8 = posição atual do jogador, ctx+0x70 +0x164/168/16c,
                         ctx+0xd0 +0x168   /* o portão — ctx+0xd0 é o personagem local */

pedido = { tipo 0, motivo 4, param_2[0], -1, 0, FUN_1402d4830(papel),
           param_2[1..3], 1.0f, quaternion de param_2[5] };
if (!ctx->slot40(ctx, &pedido, 1))        { slot30(sessao, 0x13);     return; }

FUN_1402bbf20(sessao);
FUN_140500fd0(ctx+0x22e0);
FUN_1402900b0(sessao+0x110, papel, FUN_14028f320(...));   // avisa o par
sessao[0x1f] = 3;
sessao+0x1c9 = param_2[8];
```

Three conclusions, and each one kills an attempt:

- **The warp was never the missing half.** The handler builds exactly the same
  request that attempts 1–4 built by hand, and calls the same slot with the
  same flag 1 and the same gate. What decides which world the player lands in
  is what comes **around** it: `FUN_1402bbf20`, the message to the peer,
  state 3.
- **`sessao+0x1a4` is not the host's position.** It is written *by this
  handler*, from the player object, as a record of where the guest was before
  being summoned. Attempt 4 handed that record to the warp as the destination
  — and the guest went back to his own world, exactly as measured.
- **The destination is not in the session.** It arrives in `param_2`, from the
  host, over the network. Only three fields are kept afterwards: `[0]` in
  `+0x19c`, `[7]` in `+0x198`, `[8]` in `+0x1c9`.

### The payload, field by field

| field | use |
| --- | --- |
| `[0]` | map; also copied to `+0x19c` |
| `[1] [2] [3]` | the destination handed to the warp |
| `[4]` | never read |
| `[5]` | rotation; becomes the quaternion via cos/sin |
| `[6]` | read as a short, goes to `FUN_14051c6a0` |
| `[7]` | also copied to `+0x198` |
| `[8]` | read as a byte, also copied to `+0x1c9` |

### The sixth attempt: replay the invitation

If what is missing comes from the host and nothing else, then there is nothing
to synthesise: the invitation is **copied in passing** from the real join and
replayed on death. The replayed destination is the summon point — a place the
host walked to on purpose, a guarantee no invented position would have. Where
the guest died is no good: he may have died in the water, and that is what
happened in the attempt 5 test.

Two traps the code avoids because decompiling showed them first:

- the byte at `*(sessao+0x108) +8` is the handler's first test, and if it is
  not zero he **does not arrive** — it goes straight to `EndSession(0x13)`,
  taking the staging with it and saying nothing. It is read on a real
  invitation and checked before any replay;
- the `+0x1a0..+0x1c8` block is rebuilt from what is true **now**, and one of
  those values is the gate, which is 0 on the guest's death. Replaying
  carelessly would swap the original invitation's record — where the way back
  home comes from — for garbage. It is saved and put back.

Only one thing is left to measure: **state 3 is not in the switch either**, so
the arrival sends the message to the peer and waits for an answer. Whether a
host answers a guest it already counts as inside, nobody knows. The state
trail after the arrival answers that in a single test — `3→5→6→7` is the join
closing, `3` stuck is the host ignoring, and `3→4` followed by `0xf` is the
join blowing the case 4 timer.

### The arrival guard

The sixth attempt never happened, and the reason is a line that only turned up
because the handler was decompiled before being called:

```c
if (*(char*)(*(sessao+0x108) + 8) != 0) { EndSession(0x13); return; }
```

It is the **first** test in `FUN_1402c2a80`, before anything else — and the
same test opens `FUN_1402c37a0`, state 0, which is where a join begins. So it
is not "this arrival is not allowed": it is **"you are in no condition to
enter anywhere"**.

Measured with the whole staging standing — invitation, summon, live session,
guest killed on purpose in the water:

```
chegada: mapa=0a1f0000 destino=6.09,-18.50,209.16 giro=2.548 [6]=0101 [7]=1 [8]=01
guarda da chegada: antes=00 depois=00
morte de fantasma: motivo=2 papel=1 -> reentrando pelo estado 1
guarda da chegada = 01; a chegada seria recusada. Nao repetindo.
```

The copied destination matches where the two of them were (6.19, −18.52,
209.05), and it also matches the warp the game itself emitted on the join —
`ponto=40c30000` is 6.09375, the same X. The payload format is right.

**The guard is 0 on a real invitation and 1 at the moment of death.** And it
does not open on its own: read live minutes later, at the address the
dispatcher itself hands over (`bp 2c3630 deref rcx+108`), it was still `01`,
with the session stuck at state 2. Replaying the arrival there would have
fallen straight into `EndSession(0x13)`, taking the staging with it and saying
nothing about why — it was the check that ended up with the finding instead of
with nothing.

That kills the sixth attempt in the form it was conceived, and points at the
seventh: instead of holding the dead guest where he fell, **let the death run
as the game wrote it** — he gets up at his own bonfire, alive — and only then
pull him back. The session survives the trip because the teardown is refused
separately, which is the one thing already proved about this whole path: state
7 is still there when he gets up.

### The seventh attempt, and why it closes the door

Measured on 12/09 with the full staging — sign 1000, summon confirmed
(`RequestNotifyJoinGuestPlayer` + `RequestNotifyJoinSession`), invitation
copied (`destino=6.19,-18.50,209.03`), guest killed on purpose in the water:

```
morte de fantasma: motivo=2 papel=1 -> morte normal, e depois puxar de volta
fim de sessao RECUSADO sessao=... papel=1 estado=7 motivo=2 de=+0x2c9246
esperando: estado=7 guarda=01 apos 1200 quadros
...
desisti de puxar de volta: estado=7 guarda=01
```

The session **survived** — state 7 the whole time, as M1 promises. But the
guest **never got up**: he stayed dead underwater, with the host's bar still
on the HUD, and the arrival guard at `01` for 7200 frames straight.

This is the conclusion that settles the matter: **the respawn is downstream of
the teardown.** Letting the death run is not enough, because a phantom's death
does not get him up — what gets him up is the session teardown, which M1
refuses on purpose. The two things M2 needs, keeping the session and putting
the player on his feet, are tied to the same request, and it is **a single one
and is not repeated**.

What is left, and it is the design of the eighth attempt: **get the player up
ourselves**. `FUN_14044fde0(*(ctx+0x70))` is the "send him to the last
bonfire" that `DS2_SeamlessCoopHook` already knows how to call. With the
session held at state 7 and the player on his feet on his own, the guard has a
chance to open — and then the invitation replay, which is already built and
tested up to the door, runs.

### Unsticking a session without killing the client

The end request is **a single one**: refused once, it is never made again.
Lifting the block afterwards does not help, and the guest is stuck between
states. Killing the client fixes it and **costs dearly** (see CLAUDE.md on
illegal disconnects).

The cheap way uses the model already mapped — state 7 leaves when
`sessao+0x1cc` stops being zero:

```
bp 2c3630 deref rcx+f8 8          # o despachante entrega o ponteiro da sessão
pokeabs fim <sessao+0x1cc> 02000000 00000000
```

Measured: the guest went from the bottom of the water to his own bonfire,
alive, in seconds. It is the end-to-end confirmation that `+0x1cc` is the
single trigger from 7 to 8.

### The eighth: the guard opens when the player gets up

The seventh showed that nobody gets up a phantom whose session does not tear
down. The eighth gets him up — `FUN_14044fde0(*(ctx+0x70))`, ninety frames
after the death, so as not to run over what the death still has to do.
Measured on 12/09, with the full staging:

```
morte de fantasma: motivo=2 papel=1 -> morte normal, e depois levantar e puxar de volta
fim de sessao RECUSADO  papel=1 estado=7 motivo=2
levantando na ultima fogueira apos 90 quadros
warp motivo=1 forca=0 tipo=3 mapa=0a1f0000 de=+0x44fe22 / warp aceito=1
de pe de novo apos 421 quadros, guarda=00; recomecando o join
estado 2; repetindo o convite do host: mapa=0a1f0000 destino=6.09,-18.50,209.16
depois da chegada: estado=11
```

Line by line, everything the seven previous attempts tried to guess:

- **the respawn warp is accepted** even with the session standing at state 7;
- **the arrival guard opens** — from `01` to `00` — as soon as the player is
  on his feet. It is not "this arrival is not allowed", it is "you are in no
  condition", and the condition is being alive somewhere;
- the session goes back to state 1, walks on its own to 2, and the invitation
  replay **runs**;
- the arrival handler goes in, and comes out through **0xb**.

### What 0xb means

It is the "not allowed" exit from `FUN_1402c6570`, which asks two questions:

```c
FUN_14014ed40(modo, papel)   // uma tabela em 0x141568810, índice papel + modo*0x14
FUN_1402ca190()              // entre outras coisas: *(ctx+0x70 + 0x1b0) == 0
```

The second is the suspect. If `+0x1b0` means "this player is still settling",
getting him up by hand is exactly what would leave it lit — and the replay was
issued on the first frame the guard opened, which is the worst possible moment
to ask.

It is a pure, cheap question, so the replay now **waits for the answer to be
yes** instead of spending the single attempt finding out. The log says what
the answer was every time it changes.

The guest ended up alive at his own bonfire, out of the session, with nothing
locked up — the cleanest failure this path has had, and the first in which the
game refused for a reason that has a name.

### What each attempt costs

Measured on 12/09, and it is a practical limit on the whole path: **the
experiment cuts the guest off.** Chico placed sign 1001 without trouble at
19:26, the eighth attempt's test ran at 19:31, and by 19:40 he could not place
a sign at all and had even stopped asking the server for the list of them.
Nothing else happened in between.

From the game's side that is fair — a guest who refuses the teardown and then
leaves *is* an illegal disconnect, and the game counts it. The warning appears
once, only on the client's screen, and none of it reaches any log:

> Due to repeated illegal multiplayer disconnects, your connection to other
> worlds was lost. Only a Bone of Order can restore your connection.

A cut-off player cannot place a sign, cannot use the orb and **cannot
summon** — so swapping the roles is no way around it. The item cure does not
scale: there are few Bones of Order in a playthrough and both characters spent
theirs.

The cure that scales is the save. The private server keeps its own
(`EnableSeperateSaveFiles`), one file per account:

```
<prefixo>/drive_c/users/steamuser/AppData/Roaming/DarkSoulsII/<steamid>/DS2SOFS0000.ds3os
```

Copying before the test and putting it back afterwards takes seconds and makes
the cut-off irrelevant. With the game stopped — a live client rewrites the
file on the way out.

### The predicate was transient, and the arrival works

With the replay held until `FUN_1402c6570` answers yes, measured on 12/09:

```
morte de fantasma: motivo=2 papel=1 -> morte normal, e depois levantar e puxar de volta
fim de sessao RECUSADO  papel=1 estado=7 motivo=2
levantando na ultima fogueira apos 90 quadros
warp motivo=1 ... de=+0x44fe22 / warp aceito=1
de pe de novo apos 421 quadros, guarda=00; recomecando o join
pode entrar = 0 apos 0 quadros (papel=1 assentando=0)
pode entrar = 1 apos 96 quadros (papel=1 assentando=0)
estado 2 apos 96 quadros, pode entrar; repetindo o convite: destino=6.16,-18.50,209.16
depois da chegada: estado=3
warp motivo=4 forca=1 tipo=0 mapa=0a1f0000 ponto=40c50000 de=+0x2c2e48 / warp aceito=1
```

**The predicate was transient**: no on frame zero, yes 96 frames later — and
`assentando` was 0 in both readings, so what was refusing was one of the other
halves of `FUN_1402ca190`, probably the pair of virtuals `+0x48`/`+0x50` that
ask whether the game is loading. The eighth attempt had fired the replay on
the first possible frame, which was the only wrong one.

With the wait, **the arrival handler comes out through state 3**, which is the
success branch — no longer through 0xb — and emits its warp, which is
accepted. On the guest's side the re-entry is solved: he comes out of the
death, gets up, redoes the join and arrives.

### What is missing is the host

And there it stops. State 3 is where the guest waits for the peer's answer,
and it does not come: shortly afterwards neither client has a session object
any more — `bp 2c3630` fires on neither. The guest ended up alive in his own
world, the host alone.

The host **never asks for a session end** — its log stays empty, which
confirms again that it takes no part in the guest's death. So the host's
session did not die from a request: it fell apart because the link to the peer
dropped, and what drops the link is precisely putting the guest's machine back
to state 1.

Two directions from here, and the second looks cheaper than the first:

1. **Hold the host.** Find out what in the host releases the guest's slot when
   the link blinks, and hold it for the few seconds of the re-entry.
2. **Re-summon instead of re-entering.** `DS2_RematchHook` already does
   exactly that for red signs after a duel — the host re-summons the same
   player by itself. Applying it to the white sign after a co-op death reuses
   proven code and delivers what the design asks for ("died, respawns and
   stays in the session") at the cost of one load.

### The ninth: without the handshake, the machine moves

Going straight to state 2, without putting the session back to 1, changes the
result:

```
morte de fantasma: motivo=2 papel=1 -> morte normal, e depois levantar e puxar de volta
fim de sessao RECUSADO  papel=1 estado=7 motivo=2
levantando na ultima fogueira apos 90 quadros
de pe de novo apos 421 quadros, guarda=00; indo para o estado 2 (sem refazer o handshake)
pode entrar = 0 apos 0 quadros -> 1 apos 97 quadros
estado 2; repetindo o convite: destino=6.16,-18.50,209.16
depois da chegada: estado=3
estado -> 4
```

and, read live a minute later, `[sessao+0xf8] = 6`. Before it stopped at 3;
now it runs **3 → 4 → 6**, and the guest is drawn as a white phantom again.
Neither 3 nor 4 is in the dispatcher's switch, so each step came from a
message: something on the other side is answering.

There also turned up, on the first round of this variant, a second end request
that was beside the point: `motivo=3` coming out of `+0x2c385c`, inside the
state 7 handler, in the window between the death and the re-entry. Blocking it
(`block 3`) is one line in the request file and took that noise out of the
way.

### The host does not use this machine

And even so the session ends, because the host disappears. What is known about
it:

- it **never** asks for a session end — the host's `DS2_SeamlessSessionHook`
  log stays empty for the whole session;
- `bp 2c3630` **does not fire** on the host after the guest's death, and the
  dispatcher is per frame: if it does not run, there is no session object to
  run on.

So the host is not the other end of this same state machine — or its object is
destroyed by a path that does not go through `FUN_1402c2f20`. That is the next
thing to measure, and the measurement is cheap: arming `bp 2c3630` on the host
**while the session is live** answers at once whether it has one of these
objects and what state it sits in.

### The host has its own machine, and it has a name

The binary carries RTTI, and the classes read themselves:

```
NetMultiplayCtrl
├── NetJoinMultiplayCtrl     → NetSummonJoinMultiplayCtrl    (o convidado)
└── NetAcceptMultiplayCtrl   → NetSummonAcceptMultiplayCtrl  (o host)
```

Everything this document has called "the session" up to here is the first one:
the vtable at `0x1410d7bd8`, the state at `+0xf8`, the dispatcher
`FUN_1402c3630`, `FUN_1402c2f20` to end it. It is the machine for **entering
someone's world**.

The host uses the other one, and it differs in everything that matters:

| | guest | host |
| --- | --- | --- |
| class | `NetSummonJoinMultiplayCtrl` | `NetSummonAcceptMultiplayCtrl` |
| vftable | `0x1410d7bd8` | `0x1410d7998` |
| state | `+0xf8` | `+0x150` |
| per-frame dispatcher | `FUN_1402c3630` | `FUN_1402bddb0` |
| cases in the switch | 0,1,4,5,6,7,8,10,0xb | 2,4,5,7,8,10,0xd,0xf,0x10,0x11,0x12 |

That explains at once two things that had been read as fact about the game and
were facts about the instrumentation:

- **"the host never asks for a session end"** — `DS2_SeamlessSessionHook`
  watches `FUN_1402c2f20`, which belongs to the guest's class. The host was
  never going to show up there.
- **"the host has no session object after the death"** — `bp 2c3630` is the
  guest's dispatcher. The host was never going to fire there either.

The measurement that matters now is direct: `bp 2bddb0 deref rcx+150 8` on the
host, with the session live, and see what its state does when the guest dies.

### The host survives the guest's death

With the right class in hand, the measurement is direct. The host's controller
needs no breakpoint: sweeping live memory for its vtable is enough.

```
scan host 1410d7998 8 6      # NetSummonAcceptMultiplayCtrl::vftable
```

With the session standing that returns an object on the heap —
`0x7ffffe5bf120` in one round — and `+0x150` in it is **0x10**. Forty seconds
after the guest's death, still **0x10**: the host does not let go right away.
Minutes later the object no longer shows up in the sweep, so it does let go at
some point, but not at the one assumed.

That knocks down the previous section's conclusion. The host does not
disappear when the guest dies; it stays exactly where it was.

And on the guest's side, with the ninth variant, the machine **completes the
join**: read live after the re-entry, `[sessao+0xf8] = 7` in a new session
object, and the character is drawn as a white phantom. So both sides think
they are in a session — and even so neither sees the other, because the guest
is in his own world.

What is missing, then, is not keeping the host alive or completing the guest's
machine: both already happen. It is **making the host put the phantom back in
its world**. In the original join that comes from a message the host receives;
the re-entry never sends it.

### The host's timeline, finally

With `FUN_1402bddb0` detoured, the host tells its own story. A normal join,
frame by frame:

```
host: estado -> 4 -> 5 -> 7 -> 8 -> 10(0xa) -> 11(0xb) -> 13(0xd) -> 14(0xe) -> 15(0xf) -> 16(0x10)
```

`0x10` is playing. And on the guest's death, with the ninth variant running:
**nothing**. Not one transition. The host stays at `0x10` from start to
finish, never knowing the guest left, let alone that he came back.

But the server knows:

```
22:06:41  3:Chico   RequestNotifyDeath
22:07:04  1:Samuel  RequestNotifyLeaveGuestPlayer
```

Twenty-three seconds after the death — and *after* the guest's re-entry had
completed, which takes about eight. It is not a reaction to the death: it is a
**timer**. The host went twenty seconds without receiving anything from the
guest and dropped him.

Which says where the problem really is. It is not either one's state machine:
both stay standing, one at `0x10` and the other reaching 7. It is the **flow
between them**. The ninth variant preserves the link object but does not make
the guest talk over it again, and the silence is what the host times.

### The rematch works for a white sign

`DS2_RematchHook` was written for duels: it keeps the manager pointer when the
player summons, and by itself re-summons any sign that later reaches the
host's cache. It had never fired for a white sign, and the hook only spoke
when it acted — so there was no way to tell whether it was not being called or
was being called and giving up.

With one more line, logging every sign that comes in:

```
placa recebida: tipo=1 alca=80000011 armado=1
revanche pedida, mas ninguem invocou ainda: sem o manager nao da
```

`tipo=1` is the white sign. It **is** called, the handle is valid and it is
armed: all that was missing was the manager, which only arrives when the
player summons once per game session.

That makes viable the path the ninth attempt does not reach. Instead of
stitching the guest back into a session whose peer flow is already dead —
which is what the host times and drops — the guest goes home through the
normal death, places a sign, and the host summons it by itself. It is a
genuinely new session, with a new link, at the cost of one load.

What is left to close it: seed the manager (one manual summon per game
session, or find where else the pointer comes from) and place the sign by
itself on the guest's side.

### Two staging traps that cost cycles

**`up` rewrites `Injector.config`.** Turning `--auto-rematch` on in
`game prepare` and then running `up --seamless` turns it back off, and the
hook's log keeps showing the previous boot's lines — which reads exactly like
a live hook. Check the log's **mtime** before believing it.

**Heide's bonfire sits on a narrow slab over the water.** Any `dpad` that
misses the menu and reaches the world pushes the character into the sea, and
the test dies with him. Every menu walk has to confirm by screenshot that the
menu opened before the next direction.

### Automatic re-summon, end to end

Measured on 12/09, 22:46, with no human intervention between the sign and the
session:

```
22:45:05  3:Chico   Sign 1016 created: type 1
22:46:03  1:Samuel  Sign poll: 1 signs cached
          (host)    placa recebida: tipo=1 alca=80000031 armado=1
          (host)    revanche: invocando a placa 80000031 que acabou de chegar
22:46:03  1:Samuel  Summoning sign 1016
22:46:14  1:Samuel  RequestNotifyJoinGuestPlayer
22:46:16  3:Chico   RequestNotifyJoinSession
          (host)    estado -> 0xb -> 0xd -> 0xe -> 0xf -> 0x10
```

And on the screens: Samuel's name and bar on Chico's HUD, Samuel visible
beside him, Chico drawn as a white phantom. A real session, a new link, the
two of them seeing each other.

**This is a workaround, not M2** (corrected on 13/09: M2's criterion is dying
and staying in the *same* session, with no sign and no re-summon — see the
task list). What the paragraph below said on 12/09: not to stitch the guest
back into a session whose peer flow is already dead — the host times that
silence and drops him, measured — but to let the death run, the guest go home,
place a sign, and the host re-summon him by itself. It costs one load and
delivers what the design asks for: died, respawns, and carries on with his
friend.

One piece is missing, and it is small next to the rest: **the guest placing
the sign by itself**. Today it is a manual X. The rest of the chain is already
automatic.

Two known rough edges:

- the manager only exists after one manual summon per game session, which is
  where `DS2_RematchHook` takes the pointer from. It is worth looking for
  another source;
- the hook tries **every** sign that arrives, including old signs still in the
  client's cache, and each of those earns a "Summoning failed. The sign has
  disappeared." on the host's screen. Filtering by owner would fix it.

### The missing piece, and where it is

For M2 to be hands-free, the guest needs to place the sign by itself on
getting home. The RTTI already hands over the neighbourhood:

```
ISummonSignSetCtrl / SummonSignSetCtrl   vftable 0x1410cb698
   métodos em 0x140212cd0 .. 0x140213c80
   FUN_140213160 (o AddSign que o DS2_RematchHook usa) é desta classe
AbstractNetSvrMySignManager              vftable 0x1410d3518
Frpg2RequestMessage::RequestCreateSign   vftable 0x141113378
```

The message constructor (`FUN_1406a0b10`, via the `FUN_140caa440` factory) is
no good as a hook point: it is protobuf allocation, far from whoever decides
to place the sign.

**The short way is to measure, not to read.** A breakpoint sweep over
`0x140212cd0`–`0x140213c80` with the guest pressing X says in one pass which
method is the placement. It is the same technique that found the warp, and
here the range is thirty functions instead of three hundred.

It is worth remembering that the honest alternative exists and already works:
**an X from the player**. Died, got home, pressed X, and the host brings him
back by itself. It is not "seamless" the way the brief means it, but it is one
button per death and depends on nothing else.


## 13/09 assessment: how the game respawns, and why the warp takes the phantom

Asked of Fable with this document's entire record. Marked below is what was
**re-checked in the binary** afterwards; the rest is his reading, with the
addresses for whoever wants to check.

### The ordinary respawn does not use coordinates

- **[re-checked]** The last bonfire's record is `*(ctx+0x70)`: `+0x164` map,
  `+0x168` type, `+0x16c` id. `FUN_14044ed40` builds the request from it —
  record type 0 becomes request type 3, type 2 becomes the map's "player
  start", and anything else becomes a default destination (`FUN_14039a9a0`).
- What writes the record: lighting or interacting (`FUN_1401caf50`) and
  sitting (`FUN_1401cb950`) — only if whoever interacted is the local player;
  travel through the menu (`FUN_14017fdb0`); and two other callers that were
  not read (`FUN_140040060`, `FUN_140461f20`). The record is rebuilt on every
  load.
- The id (`0x7ba7` in the measurement) belongs to a **map object**, and is
  only resolved **after** the reload: `FUN_1401c3c60` looks the object up in
  the list `*(ctx+0x70)+0x58` and **[re-checked]** `FUN_1401cb1b0` puts the
  player at `translação − 1,1 × eixo Z` of the object's matrix
  (`DAT_1410bf020 = 1.1f`), facing the same way as it.

So **bonfire coordinates do exist, but only for the loaded map.**

### Every warp reloads, and that is what takes the phantom away

Fable's reading of the loader's state machine (`GameManagerImp`, vftable
`0x1410c4c68`): the warp entry only accepts in state `0x1e`; on accepting it
notifies multiplay and arms a delay (6 s for death, 2 s for the others); state
`0x14` **unconditionally** destroys the map, the characters and
**`ctx+0xd0 = 0`**; `0xb` rebuilds everything. Types 0–4 resolve the
destination only after that. There is no "same map" shortcut.
**[re-checked]** `FUN_140419610` rebuilds the character with
`ctx[0x1a] = chr` — so **`ctx+0xd0` is the local character**, and what this
document called the "multiplay counter at `ctx+0xd0 +0x168`" is a field of the
character, rebuilt on every load.

The session manager (`ctx+0x22f0`) survives the reload — which is why the
sessions stayed standing in state 7. Presence does not survive: on the guest's
side, the remote players are registered in state 5 (`FUN_1402c3c80` →
`FUN_14051b0e0`), which **does not appear in the ninth attempt's trace**; on
the host's side, the guest's character is created by the sequence `0xd → 0xe
(WaitGuestWarpFinished) → 0xf`, which a host stopped at `0x10` never does
again.

### Death has a single point

> **Corrected by the 13/09 measurement** (section "Death measured, and held",
> at the end of this document): the byte is the right point, but what consumes
> it for the local player is slot **`+0x20`**, `FUN_14013c720`; the `+0x10`
> below was never called for him. And `+0x759` has ten writers, not one.

**[re-checked]** `FUN_14013c3b0` (slot `+0x10` of `ChrDeadActionCtrl`, vftable
`0x1410bf308`) leaves if `*(chr+0xb8)+0x5fc != 0`, leaves if `+0x759 == 0`,
copies the death parameters from `+0x75c..+0x76d` and only then calls
`FUN_14013d430`. According to Fable, lethal damage (`FUN_14013a9b0`),
character flags, animation event `0x19` and status all write `+0x759 = 1`.
Cancelling there keeps the rest of the game from seeing a death — no "YOU
DIED" sequence, no `RequestNotifyDeath`, no `EndSession`, no warp. It is the
M2 plan in the task list.

Risks he named: death sources that do not go through `+0x759` (the damage
function is virtual in four vtables and only one was read); leaving `+0x759`
on with `+0x5fc != 0` makes the consumer ignore the death; and every
consequence reproduced by hand that is left out is a save divergence between
the two players.

### The host's death, read and not measured

The host's warp calls `FUN_1402bd0d0(ctrl, 4)`, which **only ends the session
if the host is not at `0x10`**. What ends it, on the host's death, is the
guest: the phantom's death terminal runs with reason ≠ 2 and asks for the end.
With death intercepted on both clients, the host never dies and never warps,
and there is nothing to suppress.

## Teleport without a warp, and the bonfire's coordinates (13/09)

Steps 1 and 2 of the M2 plan, measured solo with Samuel in Heide, with no
session.

### Where the position really lives

Writing the position that looks like the character's moves nothing: every
visible copy is rewritten on the next frame. The write watchdog (`wp` in
`DS2_Trace`, see [DS2_INVESTIGATION_TOOLS.md](DS2_INVESTIGATION_TOOLS.md))
followed the chain from the top down, one layer per measurement:

| copy | what rewrites it per frame | what it is |
| --- | --- | --- |
| `ChrPhysicsCtrl+0x80` | `+0x36ec7f`, in `FUN_14036ec00` | speed meter: samples the position through `PlayerCtrl`'s virtual getter `+0x148` and derives the velocity |
| `PlayerCtrl+0x90` | `+0x314df1` (setter `+0x140`, coming from `ChrMotionCtrl`) and `+0x36df42` (physics) | the world matrix's translation `+0x60..+0x9f`; animation writes, physics corrects |
| `ChrPhysicsCtrl+0x1c0` | `+0xbd2c18`, in `FUN_140bd2bd0` | the body's cache, `y` 0.05 m above the feet (`ChrPhysicsCtrl+0x104`) |
| **`hkpRigidBody+0x1a0`** | Havok's integrator | **the authoritative position** |

The objects, all named through RTTI:

```
ctx+0xd0                          PlayerCtrl                (HP em +0x168, máximo em +0x170)
PlayerCtrl+0xf8                   ChrMotionCtrl
PlayerCtrl+0x100                  ChrPhysicsCtrl
ChrPhysicsCtrl+0x320              hkpCharacterRigidBody
hkpCharacterRigidBody+0x20        hkpRigidBody
hkpRigidBody+0x170..+0x190        rotação
hkpRigidBody+0x1a0                translação
hkpRigidBody+0x1b0 / +0x1c0       centros de massa do swept transform (os w são tempos)
hkpRigidBody+0x250 / +0x260       posições de quadros anteriores
```

### The teleport that worked

A single request, writing only XYZ and preserving every `w`:

1. the game's reference copies — `PlayerCtrl+0x90` and `+0xa0`,
   `ChrPhysicsCtrl+0x80`, `ChrMotionCtrl+0x50` — and zeroing the meter's
   velocities (`ChrPhysicsCtrl+0x60` and `+0x70`);
2. in the `hkpRigidBody`: `+0x250`, `+0x260`, `+0x1b0`, `+0x1c0`, `+0x1a0`,
   with `0,05` added to `y`;
3. last of all the cache `ChrPhysicsCtrl+0x1c0`.

Result: Samuel left the Heide bonfire and appeared **standing in the Cathedral
of Blue, 69 m away and 12 m above, with no loading screen**. The server
confirmed it in its own log (`Location: ... position 13.6 -6.2 276.0`), the
position stayed stable, and he walked 1.8 m with the stick right afterwards,
with `y` constant. He ended up about 85 cm off the exact point — the physics
pushing the capsule out of the bonfire's geometry.

**Resolve the chain again on every use.** After a reload (a death, a warp) the
`PlayerCtrl`, the physics and the motion control came back at the same
addresses, but the `hkpRigidBody` did not: the old address had become some
other thing's body, at (−100, 0.7, 188), and a teleport with the stored
pointer moved that body and left the character where he was (13/09). The
body's vftable (`0x141126578`) does not tell one from the other; what tells
them apart is having just come from `*(*(ChrPhysicsCtrl+0x320)+0x20)`.

Two earlier attempts say what is **not** enough: writing only the physics
(`ChrPhysicsCtrl+0x80`) is undone on the next frame; writing the game's copies
plus the body's cache, without Havok, is undone too (the character moved 16 cm
and came back). And a side case: writing only the body's cache moved the
character 0.8 m the wrong way — the capsule's sweep between the old position
and the new one hitting the geometry — which reads as a teleport and is not.

### The bonfire's coordinates

The last bonfire's record (`*(ctx+0x70)`, fields `+0x164` map, `+0x168` type,
`+0x16c` id) said `0x0a1f0000 / 0 / 0x7ba7`. The list at
`*(*(ctx+0x70)+0x58)` (first node at `+8`, next at `+0x60`, object at `nó+8`)
had **3 nodes** in the loaded map — the bonfires. Each one's spawn point, by
the game's own arithmetic (`translação(+0x70) − 1,1 × eixoZ(+0x60)` of the
object's matrix), gave `(6.1855, -18.5166, 209.0531)` for the Heide one,
**0.000 m** from where the game had put Samuel on loading. The other two:
`(13.0562, -6.1674, 276.6603)` — the destination of the teleport above — and
`(-162.0097, -1.7606, 190.6973)`.

### From any bonfire in the record to the coordinates

Each node's id is what `FUN_14017f170` compares when looking for the type 3
request's bonfire, and now it is read from outside. `FUN_1403ba6a0(obj)` calls
`FUN_1401ca770(obj+0xb8, obj)`: if the byte `obj+0xa2` is 1 or 5, the
component is `*(*(obj+0xb8)+0x20)`; otherwise it walks the component list at
`obj+0x18` comparing types (`FUN_1401ca700`). The id is `**(componente+0xe0)`.
Heide's three bonfires have `+0xa2 = 1`, so the short path is enough:

```
chain id 16148f0 70,58,8[,60...],8,b8,20,e0 4
```

Measured on 13/09, with the record saying
`mapa 0x0a1f0000 / tipo 0 / id 0x7ba7`:

| node | id | spawn point |
| --- | --- | --- |
| 1 | `0x7bac` | `(-162.0097, -1.7606, 190.6973)` |
| **2** | **`0x7ba7`** | **`(6.1855, -18.5166, 209.0531)`** — where the game put Samuel on loading |
| 3 | `0x7ba2` | `(13.0562, -6.1674, 276.6603)` — the Cathedral of Blue |

The record's id matches the node whose spawn point agrees with the game to
0.000 m, so the recipe is checked against the game itself:

1. `registro = *(ctx+0x70)`: map `+0x164`, type `+0x168`, id `+0x16c`;
2. `nó = *(*(registro+0x58)+8)`, next at `nó+0x60`;
3. `obj = *(nó+8)` (a `MapEntity`); the component by the path above (a
   `MapObjReactionComponent`);
4. the node whose `**(componente+0xe0)` is the record's id;
5. the spawn at `translação(obj+0x70) − 1,1 × eixoZ(obj+0x60)`, facing the
   same way as the object's matrix.

Two notes. The record only changes when you interact with a bonfire: after the
teleport to the Cathedral it stayed at `0x7ba7`. And the list is **the loaded
map's** — a bonfire from another map is not in it, which is the approach's
already known limit.

## Death measured, and held (13/09)

Step 3 of the M2 plan. Measured solo with Samuel, with no session.

### A real death, from outside

Before writing any hook, with the DLL that was already running: Samuel's HP
was zeroed by `DS2_MemProbe` (`PlayerCtrl+0x168 = 0`) with three tracer
breakpoints (`FUN_14013d430`, `FUN_140416960`, `FUN_14013d560`) and the write
watchdog on `*(chr+0xb8)+0x759`. Zeroing the HP really kills, through the
ordinary path: in the same second the server received `RequestNotifyKillEnemy`
and `RequestNotifyDeath`, and 6 s later came
`warp motivo=1 ... ponto=00007ba7`, out of `FUN_14044fde0`.

| what | where |
| --- | --- |
| turns `+0x759` on | `+0x16a695`, in `FUN_14016a650`, called by the player's update (`FUN_140315520`) |
| clears `+0x759` | `+0x13cb5b`, in `FUN_14013c720` |
| `FUN_14013d430` (the death notice) | called from `+0x13c938`, in `FUN_14013c720` |
| `FUN_14013d560` (souls, `RequestNotifyKillEnemy`) | called from `+0x13c97a`, in `FUN_14013c720` |

`FUN_14013c720` is `ChrDeadActionCtrl`'s slot **`+0x20`** and runs once per
frame for each character, called from `FUN_14030eb60`. With breakpoints on
both slots, only it fired; `+0x10` (`FUN_14013c3b0`) was never called for the
local player. After a reload it shows up for two characters that are not the
player, called from `+0x30ea2f`.

### The HP source, and the controller's state machine

`FUN_14016a650` turns the byte on when `chr+0x168 < 1`, bit 15 of `+0x4c8` is
clear and the byte is still zero, and it fills in `+0x75c` (a handle for the
killer), `+0x760` (flags that suppress individual consequences), `+0x768 = 10`
(the cause). It runs every frame: **clearing the byte without giving the HP
back only puts the death off by one frame.**

`FUN_14013c720`'s state machine, in the byte `ctrl+0x10`:

- **0, alive.** If `+0x5fc == 0`, it calls `FUN_14013cc30` (the flag sources:
  `*(chr+0xd8)`, `*(chr+0xd0)`, animation event `0x19`). With the byte on: it
  copies the parameters, calls `FUN_14013d9f0`, **clears the byte**, fetches
  the timing row (`FUN_14013d880`, kept at `+0x70`), calls `FUN_14013d250`
  (which tells the manager at `ctx+0x40` and the bonfire record at `ctx+0x70`)
  and `FUN_14013cec0`, and goes to 2 (or 1, if `+0x5d0`).
- **2, dying.** It adds up the time in `+0x58` and fires each consequence once
  when the time passes that row's threshold; the latches are `+0x14..+0x4c`.
  After the measured death they were all at 1, with `+0x58 ≈ 7,49 s`.
- The tail turns on bits `0x4000`/`0x8000` of `+0x4c8` outside state 0, and it
  is `0x8000` that silences `FUN_14016a650` while the character dies.

A second door, which does not go through the byte: `FUN_14013c500(ctrl, tipo)`,
reached by a jump from `FUN_14030eb20` (virtual in five vftables). With type 1
or 2 it fires every consequence at once and parks the controller in state 3.

`+0x759` gets `1` in ten places (a scan of the whole `.text` for `0x759(`):
`+0x13aae5` (lethal damage, `FUN_14013a9b0`), the three in `FUN_14013cc30`,
`+0x145f3f` (cause `0x6e`), `+0x16a695` (HP), `+0x31b753`, `+0x37046b`
(`FUN_1403703e0`, death on landing, cause 10), `+0x372ed7` (`FUN_140372e20`,
death by falling, cause `0x5a`) and `+0xd1c7f8`. There is also a copy of one
structure's whole block into another at `+0x8d615`, which may be replication.
The hook goes on the consumer, not on each source. Two caveats: the list comes
from a pattern scan, which does not see a displacement folded into a register;
and the byte has a third reader, `+0x13683a` in `FUN_140136570`, which was not
read.

### The hook: `DS2_DeathInterceptHook`

It detours `FUN_14013c720` and only acts when `*(ctrl+8)` is the local
character (`ctx+0xd0`). The 15 live controllers include enemies, and
cancelling without that filter would leave them immortal. With the controller
in state 0, `+0x5fc == 0` and the byte already on **before** the call, it:

- in `observe` (the default), writes the death down and lets it through;
- in `cancel`, clears the byte, gives the HP back to `chr+0x174` (the
  effective maximum, with hollowing already taken off; `+0x170` is the base),
  clears `0x4000|0x8000` of `+0x4c8` and **does not call the original** on
  that frame.

If the state leaves 0 without the byte beforehand, the log says
`SEM +0x759 ANTES`: it is a death from `FUN_14013cc30`'s sources, which turn
the byte on inside the call and would slip past the check. `FUN_14013c500` and
slot `+0x10` are only logged. `DS2_Death.req` changes the mode without
relaunching (`observe`, `cancel`, `status`), and the log is `DS2_Death.log`.

### The result

**Death by HP, cancelled.** With `cancel`, the zeroed HP went back to 869 on
the same frame (`morte CANCELADA #1 hp=0 -> 869 ... causa=10`), the controller
stayed in state 0, `+0x759` and `+0x4c8` zeroed, no warp in
`DS2_Seamless.log`, and the character walked. On a second cancellation he went
2.5 m towards the stairs, with `y` constant and a normal camera. The server's
positive control came afterwards: the first death let through on that same
connection (in `observe`, 13:42:49) printed `First ... RequestNotifyDeath`, so
none of the earlier ones had got there.

**Death by falling: the byte is held, the rest is not.** The first control
test took Samuel backwards and off Heide's platform. He fell to `y = -3000`,
and:

1. `FUN_140372e20` (the death by falling, called by the fall control
   `FUN_140372620` when the time in the air passes the threshold) zeroed the
   HP, turned on the bit **`0x200` of `*(chr+0xb8)+0x4c0`** and the byte with
   cause `0x5a` and `+0x76d = 2`. The hook cancelled (#2).
2. While he fell, the HP went back to zero every frame, and the hook cancelled
   **2956 times in 100 s**, once per frame, with nothing reaching the server.
   (The server stopped receiving position: `Location` stayed at
   `6.2 -18.7 211.4`, the edge.)
3. Step 1's teleport to the spawn point of bonfire `0x7ba7`, writing only XYZ,
   **stopped the loop**: the counter stopped moving and the server went back
   to saying `position 6.2 -18.5 209.1`.
4. But the character was left **without control and with the camera frozen**
   at the point of the fall. The game thread was alive (a breakpoint on the
   per-frame caller fired at once), START opened the menu, and the stick moved
   nothing. Clearing the bit `0x200` did not give the control back.

The way out was to go back to `observe` and zero the HP: an ordinary death, a
warp, a reload, and him standing at the bonfire again.

### Death by falling, undone (13/09)

The first fall's "control lock" did not exist. What held Samuel was the
camera, and death by falling leaves three marks that the byte does not undo.

**Where the fall comes from.** `FUN_14036fdf0` handles the character's contact
with a map collision volume, by the type at `*(*(volume+0x30)+0x70)+0x14`. On
types 1, 2, 5 and 6 it turns on **bit 51** of `*(chr+0xb8)+0x4c0`
(`0x8000000000000`); on types 3, 4, 7 and 8, **bit 52**. On types 1, 3, 5, 7
and 10 it also sends the camera a type 7 request, for characters whose type
passes the table `0x1410bfff1` (Samuel passes; who else passes was not read).
The water under Heide's platform is one of those volumes. With bit 51 on, the
fall control (`FUN_140372620`, called by the character's per-frame update
before the death controller) calls `FUN_140372e20` on every frame the
character spends in the air: HP to zero, **bit 9** (`0x200`) and the byte with
cause `0x5a`. That is why the hook was cancelling once per frame for the whole
fall.

**The camera.** The type 7 request (`FUN_140492080`, case 6) only writes
**`CameraManager+0x450 = 1`** (`CameraManager` at `ctx+0x20`, vftable
`0x1410f45a8`). Every frame, `FUN_140492880` compares that byte with the id at
`+0x454`: on with no id, it pushes a type 5 request onto the
`IngameCameraOperator` (`FUN_140495380`, vector at `+0x100..+0x108`,
`0x40`-byte entries with the id at `+0x30`), which activates the
`FallDeadCameraOperator` (index `+0x1520 = 6`); off with an id, it **removes
its own request** (`FUN_1404955a0`). The fall operator pins the position and
only turns to look at the character. Nothing turns the byte off short of a
reload, and with the camera looking from where he fell, the stick, which is
relative to the camera, moves the character almost nowhere. On the first fall
that gave zero movement; on the reproduction, 0.3 m in 600 ms.

The memory comparison (character, `*(chr+0xb8)`, action controls, physics,
camera) between Samuel standing and Samuel stuck found nothing else: apart
from positions and landing values, only the two bits of `+0x4c0`,
`CameraManager+0x450`/`+0x454` and the camera operator's state. A first
attempt at turning the camera off by writing the index `+0x1520` did not last
a frame: `FUN_140495e10` rewrites it from the request vector.

**The recipe, measured by hand:** teleport to the bonfire's spawn; the fall
control says he has landed on the very first read (`+0x08 = 0`); then
`CameraManager+0x450 = 0` (the manager removes the request, index back to 10)
and bits 9, 51 and 52 of `+0x4c0` cleared. Cancellations stop, the camera is
normal, and Samuel walked 1 m in 500 ms at the same height. Clearing the bits
before landing is no good: in the air, with the fall time still above the
threshold, the fall control kills again.

**The hook.** In `cancel` mode, a refused death that brings the bits or the
camera byte starts a recovery: teleport to the spawn of the record's bonfire
(step 2's recipe, read inside the game; with no bonfire in the loaded map, the
fall control's last position on the ground), and on the following frames, as
soon as the fall control says he has landed, it clears the bits and the camera
byte. It redoes the teleport every 30 frames and gives up at 300.

**Measured with the hook (13/09, Heide, solo):**

| test | result |
| --- | --- |
| teleport into the void | one cancellation (cause 90, `+0x4c0 = 0008000000000200`), teleport to bonfire `0x7ba7`, "queda desfeita em 1 quadros" 18 ms later; camera in mode 10, byte and bits zeroed, HP 823, and he walked 2.8 m in 1 s |
| walking off the edge | two falls (the stick was still pushing after the first return), two 1-frame recoveries, standing at the bonfire |
| HP zeroed, no fall | an ordinary cancellation, no "queda", position identical bit for bit |
| server | no `RequestNotifyDeath` and no `RequestNotifyKillEnemy` on the connection, no warp in `DS2_Seamless.log` |

One cost along the way: a write watchdog (`wp`) on the
`IngameCameraOperator`'s page, at 24 thousand faults per second, brought the
game down with `0xC0000005` the instant it was raised. It was a race in the
watchdog itself, fixed the same day (see DS2_INVESTIGATION_TOOLS.md).

### What is left for step 5

- Death by HP is solved in place: cancelling and giving the HP back leaves the
  character controllable with no loading. What is missing is taking him to the
  bonfire the way the fall already does.
- Death by falling is solved with a teleport to the bonfire, solo.
- Of the ten sources of `+0x759`, only two were exercised: HP and falling (the
  fall through the death volume; death from landing damage, `FUN_140372c00`,
  was not).

## Respawning and paying for the death (step 5, 13/09)

Measured solo with Samuel in Heide. `DS2_DeathInterceptHook`'s `respawn` mode
refuses the death and charges for it what the game would charge, with the
game's own functions, **with no reload**. Each piece was found against a death
the game did on its own (`observe`), with the write watchdog on the fields and
Ghidra.

### What a death costs, and what charges it

| cost | where it lives | what the game uses |
| --- | --- | --- |
| souls | `PlayerParam+0xec` (`PlayerParam = *(chr+0x490)`) | `FUN_14026af40(NetSvrBloodstainManager, saída)`, reached by the "YOU DIED" sequence through `NetSvrManager` slot `+0xe0` |
| the bloodstain | the `NetSvrBloodstainManager`'s record (`*(*(*(0x141616cf8)+0x30)+0x90)`): `+0x2c` has one, `+0x2d` already charged, `+0x30` souls, `+0x34` map, position, angle and cell | after the reload, slot `+0x28` (`FUN_14026b0d0`) creates the `BloodstainSetCtrl`'s type 10 marker |
| hollow | the level at `PlayerParam+0x1ac` | `FUN_140202c30(PlayerParam, *(data+0x76d))`, called by `FUN_14037dcc0` on the frame the controller enters state 2 |
| appearance and maximum HP | `*(chr+0xb0)+0x3e` (0 human, 1 hollow, 2 very hollow); effective maximum `chr+0x174` | `FUN_1402026e0(chr)` on the next load |
| Estus | `+0x24` of the Estus Flask entry (item 60155000) in the inventory `*(*(ctx+0xa8)+0x10)` | `FUN_1401ac370(inventário)`, what the bonfire rest's SpEffect calls |

Details that cost measurement:

- **The bloodstain's position is the last safe position**, not where the
  character is: the record takes it from `IBloodstainSetCtrl` slot `+0x80`, a
  ring of positions on solid ground. That is why the charge has to come before
  the teleport, and why on a fall the bloodstain stays at the edge.
- **The old bloodstain does not go away on its own.** In an ordinary death
  what removes it is the reload; `FUN_14020e4e0` only evicts a marker when the
  set is full. The hook walks the `BloodstainSetCtrl`'s two sets (`+0x18`,
  `+0x20`; count at slot `+0x18`, entry at `+0x10`, active when `+0x14` is
  negative, type in the handle's low nibble) and removes the type 10 markers
  through the interface (slot `+0x20`, which takes the entry's pointer) before
  creating the new one.
- **The hollowing level on its own changes nothing.** The maximum HP
  multiplier (`FUN_140202820`) is 1.0 while `*(chr+0xb0)+0x3e` says human.
  Calling only the recalculation (`FUN_140202ca0`) left the maximum at 915
  with hollow 1; what turns the level into a state, swaps the model and
  recalculates is `FUN_1402026e0`, the inverse of `FUN_140203d50` (what the
  Human Effigy calls).
- **The hollowing checks** are `FUN_14037dcc0`'s: no hollowing if
  `FUN_14031c850(data)` (two effect bits at `+0x4b8`), if `+0x4c8` bit 55, if
  `FUN_14016f740(chr)` (NPC or phantom type), or if `+0x4b8` bit 58. The
  fifth, `thunk_FUN_140014b03`, jumps into obfuscated code and was left out.

### What `respawn` mode does

On the first refusal of a death (the ones after it, while the recovery runs,
are the same death being held):

1. souls to the bloodstain (`FUN_14026af40`), if the record is not marked;
2. hollowing with the checks, and `FUN_1402026e0`;
3. type 10 bloodstains removed, and the new one created (`FUN_14026b0d0`,
   which also unmarks the record for the next death);
4. Estus refilled;
5. the fall recovery: teleport to the spawn of the record's bonfire, and, when
   the fall control says he has landed, bits and camera cleared and the HP
   full at the new maximum.

Every call into the game sits behind a `__try` of its own, and each function's
bytes are checked at installation.

### The result

| test | hook log | checked |
| --- | --- | --- |
| HP zeroed 6 m from the bonfire, 4321 souls, Estus at 0 | `almas 4321 -> 0 (registradas: 4321 ..., 10000 perdidas da anterior); hollow 1 -> 2; 1 antiga removida, nova criada, agora 1 no mundo; estus recarregado` | standing at the bonfire; the green bloodstain where he died, the bonfire's one gone; on touching it, 4321 souls back and the record empty |
| fall into the void | `almas 4321 -> 0; hollow 0 -> 1; nova criada`, fall recovery in 1 frame | the bloodstain at the last safe position; camera normal |
| second death before recovering | `registradas: 1000 almas para a mancha, 4321 perdidas da anterior; 1 antiga removida` | only one bloodstain in the world |
| human (effigy) dying | `hollow 0 -> 1, hp maximo 915 -> 869`, `renascer concluido ... hp 915 -> 869` | `*(chr+0xb0)+0x3e = 1`, HP 869/869 |
| server | — | no `RequestNotifyDeath` and no `RequestNotifyKillEnemy` on the connection; no warp |

The Estus showed up refilled in the game's own inventory (1 charge after being
zeroed by hand).

## With a session (step 6, 13–14/09)

Samuel in Heide, Chico summoned by a white sign, both in `respawn` mode. Two
sessions, each ended the legal way (`observe` mode on Chico and an ordinary
phantom death: `RequestNotifyLeaveSession` and `RequestNotifyLeaveGuestPlayer`
within a second). Saves snapshotted beforehand (`pre-passo6`) and restored
afterwards.

### Who pays what, by the game's own checks

The billing in step 5 charged anybody. What the game decides lives in the
death sequence and in `FUN_14037dcc0`, and the hook now goes through the same
doors:

| cost | who decides | white phantom (role 1) |
| --- | --- | --- |
| souls | `FUN_14018fbc0`, the `EventResult` sequence step for death type 1: there is a session manager (`ctx+0x22f0`) and a `NetSvrManager`, and `FUN_14018fd70` agrees — `*(ctx+0x70)+0x1b9` clear and, if the role is a guest's (second byte of the role's row in the table `0x1410c0050`, 16 bytes per role), the role's param (`FUN_14016f540`) with `+0x2e == 1` | `+0x2e = 0`: **the souls stay** |
| bloodstain | only when souls went in just now, and only where `FUN_14026b0d0` would put one (slot `+0x58` of the context) | untouched |
| hollow | the five checks in `FUN_14037dcc0`; the one the decompiler does not show is `call 0x14016f7d0`, a jump into obfuscated code, `bool(chr)` | the obfuscated one says **exempt** |
| death counter, ring | `call 0x140203be0` (another obfuscated jump) picks the branch for this machine's player: `FUN_140203ad0` adds into `PlayerParam+0x104+papel*8` and `+0x1a4`, `FUN_1401ac240` breaks the protection ring being worn, and an open menu from `ctx+0x22e0` is closed | adds |
| Estus | the ordinary respawn refills for everyone | refilled |

The role is the byte `*(chr+0xb0)+0x3c` (0 world owner, 1 white phantom); the
hollow state is the `+0x3e` beside it. Both obfuscated jumps were called the
way the game calls them, with the character in `rcx`, and the jump's bytes are
checked on install.

### The other player's copy was dying

The first round (build `1424e7c8`) got everything right on the side of the one
dying and wrong on the other side. With Chico's HP zeroed:

```
Chico   custos da morte (papel 1, convidado 1): almas 1234 -> 1234 (convidado que nao paga ...);
        hollow isento (... ofuscada=1 ...); mortes 69 -> 70 ...; renascer concluido em 1 quadros
Samuel  outro controlador ... personagem ... (vftable +0x10e4bb8, papel 1) estado 0 -> 2 hp=0
```

On Samuel's screen, **"Phantom Chico has been vanquished."**; on the server,
`RequestNotifyKillEnemy` from Samuel. Chico's HP 0 had already gone out over
the network before the hook gave it back, and his copy in Samuel's world
(`PlayerCtrl`, character type 2 at `chr+0x54`) died through its own controller
— the same shape as a local death: cause 10, bits `0x4000/0x8000` at `+0x4c8`.
The session machines stayed at `0x10` and 7, but the host no longer saw the
phantom.

It is a race: it happened once in two deaths with the HP zeroed from outside
the frame, and not once in the seven that followed (six with the HP zeroed,
one fall).

A player's death belongs to the machine that plays that character. Build
`69a68af9` refuses, in `cancel` and `respawn` modes, the pending death of a
`PlayerCtrl` copy with the same test as the local controller (state 0,
`+0x5fc` zero, `+0x759` set): it clears the byte, gives back the HP and the
bits, and does not let the controller run on that frame. Measured with the
byte set by hand on Chico's copy inside Samuel:

```
morte da copia RECUSADA #1 ... personagem ... tipo 2 papel 1 hp=853 -> 853 ...
```

and Chico stayed standing beside him. Zeroing the copy's HP by hand is **not**
a test: the network rewrites it before the next frame.

### "YOU DIED"

The death sequence (`EventResult` slot `+0x20`, `FUN_14018f830`) builds jobs
from a parameter row: id `papel + tipo*100` in the bonfire record's table
(`FUN_14044ed10`), or `tipo*100 + 99`. The first byte is an FE type, and the
ten-int table at `0x1410c3580` translates it into a banner for
`FUN_1405012e0(*(ctx+0x22e0), id)`. Read live: row 100 (world owner) and row
199 (white phantom) say FE 1, banner 3.

Banner 3 hides the HUD (`+0x46c` of `*(frontend+0xd8)`), and only a load gives
it back: `FUN_1404fffb0` sets `+0x468`, which the HUD update (`FUN_140507360`)
reads as "show everything again". The hook calls the same function when
`FUN_140500b10` says the front end has finished. Measured on Chico and on
Samuel, in a session: the lettering, the HUD gone, and
`banner 3 acabou em 203 quadros: HUD
devolvido`. On by default since `22880bf2`, checked solo with that build
(only `respawn` written, `banner=1` in the status). The death sound
(`FUN_1401905c0`, a BGM overlay nobody undoes without a load) was left out.

For a phantom, the ordinary death shows, after the banner, "You have been
vanquished. Returning to your world..." (message `0x129da1` from row 199). The
hook does not show it: the phantom does not go home.

### The online bloodstain does not go out

`FUN_14026c0b0` (or `FUN_14026c1b0` for death type 3) is what the
`NetSvrBloodstainManager` update calls on the frame where the local character
has HP 0 and the `0x4000` bit. Traced with timestamps on an ordinary death:
the call on the same frame as the death, the job (`FUN_14026bd10`) **five
seconds later**, and then `FUN_14019f520` reads the ghost recorder and
`FUN_140269650` sends; the server logged `RequestCreateBloodstain` five
seconds after the death.

Called by the hook, the same chain passes the three checks in
`FUN_14026bc50`, creates the job, the job runs five seconds later — and sends
nothing. `FUN_14019f520` only accepts if among the recorder's last 16 frames
there is one marked `0x1000` (`FUN_1401a25e0`): a frame recorded with the
character dead. With the death refused on the same frame, that frame is never
recorded. The `mancha_online` switch stays off.

### The result

| test | who died | checked |
| --- | --- | --- |
| HP zeroed 7.6 m from the bonfire (build 1) | Chico, phantom | his side right; **copy dead on the host**, "vanquished" |
| HP zeroed (build 1) | Samuel, host | 4321 souls into the bloodstain, hollow 0→1, maximum 915→869, deaths 55→56, teleport; on Chico's screen, Samuel at the bonfire; 60 s later: no `NotifyDeath`/`LeaveGuestPlayer`, host `0x10`, guest 7 |
| HP zeroed (build 2) | Chico | **the host sees Chico at the bonfire**, no "vanquished"; 60 s later host `0x10`, guest 7, no `LeaveGuestPlayer` |
| death byte set on the copy (build 2) | Chico's copy inside Samuel | `morte da copia RECUSADA`, copy standing |
| four deaths in a row (build 2) | Chico | all refused, no copy dead |
| fall into the sea (build 2) | Chico | cause 90, fall camera off, bonfire in 1 frame |
| fall into the sea (build 2) | Samuel | souls into the bloodstain at the edge, hollow 0→1, bonfire; 60 s later host `0x10`, guest 7 |
| banner (build 2) | Chico and Samuel | "YOU DIED", HUD back in 203 frames |
| server | — | the first `RequestNotifyDeath` on Chico's connection only appeared on the ordinary death on the way out, after seven deaths refused in the session |

### Two traps from this round

**The mode goes back to `observe` on every boot.** A `goto --to-instance 2`
done before writing `respawn` took Samuel off the Heide slab: a real death by
falling, with no session open, costing the Human Effigy and the test souls.
Write the mode into both `DS2_Death.req` before moving any character.

**A breakpoint set in the middle of an instruction crashes the game** when the
flow reaches it: the `0xCC` cuts the instruction in half. `bp` needs the exact
start of an instruction; check it in objdump before setting one.

## The host's bonfire (step 7, 14/09)

Up to step 6 the guest respawned at the bonfire in his **own** record, and it
only worked because Samuel and Chico had the same one (`0x7ba7`). A guest's
record is his own world's, and the game does not write another while he is in
the host's world: `FUN_1401caf50` (light) and `FUN_1401cb950` (rest) only
write when the one who interacted is the local character **and** slot `+0x58`
of the context says he is not in someone else's world. The host's record
exists only on the host's machine.

### A P2P channel, not the server

The task list expected a channel "probably through the server". It was not
needed: the session between the two is already Steam P2P, and the game uses
**one channel only** of it. Of the eight calls to `SteamNetworking()` in the
binary, the six that take a channel pass 0 — `FUN_140a75800` asks and
`FUN_140a73de0` reads, `FUN_140a7a410` and `FUN_140a76d90` send —, and the
other two accept and close the session with a user. A packet on another
channel travels over the same P2P session and sits waiting on the other
machine, untouched by the game, until somebody reads that channel. The server
takes no part and nothing changes on the VPS.

| piece | where |
| --- | --- |
| the session poll | `FUN_140a75800`, slot `+0x108` of `DLNRD::SteamSessionLight` (vftable `0x1411b1058`), on the session manager's thread; about 107 calls per second in a session (74 224 in 11.5 min) |
| the members | vector at `+0x68..+0x70`; each one is a `SteamSessionMemberLight` (vftable `0x1411b35e8`) with the CSteamID at `+0xc8`, where the game looks for a packet's sender. The player himself is on the list |
| who the host is | `+0xad` of the member, set by `FUN_140a72740` when adding him if his id is the lobby's `GetLobbyOwner` (`+0x3f0` of the session); the game's debug log calls it "Host". Read live: 1 for Samuel and 0 for Chico, on both machines |
| accepting packets from someone | `FUN_140a735f0` (`P2PSessionRequest_t`) only accepts someone who is in the session's lobby |
| sending and reading | `ISteamNetworking` slots `+0x00`, `+0x08`, `+0x10`, with the same arguments the game passes; your own SteamID comes out of `ISteamUser` slot `+0x10`, by pointer |
| outside a session | solo scan on 14/09: no object with `SteamSessionLight`'s vftable |

### What the mod does

`DS2_CoopChannelHook` detours the poll: it lets the game run and then reads
channel 7. If the local player is the session's host and the owner of the
world he is in (role 0), it announces `{mapa, tipo, id}` from his record to
every other member, every 2 s and the moment it changes. The announcement is
24 bytes (`JMJC`, version, type, sender's role) and is only kept if it comes
from the host of a session seen in the last 5 s. The network thread does not
read the game's world: `DS2_DeathInterceptHook` publishes the role and the
record every frame, on the game thread. `DS2_Channel.req` with `status` writes
what the channel saw into `DS2_Channel.log`.

On the death of someone who is not the world's owner, the respawn looks for
the announced bonfire on the loaded map; with no announcement newer than 30 s,
or with the host's bonfire off the map, it falls back to the one in his own
record and, failing that, to the last position on the ground. The search now
checks the map as well as the id: `*(*(obj+0x28)+8)`, the same field
`FUN_1401caf50` writes into the record (`FUN_1403ba320`), `0x0a1f0000` on
Heide's three bonfires, read on 14/09. The guest's record is **not** touched —
writing the host's bonfire there would carry it into his save — and the
`fogueira_do_host` switch in `DS2_Death.req` turns the choice off without a
build.

### The result

Staging: Chico rested alone at the "Tower of Flame" bonfire (`0x7ba2`, 69 m
north of Heide's) and was taken back to Samuel's bonfire; his record stayed at
`0x7ba2`, Samuel's at `0x7ba7`. Both human, white sign, session formed
(`RequestNotifyJoinGuestPlayer` at 01:48:08, `RequestNotifyJoinSession` at
01:48:10), both in `respawn`. Build `d3cd29e5`.

| test | who | hook log | checked |
| --- | --- | --- | --- |
| HP zeroed at the Tower of Flame | Chico | `levando para fogueira do host (6.186, -18.517, 209.053) mapa=0a1f0000 tipo=0 id=00007ba7; papel 1, anunciada por 011000010afd1a3a ha 1050 ms` | from (13.08, 276.66) to (6.27, 209.91); on Samuel's screen, Chico at his bonfire |
| HP zeroed beside Samuel, `fogueira_do_host off` | Chico | `levando para fogueira do registro (13.056, -6.167, 276.660) ... id=00007ba2` | the control: with no announcement, 69 m to his own |
| HP zeroed at the Tower of Flame, switch back on | Chico | `fogueira do host ... id=00007ba7 ... ha 1064 ms` | beside Samuel again |
| fall into the sea | Chico | `morte CANCELADA #4 queda ... causa=90`, `fogueira do host ... ha 1778 ms`, `renascer concluido em 1 quadros ... camera de queda desligada` | at Samuel's bonfire |
| HP zeroed at the Tower of Flame | Samuel | `levando para fogueira do registro ... id=00007ba7`, with no host note; hollow 0→1, maximum 915→869, deaths 55→56, bloodstain moved | back at his own bonfire; on Chico's screen, Samuel there |
| 60 s after the last death | — | — | host `0x10`, guest 7; no `RequestNotifyDeath`, `KillEnemy` or `Leave` on the server |
| channel | — | host: 347 sent, 0 failures; guest: 346 received, 0 refused | one announcement every 2 s for 11 minutes |
| exit | Chico in `observe`, `copias off` on Samuel | — | the first `RequestNotifyDeath` on Chico's connection at 02:00:09, `LeaveSession` and `LeaveGuestPlayer` at 02:00:21/22; Chico went home to his **own** bonfire, `0x7ba2` |

The last row is the other half of the proof: the guest's record stayed his.

### The guest who is still coming in

The same test showed up a bug in the first build. In the 10 seconds between
the lobby forming and him reaching Samuel's world, Chico was still the owner
of his own world (role 0) and announced his **own** bonfire five times — and
Samuel accepted it. With two players it had no effect, because the host does
not use the announcement; with three, a guest already inside would go to the
bonfire of whoever is coming in. Role 0 does not identify the session's host.
What does is the mark the game puts on the member (`+0xad`), and build
`4862d723` only announces and only accepts with it.

Checked in a second session with that build, the same staging (saves with
Chico still at `0x7ba2`):

- both logs list `011000010afd1a3a (host) 0110000140d6d6d1`;
- Chico **announced nothing** on the way in (`enviados=0`), and Samuel
  received nothing (`recebidos=0`); Chico received 22 announcements from
  Samuel in 40 s, none refused;
- Chico's HP zeroed at the Tower of Flame: `levando para fogueira do host ...
  id=00007ba7 ...; papel 1, anunciada por 011000010afd1a3a ha 753 ms`, and on
  Samuel's screen Chico at the bonfire;
- 60 s later, host `0x10` and guest 7; exit the legal way
  (`RequestNotifyDeath` at 02:12:41, `LeaveSession` and `LeaveGuestPlayer` at
  02:12:53/54), and Chico home at `0x7ba2`.

Saves restored to `pre-passo6` after both sessions.

### A staging trap

**The teleport does not turn the character, and the bonfire only offers "Rest"
to someone facing it.** Put on the Tower of Flame's exact spawn point, Chico
got no prompt at all; and walking with the stick moves the camera along with
it, so that "down" changes meaning at every step. What worked was to measure
the displacement of a short tap on the stick, convert the world direction into
the screen one and take the last step *towards* the bonfire: the prompt
appeared 0.16 m from the spawn point.

## A bonfire on another map (step 8, 14/09)

Up to step 7 the respawn looked for the bonfire in the list of loaded
bonfires, and without it left the character at the last position on the
ground: he paid the death and stayed where he died. It happens whenever
somebody dies on one map after resting on another. The project's owner chose
to keep the design to the letter — the last bonfire, with loading — instead of
respawning at the nearest lit bonfire.

Every load the game does goes through the warp, and the warp tears down the
other player's presence. But walking from one area to another does not go
through a warp: the game loads in parts, every frame, and that is the route
step 8 uses.

### How the game loads maps

| piece | where |
| --- | --- |
| the bonfire list | the instantiated `MapObjBonfireComponent`s: `FUN_1401cb310` inserts, `FUN_1401caee0` removes. A bonfire is only there with its map part loaded |
| the bonfire table | 77 `MapObjectBonfireParam` entries at `EventBonfireManager+0x20` (count at `+0x28`, 0x18 bytes: id, lit in your own world at `+2`, in the host's at `+3`, the param row at `+8`). No position |
| one owner per map | 38 `MapAreaCtrlOwner` (vftable `0x1410e87f0`), created on every load for every map (`FUN_1403dc0a0`): `+8` map, `+0xc` index, seven 128-bit part masks from `+0x10` on, `+0x1e8` state (5 is loaded), `+0x1e9` force, `+0x1ea` want. Update `FUN_1403cc3f0`, state machine `FUN_1403cc450` |
| the streamer | `*(*(ctx+0x38)+8)`: owners at `+0x38` (count `+0x1b6`), the part under the player at `+0x28` and its map index at `+0x30` (both written by `FUN_1403dc8e0`), the cell world at `+0x18` |
| which parts | a graph search over navigation cells starting from the cell the player is in (`FUN_1403dadd0` stores position and cell, `FUN_1403da960` searches). Every part reached adds the set of parts it brings with it, `*(*(parte+0x30)+0x70)` (128 bits), to its own map's mask |
| the part under a character | `FUN_140312ba0`: the physics contact `*(chr+0x100)+0x10`, whose handle at `+0xe0` has the type in the low nibble (7 map collision, 1 object), the map index in bits 4..9 and the collision's from bit 10 on; it is a part when the entity's type (`+0xa2`) is 2 |
| the cell of a position | `*(ctx+0xbc0)+0x10` is the navigation manager; `FUN_140badb90` finds the navigation map by the key `(índice & 0x3f) << 24 \| 0xffffff`, and `FUN_140babf90` the nearest cell within 10 units. It reads the position with aligned SSE |

Loaded together, Heide's first bonfire (`0x7ba7`, 6.186 / -18.517 / 209.053)
sits 226.7 m from Majula's (`0x122a`, 10.526 / 5.916 / -16.255), and the two
maps do not have to fit into one space: the game has a world offset for some
transitions (`FUN_1401c3fe0` asks for it at `ctx+0x2530`, `FUN_1401c3b40`
moves player and camera), which this step does not use, and on a manual trip
back Samuel landed on a Majula rock at the point of Heide's bonfire (below).

### What gave no ground

Samuel alone, standing in Heide, trying to get Majula loaded beside him:

| attempt | result |
| --- | --- |
| the streamer's own override (`+0x1f0` index, `+0x1d0` mask) on Majula | zeroes the other maps: Heide unloaded under the character and **the game closed** |
| the same override on Heide | nothing broke: loading every part is not the problem |
| `+0x1e9` on Majula, by writing | state 5 in half a second, Majula's characters created, `0x122a` on the list 226.7 m away — and no ground: taken there, the character fell |
| a hook forcing and adding `+0x10`/`+0x40`, then every mask but `+0x70` | fell; no new rigid body (197 before and after) |
| the game's own position request (`*(chr+0xc8)`, bit 0 of `+0xfc`) | fell just the same |

The ground did not come because the part search starts from the player's cell,
and a character in the air has no cell.

### What gave ground: the focus

`DS2_BackreadHook` detours two functions. In each owner's update
(`FUN_1403cc3f0`) it sets the force byte and adds the requested parts. In the
streamer (`FUN_1403dc8e0`) it hands over, in place of the player's cell, the
cell of a position on the requested map. The game then loads the ground around
that position as if the player had walked there. The first version found cell
-2 — an exception inside `FUN_140babf90`, which reads the position with
aligned SSE; with the vector aligned to 16 bytes, cell `0x0C000128`, and
Samuel went from Majula's bonfire to Heide's on his feet, with no death and no
warp. Once the focus was released, Majula unloaded and the server saw Heide.

`DS2_Backread.req` exposes the same pieces for measurement
([DS2_INVESTIGATION_TOOLS.md](DS2_INVESTIGATION_TOOLS.md)).

### Respawning on another map

When the target bonfire — the record's, or the one the host announced to a
guest — is not on the list and its map is known, the respawn
(`DS2_DeathInterceptHook`, switch `outro_mapa`):

1. asks for the map with every part and leaves the character at the last
   position on the ground;
2. every 10 frames it checks: with the map in state 5 and the bonfire on the
   list, it focuses the bonfire and takes the character there;
3. releases focus and map when the streamer says the character is standing on
   the new map. If he is standing on another map, it asks again every 90
   frames and gives up at 600.

In the measurements below the map reached state 5 in 500 ms, always in 40
frames, and the character stood on the new map 5 to 10 frames later. Step 3's
re-send never fired.

In a session, the map where another player's copy is stays loaded on this
machine too, until 5 s after the copy stops standing on it. In the first
session, without that (build `3392846c`), Chico's game **closed** the second
his respawn in Majula released the map (11:40:50.835; telemetry stopped at
11:40:50 and Steam saw the exit at 11:40:51), with Samuel's copy standing in
Heide — an illegal disconnect, cured with the `pre-passo6` saves. The exact
cause was not read: `DS2_CrashHook` was written at that point, a vectored
handler that notes the access violations in `DS2_Crash.log` with the
instruction in the game's image, and with the map kept no game closed or noted
anything in the four deaths in a session that came after.

### The fall the teleport was causing

The first death in the reverse direction — Samuel alone in Majula, record in
Heide — charged **two** deaths. The map loaded, the character went to the
bonfire, and a quarter of a second later:

```text
12:29:27.754  morte CANCELADA #2 (seguida 2) queda hp=-182 -> 732 ... causa=60 +0x4c0=0000000000000200
```

Hollow from 3 to 5, deaths from 55 to 57, and the first death's bloodstain
replaced by a second. The fall controller (`FUN_140372620`, at
`*(*(chr+0xe0)+0xb0)`) keeps at `+0x20` the position where it saw the
character on the ground — rewritten every frame on the ground or going up —
and measures the landing from it: `FUN_140372560` is `*(queda+0x24)` minus the
current height, and `FUN_140372c00` turns the height into damage. The teleport
did not touch that. From the last position on the ground in Majula (y 6.006)
to Heide's bonfire (y -18.517), one frame in the air before landing in Heide
was enough for the landing to count as a 24.5 m fall. The game itself solves
this in the position request: with bit 0 of `*(chr+0xc8)+0xfc`,
`FUN_140372620` copies the requested position into `+0x20`. The teleport now
does the same.

In the outbound direction (Heide to Majula) the bonfire is 24 m **above** the
death, and the same flaw did not show. By the mechanism, any respawn at a
bonfire well below the place of death, with one frame in the air before the
landing, would have hit the same bill since step 5; on the same map this was
not measured.

The second death was only charged because the respawn had already finished:
`renascer concluido em 1 quadros` came out **on the same frame** as the
teleport, reading "on the ground" from the fall controller, which had not run
since the change. On that frame the respawn no longer asks; while it is
active, a death that arrives is the same death, and is not paid for twice.

### The other player's map, whole and then only around him

The first version kept the copy's map **whole** (forced, every part). I
thought that would put Majula's sea rocks over Heide's first bonfire: on a
manual Majula → Heide trip releasing the focus early, Samuel landed on a
Majula rock at that point. Measured afterwards, alone, with the `keep` request
playing the copy's part: Majula whole in state 5, a death in Majula respawned
in Heide standing on Heide (contact `0xc7`, map 12), with no Majula geometry
on screen. What put the rock under Samuel on the manual trip was not measured;
Majula whole, alone, does not put one there.

Even so, what stayed is only what the game would load for a player standing
where the copy is: the forced map and the set of the part under the copy
(`*(*(parte+0x30)+0x70)`). At Majula's bonfire that set is bit 37; at Heide's,
bit 1. With no part (in the air, on an object), what was already there stands,
or every part. And releasing a map requested for the respawn no longer clears
the force byte of a map another player holds (`solto, mas segue mantido por
outro jogador`), which used to happen for one frame.

### The result

Alone, Samuel, with no warp in any case:

| build | test | log | checked |
| --- | --- | --- | --- |
| `3392846c` | death in Heide, record in Majula | `o mapa 0a040000 carregou em 40 quadros; levando para a fogueira 0000122a`, released in 6 frames | 0.7 s from HP zeroed to standing in Majula; stable for 15 s; server in "Majula" |
| `36a6cc62` | death in Majula, record in Heide | Heide in 40 frames, released in 5 frames, and the second death by falling, above | the fall defect |
| `7eaa79f1` | the same | one `custos da morte` (hollow 3→4, deaths 55→56), no `queda`; `concluido` 17 ms after the teleport; released in 10 frames at (6.221, -18.532, 207.326), contact `0xc7` | HP 732/732 afterwards |
| `7eaa79f1` | the same, with `keep 1 600000` (Majula whole) | one charge; released in 6 frames, contact `0xc7` | Majula and Heide in state 5; nothing of Majula over the bonfire |
| `7eaa79f1` | `keep 1 600000` with the bonfire's set | — | Majula forced, state 5, mask with bit 37 only |

In a session, Chico summoned into Samuel's world by a white sign, both in
`respawn`, a Human Effigy burned through the Inventory on both before the
sign:

| build | test | log | checked |
| --- | --- | --- | --- |
| `3392846c` | Chico dies in Heide, host's bonfire in Majula | Majula in 40 frames, released in 6 | **Chico's game closed** straight after; `LeaveGuestPlayer` from Samuel at 11:40:51 |
| `1f0c387b` | the same | `a fogueira do host (mapa 0a040000 id 0000122a) nao esta no mapa carregado`, Majula in 40 frames, released in 6, `mapa de indice 1 mantido` | Chico at Majula's bonfire; Samuel in Heide still with his bar; 96 s later host `0x10`, guest 7, no exception and no `Leave` |
| `1f0c387b` | Samuel dies in Heide, record in Majula | Majula in 40 frames, released in 6, Heide released 5 s later | Samuel in Majula beside Chico; 62 s later `0x10`/7 |
| `8c278604` | Samuel dies in Majula, record in Heide, Majula kept for Chico | one charge (hollow 0→1, maximum 915→869, deaths 55→56); Heide in 40 frames, released in 6, contact `0xc7`; on Chico's machine, `morte da copia RECUSADA` | Samuel at Heide's bonfire with Chico's bar; Chico in Majula with Samuel's; 69 s later `0x10`/7, no `Leave`, `Death` or `KillEnemy`, channel on announcement 174 |
| `8c278604` | Chico dies in Majula, host's bonfire in Heide | a guest's charge (no souls, hollow exempt, deaths 69→70); Heide in 40 frames, released in 10, contact `0xc7`; on his machine, `mapa 0a1f0000 solto, mas segue mantido por outro jogador` | the two side by side at Heide's bonfire, each seeing the other; 74 s later `0x10`/7, only Heide loaded on both machines |

Both sessions ended the legal way (Chico in `observe`, `copias off` on Samuel,
HP zeroed): `RequestNotifyDeath`, `LeaveSession` and `LeaveGuestPlayer` at
12:06:36/48/49 and at 13:38:40/52/53, and Chico home at his own `0x7ba7`. Not
a line in `DS2_Crash.log` on either machine. Saves restored to `pre-passo6`.

After Chico's respawns on another map the HP settled at 853 of 854, both at
12:02 (bonfire above the death) and at 13:36 (below). It is not the fall, and
it was not investigated.

## Without a Human Effigy (M3, 14/09)

### Where the Human Effigy was blocking

Measured with both characters hollow (level 3, state 1) at Heide's bonfire:

| who | what hollow does | proof |
| --- | --- | --- |
| guest | places the White Sign Soapstone | `Sign 1002 created: type 1` |
| host | **receives** the sign from the server | his poll goes from `room for 20` to `19` |
| host | does **not** get the prompt | only "Rest at bonfire" and "Pick up item" on Y; human (Human Effigy through `human`), "Touch Summon Sign" in the same place |

The block is on the host's client, after delivery.

### How it was found

No instruction read the hollow state (`*(chr+0xb0)+0x3e`) in 20 s beside the
sign — a new read watchdog (`wpr`, see
[DS2_INVESTIGATION_TOOLS.md](DS2_INVESTIGATION_TOOLS.md)), with the positive
control of 9 instructions reading the HP in 5 s. The **level**
(`PlayerParam+0x1ac`) was read by four, two of them one-line getters:

```c
bool FUN_1402ab0e0(void) { return FUN_140203db0(PlayerParam(local)->hollow) != 0; }  // hollow
bool FUN_1402ab120(void) { return FUN_140203db0(PlayerParam(local)->hollow) == 0; }  // humano
```

The first is called every frame by `FUN_1402a1980`, from the
`SummonSignSetCtrl` (return `+0x212a01` → `+0x2a1b23`), which refuses the sign
when its role in the table `0x1410c0050` is 2 or 3 and the local player is
hollow:

```
+0x2a1b1e  e8 bd 95 00 00   call FUN_1402ab0e0
+0x2a1b23  84 c0            test al,al
+0x2a1b25  75 20            jne  (recusa)
```

The getters' other users were left as they are: `FUN_140297f20` (case 8)
reports "human" to the server, `FUN_1402a1bf0` blocks items of some types, and
`FUN_140272120` / `FUN_1402af7e0` use the "human" in automatic summoning
eligibility.

### The patch and the result

`DS2_HollowSummonHook` swaps the `call` for `xor eax,eax` plus a three-byte
nop, checking `e8 bd 95 00 00` first. It installs alongside `DS2SeamlessCoop`.
The level, the maximum HP and the appearance stay the hollow ones.

| when | host | result |
| --- | --- | --- |
| original byte | hollow | no prompt |
| live poke (MemProbe, expected bytes) | hollow | prompt; `Summoning sign 1003` → `RequestNotifyJoinGuestPlayer` → `RequestNotifyJoinSession`, `p2pSessionVerified: true` |
| DLL `716f0c5b`, receipt `DS2 Hollow Summon: true` | hollow | the same, with sign 1000 and Soul Memory out of tier (below) |

In both sessions the end was `session end`.

## Without Soul Memory (M3, 14/09)

The server already filters by tier in `DS2_SignManager::CanMatchWith`, and the
poll now says what it filtered:
`sent N, refused by matching M (soul memory S)`. Samuel has 5130 and Chico
2551, the same default tier, so the control used tiers on purpose: `2999` at
the front of the white sign's list, 0 tiers below and above.

| `DisableSoulMemoryMatching` | Samuel's poll with Chico's sign in the cache | on the hollow host's screen (with the hook) |
| --- | --- | --- |
| `false` | `sent 0, refused by matching 1 (soul memory 5130)` | no prompt |
| `true` | `sent 1, refused by matching 0` | prompt, and the summon reached the server |

The local configuration was left with the default tiers and
`DisableSoulMemoryMatching` on for the White and the Small White Sign
Soapstone; the red one still has tiers.

## Coming in without a soapstone (M3, 14/09)

### Where the game places the sign

A breakpoint on `NetSvrSummonSignInterface::CreateSummonSign`
(`FUN_14029dfa0`, the only caller of the `NetSvrCreateSummonSignJob`
constructor, `FUN_14029d6c0`) only fired on a real use of the White Sign
Soapstone, with the caller `+0x2a2b98`, the area in `rdx` and Chico's Soul
Memory and level in the `MatchingParameter`. Going up:

| function | what it is |
| --- | --- |
| `FUN_1402a2780(manager, &tipo)` | place my own sign: checks, builds cell and matching, calls `CreateSummonSign`; keeps the sign at `manager+0x18` (placed), `+0x24` (handle), `+0x40` (type); a second call replaces the sign |
| `FUN_1402a1410(manager, tipo)` | a `NetSvrSummonSignManager` method: converts the type by the player's state (`FUN_14029c9b0`) and calls the one above |
| `FUN_14029fff0(manager, tipo)` | the neighbouring "can one be placed right now?", the check only (`FUN_1402a1bf0`); the item code calls it every frame — **only** on whoever has the soapstone under evaluation, it never ran on Samuel |
| `FUN_140291cb0`, `FUN_14024fb80` | item-use queries by type, not creation — this was the first mistake of this round |

The manager comes out of the game's getter, `FUN_1405132a0`:
`*(*0x141616cf8 + 0x30)` is the `NetSvrManager` (vftable `0x1410d53a8`), and
its `+0x78` field is the `NetSvrSummonSignManager` (vftable `0x1410d61f8`),
the same on both instances. The `NetSvrManager` wrappers that carry that
`+0x78` are slots 14 (place) and 17 (summon).

Careful with MemProbe's `chain`: the first dereference is implicit.
`chain x 1616cf8 30,78 8` is `*(*(*(base+0x1616cf8)+0x30)+0x78)`; with a `0`
in front it dereferences one time more and answers `chain_unresolved`.

### The hook

`DS2_PartyHook` (with `--seamless`) detours the `SummonSignSetCtrl` update
(`+0x2139d0`), which runs every frame on the host and on the guest, and
executes there, on the game thread, whatever arrived in `DS2_Party.req`:
`placa <tipo>` calls `FUN_1402a1410`; `status` says what the manager holds.
The rematch, armed by hand with `alvo` before any summon, uses the same
resolved manager.

### The result

Both hollow at Heide's bonfire, build `b30cadef`, `up --seamless
--keep-fog --auto-rematch`, **not a key pressed** on either instance:

    23:31:51  Chico   DS2_Party   placa tipo 1: "placa posta, alca 60000001"
    23:31:51  3:Chico             Sign 1015 created: type 1
    23:32:33  Samuel  DS2_Rematch "revanche: invocando a placa 80000021 do jogador 3"
    23:32:33  1:Samuel            Summoning sign 1015
    23:32:44  1:Samuel            RequestNotifyJoinGuestPlayer
    23:32:46  3:Chico             RequestNotifyJoinSession
              session             p2pSessionVerified: true

Almost a minute from the order to the session, most of which is the interval
of the host's sign poll. The session ended with `session end`.

### Coming in is born from the configuration (15/09)

`DS2PartyGuest` and `DS2PartyAccept` in `Injector.config` (with
`DS2SeamlessCoop`); in the harness, `up --seamless --party` makes account 2
the guest and account 1 the host that accepts account 2's configured steam id;
`--party-host 2` swaps them (measured 15/09: Chico hosted, Samuel came in by
himself, `p2pSessionVerified: true`).

- **Guest**, once a second on the `SummonSignSetCtrl` frame: in his own world
  (role 0) with no white sign, it places one. On coming back from another
  world (role 1 → 0), it places it again. When the sign disappears with him
  still in his own world, it waits 30 s: being summoned takes the sign away
  seconds before the role changes, and placing one again in that window races
  the join (seen once, with no damage, before the grace period). The manager
  clears the "sign placed" when the sign is summoned.
- **Host**, in the `AddSign` detour: it finds the entry by the handle
  (`FUN_14020e6f0` in the collection `*(this-8)`), reads the steam id at
  `+0x38` and, if it is in `DS2PartyAccept` and the host is in his own world,
  summons (`FUN_1402a14c0`).
- **`pausa` / `retoma`** in `DS2_Party.req` stop and restart both sides;
  `ds2os-dev session end` pauses by itself, and confirms by the echo.

Measured with both hollow, `DS2AutoRematch` off, not a key pressed and no
request file:

| time | what |
| --- | --- |
| 00:39:48 | Chico places the sign on arrival: `Sign 1016 created` |
| 00:40:19 | Samuel summons partner 76561199048087249 |
| 00:40:30–32 | `JoinGuestPlayer`, `JoinSession`, `p2pSessionVerified: true` |
| 00:41:21 | `session end` (build with no pause) |
| 00:41:39 | Chico, back, places another: `Sign 1017 created` |
| 00:42:19–32 | Samuel summons again, `JoinGuestPlayer`, `JoinSession` |

With the grace period and the pause (build `cc2a3272`): at 00:50:18 the guest
logged "a placa sumiu; espero 30 s", at 00:50:22 he was already a phantom, and
the session formed; `session end` paused both accounts and in two minutes the
server saw no sign, no summon and no join.

### The password (15/09)

**The channel.** The DS2 protocol's `MatchingParameter` has
`name_engraved_ring` (the Name-Engraved Ring, the game's native "password"),
and it goes in both `RequestCreateSign` and `RequestGetSignList`.
`DS2_PartyHook` detours `NetSvrSummonSignInterface::CreateSummonSign`
(`+0x29dfa0`, the parameter in the 4th argument) and `GetSummonSignList`
(`+0x29e230`, in the 5th) and writes into it
`0x80000000 | (FNV-1a(senha) & 0x7fffffff)`.

**Which word of the structure.** The client keeps the parameter as 16 `uint32`
in an order that is not the `.proto`'s. Writing 3 into one word at a time and
reading the server (which now logs the named fields of every sign created):

| word | field | word | field |
| --- | --- | --- | --- |
| [0] | calibration (20200) | [5] | **name_engraved_ring** |
| [1] | soul_memory | [6] | covenant |
| [2] | soul_level | [7] | unknown_7 |
| [3] | clear_count | [8] | cross_region |
| [4] | unknown_4 | [9] | unknown_9 |

`[10]` and `[11]` do not reach any field. Up to `0x80000000` the ring arrives
intact; the first probe, with large values in [5], [6], [10] and [11]
together, stopped the sign from leaving the client (no `RequestCreateSign` on
the server).

**The server.** `DS2_SignManager::CanMatchWith`, before the type and Soul
Memory rules: if the sign or the poll have bit 31 in the ring, it matches only
if both have it and the code is the same. The poll's log says the ring of
whoever asked.

**Measured** (build `d5219e06`, server with the filter), always with no key
pressed:

| case | poll of the one asking | result |
| --- | --- | --- |
| same password, host **with no** steam id list | Samuel, ring 2900300237 | `sent 1`; the host summons by the password; `JoinGuestPlayer`, `JoinSession`, `p2pSessionVerified: true` |
| different passwords | Samuel, ring 2594159542, sign with 2900300237 | `refused by matching 1` on three polls, no summon |
| public host, guest with a password | Samuel, ring 0 | `refused by matching 1` |
| host with a password, public sign | Chico, ring 2900300237, sign 1002 with ring 0 | `refused by matching 1` on every poll, no summon |

A detail of the last one: with his own sign on the ground the client does not
ask for other people's signs, so to see Chico's poll he had to be the host,
not the guest.

### Far from each other (15/09)

**What was in the way.** A sign is filed by area and cell, and the poll asks
for the cells around the one asking. With the guest in Majula and the host in
Heide, the sign never arrived.

**The server.** On a poll with a party code, after the normal pass (and before
the reply is sent — the first version sat after the `Send`, logged the offer
and never sent it), a pass over the whole cache offers the party signs with
the same code, reported under the area and the first cell the host asked for.
The client decodes the position from the sign's `player_struct`
(`FUN_14029cce0`: version 6, 0x50 bytes, `int16` with 5 fraction bits at
`+4/+6/+8`), so for a sign from another area that position is rewritten, in
that reply only, to the host's last position. The summon finds it by the id.

**What the guest does.** The destination of the entry warp (`FUN_1402c2a80`,
`motivo=4 forca=1`) comes from the guest's own sign, converted to the host's
map; the host's `player_struct` in the summon (71 bytes, version 4) carries
area, cell and name, not position. From Majula to Heide the point was always
(-7.81, -33.53, 372.06), in the void. With `observe` mode the guest died and
the session collapsed; with `respawn` the death hook cancels the fall and
takes him to the host's bonfire that the P2P channel announced, and the
session holds:

    03:00:28  Chico (Majula)  Sign 1004 created, area 0x009932c0
    03:01:21  Samuel (Heide)  Party sign 1004 ... moved to 6.4 -18.5 209.2; Summoning sign 1004
    03:01:38  Chico           morte CANCELADA queda; renascer: fogueira do host (6.186, -18.517, 209.053) teleportado
    03:01:38  Samuel          RequestNotifyJoinGuestPlayer
    03:01:40  Chico           RequestNotifyJoinSession        p2pSessionVerified: true

The staging has to be a real bonfire trip: with `goto-map` and a trip out to
the title the client ended up with one map's area and another's position, and
placed signs with position (0,0,0) that the host did not accept.

**Resuming revisits the cache.** A sign that arrives with the host paused is
not delivered again; `retoma` now walks the `SummonSignSetCtrl` collection
(slots `0x18` count and `0x10` item, a live entry with `+0x14` negative, a
handle with the `0x80000000` mark) and summons the partner's. Measured: sign
1005 ignored at 03:10:11 with the host paused, summoned at 03:10:29 on
resuming, an entry from Majula with the fall recovered and
`p2pSessionVerified: true`.

**Arrival, without waiting to fall.** Depending on the fall tied the entry to
`respawn` mode and to a point that happened to be empty. The death hook now
watches the arrival: when the local player's role goes from world owner to
something else, it remembers the map he came from, waits for the host's
bonfire announcement on the channel and, if the host is on a different map,
takes the guest there (the same `StartRecovery` as the fall). Two things
measured along the way, which brought down the first two versions:

- the local player's controller **survives** the entry warp — not a
  "controlador do jogador local" line on arrival —, so the trigger is the role
  change, not a new controller;
- at the arrival point there is no part under the feet, and the streamer's map
  reads **0**; requiring the host's map there never matched. Map 0 or the
  host's, with an announcement from another map, are enough (after 30 frames).

    03:50:40  Samuel  host: invocando a placa 80000011 do parceiro
    03:50:50  Chico   chegada de outro mapa: vim de 0a040000, o host esta em 0a1f0000, sob os pes 00000000
    03:50:50  Chico   levando para fogueira do host (6.186, -18.517, 209.053) ... teleportado; anunciada ha 1590 ms
    03:50:50  Chico   concluido em 2 quadros                      p2pSessionVerified: true, nenhuma morte

Repeated at 03:53:50 after `session end` and `retoma`, the same. The control,
with Chico taken to Heide by a bonfire trip: entry at 03:57:50 on the same
map, session verified, not a single arrival line.
