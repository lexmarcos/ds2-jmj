# Working on the running game

Everything below exists and works. It is written down because the slow part of
this investigation was building it, not using it.

## Before trusting any reading

**Confirm the item works in Heide first.** The Red Sign Soapstone is refused
while the character is hollow, and a hollow refusal looks exactly like the
Majula one - no animation, no message, nothing sent. A death costs hours if it
goes unnoticed. Use a Human Effigy ("Reverses hollowing") and re-check.

The server is **not** a witness. DS3OS logs nothing for a sign that is plainly
placed.

## The oracle

`trial.sh` presses the item and answers used-or-refused by watching the
character. It captures a reference frame, presses X, captures five more, and
compares a crop of the torso only - above the interaction banner, below the
HUD, away from the bonfire flame, all three of which produced false positives
before the crop was tightened.

The verdict is **sustained** change across the last three frames, not a peak. A
real animation holds the character in another pose for seconds; a flickering
prompt or a fire is a one-frame spike. Calibrated in both directions:

```text
item used            7-11%
two-handing with Y   9.9%
item refused         0.2-2.5%
```

## Reading and writing the game

`DS2_MemProbeHook` answers requests dropped in `DS2_MemProbe.req` beside the
injector and writes to `DS2_MemProbe.log`. It runs inside the process, which
matters: `/proc/<pid>/mem` works too, but only from an ancestor of the game,
and Steam reparents it out of reach.

```text
abs   <label> <hex address> <decimal length>
mod   <label> <hex offset from module base> <decimal length>
chain <label> <hex offset> <off,off,...> <decimal length>
pokeabs/pokemod/pokechain <label> <where> [<offsets>] <hex bytes> [<expected>]
scan  <label> <hex value> <width 1|2|4|8> [max hits]
```

Two things learned the hard way:

- **Lengths are decimal.** `1024`, not `400`.
- **Always pass the expected bytes on a poke.** Addresses a scan reported go
  stale, and writing to one that has been reused killed the game twice. With an
  expectation the write is refused instead. It also caught a wrong instruction
  encoding once: `xor al,al` is `32 c0`, not `30 c0`.

Write the request file **atomically** - build it elsewhere and `mv` it into
place. The probe polls twice a second and will happily read a half-written
file.

## Tracing what actually ran

`DS2_TraceHook` answers `DS2_Trace.req`:

```text
bp <hex offset from module base>
clear
report
```

Breakpoints are one-shot: the first hit records the address with `rcx/rdx/r8/r9`
and restores the byte for good. A hot function costs one exception instead of
thousands.

The method that worked:

1. Arm a range of function entries. `.pdata` lists every function in the
   binary - 95,446 of them - with exact bounds:
   `python3 pdata.py <lo> <hi>` prints entry and size.
2. Let the game idle ten seconds. Per-frame functions fire and disarm
   themselves, leaving a clean set.
3. Press the button. What fires now is what the press reached.
4. Do it in both areas and take the difference.

Limits found by hitting them: about 600 breakpoints is comfortable, 3229 killed
the game when the character moved. A second thread can reach an address between
the first restoring the byte and the handler running, so the handler owns an
address whether or not it is still armed - without that it died immediately.

## O breakpoint que segue um ponteiro

`DS2_Trace.req` aceita, desde 12/09:

```
bp <deslocamento hex>
bp <deslocamento hex> deref <registrador>[+<hex>] <bytes>
```

Registradores: `rcx rdx r8 r9 rax rbx rsi rdi`; no máximo 64 bytes.

Isto existe porque **metade do que interessa neste binário está atrás de um
ponteiro**, e o ponteiro morre antes de qualquer sonda conseguir responder. O
ponto de entrada do summon recebe um `SignHandle` por endereço; o objeto que
decide se uma morte desfaz a sessão guarda o tipo em `+0xe0` e é reciclado em
segundos. Tentar ler esses endereços depois, com `DS2_MemProbe.req`, devolve
memória já reaproveitada — foi medido, e a leitura tardia deu um ponteiro de
heap onde deveria haver um byte de tipo.

Exemplos reais:

```
bp 2a14c0 deref rdx 4        # o SignHandle que o host invocou
bp 190950 deref rcx+e0 1     # o tipo que decide se a morte encerra as sessões
```

A leitura é protegida: um registrador pode apontar para qualquer coisa, e uma
falha dentro de um handler vetorizado leva o jogo junto. Endereço ilegível sai
como `[rcx=... ilegivel]` em vez de virar crash.

## A varredura de breakpoints, com a lista vinda do Ghidra

O `pdata.py` mencionado acima não existe mais. `Entries.java`, em
`/home/suel/tools/scripts`, faz o mesmo trabalho lendo a lista de funções do
próprio Ghidra e imprimindo os **deslocamentos de módulo**, um por linha:

```
analyzeHeadless ... -postScript Entries.java <saida> 0x140270000 0x1402a0000
```

O método, confirmado em 12/09 achando o que o botão A numa placa de invocação
alcança:

1. `head -520 <saida> | sed 's/^/bp /' > <install>/DS2_Trace.req`
   (acima de ~600 o jogo fica instável, e 3229 já matou o processo)
2. deixe o jogo parado uns quinze segundos: o que roda por quadro dispara e se
   desarma sozinho
3. **apague o `DS2_Trace.log`** — é o que separa o ruído do que você quer
4. aperte o botão
5. o que aparecer no log novo é o que a ação alcançou

Na prática o primeiro toque (abrir o diálogo "Summon this dark spirit?") deixou
duas funções: uma é alocador de pool, o que por si só diz que o toque
**alocou** alguma coisa. O confirme deixou onze, com as pilhas inteiras.

Vale saber o que o ruído parece: `FUN_140279d50` é inicialização de free-list,
e `FUN_140276050`/`FUN_140276110` são invólucros de envio de pedido — pegam um
subsistema, pedem um id ao objeto pelo slot virtual `+0x58` e repassam. Um
alvo de verdade tem os campos do pedido nas mãos.

## Reading the binary

`objdump` reads the PE directly and dumps all 27MB in about three seconds:

```bash
x86_64-w64-mingw32-objdump -d --no-show-raw-insn DarkSoulsII.exe > ds2.asm
```

That is a grep-able 5.6M line listing and it answered "who calls this" faster
than Ghidra opens the project. The image has two `.text` sections and a `.bind`
section, so linear disassembly has misaligned stretches; a reference that greps
to nothing may still exist inside one.

`rtti.py <ClassName>` maps a class name to its vtable through the RTTI
descriptors, and `vt.py <address>` finds the vtable a function sits in.

## Driving the game

`ds2os-dev pad` and `game shot` cover it. Things that cost time to learn:

- **Several interaction prompts can overlap and `Y` cycles between them.** A
  bonfire will not respond to `A` while "Touch your bloodstain" is the active
  prompt. This is the single biggest source of lost minutes.
- Standing **on** a bonfire gives no prompt; stand beside it.
- The d-pad works in menus and is how to move a selection; the left stick does
  not move menu selections.
- Quit cleanly through the menu: Start, RB five times to the gear, down twice to
  Quit Game, A, then **left** to YES. The confirm defaults to NO.
- Loading a save reliably leaves the character at the bonfire with a working
  prompt, which is often faster than walking back to one.
- On the character list, **X is Delete**. Only ever press A there.
