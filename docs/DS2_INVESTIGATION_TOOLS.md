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

Three things learned the hard way:

- **Lengths are decimal.** `1024`, not `400`.
- **A scan finds its own needle.** Every scan answers with one hit in the
  probe's own memory, where the value it was asked for is kept (`0x9c5fb08`,
  `0x9d5fb08`, `0x9fffb08`... - low, and the same for any value in one boot).
  A vftable that is not in the process still returns that one hit.
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

## Onde o jogador está, e como levá-lo a algum lugar

`DS2_NavHook` publica a posição do jogador local em `DS2_Nav.txt`, ao lado da
DLL, reescrito inteiro a cada 50 ms:

    <x> <y> <z> <facing x> <facing z> <ponteiro> <amostra>

A cadeia é a mesma que o próprio jogo usa quando um convidado entra no mundo do
host e precisa dizer onde está (`FUN_1402c2a80`):

    jogador  = *(*(*(0x1416148f0) + 0xa8) + 0xc0)
    posição  = jogador + 0xa8      três floats, x y z
    direção  = jogador + 0xbc e + 0xc4, normalizada

**Há mais duas triplas de posição logo antes de `+0xa8`** e elas não seguem o
personagem — dá para escolher a errada e ficar com um valor que nunca muda.
A boa foi achada andando e relendo; é o único teste que separa as três.

O **contador de amostra** no fim não é enfeite. Um leitor não distingue um
personagem parado de um arquivo que parou de ser escrito, e essa diferença é
exatamente "a caminhada chegou" contra "a caminhada travou". Custou um
diagnóstico errado: a primeira tentativa relatou `travou depois de 6 passos,
ainda a 1,8 m` com o personagem **em cima do alvo**, lendo uma posição de três
passos antes. O publicador escreve por `rename`, o `rename` falha às vezes sob
Wine enquanto o leitor tem o arquivo aberto, e engolir esse erro deixa a
amostra velha no lugar parecendo atual.

O **ponteiro não serve para distinguir as instâncias**: sem ASLR, as duas
cópias do jogo caem no mesmo endereço de heap e publicam o mesmo valor. Duas
leituras idênticas nos dois arquivos são um resultado plausível, não um bug —
foi assim que se descobriu que a caminhada tinha chegado.

Com isso o harness anda sozinho:

```
ds2os-dev where
ds2os-dev goto --instance 1 --to-instance 2
```

`--to-instance` é "vá para onde o outro está", que na prática é "pise na placa
dele": o convidado põe a placa onde está parado, e os dois mundos usam as
mesmas coordenadas, então não é preciso descobrir a posição da placa em lugar
nenhum.

O stick é **relativo à câmera**, e a câmera gira junto com o personagem, então
não existe um mapeamento fixo para aprender uma vez. Cada passo mede o
deslocamento que produziu: o ângulo entre o que foi pedido e o que aconteceu é
a guinada da câmera, suavizada no passo seguinte. Cair e travar são relatados,
não combatidos — um teste que dependia da caminhada falha dizendo o que houve
em vez de estourar o tempo.

Três coisas que custaram caro para descobrir, e que valem para qualquer coisa
que dirija o jogo:

- **O foco tem que ser reafirmado a cada vez.** Não basta a janela já estar
  ativa: o jogo para de aceitar o controle virtual se o foco não for reclamado
  de novo. Um atalho que devolvia cedo quando `_NET_ACTIVE_WINDOW` já apontava
  para a janela fez toda caminhada andar no primeiro passo e congelar depois —
  e isso se parece exatamente com terreno bloqueado. Foram gastas horas
  culpando a fogueira.
- **O eixo Y do controle chega ao mundo invertido.** O mapeamento é uma rotação
  **composta com um espelho**, e um espelho não é absorvível por uma estimativa
  que só sabe girar: errar isso faz o personagem andar firme para longe do alvo
  enquanto a estimativa persegue o próprio rabo. Medido, não chutado: stick
  para a direita deu ângulo −164,8° no mundo e stick para a frente −82,2°, e
  frente só é +90° de direita sob essa leitura.
- **O personagem gira antes de andar.** Um burst que acaba durante a virada não
  cobre chão nenhum — um de 700 ms depois de noventa graus mediu deslocamento
  zero. Por isso os passos são de 1,2 s, crescem quando rendem pouco, e só
  passos que cobriram mais de meio metro têm direito de ensinar a estimativa.

O que ela **não** faz: desviar de obstáculo. Ela varre as oito direções quando
para de sair do lugar, o que resolve encavalar, mas não contorna geometria. Na
prática chega a 2 m do alvo em terreno com degraus, que é folga de sobra para
pisar numa placa, e é por isso que o raio padrão é 2 m.

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

### Rearmar o mesmo endereço exige `clear`

O tracer guarda todo endereço que já armou, e `Arm` desiste silenciosamente se
o endereço já está no mapa. Mandar `bp 2a14c0` uma segunda vez **parece
funcionar** — o log responde `=== armados 1 enderecos ===` — e não arma nada.
Custou uma rodada inteira de duelo até o hit que não veio explicar isso.

Para medir a mesma função duas vezes:

```
clear
bp 2a14c0 deref rdx 4
```

### O endereço tem que ser o início de uma instrução

`bp` escreve `0xCC` no byte pedido, sem conferir se é o começo de uma
instrução. Armado no meio de uma (`bp 26bdab`, dentro de um `movsd` de 5 bytes,
em 14/09), só não derrubou o jogo porque o salto anterior desviou antes; o
primeiro fluxo que passasse por ali executaria uma instrução cortada. Confira o
endereço num objdump da faixa antes de armar, e mande `clear` se errou.

### A hora de cada alcance

A linha do alcance não tem hora. Para medir quanto tempo separa dois pontos
(a mancha online: a chamada no quadro da morte, o job cinco segundos depois),
carimbe o log por fora enquanto ele cresce:

```
tail -n0 -F DS2_Trace.log | while IFS= read -r l; do echo "$(date +%T.%3N) $l"; done
```

## A vigia de escrita: quem grava este endereço

`DS2_Trace.req` aceita, desde 13/09:

```
wp <endereço absoluto em hex> <bytes> [segundos, padrão 3, máximo 20]
wpclear
```

e responde, quando o prazo acaba, com **cada instrução distinta** que escreveu
no intervalo, quantas vezes, os registradores do primeiro acerto e a pilha:

```
=== vigia de escrita em 00007fffe3649ad0 encerrada (prazo): 5040 faltas na pagina, 2 instrucoes ===
  escreveu em +0x314df1 (180x) rax=... rbx=... rcx=... rdi=00007fffe3651600 ... pilha: +0x322827 ...
  escreveu em +0x36df42 (180x) rax=00007fffe3649a40 ... rsi=00007fffe3650fc0 ... pilha: +0x370c0e ...
```

Uma escrita por quadro aparece com ~60 acertos por segundo, e o registrador
que aponta para a origem do valor costuma estar entre os de cima — foi assim
que a posição do personagem foi seguida, em quatro passos, até o corpo do
Havok.

**Não é um watchpoint de hardware, e isso é de propósito.** Sob o Wine, os
registradores de depuração de outra thread são gravados pelo wineserver via
`ptrace`, e com `/proc/sys/kernel/yama/ptrace_scope = 1` (o valor desta
máquina) esse attach é recusado e a gravação se perde sem erro. A vigia usa
proteção de página: a página do alvo fica só-leitura, toda escrita nela falta
com o endereço exato da instrução, e a página é liberada por uma instrução com
o trap flag antes de ser protegida de novo.

O custo é que **toda** escrita na página de 4 KB falta, não só as do alvo —
numa página de heap ao lado do `PlayerCtrl`, 5 a 15 mil faltas em 3 segundos.
O jogo aguenta isso, mas por isso toda vigia tem prazo. Ela recusa páginas que
não sejam de dados graváveis e convive com o single-step do
`DS2_ForceMultiPlayZoneHook`.

**Até 13/09 ela podia derrubar o jogo ao ser levantada.** Uma escrita que falta
com a página ainda só-leitura pode chegar ao handler depois que a thread de
pedidos desarmou; o handler via a vigia desligada, passava a falta adiante, e o
processo morria com `0xC0000005`. Aconteceu numa página do
`IngameCameraOperator` com 24 mil faltas por segundo, no instante exato em que
a vigia encerrou. Agora a página da última vigia fica guardada, e uma escrita que
falta nela com a página já gravável é só repetida. A DLL anterior ao commit
`trace: a vigia de escrita nao derruba mais o jogo` ainda tem a corrida.

## A telemetria de posição não acompanha teleporte

`DS2_Nav.txt` (e portanto `where` e o `goto`) lê a posição de
`*(*(*(ctx)+0xa8)+0xc0)+0xa8`, um bloco de dados sem vtable. Ele acompanha a
caminhada, mas **não** acompanha uma posição escrita no corpo físico: depois de
um teleporte de 69 m ele continuou mostrando a fogueira de onde o personagem
saiu, enquanto o servidor e a tela já mostravam o destino. A posição viva é a
translação do `PlayerCtrl` (`ctx+0xd0`, campo `+0x90`), que é o que o getter
virtual `+0x148` do jogo devolve.

## O canal do co-op: `DS2_Channel.req`

`DS2_CoopChannelHook` (passo 7, [DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md),
"A fogueira do host") responde `status` em `DS2_Channel.log`:

```text
=== canal 7: polls=4657 estranhos=0 enviados=22 falhas=0 recebidos=0 recusados=0 eu=011000010afd1a3a ===
    sessao 00007FFFFE5BFA00 vista ha 3 ms: 2 membros: 011000010afd1a3a (host) 0110000140d6d6d1
    local ha 7 ms: papel 0, registro mapa 0a1f0000 tipo 0 id 00007ba7
    do host: nada recebido
```

- `polls` conta as chamadas ao poll da sessão; parado em sessão quer dizer que
  o jogo não está consultando a sessão. `estranhos` é o detour chamado com um
  objeto que não é `SteamSessionLight`.
- `enviados`/`falhas` são os `SendP2PPacket` do host; `recebidos`/`recusados`,
  os pacotes lidos no canal 7. Um host só envia, um convidado só recebe.
- `sessao` lista os membros como o jogo os guarda, com `(host)` onde a marca
  `+0xad` está ligada. Fora de sessão não há objeto e a linha some.
- `local` é o que a thread do jogo publicou no último quadro; "ha" acima de 2 s
  quer dizer carregamento, e o host para de anunciar.
- `do host` é o último anúncio guardado; o renascer só o usa com menos de 30 s
  e vindo de quem ainda é host.

Fora do `status`, o log escreve uma linha quando os membros mudam, quando o
anúncio muda (host) e quando o anúncio recebido muda (convidado).

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
- **A teleport keeps the character's facing, and a bonfire only offers "Rest"
  to someone facing it.** Standing on the exact spawn point after a teleport
  gave no prompt. The stick moves the camera too, so "down" changes meaning
  between presses: measure what a short press did to the position, convert the
  world direction to a stick direction, and take the last step *toward* the
  bonfire. The prompt came up 0.16 m from the spawn point.
- On the character list, **X is Delete**. Only ever press A there.
