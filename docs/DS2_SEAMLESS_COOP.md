# Co-op seamless: o warp, e o que ele já resolve

O objetivo: dois jogadores atravessando o jogo juntos de ponta a ponta, e uma
morte — de qualquer um dos dois — mandando o jogador para a **última fogueira**
em vez de desfazer tudo e mandá-lo de volta ao próprio mundo.

Este documento começou em 12/09 como a lista do que se sabia; hoje ele registra
o caminho inteiro do warp, porque foi ele que se abriu. Para a cadeia de fim de
sessão, ver [DS2_SESSION_END_CLIENT.md](DS2_SESSION_END_CLIENT.md); para a
revanche por placa vermelha, [DS2_REMATCH_AFTER_DEATH.md](DS2_REMATCH_AFTER_DEATH.md).

## As três coisas que a morte de um fantasma faz

Confundi-las custou uma madrugada:

1. **Encerrar as sessões.** Uma tabela em `0x1410c0050`, vinte entradas de 16
   bytes indexadas por um papel lido de `objeto+0xe0`, e o byte `+1` de cada
   entrada decide se aquele papel desfaz as sessões ao morrer. O fantasma é o
   **índice 7**; o byte dele mora em `0x1410c00c1`.
2. **Demolir a sessão.** `FUN_1402c3900`, o handler do estado 8 da máquina de
   estados da sessão. Manda o `RequestNotifyLeaveSession` e põe o estado em 9.
3. **Mandar o jogador para casa.** Um **warp** — e é este que dá o co-op.

Medido: zerar o byte da tabela para o índice 7 **não** impede o retorno. O
fantasma vê "You have been vanquished. Returning to your world…" do mesmo
jeito; muda só se as sessões são desfeitas junto. Suprimir a tabela deixaria o
jogador indo para casa com a sessão pendurada — pior que o original. O warp é a
peça.

## Todo warp do jogo passa por uma função só

`0x1401c2a80` (módulo `+0x1c2a80`), alcançada pelo slot virtual `+0x40` do
contexto global do jogo, `*(0x1416148f0)`:

```
mov  rcx, [0x1416148f0]
lea  rdx, <pedido>
xor  r8d, r8d
mov  rax, [rcx]
call [rax+0x40]
```

Assinatura: `void Warp(void* Contexto, WarpRequest* Pedido, uint8_t Flag)`.

A primeira coisa que ela faz é ler o **segundo dword do pedido**, em
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

Depois disso ela copia os `0x38` bytes do pedido para `contexto+0x24c8` e liga
bits em `contexto+0x24b1` e `+0x24b2`. O warp é **enfileirado**, não executado
ali — o que explica por que um `bp` no fim da cadeia nunca mostrava o destino.

## O pedido, 0x38 bytes

O jogo tem um construtor para ele, `FUN_14044ed40(registro, saida)`, e foi ele
que deu os nomes:

| campo | quem escreve | o que é |
| --- | --- | --- |
| `+0x00` | `3`, `4` ou `2` conforme `registro+0x168` | família do destino |
| `+0x04` | sempre `1` no construtor | **motivo** — é o que a entrada lê |
| `+0x08` | `registro+0x164` | id do mapa (`0x0a1f0000` = Heide's Tower of Flame) |
| `+0x0c` | fica `-1` da inicialização | — |
| `+0x10` | fica `0` | — |
| `+0x14` | byte `3` | — |
| `+0x18` | `registro+0x16c` | ponto de nascimento dentro do mapa |
| `+0x1c`…`+0x34` | só o caminho de cópia | — |

Quando o construtor não reconhece o registro ele copia os `0x38` bytes inteiros
do destino padrão, que vem de `FUN_14039a9a0`.

### A tabela de motivos, e a armadilha que ela esconde

O que está medido até agora, cada linha vinda de um pedido capturado em
execução:

| motivo | terceiro argumento | quem pede | o que é |
| --- | --- | --- | --- |
| `1` | `0` | `FUN_140190920` → `FUN_14044fde0` | morte comum: a última fogueira |
| `4` | `0` | `FUN_1402c3900`, `0x1402c3bdb` | **volta forçada para o próprio mundo** |
| `4` | `1` | `0x1402c2e45` | a entrada do convidado **no mundo do host** |

**O motivo 4 sozinho não quer dizer "para casa".** A primeira versão do hook
trocou todo motivo 4 e quebrou a invocação: o convidado viu "Summoning
canceled.", o host viu "Summoning failed. The sign has disappeared.", e a
sessão nunca nasceu. As duas coisas passam pela mesma porta com o mesmo motivo.

Quem separa é o **terceiro argumento** — e não é palpite, é o que a própria
entrada testa: motivo 4 só pula o portão de permissão quando o argumento é
zero. O desmonte manda `xor r8d,r8d` (`0x1402c3bd8`); a entrada no mundo do
host manda `mov r8b,1` (`0x1402c2e42`). O hook testa exatamente isso.

Qualquer outro motivo precisa passar por `0x140248940`; nenhum outro foi visto
ainda. O `DS2_Seamless.log` escreve uma linha por warp, com motivo, argumento e
o endereço de retorno de quem pediu, e é por ele que essa tabela cresce.

### Os dois pedidos, byte a byte

Morte comum (Samuel caiu na água em Heide's Tower of Flame), capturada com
`bp 1c2a80 deref rdx 56`:

    +0x00  03 00 00 00   família 3
    +0x04  01 00 00 00   motivo 1 - última fogueira
    +0x08  00 00 1f 0a   mapa 0x0a1f0000, Heide's Tower of Flame
    +0x0c  ff ff ff ff
    +0x10  00 00 00 00
    +0x14  03 eb b0 c1   o byte 3; o resto é lixo de pilha
    +0x18  a7 7b 00 00   ponto 0x7ba7
    +0x1c  48 6e 3d 41   lixo de pilha daqui para baixo

Volta forçada de um convidado, capturada com `bp 2c3bdb deref rdx 32` numa
morte de fantasma no mundo do host:

    +0x00  03 00 00 00
    +0x04  04 00 00 00   motivo 4 - de volta ao próprio mundo
    +0x08  00 00 1f 0a   mesmo mapa: o duelo foi ali
    +0x0c  ff ff ff ff
    +0x10  00 00 00 00
    +0x14  03 7f 00 00
    +0x18  a7 7b 00 00

Duas medições da mesma morte comum, de posições diferentes, mudaram só
`+0x15..+0x17` e `+0x1c` — que é exatamente o que o construtor **não** escreve.
Isso é a prova de que o resto do struct é lixo de pilha e não campo.

## O registro de renascimento está pendurado no mesmo contexto

O que faz a substituição ser barata:

```c
void FUN_140190920(longlong param_1)
{
  FUN_14044fde0(*(undefined8 *)(DAT_1416148f0 + 0x70));
  *(undefined1 *)(param_1 + 0xce) = 1;
}
```

`FUN_14044fde0` é "mande o jogador para onde ele descansou por último": monta o
pedido pelo construtor e chama o warp. O único argumento é o registro de
renascimento, e ele mora em `*(contexto + 0x70)` — o mesmo contexto que chega
como primeiro argumento do warp. Ou seja, **de dentro do hook dá para pedir o
renascimento do próprio jogo**, sem montar struct nenhum à mão.

## O hook

`Source/Injector/Hooks/DarkSouls2/DS2_SeamlessCoopHook.{h,cpp}`, ligado por
`DS2SeamlessCoop` no `Injector.config` (`ds2os-dev up --seamless`, ou
`game prepare --seamless`). Um detour só, em `+0x1c2a80`, com os bytes de
`+0x1c2a80` e `+0x44fde0` conferidos antes de escrever qualquer coisa:

```
se o pedido tem motivo 4 e o terceiro argumento é 0:
    FUN_14044fde0(*(contexto + 0x70))   // última fogueira, do jeito do jogo
senão:
    passa adiante
```

Há uma trava de reentrância porque o warp substituto passa pela mesma entrada.
Escrever `0` em `DS2_Seamless.req` desliga a troca e deixa só o registro;
qualquer outra coisa religa. O log fica em `DS2_Seamless.log`, ao lado da DLL,
e sai uma linha por warp com motivo, mapa, ponto e o endereço de retorno de
quem pediu — o `de=+0x...` é o que identifica o caminho, e o `cru=` traz os
`0x38` bytes inteiros, porque metade deles ainda não tem nome e uma linha que
só imprime a metade nomeada não responde pergunta que ninguém fez ainda.

### Como ligar, e como jogar um co-op hoje

```
ds2os-dev up --auto-rematch --seamless
```

`--seamless` liga este hook; `--auto-rematch` liga o
[DS2_RematchHook](DS2_REMATCH_AFTER_DEATH.md), e os dois juntos são o laço que
existe hoje:

1. o convidado usa a **Small White Sign Soapstone** ou a
   **White Sign Soapstone** (inventário → categoria de consumíveis → 7 para
   baixo, 2 para a direita → Use; funciona hollow);
2. o host invoca **sozinho**, sem apertar nada, assim que a placa chega — é o
   hook da revanche, que não olha o tipo da placa;
3. jogam juntos;
4. quem morre vai para a última fogueira e a sessão acaba;
5. o convidado põe a placa de novo, e o passo 2 se repete.

O passo 5 é o único que ainda precisa de mão, e é o próximo pedaço óbvio.

### Onde começa o passo 5

O caminho da placa já está localizado, por breakpoint numa colocação de placa
branca de verdade. `FindImmediate.java 0x394` (o id de `RequestCreateSign`) dá
cinco candidatos; colocar a placa acendeu **dois**:

    alcancado +0x6a1de0 de=+0x29ec4a ...
    alcancado +0x6a1170 de=+0x284f52 ...

e o servidor registrou `Sign 1000 created: type 1` logo depois. O interessante
é o chamador do primeiro:

```c
void FUN_14029ec00(longlong trabalho, longlong *dono, undefined8 p3)
{
  interface = (**(code **)(*dono + 0xe8))(dono);
  (**(code **)(*interface + 8))
      (interface, p3, trabalho[0x28], trabalho[0x2c], trabalho+0x30,
       trabalho[0x5c], trabalho+0x60);
}
```

É o mesmo desenho Manager / Interface / Job do resto do subsistema
([DS2_CLIENT_NETSVR_API.md](DS2_CLIENT_NETSVR_API.md)): o **job** em
`param_1` já carrega tudo que a placa precisa, e quem o monta está acima, em
`+0x286239` na pilha capturada. Repor a placa sozinho é achar esse construtor e
chamá-lo — o mesmo movimento que o `DS2_RematchHook` já faz do lado do host.

## O resultado que vira o enunciado do avesso

Com o hook instalado e o redirecionamento ligado, um fantasma morreu no mundo
do host. O log do convidado, de cima a baixo:

    warp motivo=4 forca=1 tipo=0 mapa=0a1f0000 ponto=c26abe77 de=+0x2c2e48   <- entrada, passou
    warp motivo=4 forca=0 tipo=3 mapa=0a1f0000 ponto=00007ba7 de=+0x2c3bde   <- volta forçada
    co-op: em vez de voltar para o proprio mundo, ultima fogueira
    (reentrada) motivo=1 forca=0 tipo=3 mapa=0a1f0000 ponto=00007ba7 de=+0x44fe22

Funcionou: só a volta forçada foi trocada, a entrada passou intacta e a sessão
nasceu (`RequestNotifyJoinGuestPlayer` seguido de `RequestNotifyJoinSession`).
Mas **os dois pedidos apontam para o mesmo lugar** — `tipo=3`, mesmo mapa,
mesmo ponto `0x7ba7`. E `0x7ba7` é o ponto da última fogueira do convidado:
está medido à parte, numa morte comum dele no próprio mundo, que produziu
`motivo=1 tipo=3 mapa=0a1f0000 ponto=00007ba7`.

Ou seja: para um **invasor de placa vermelha**, o Dark Souls II já manda para a
última fogueira quem morre. O enunciado "em vez de voltar para o mundo, volta
para a última fogueira" já é o comportamento de fábrica nesse caso — e por um
tempo isso pareceu ser o fim da história. Não é: **depende de quem morreu.**

O código confirma sem depender da medição. `FUN_1402c3900` monta **dois**
destinos e escolhe um:

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

Os dois retratos são tirados **na entrada**, no handler do estado 2: o registro
de renascimento (`*(contexto+0xe)` → `+0x164/+0x168/+0x16c`) e a posição do
jogador no próprio mundo.

E quem decide é uma **linha de param**, não código:

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

Três bytes por papel — `+0x2c`, `+0x2d`, `+0x2e` — dizem, para cada motivo de
fim de sessão, se o jogador volta para a fogueira ou para onde estava. É um
interruptor de dados, e mexer nele não precisa de detour nenhum.

### O co-op escolhe a outra forma, e aí o hook faz diferença

A mesma medição, feita de novo com um **fantasma branco de co-op** — placa
branca, `Sign ... type 1`, invocada sozinha pela revanche — dá o contrário.

Com o redirecionamento **desligado**:

    warp motivo=4 forca=0 tipo=0 mapa=0a1f0000 ponto=40c5efa4 de=+0x2c3bde
    cru=00000000 04000000 00001f0a ffffffff 00000000 03000000
        a4efc540 002294c1 9a0d5143 0000803f ...

`tipo=0` é a **forma posição**, e os três floats são `6.1855, -18.516, 209.05`
— exatamente onde o convidado estava no próprio mundo quando foi invocado, o
mesmo que o servidor tinha registrado (`position 6.2 -18.5 209.1`).

Com o redirecionamento **ligado**, mesma morte:

    warp motivo=4 forca=0 tipo=0 mapa=0a1f0000 ponto=40c5efa4 de=+0x2c3bde
    co-op: em vez de voltar para o proprio mundo, ultima fogueira
    (reentrada) motivo=1 forca=0 tipo=3 mapa=0a1f0000 ponto=00007ba7 de=+0x44fe22

A forma posição virou a forma fogueira. **Para o co-op, que é o caso do
enunciado, o hook muda o destino de verdade.** Por isso ele fica ligado; para
o invasor de vermelho ele continua sendo um não-operação, o que é correto.

Resumo do que os três bytes de param decidem, medido:

| quem morre | forma escolhida pelo jogo | o hook muda? |
| --- | --- | --- |
| invasor de placa vermelha | fogueira (`tipo 3`, ponto do registro) | não, já era |
| fantasma branco de co-op | posição (`tipo 0`, onde ele estava) | **sim** |

### O host que morre leva o convidado pelo mesmo caminho

A terceira medição, a que faltava: o **host** morreu com o fantasma de co-op
dentro. Os dois logs, lado a lado:

    host     warp motivo=1 forca=0 tipo=3 ponto=00007ba7 de=+0x44fe22
    convidado warp motivo=4 forca=0 tipo=0 ponto=40c5efa4 de=+0x2c3bde
              co-op: em vez de voltar para o proprio mundo, ultima fogueira
              (reentrada) motivo=1 forca=0 tipo=3 ponto=00007ba7 de=+0x44fe22

O host faz uma morte comum e vai para a própria fogueira — o hook nem encosta,
porque o motivo é 1. E o convidado passa **exatamente pelo mesmo caminho** de
quando é ele quem morre: mesma função, mesmo motivo, mesma forma posição, mesma
troca. Ou seja, um hook só cobre os dois casos do enunciado, "se o phantom ou o
host morrer", sem nenhum código específico para cada um.

**Mas o que uma morte custa a um co-op continua não sendo o lugar de chegada.
É a sessão** — e essa ainda acaba.

## O que isto **não** faz

Ser honesto aqui importa mais que a feature:

- **A sessão continua acabando.** O `RequestNotifyLeaveSession` sai antes do
  warp, e a demolição em `FUN_1402c3900` não é tocada.
- Para um invasor de vermelho **o destino não muda**: o jogo já escolhe a
  fogueira sozinho. Para o fantasma de co-op muda, e é o caso do enunciado.
- Portanto isto ainda não é o co-op seamless do enunciado. Para "zerar o jogo
  de ponta a ponta juntos" faltam duas coisas, e só uma delas é código nosso:
  1. a sessão sobreviver a uma morte, o que o jogo nunca faz — um fantasma
     nunca carrega área nenhuma dentro do mundo do host;
  2. ou, aceitando que ela acabe, a dupla se reencontrar sozinha — que é
     exatamente o que a revanche por placa vermelha já faz.
- Com dois jogadores em uma máquina não dá para testar três; ver
  [DS2_TO_VALIDATE.md](DS2_TO_VALIDATE.md).

## A sessão é uma máquina de estados, e o warp é o que ela usa para mover gente

O objeto da sessão guarda o estado em `objeto+0xf8`, e cada estado tem o seu
handler. Dois interessam:

| estado | handler | o que faz |
| --- | --- | --- |
| `2` | `FUN_1402c2a80` | leva o convidado **para dentro** do mundo do host |
| `8` | `FUN_1402c3900` | desmonta a sessão e manda o convidado para casa |

O handler do estado 2 monta o pedido assim — e este é o molde de "ponha o
jogador **aqui**, neste mapa":

    +0x00  0            tipo 0
    +0x04  4            motivo
    +0x08  mapa         vem do pedido que chegou pela rede
    +0x14  byte         de FUN_1402d4830
    +0x18  x, y, z      posição
    +0x24  1.0f
    +0x28  ...          orientação, normalizada ali mesmo

e chama o warp com o terceiro argumento **1**. O retorno importa: se o warp
recusa, o handler joga a máquina para o estado `0x13` (`0x1402c2ee6`). Ou seja
**o warp devolve um byte**, e um detour declarado `void` entrega ao chamador o
que sobrou em `al` — foi um bug real deste hook, corrigido.

### O estado 7 só sai por um campo

E o desmonte não começa sozinho. O handler do estado 7 — `FUN_1402c3830`, o
estado em que a sessão fica enquanto se joga — tem uma linha só que interessa:

```c
if (*(int *)(sessao + 0x1cc) != 0) {
    sessao[0x1f] = 8;          // +0xf8, o estado: vai para o desmonte
}
```

`+0x1cc` é o **motivo do fim**, e quem escreve nele é `FUN_1402c2f20`, o slot
**`+0xa0`** da sessão — "encerre, e este é o motivo". (Este parágrafo dizia
`+0x30` até 13/09; o slot `+0x30` é `FUN_1402c2820`, e a chamada medida em
`de=+0x2c9246` é `call *0xa0(%rax)`. O hook sempre funcionou porque detoura a
função, não o slot.) Ele confere um virtual
`+0xa8` antes, guarda `FUN_1402d4750(papel, motivo)` em `+0x1c8` e avisa o
resto do jogo.

Ou seja: **manter `+0x1cc` em zero mantém a sessão no estado 7**, e recusar
aquela função é suficiente para isso — não é palpite, é o único caminho do 7
para o 8. É nisso que o `DS2_SeamlessSessionHook` se apoia.

Isto é também o desenho do próximo passo. Se numa morte de convidado a máquina
fosse levada de volta ao estado 2 com um destino novo, em vez de ao estado 8, o
convidado renasceria **dentro do mundo do host**. Não está testado, e há contas
que o jogo pode ter a acertar (o host precisa concordar, o corpo do fantasma
precisa sumir, a bloodstain fica em algum lugar). Mas é a primeira hipótese com
endereço.

## Os sítios que chamam o warp não saem do Ghidra

`Xrefs.java` em `0x1416148f0` devolve 40 leituras e **nenhuma** delas é uma das
três da tabela acima. A lista não é uma amostra ruim, é incompleta: os sítios
conhecidos (`0x1402c3bca`, `0x1402c2e2f`, `0x14044fe0d`) simplesmente não
aparecem. É a mesma parede que o grafo estático levantou nos envios de invasão,
e vale a mesma conclusão: aqui o inventário honesto é o log em execução, com o
`de=+0x...` de cada warp, e não a análise.

## A sessão pode sobreviver a uma morte

Medido em 12/09, e é o resultado que muda o resto do plano. O caminho inteiro
do desmonte pende de um pedido só, `FUN_1402c2f20`, e recusá-lo **do lado de
quem morreu** basta:

- o estado nunca sai do 7, porque `+0x1cc` nunca fica diferente de zero;
- nenhum warp é emitido — o log de warp do convidado fica vazio;
- o host não participa: ele não pede fim de sessão nenhum quando o convidado
  morre, então não é preciso tocar no cliente dele;
- os dois HUDs continuam listando o outro jogador.

O convidado fica **morto no lugar onde caiu**. Recusar o fim impede o
desmonte, não faz renascer — essa é a peça seguinte.

## O que ainda não se sabe

- Que motivos existem além de 1 e 4, e o que o portão em `0x140248940` cobra.
- O que significam `+0x00` (família 3/4/2) e `+0x14`.
- Se a fogueira de um convidado é alcançável a partir do mundo do host — isto
  é, se o mapa e o ponto que o registro guarda ainda são os dele enquanto ele
  é fantasma. É a primeira coisa que o log responde.
- Se o host que morre com fantasma dentro emite motivo 4 para alguém, e por
  qual caminho.

## Cinco tentativas de fazer o convidado renascer dentro da sessão

Recusar o fim da sessão manteve o convidado dentro dela — morto. Fazê-lo
renascer **no mundo do host** levou cinco medições, e as quatro primeiras
falharam do mesmo jeito por baixo.

| # | o que foi feito | o que aconteceu |
| --- | --- | --- |
| 1 | warp motivo 4, flag 1, portão como estava | recusado pelo portão |
| 2 | warp motivo 4, flag 0 | aceito, nenhum pedido de fim, convidado vivo — **mandado para casa**, mapa e posição ignorados |
| 3 | flag 1 na morte do **host**, portão em 811 | aceito, e o **host travou** |
| 4 | flag 1 na morte do convidado, portão erguido à mão | aceito, convidado vivo e de pé, sessão em 7 dos dois lados, host ainda listando ele — **no próprio mundo** |
| 5 | estado da sessão devolvido a 1 | nenhum warp, nenhum fim de sessão, convidado morto na água; a máquina andou sozinha até o **estado 2** e parou |

A quinta é a que explica as outras. Com `bp 2c3630 deref rcx+f8 8` o
despachante mostrou `[rcx+f8]=02`: o handler do estado 1 rodou, viu o elo com
o par de pé, escreveu 2 e saiu. E o **estado 2 não está no switch do
despachante** — nem o 3. Eles só rodam quando chega uma mensagem.

### O handler do estado 2 responde as quatro primeiras de uma vez

`FUN_1402c2a80`, o slot `+0x28` da sessão. Decompilado, ele:

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

Três conclusões, e cada uma mata uma tentativa:

- **O warp nunca foi a metade que faltava.** O handler monta exatamente o
  mesmo pedido que as tentativas 1–4 montavam à mão, e chama o mesmo slot com
  a mesma flag 1 e o mesmo portão. Quem decide em qual mundo o jogador cai é o
  que vem **em volta**: `FUN_1402bbf20`, a mensagem ao par, o estado 3.
- **`sessao+0x1a4` não é a posição do host.** É escrita *por este handler*, a
  partir do objeto do jogador, como registro de onde o convidado estava antes
  de ser invocado. A tentativa 4 entregou esse registro ao warp como destino —
  e o convidado voltou para o próprio mundo, exatamente como medido.
- **O destino não está na sessão.** Ele chega em `param_2`, do host, pela
  rede. Só três campos ficam guardados depois: `[0]` em `+0x19c`, `[7]` em
  `+0x198`, `[8]` em `+0x1c9`.

### O payload, campo a campo

| campo | uso |
| --- | --- |
| `[0]` | mapa; também copiado para `+0x19c` |
| `[1] [2] [3]` | o destino entregue ao warp |
| `[4]` | nunca lido |
| `[5]` | giro; vira o quaternion por cos/sin |
| `[6]` | lido como short, vai para `FUN_14051c6a0` |
| `[7]` | também copiado para `+0x198` |
| `[8]` | lido como byte, também copiado para `+0x1c9` |

### A sexta tentativa: repetir o convite

Se o que falta chega do host e nada mais, então não há o que sintetizar: o
convite é **copiado na passagem** do join de verdade e repetido na morte. O
destino repetido é o ponto de invocação — um lugar aonde o host andou de
propósito, garantia que nenhuma posição inventada teria. Onde o convidado
morreu não serve: ele pode ter morrido na água, e foi o que aconteceu no teste
da tentativa 5.

Duas armadilhas que o código evita porque decompilar as mostrou antes:

- o byte em `*(sessao+0x108) +8` é o primeiro teste do handler, e se não for
  zero ele **não chega** — vai direto para `EndSession(0x13)`, levando a
  encenação junto e sem dizer nada. É lido num convite de verdade e conferido
  antes de qualquer repetição;
- o bloco `+0x1a0..+0x1c8` é refeito a partir do que é verdade **agora**, e um
  desses valores é o portão, que vale 0 na morte do convidado. Repetir sem
  cuidado trocaria o registro do convite original — de onde sai o caminho de
  volta para casa — por lixo. Ele é salvo e devolvido.

O que sobra para medir é uma coisa só: **o estado 3 também não está no switch**,
então a chegada manda a mensagem ao par e espera resposta. Se um host responde
a um convidado que ele já conta como dentro, ninguém sabe. O rastro de estado
depois da chegada responde isso num teste só — `3→5→6→7` é o join fechando,
`3` parado é o host ignorando, e `3→4` seguido de `0xf` é o join estourando o
temporizador do case 4.

### A guarda da chegada

A sexta tentativa não chegou a acontecer, e o motivo é uma linha que só
apareceu porque o handler foi decompilado antes de ser chamado:

```c
if (*(char*)(*(sessao+0x108) + 8) != 0) { EndSession(0x13); return; }
```

É o **primeiro** teste de `FUN_1402c2a80`, antes de qualquer outra coisa — e o
mesmo teste abre `FUN_1402c37a0`, o estado 0, que é onde um join começa. Ou
seja não é "esta chegada não pode": é **"você não está em condição de entrar em
lugar nenhum"**.

Medido com a encenação inteira de pé — convite, invocação, sessão viva,
convidado morto de propósito na água:

```
chegada: mapa=0a1f0000 destino=6.09,-18.50,209.16 giro=2.548 [6]=0101 [7]=1 [8]=01
guarda da chegada: antes=00 depois=00
morte de fantasma: motivo=2 papel=1 -> reentrando pelo estado 1
guarda da chegada = 01; a chegada seria recusada. Nao repetindo.
```

O destino copiado bate com onde os dois estavam (6.19, −18.52, 209.05), e bate
também com o warp que o próprio jogo emitiu no join — `ponto=40c30000` é
6.09375, o mesmo X. O formato do payload está certo.

**A guarda vale 0 num convite de verdade e 1 no instante da morte.** E não
abre sozinha: lida ao vivo minutos depois, pelo endereço que o próprio
despachante entrega (`bp 2c3630 deref rcx+108`), continuava em `01`, com a
sessão parada no estado 2. Repetir a chegada ali teria caído direto em
`EndSession(0x13)`, levando a encenação junto e sem dizer por quê — foi a
verificação que sobrou com o achado em vez de com nada.

Isso mata a sexta tentativa na forma em que ela foi pensada, e aponta a
sétima: em vez de segurar o convidado morto no lugar onde caiu, **deixar a
morte correr como o jogo a escreveu** — ele levanta na própria fogueira, vivo —
e só então puxá-lo de volta. A sessão sobrevive à viagem porque o desmonte é
recusado à parte, que é a única coisa já provada deste caminho todo: o estado
7 continua lá quando ele se levanta.

### A sétima tentativa, e por que ela fecha a porta

Medida em 12/09 com a encenação completa — marca 1000, invocação confirmada
(`RequestNotifyJoinGuestPlayer` + `RequestNotifyJoinSession`), convite copiado
(`destino=6.19,-18.50,209.03`), convidado morto de propósito na água:

```
morte de fantasma: motivo=2 papel=1 -> morte normal, e depois puxar de volta
fim de sessao RECUSADO sessao=... papel=1 estado=7 motivo=2 de=+0x2c9246
esperando: estado=7 guarda=01 apos 1200 quadros
...
desisti de puxar de volta: estado=7 guarda=01
```

A sessão **sobreviveu** — estado 7 o tempo todo, como o M1 promete. Mas o
convidado **nunca levantou**: ficou morto embaixo d'água, com a barra do host
ainda no HUD, e a guarda da chegada em `01` por 7200 quadros seguidos.

É esta a conclusão que fecha o assunto: **o renascimento é rio abaixo do
desmonte.** Deixar a morte correr não basta, porque a morte de um fantasma não
o levanta — quem o levanta é o desmonte da sessão, que o M1 recusa de
propósito. As duas coisas que o M2 precisa, manter a sessão e pôr o jogador de
pé, estão ligadas ao mesmo pedido, e ele é **um só e não se repete**.

O que sobra, e é o desenho da oitava tentativa: **levantar o jogador nós
mesmos**. `FUN_14044fde0(*(ctx+0x70))` é o "mande-o para a última fogueira" que
o `DS2_SeamlessCoopHook` já sabe chamar. Com a sessão segura no estado 7 e o
jogador de pé por conta própria, a guarda tem a chance de abrir — e aí a
repetição do convite, que já está construída e testada até a porta, roda.

### Destravar uma sessão sem matar o cliente

O pedido de fim é **um só**: recusado uma vez, nunca mais é feito. Liberar o
bloqueio depois não adianta, e o convidado fica preso entre estados. Matar o
cliente resolve e **custa caro** (ver o CLAUDE.md sobre desconexões ilegais).

O jeito barato usa o próprio modelo já mapeado — o estado 7 sai quando
`sessao+0x1cc` deixa de ser zero:

```
bp 2c3630 deref rcx+f8 8          # o despachante entrega o ponteiro da sessão
pokeabs fim <sessao+0x1cc> 02000000 00000000
```

Medido: o convidado foi do fundo d'água para a própria fogueira, vivo, em
segundos. É a confirmação de ponta a ponta de que `+0x1cc` é o gatilho único
do 7 para o 8.

### A oitava: a guarda abre quando o jogador levanta

A sétima mostrou que ninguém levanta um fantasma cuja sessão não desmonta. A
oitava levanta ele — `FUN_14044fde0(*(ctx+0x70))`, noventa quadros depois da
morte, para não correr por cima do que a morte ainda tem a fazer. Medida em
12/09, com a encenação completa:

```
morte de fantasma: motivo=2 papel=1 -> morte normal, e depois levantar e puxar de volta
fim de sessao RECUSADO  papel=1 estado=7 motivo=2
levantando na ultima fogueira apos 90 quadros
warp motivo=1 forca=0 tipo=3 mapa=0a1f0000 de=+0x44fe22 / warp aceito=1
de pe de novo apos 421 quadros, guarda=00; recomecando o join
estado 2; repetindo o convite do host: mapa=0a1f0000 destino=6.09,-18.50,209.16
depois da chegada: estado=11
```

Linha por linha, tudo o que as sete tentativas anteriores tentaram adivinhar:

- **o warp de renascimento é aceito** mesmo com a sessão de pé no estado 7;
- **a guarda da chegada abre** — de `01` para `00` — assim que o jogador está
  de pé. Ela não é "esta chegada não pode", é "você não está em condição", e a
  condição é estar vivo em algum lugar;
- a sessão volta ao estado 1, anda sozinha até o 2, e a repetição do convite
  **roda**;
- o tratador da chegada entra, e sai pelo **0xb**.

### O que o 0xb quer dizer

É a saída "não pode" de `FUN_1402c6570`, que faz duas perguntas:

```c
FUN_14014ed40(modo, papel)   // uma tabela em 0x141568810, índice papel + modo*0x14
FUN_1402ca190()              // entre outras coisas: *(ctx+0x70 + 0x1b0) == 0
```

O segundo é o suspeito. Se `+0x1b0` quer dizer "este jogador ainda está se
assentando", levantá-lo à mão é exatamente o que o deixaria aceso — e a
repetição foi emitida no primeiro quadro em que a guarda abriu, que é o pior
momento possível para perguntar.

É uma pergunta pura e barata, então a repetição passou a **esperar a resposta
ser sim** em vez de gastar a única tentativa descobrindo. O log diz de que a
resposta foi feita a cada vez que ela muda.

O convidado terminou vivo na própria fogueira, fora da sessão, sem travar
nada — a falha mais limpa que este caminho já teve, e a primeira em que o
jogo recusou por um motivo que tem nome.

### O custo de cada tentativa

Medido em 12/09, e é um limite prático do caminho todo: **o experimento corta
o convidado.** Chico colocou a marca 1001 sem problema às 19:26, o teste da
oitava rodou às 19:31, e às 19:40 ele não colocava mais marca nenhuma e tinha
parado até de pedir a lista delas ao servidor. Nada mais aconteceu no meio.

Do lado do jogo é justo — um convidado que recusa o desmonte e depois sai *é*
uma desconexão ilegal, e o jogo conta. O aviso aparece uma vez, só na tela do
cliente, e nada disso chega a log nenhum:

> Due to repeated illegal multiplayer disconnects, your connection to other
> worlds was lost. Only a Bone of Order can restore your connection.

Um jogador cortado não coloca marca, não usa orbe e **não invoca** — então
inverter os papéis não contorna. A cura pelo item não escala: existem poucos
Bones of Order numa jogatina e os dois personagens gastaram os seus.

A cura que escala é o save. O servidor privado guarda o seu
(`EnableSeperateSaveFiles`), um arquivo por conta:

```
<prefixo>/drive_c/users/steamuser/AppData/Roaming/DarkSoulsII/<steamid>/DS2SOFS0000.ds3os
```

Copiar antes do teste e devolver depois leva segundos e torna o corte
irrelevante. Com o jogo parado — um cliente vivo reescreve o arquivo na saída.

### O predicado era transitório, e a chegada funciona

Com a repetição travada até `FUN_1402c6570` responder sim, medido em 12/09:

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

**O predicado era transitório**: não no quadro zero, sim 96 quadros depois — e
`assentando` valia 0 nas duas leituras, então quem recusava era uma das outras
metades de `FUN_1402ca190`, provavelmente o par de virtuais `+0x48`/`+0x50`
que perguntam se o jogo está carregando. A oitava tentativa tinha disparado a
repetição no primeiro quadro possível, que era o único errado.

Com a espera, **o tratador da chegada sai pelo estado 3**, que é o ramo de
sucesso — não mais pelo 0xb — e emite o seu warp, que é aceito. Do lado do
convidado a reentrada está resolvida: ele sai da morte, levanta, refaz o join
e chega.

### O que falta é o host

E ali para. O estado 3 é onde o convidado espera a resposta do par, e ela não
vem: pouco depois nenhum dos dois clientes tem mais objeto de sessão —
`bp 2c3630` não dispara em nenhum. O convidado terminou vivo no próprio mundo,
o host sozinho.

O host **não pede fim de sessão nenhuma vez** — o log dele fica vazio, o que
confirma de novo que ele não participa da morte do convidado. Então a sessão
do host não morreu por um pedido: ela se desfez porque o elo com o par caiu, e
o que derruba o elo é justamente devolver a máquina do convidado ao estado 1.

Duas direções a partir daqui, e a segunda parece mais barata que a primeira:

1. **Segurar o host.** Descobrir o que no host solta o slot do convidado
   quando o elo pisca, e segurá-lo pelos poucos segundos da reentrada.
2. **Reinvocar em vez de reentrar.** `DS2_RematchHook` já faz exatamente isso
   para marcas vermelhas depois de um duelo — o host reinvoca o mesmo jogador
   sozinho. Aplicá-lo à marca branca depois de uma morte de co-op reaproveita
   código provado e entrega o que o desenho pede ("morreu, renasce e continua
   na sessão") ao custo de um carregamento.

### A nona: sem o handshake, a máquina anda

Ir direto ao estado 2, sem devolver a sessão ao 1, muda o resultado:

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

e, lido ao vivo um minuto depois, `[sessao+0xf8] = 6`. Antes ela parava no 3;
agora percorre **3 → 4 → 6**, e o convidado volta a ser desenhado como
fantasma branco. Nem o 3 nem o 4 estão no switch do despachante, então cada
passo veio de uma mensagem: alguma coisa do outro lado está respondendo.

Também apareceu, na primeira rodada desta variante, um segundo pedido de fim
que não vinha ao caso: `motivo=3` saindo de `+0x2c385c`, dentro do tratador do
estado 7, na janela entre a morte e a reentrada. Bloqueá-lo (`block 3`) é uma
linha no arquivo de pedido e tirou esse ruído do caminho.

### O host não usa esta máquina

E mesmo assim a sessão acaba, porque o host some. O que se sabe dele:

- ele **nunca** pede fim de sessão — o log do `DS2_SeamlessSessionHook` do
  host fica vazio a sessão inteira;
- `bp 2c3630` **não dispara** no host depois da morte do convidado, e o
  despachante é por frame: se ele não roda, não há objeto de sessão para
  rodar.

Ou seja o host não é a outra ponta desta mesma máquina de estados — ou o
objeto dele é destruído por um caminho que não passa por `FUN_1402c2f20`. Essa
é a próxima coisa a medir, e a medição é barata: armar `bp 2c3630` no host
**enquanto a sessão está viva** responde de uma vez se ele tem um objeto
destes e em que estado ele fica.

### O host tem a sua própria máquina, e ela tem nome

O binário carrega RTTI, e as classes se leem sozinhas:

```
NetMultiplayCtrl
├── NetJoinMultiplayCtrl     → NetSummonJoinMultiplayCtrl    (o convidado)
└── NetAcceptMultiplayCtrl   → NetSummonAcceptMultiplayCtrl  (o host)
```

Tudo o que este documento chama de "a sessão" até aqui é a primeira: a vtable
em `0x1410d7bd8`, o estado em `+0xf8`, o despachante `FUN_1402c3630`,
`FUN_1402c2f20` para encerrar. É a máquina de **entrar no mundo de alguém**.

O host usa a outra, e ela é diferente em tudo o que importa:

| | convidado | host |
| --- | --- | --- |
| classe | `NetSummonJoinMultiplayCtrl` | `NetSummonAcceptMultiplayCtrl` |
| vftable | `0x1410d7bd8` | `0x1410d7998` |
| estado | `+0xf8` | `+0x150` |
| despachante por frame | `FUN_1402c3630` | `FUN_1402bddb0` |
| casos no switch | 0,1,4,5,6,7,8,10,0xb | 2,4,5,7,8,10,0xd,0xf,0x10,0x11,0x12 |

Isso explica de uma vez duas coisas que vinham sendo lidas como fato sobre o
jogo e eram fato sobre a instrumentação:

- **"o host nunca pede fim de sessão"** — o `DS2_SeamlessSessionHook` observa
  `FUN_1402c2f20`, que é da classe do convidado. O host nunca ia aparecer ali.
- **"o host não tem objeto de sessão depois da morte"** — `bp 2c3630` é o
  despachante do convidado. O host nunca ia disparar ali tampouco.

A medição que interessa agora é direta: `bp 2bddb0 deref rcx+150 8` no host,
com a sessão viva, e ver o que o estado dele faz quando o convidado morre.

### O host sobrevive à morte do convidado

Com a classe certa em mãos, a medição é direta. O controlador do host não
precisa de breakpoint: basta varrer a memória viva pela vtable dele.

```
scan host 1410d7998 8 6      # NetSummonAcceptMultiplayCtrl::vftable
```

Com a sessão de pé isso devolve um objeto no heap — `0x7ffffe5bf120` numa
rodada — e `+0x150` nele vale **0x10**. Quarenta segundos depois da morte do
convidado, ainda **0x10**: o host não larga na hora. Minutos depois o objeto
já não aparece na varredura, então ele larga em algum momento, mas não no que
se supunha.

Isso derruba a conclusão da seção anterior. O host não some quando o
convidado morre; ele fica exatamente onde estava.

E do lado do convidado, com a nona variante, a máquina **completa o join**:
lida ao vivo depois da reentrada, `[sessao+0xf8] = 7` num objeto de sessão
novo, e o personagem é desenhado como fantasma branco. Ou seja os dois lados
acham que estão numa sessão — e mesmo assim nenhum vê o outro, porque o
convidado está no próprio mundo.

O que falta, então, não é manter o host vivo nem completar a máquina do
convidado: as duas coisas já acontecem. É **fazer o host voltar a colocar o
fantasma no mundo dele**. No join original isso vem de uma mensagem que o
host recebe; a reentrada nunca a manda.

### A linha do tempo do host, finalmente

Com `FUN_1402bddb0` detourado, o host conta a própria história. Um join
normal, por quadro:

```
host: estado -> 4 -> 5 -> 7 -> 8 -> 10(0xa) -> 11(0xb) -> 13(0xd) -> 14(0xe) -> 15(0xf) -> 16(0x10)
```

`0x10` é jogar. E na morte do convidado, com a nona variante rodando: **nada**.
Nem uma transição. O host fica em `0x10` do princípio ao fim, sem jamais
saber que o convidado saiu, muito menos que voltou.

Mas o servidor sabe:

```
22:06:41  3:Chico   RequestNotifyDeath
22:07:04  1:Samuel  RequestNotifyLeaveGuestPlayer
```

Vinte e três segundos depois da morte — e *depois* de a reentrada do
convidado ter completado, que leva uns oito. Não é reação à morte: é
**temporizador**. O host passou vinte segundos sem receber nada do convidado
e o descartou.

O que diz onde o problema realmente está. Não é a máquina de estados de
nenhum dos dois: as duas ficam de pé, uma em `0x10` e a outra chegando ao 7.
É o **fluxo entre eles**. A nona variante preserva o objeto do elo mas não
faz o convidado voltar a falar por ele, e o silêncio é o que o host cronometra.

### A revanche serve para marca branca

`DS2_RematchHook` foi escrito para duelos: ele guarda o ponteiro do manager
quando o jogador invoca, e reinvoca sozinha qualquer placa que chegue depois
ao cache do host. Nunca tinha disparado para uma marca branca, e o hook só
falava quando agia — então não dava para saber se ele não era chamado ou se
era chamado e desistia.

Com uma linha a mais, registrando toda placa que entra:

```
placa recebida: tipo=1 alca=80000011 armado=1
revanche pedida, mas ninguem invocou ainda: sem o manager nao da
```

`tipo=1` é a marca branca. Ele **é** chamado, a alça é válida e ele está
armado: o que faltava era só o manager, que só chega quando o jogador invoca
uma vez por sessão de jogo.

Isso torna viável o caminho que a nona tentativa não alcança. Em vez de
costurar o convidado de volta a uma sessão cujo fluxo peer já morreu — que é
o que o host cronometra e derruba — o convidado volta para casa pela morte
normal, põe uma marca, e o host a invoca sozinho. É uma sessão nova de
verdade, com elo novo, ao custo de um carregamento.

O que falta para fechar: semear o manager (uma invocação manual por sessão de
jogo, ou achar de onde mais o ponteiro sai) e pôr a marca sozinho do lado do
convidado.

### Duas armadilhas de encenação que custaram ciclos

**`up` reescreve o `Injector.config`.** Ligar `--auto-rematch` no
`game prepare` e depois rodar `up --seamless` desliga de volta, e o log do
hook continua mostrando as linhas do boot anterior — o que se lê exatamente
como um hook vivo. Confira o **mtime** do log antes de acreditar nele.

**A fogueira de Heide fica numa laje estreita sobre a água.** Qualquer `dpad`
que erre o menu e chegue ao mundo empurra o personagem para o mar, e o teste
morre junto. Toda navegação de menu tem que confirmar por captura que o menu
abriu antes do próximo direcional.

### Reinvocação automática, ponta a ponta

Medido em 12/09, 22:46, sem nenhuma intervenção humana entre a marca e a
sessão:

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

E nas telas: o nome e a barra do Samuel no HUD do Chico, o Samuel visível ao
lado dele, o Chico desenhado como fantasma branco. Sessão de verdade, elo
novo, os dois se vendo.

**Isto é um contorno, não o M2** (corrigido em 13/09: o critério do M2 é
morrer e continuar na *mesma* sessão, sem marca e sem reinvocação — ver a lista
de tarefas). O que o parágrafo abaixo dizia em 12/09: não costurar o convidado de volta a uma
sessão cujo fluxo peer já morreu — o host cronometra esse silêncio e derruba,
medido — e sim deixar a morte correr, o convidado voltar para casa, pôr uma
marca, e o host reinvocá-lo sozinho. Custa um carregamento e entrega o que o
desenho pede: morreu, renasce, e continua com o amigo.

Falta uma peça, e é pequena perto do resto: **o convidado pôr a marca
sozinho**. Hoje é um X manual. O resto da cadeia já é automático.

Duas arestas conhecidas:

- o manager só existe depois de uma invocação manual por sessão de jogo, que
  é de onde `DS2_RematchHook` tira o ponteiro. Vale procurar outra origem;
- o hook tenta **toda** placa que chega, inclusive marcas velhas ainda no
  cache do cliente, e cada uma dessas rende um "Summoning failed. The sign has
  disappeared." na tela do host. Filtrar por dono resolveria.

### A peça que falta, e onde ela está

Para o M2 ficar sem mão humana, o convidado precisa pôr a marca sozinho ao
voltar para casa. O RTTI já entrega a vizinhança:

```
ISummonSignSetCtrl / SummonSignSetCtrl   vftable 0x1410cb698
   métodos em 0x140212cd0 .. 0x140213c80
   FUN_140213160 (o AddSign que o DS2_RematchHook usa) é desta classe
AbstractNetSvrMySignManager              vftable 0x1410d3518
Frpg2RequestMessage::RequestCreateSign   vftable 0x141113378
```

O construtor da mensagem (`FUN_1406a0b10`, via a fábrica `FUN_140caa440`) não
serve de gancho: é alocação de protobuf, longe de quem decide pôr a marca.

**O jeito curto é medir, não ler.** Uma varredura de breakpoints sobre
`0x140212cd0`–`0x140213c80` com o convidado apertando X diz em uma passada
qual método é a colocação. É a mesma técnica que achou o warp, e aqui a faixa
é de trinta funções em vez de trezentas.

Vale lembrar que a alternativa honesta existe e já funciona: **um X do
jogador**. Morreu, voltou para casa, apertou X, o host o traz de volta
sozinho. Não é "seamless" do jeito do enunciado, mas é um botão por morte e
não depende de mais nada.


## O parecer de 13/09: como o jogo renasce, e por que o warp tira o fantasma

Pedido ao Fable com o registro inteiro deste documento. Marcado abaixo o que foi
**reconferido no binário** depois; o resto é leitura dele, com os endereços para
quem for conferir.

### O respawn comum não usa coordenadas

- **[reconferido]** O registro da última fogueira é `*(ctx+0x70)`: `+0x164` mapa,
  `+0x168` tipo, `+0x16c` id. `FUN_14044ed40` monta o pedido a partir dele —
  tipo 0 do registro vira pedido tipo 3, tipo 2 vira "player start" do mapa, e
  qualquer outro vira um destino padrão (`FUN_14039a9a0`).
- Quem grava o registro: acender/interagir (`FUN_1401caf50`) e sentar
  (`FUN_1401cb950`) — só se quem interagiu é o jogador local; viagem pelo menu
  (`FUN_14017fdb0`); e dois outros chamadores não lidos (`FUN_140040060`,
  `FUN_140461f20`). O registro é recriado a cada carga.
- O id (`0x7ba7` na medição) é de um **objeto de mapa**, e só é resolvido
  **depois** da recarga: `FUN_1401c3c60` procura o objeto na lista
  `*(ctx+0x70)+0x58` e **[reconferido]** `FUN_1401cb1b0` põe o jogador em
  `translação − 1,1 × eixo Z` da matriz do objeto (`DAT_1410bf020 = 1.1f`),
  virado como ela.

Logo **coordenadas de fogueira existem, mas só do mapa carregado.**

### Todo warp recarrega, e é isso que tira o fantasma

Leitura do Fable da máquina do loader (`GameManagerImp`, vftable `0x1410c4c68`):
a entrada do warp só aceita no estado `0x1e`; ao aceitar, notifica o multiplay e
arma um atraso (6 s para morte, 2 s para os outros); o estado `0x14` destrói
**incondicionalmente** o mapa, os personagens e **`ctx+0xd0 = 0`**; o `0xb` recria
tudo. Os tipos 0–4 resolvem o destino só depois disso. Não há atalho de "mesmo
mapa". **[reconferido]** `FUN_140419610` recria o personagem com
`ctx[0x1a] = chr` — então **`ctx+0xd0` é o personagem local**, e o que este
documento chamava de "contador de multiplay em `ctx+0xd0 +0x168`" é um campo do
personagem, reconstruído a cada carga.

Sobrevive à recarga o gerenciador de sessão (`ctx+0x22f0`) — por isso as sessões
ficavam de pé no estado 7. Não sobrevive a presença: do lado do convidado, os
jogadores remotos são registrados no estado 5 (`FUN_1402c3c80` →
`FUN_14051b0e0`), que **não aparece no rastro da nona tentativa**; do lado do
host, o personagem do convidado é criado na sequência `0xd → 0xe
(WaitGuestWarpFinished) → 0xf`, que o host parado em `0x10` nunca refaz.

### A morte tem um ponto só

> **Corrigido pela medição de 13/09** (seção "A morte medida, e segurada", no fim
> deste documento): o byte é o ponto certo, mas quem o consome para o jogador
> local é o slot **`+0x20`**, `FUN_14013c720`; o `+0x10` abaixo nunca foi chamado
> para ele. E `+0x759` tem dez escritores, não um.

**[reconferido]** `FUN_14013c3b0` (slot `+0x10` de `ChrDeadActionCtrl`, vftable
`0x1410bf308`) sai se `*(chr+0xb8)+0x5fc != 0`, sai se `+0x759 == 0`, copia os
parâmetros da morte de `+0x75c..+0x76d` e só então chama `FUN_14013d430`. Segundo
o Fable, dano letal (`FUN_14013a9b0`), flags do personagem, evento de animação
`0x19` e status todos escrevem `+0x759 = 1`. Cancelar ali impede que o resto do
jogo veja uma morte — sem sequência de "YOU DIED", sem `RequestNotifyDeath`, sem
`EndSession`, sem warp. É o plano do M2 na lista de tarefas.

Riscos nomeados por ele: fontes de morte que não passem por `+0x759` (a função de
dano é virtual em quatro vtables e só uma foi lida); deixar `+0x759` ligado com
`+0x5fc != 0` faz o consumidor ignorar a morte; e cada consequência reproduzida à
mão que ficar de fora é uma divergência de save entre os dois jogadores.

### A morte do host, lida e não medida

O warp do host chama `FUN_1402bd0d0(ctrl, 4)`, que **só encerra a sessão se o
host não está em `0x10`**. Quem encerra, na morte do host, é o convidado: o
terminal de morte de fantasma roda com motivo ≠ 2 e pede o fim. Com a morte
interceptada nos dois clientes, o host nunca morre nem warpa, e não há o que
suprimir.

## Teleporte sem warp, e as coordenadas da fogueira (13/09)

Os passos 1 e 2 do plano do M2, medidos solo com o Samuel em Heide, sem sessão.

### Onde a posição realmente mora

Escrever a posição que parece a do personagem não move nada: toda cópia
visível é reescrita no quadro seguinte. A vigia de escrita (`wp` no
`DS2_Trace`, ver [DS2_INVESTIGATION_TOOLS.md](DS2_INVESTIGATION_TOOLS.md))
seguiu a cadeia de cima para baixo, uma camada por medição:

| cópia | quem a reescreve por quadro | o que ela é |
| --- | --- | --- |
| `ChrPhysicsCtrl+0x80` | `+0x36ec7f`, em `FUN_14036ec00` | medidor de velocidade: amostra a posição pelo getter virtual `+0x148` do `PlayerCtrl` e deriva a velocidade |
| `PlayerCtrl+0x90` | `+0x314df1` (setter `+0x140`, vindo do `ChrMotionCtrl`) e `+0x36df42` (física) | translação da matriz de mundo `+0x60..+0x9f`; animação escreve, física corrige |
| `ChrPhysicsCtrl+0x1c0` | `+0xbd2c18`, em `FUN_140bd2bd0` | cache do corpo, `y` 0,05 m acima dos pés (`ChrPhysicsCtrl+0x104`) |
| **`hkpRigidBody+0x1a0`** | o integrador do Havok | **a posição autoritativa** |

Os objetos, todos com nome pelo RTTI:

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

### O teleporte que funcionou

Um único pedido, escrevendo só XYZ e preservando cada `w`:

1. as cópias de referência do jogo — `PlayerCtrl+0x90` e `+0xa0`,
   `ChrPhysicsCtrl+0x80`, `ChrMotionCtrl+0x50` — e zerando as velocidades do
   medidor (`ChrPhysicsCtrl+0x60` e `+0x70`);
2. no `hkpRigidBody`: `+0x250`, `+0x260`, `+0x1b0`, `+0x1c0`, `+0x1a0`, com
   `y` somado de `0,05`;
3. por último o cache `ChrPhysicsCtrl+0x1c0`.

Resultado: o Samuel saiu da fogueira de Heide e apareceu **de pé na Catedral de
Blue, a 69 m e 12 m acima, sem tela de carregamento**. O servidor confirmou pelo
próprio log (`Location: ... position 13.6 -6.2 276.0`), a posição ficou estável,
e ele andou 1,8 m com o analógico logo depois, com `y` constante. Ficou uns
85 cm fora do ponto exato — a física empurrando a cápsula para fora da
geometria da fogueira.

**Resolva a cadeia de novo a cada uso.** Depois de uma recarga (uma morte, um
warp) o `PlayerCtrl`, a física e o controle de movimento voltaram nos mesmos
endereços, mas o `hkpRigidBody` não: o endereço antigo passou a ser o corpo de
outra coisa, a (−100, 0,7, 188), e um teleporte com o ponteiro guardado moveu
esse corpo e deixou o personagem onde estava (13/09). A vftable do corpo
(`0x141126578`) não distingue um do outro; o que distingue é ter vindo agora de
`*(*(ChrPhysicsCtrl+0x320)+0x20)`.

Duas tentativas anteriores contam o que **não** basta: escrever só a física
(`ChrPhysicsCtrl+0x80`) é desfeito no quadro seguinte; escrever as cópias do
jogo mais o cache do corpo, sem o Havok, é desfeito também (o personagem andou
16 cm e voltou). E um caso lateral: escrever só o cache do corpo moveu o
personagem 0,8 m para o lado errado — a varredura da cápsula entre a posição
antiga e a nova batendo na geometria —, o que se lê como teleporte e não é.

### As coordenadas da fogueira

O registro da última fogueira (`*(ctx+0x70)`, campos `+0x164` mapa, `+0x168`
tipo, `+0x16c` id) dizia `0x0a1f0000 / 0 / 0x7ba7`. A lista em
`*(*(ctx+0x70)+0x58)` (primeiro nó em `+8`, próximo em `+0x60`, objeto em
`nó+8`) tinha **3 nós** no mapa carregado — as fogueiras. A posição de
nascimento de cada uma, pela conta do jogo (`translação(+0x70) − 1,1 ×
eixoZ(+0x60)` da matriz do objeto), deu para a de Heide
`(6.1855, -18.5166, 209.0531)`, **a 0,000 m** de onde o jogo tinha posto o
Samuel ao carregar. As outras duas: `(13.0562, -6.1674, 276.6603)` — o destino
do teleporte acima — e `(-162.0097, -1.7606, 190.6973)`.

### De qualquer fogueira do registro às coordenadas

O id de cada nó é o que `FUN_14017f170` compara ao procurar a fogueira do pedido
tipo 3, e agora ele é lido de fora. `FUN_1403ba6a0(obj)` chama
`FUN_1401ca770(obj+0xb8, obj)`: se o byte `obj+0xa2` for 1 ou 5, o componente é
`*(*(obj+0xb8)+0x20)`; senão ele percorre a lista de componentes em `obj+0x18`
comparando tipos (`FUN_1401ca700`). O id é `**(componente+0xe0)`. As três
fogueiras de Heide têm `+0xa2 = 1`, então o caminho curto basta:

```
chain id 16148f0 70,58,8[,60...],8,b8,20,e0 4
```

Medido em 13/09, com o registro dizendo `mapa 0x0a1f0000 / tipo 0 / id 0x7ba7`:

| nó | id | ponto de nascimento |
| --- | --- | --- |
| 1 | `0x7bac` | `(-162.0097, -1.7606, 190.6973)` |
| **2** | **`0x7ba7`** | **`(6.1855, -18.5166, 209.0531)`** — onde o jogo pôs o Samuel ao carregar |
| 3 | `0x7ba2` | `(13.0562, -6.1674, 276.6603)` — a Catedral de Blue |

O id do registro casa com o nó cujo ponto de nascimento bate a 0,000 m com o
jogo, então a receita está conferida contra o próprio jogo:

1. `registro = *(ctx+0x70)`: mapa `+0x164`, tipo `+0x168`, id `+0x16c`;
2. `nó = *(*(registro+0x58)+8)`, próximo em `nó+0x60`;
3. `obj = *(nó+8)` (um `MapEntity`); componente pelo caminho acima (um
   `MapObjReactionComponent`);
4. o nó cujo `**(componente+0xe0)` é o id do registro;
5. nascimento em `translação(obj+0x70) − 1,1 × eixoZ(obj+0x60)`, virado como a
   matriz do objeto.

Duas notas. O registro só muda quando se interage com uma fogueira: depois do
teleporte para a Catedral ele continuou em `0x7ba7`. E a lista é **do mapa
carregado** — uma fogueira de outro mapa não está nela, que é o limite já
conhecido da abordagem.

## A morte medida, e segurada (13/09)

O passo 3 do plano do M2. Medido solo com o Samuel, sem sessão.

### Uma morte de verdade, de fora

Antes de escrever qualquer hook, com a DLL que já rodava: o HP do Samuel foi
zerado pelo `DS2_MemProbe` (`PlayerCtrl+0x168 = 0`) com três breakpoints do
tracer (`FUN_14013d430`, `FUN_140416960`, `FUN_14013d560`) e a vigia de escrita
em `*(chr+0xb8)+0x759`. Zerar o HP mata de verdade, pelo caminho comum: no mesmo
segundo o servidor recebeu `RequestNotifyKillEnemy` e `RequestNotifyDeath`, e
6 s depois veio `warp motivo=1 ... ponto=00007ba7`, saindo de `FUN_14044fde0`.

| o que | onde |
| --- | --- |
| liga `+0x759` | `+0x16a695`, em `FUN_14016a650`, chamado pela atualização do jogador (`FUN_140315520`) |
| apaga `+0x759` | `+0x13cb5b`, em `FUN_14013c720` |
| `FUN_14013d430` (o aviso da morte) | chamado de `+0x13c938`, em `FUN_14013c720` |
| `FUN_14013d560` (almas, `RequestNotifyKillEnemy`) | chamado de `+0x13c97a`, em `FUN_14013c720` |

`FUN_14013c720` é o slot **`+0x20`** do `ChrDeadActionCtrl` e roda uma vez por
quadro para cada personagem, chamado de `FUN_14030eb60`. Com breakpoints nos
dois slots, só ele disparou; o `+0x10` (`FUN_14013c3b0`) nunca foi chamado para
o jogador local. Depois de uma recarga ele aparece para dois personagens que não
são o jogador, chamado de `+0x30ea2f`.

### A fonte do HP, e a máquina do controlador

`FUN_14016a650` liga o byte quando `chr+0x168 < 1`, o bit 15 de `+0x4c8` está
limpo e o byte ainda está zerado, e preenche `+0x75c` (um handle do matador),
`+0x760` (flags que suprimem consequências isoladas), `+0x768 = 10` (a causa).
Ela roda todo quadro: **apagar o byte sem devolver o HP só adia a morte um
quadro.**

A máquina de `FUN_14013c720`, no byte `ctrl+0x10`:

- **0, vivo.** Se `+0x5fc == 0`, chama `FUN_14013cc30` (as fontes por flag:
  `*(chr+0xd8)`, `*(chr+0xd0)`, evento de animação `0x19`). Com o byte ligado:
  copia os parâmetros, chama `FUN_14013d9f0`, **apaga o byte**, busca a linha de
  tempos (`FUN_14013d880`, guardada em `+0x70`), chama `FUN_14013d250` (avisa o
  gerenciador em `ctx+0x40` e o registro de fogueira em `ctx+0x70`) e
  `FUN_14013cec0`, e vai para 2 (ou 1, se `+0x5d0`).
- **2, morrendo.** Soma o tempo em `+0x58` e dispara cada consequência uma vez
  quando o tempo passa do limiar da linha; as travas são `+0x14..+0x4c`. Depois
  da morte medida estavam todas em 1, com `+0x58 ≈ 7,49 s`.
- O rabo liga os bits `0x4000`/`0x8000` de `+0x4c8` fora do estado 0, e é o
  `0x8000` que cala `FUN_14016a650` enquanto o personagem morre.

Uma segunda porta, que não passa pelo byte: `FUN_14013c500(ctrl, tipo)`,
alcançado por um salto de `FUN_14030eb20` (virtual em cinco vftables). Com tipo
1 ou 2 ele dispara todas as consequências de uma vez e estaciona o controlador
no estado 3.

`+0x759` recebe `1` em dez lugares (varredura do `.text` inteiro por
`0x759(`): `+0x13aae5` (dano letal, `FUN_14013a9b0`), os três de
`FUN_14013cc30`, `+0x145f3f` (causa `0x6e`), `+0x16a695` (HP), `+0x31b753`,
`+0x37046b` (`FUN_1403703e0`, morte ao aterrissar, causa 10), `+0x372ed7`
(`FUN_140372e20`, morte por queda, causa `0x5a`) e `+0xd1c7f8`. Há ainda uma
cópia do bloco inteiro de uma estrutura para outra em `+0x8d615`, que pode ser
replicação. O hook fica no consumidor, e não em cada fonte. Duas ressalvas: a
lista vem de uma varredura por padrão, que não vê deslocamento dobrado num
registrador; e o byte tem um terceiro leitor, `+0x13683a` em `FUN_140136570`,
que não foi lido.

### O hook: `DS2_DeathInterceptHook`

Desvia `FUN_14013c720` e só age quando `*(ctrl+8)` é o personagem local
(`ctx+0xd0`). Os 15 controladores vivos incluem inimigos, e cancelar sem esse
filtro os deixaria imortais. Com o controlador no estado 0, `+0x5fc == 0` e o
byte já ligado **antes** da chamada, ele:

- em `observe` (o padrão), escreve a morte e deixa passar;
- em `cancel`, apaga o byte, devolve o HP para `chr+0x174` (o máximo efetivo,
  já descontado o hollow; `+0x170` é a base), limpa `0x4000|0x8000` de `+0x4c8`
  e **não chama o original** naquele quadro.

Se o estado sair de 0 sem o byte antes, o log diz `SEM +0x759 ANTES`: é uma
morte das fontes de `FUN_14013cc30`, que ligam o byte dentro da chamada e
passariam pela checagem. `FUN_14013c500` e o slot `+0x10` são só registrados.
`DS2_Death.req` troca o modo sem relançar (`observe`, `cancel`, `status`), e o
log é `DS2_Death.log`.

### O resultado

**Morte por HP, cancelada.** Com `cancel`, o HP zerado voltou para 869 no mesmo
quadro (`morte CANCELADA #1 hp=0 -> 869 ... causa=10`), o controlador ficou no
estado 0, `+0x759` e `+0x4c8` zerados, nenhum warp no `DS2_Seamless.log`, e o
personagem andou. Num segundo cancelamento ele foi 2,5 m em direção à escada,
com `y` constante e câmera normal. O controle positivo do servidor veio depois:
a primeira morte deixada passar nessa mesma conexão (em `observe`, 13:42:49)
imprimiu `First ... RequestNotifyDeath`, então nenhuma das anteriores tinha
chegado lá.

**Morte por queda: o byte é segurado, o resto não.** O primeiro teste de
controle levou o Samuel para trás e para fora da plataforma de Heide. Ele caiu
para `y = -3000`, e:

1. `FUN_140372e20` (a morte por queda, chamada pelo controle de queda
   `FUN_140372620` quando o tempo no ar passa do limiar) zerou o HP, ligou o bit
   **`0x200` de `*(chr+0xb8)+0x4c0`** e o byte com causa `0x5a` e `+0x76d = 2`.
   O hook cancelou (#2).
2. Enquanto ele caía, o HP voltava a zero todo quadro, e o hook cancelou **2956
   vezes em 100 s**, uma por quadro, sem nada chegar ao servidor. (O servidor
   parou de receber posição: o `Location` ficou em `6.2 -18.7 211.4`, a beira.)
3. O teleporte do passo 1 para o ponto de nascimento da fogueira `0x7ba7`,
   escrevendo só XYZ, **parou o loop**: o contador ficou parado e o servidor
   voltou a dizer `position 6.2 -18.5 209.1`.
4. Mas o personagem ficou **sem controle e com a câmera parada** no ponto da
   queda. O thread do jogo estava vivo (breakpoint no chamador por quadro
   disparou na hora), START abriu o menu, e o analógico não moveu nada.
   Apagar o bit `0x200` não devolveu o controle.

A saída foi voltar para `observe` e zerar o HP: morte comum, warp, recarga, e
ele de pé na fogueira de novo.

### A morte por queda, desfeita (13/09)

A "trava de controle" da primeira queda não existia. O que prendia o Samuel era
a câmera, e a morte por queda deixa três marcas que o byte não desfaz.

**De onde vem a queda.** `FUN_14036fdf0` trata o contato do personagem com um
volume de colisão do mapa, pelo tipo em `*(*(volume+0x30)+0x70)+0x14`. Nos
tipos 1, 2, 5 e 6 ele liga o **bit 51** de `*(chr+0xb8)+0x4c0`
(`0x8000000000000`); nos tipos 3, 4, 7 e 8, o **bit 52**. Nos tipos 1, 3, 5, 7
e 10 ainda manda à câmera um pedido de tipo 7, para personagens cujo tipo passa
na tabela `0x1410bfff1` (o Samuel passa; quem mais passa não foi lido). A
água embaixo da plataforma de Heide é um desses volumes. Com o bit 51 ligado, o
controle de queda (`FUN_140372620`, chamado pela atualização por quadro do
personagem antes do controlador da morte) chama `FUN_140372e20` em todo quadro
que o personagem passa no ar: HP a zero, **bit 9** (`0x200`) e o byte com causa
`0x5a`. Por isso o hook cancelava uma vez por quadro durante a queda inteira.

**A câmera.** O pedido de tipo 7 (`FUN_140492080`, caso 6) só escreve
**`CameraManager+0x450 = 1`** (`CameraManager` em `ctx+0x20`, vftable
`0x1410f45a8`). A cada quadro, `FUN_140492880` compara esse byte com o id em
`+0x454`: ligado sem id, empilha um pedido de tipo 5 no `IngameCameraOperator`
(`FUN_140495380`, vetor em `+0x100..+0x108`, entradas de `0x40` bytes com o id
em `+0x30`), que ativa o `FallDeadCameraOperator` (índice `+0x1520 = 6`);
desligado com id, **remove o próprio pedido** (`FUN_1404955a0`). O operador de
queda fixa a posição e só gira para olhar o personagem. Nada desliga o byte
fora de uma recarga, e com a câmera olhando de onde ele caiu, o analógico, que é
relativo à câmera, move o personagem quase nada. Na primeira queda isso deu zero
movimento; na reprodução, 0,3 m em 600 ms.

A comparação de memória (personagem, `*(chr+0xb8)`, controles de ação, física,
câmera) entre o Samuel de pé e o Samuel preso não achou mais nada: fora
posições e valores do pouso, só os dois bits de `+0x4c0`,
`CameraManager+0x450`/`+0x454` e o estado do operador de câmera. Uma primeira tentativa de desligar a câmera
escrevendo o índice `+0x1520` não durou um quadro: `FUN_140495e10` o reescreve
a partir do vetor de pedidos.

**A receita, medida à mão:** teleporte para o nascimento da fogueira; o controle
de queda diz que pousou já na primeira leitura (`+0x08 = 0`); então
`CameraManager+0x450 = 0` (o gerenciador remove o pedido, índice de volta a 10)
e os bits 9, 51 e 52 de `+0x4c0` limpos. Cancelamentos param, câmera normal, e o
Samuel andou 1 m em 500 ms na mesma altura. Limpar os bits antes de pousar não
serve: no ar, com o tempo de queda ainda acima do limiar, o controle de queda
mata de novo.

**O hook.** No modo `cancel`, uma morte recusada que traz os bits ou o byte da
câmera inicia uma recuperação: teleporte para o nascimento da fogueira do
registro (a receita do passo 2, lida dentro do jogo; sem a fogueira no mapa
carregado, a última posição no chão do controle de queda), e nos quadros
seguintes, assim que o controle de queda disser que pousou, limpa os bits e o
byte da câmera. Refaz o teleporte a cada 30 quadros e desiste em 300.

**Medido com o hook (13/09, Heide, solo):**

| teste | resultado |
| --- | --- |
| teleporte para o vazio | um cancelamento (causa 90, `+0x4c0 = 0008000000000200`), teleporte para a fogueira `0x7ba7`, "queda desfeita em 1 quadros" 18 ms depois; câmera no modo 10, byte e bits zerados, HP 823, e ele andou 2,8 m em 1 s |
| andar para fora da beira | duas quedas (o analógico ainda empurrava depois da primeira volta), duas recuperações de 1 quadro, de pé na fogueira |
| HP zerado, sem queda | cancelamento comum, sem "queda", posição idêntica bit a bit |
| servidor | nenhum `RequestNotifyDeath` nem `RequestNotifyKillEnemy` na conexão, nenhum warp no `DS2_Seamless.log` |

Um custo de percurso: um vigia de escrita (`wp`) na página do
`IngameCameraOperator`, com 24 mil faltas por segundo, derrubou o jogo com
`0xC0000005` no instante em que foi levantado. Era uma corrida no próprio vigia,
corrigida no mesmo dia (ver DS2_INVESTIGATION_TOOLS.md).

### O que fica para o passo 5

- A morte por HP está resolvida no lugar: cancelar e devolver o HP deixa o
  personagem controlável sem carregamento. Falta levá-lo à fogueira como a
  queda já faz.
- A morte por queda está resolvida com teleporte para a fogueira, solo.
- Das dez fontes de `+0x759`, só duas foram exercitadas: HP e queda (a queda
  pelo volume de morte; a morte por dano ao aterrissar, `FUN_140372c00`, não).

## Renascer pagando a morte (passo 5, 13/09)

Medido solo com o Samuel em Heide. O modo `respawn` do `DS2_DeathInterceptHook`
recusa a morte e cobra dela o que o jogo cobraria, com as funções do próprio
jogo, **sem recarga**. Cada peça foi achada contra uma morte que o jogo fez
sozinho (`observe`), com o vigia de escrita nos campos e o Ghidra.

### O que uma morte custa, e quem cobra

| custo | onde mora | quem o jogo usa |
| --- | --- | --- |
| almas | `PlayerParam+0xec` (`PlayerParam = *(chr+0x490)`) | `FUN_14026af40(NetSvrBloodstainManager, saída)`, alcançado pela sequência "YOU DIED" via `NetSvrManager` slot `+0xe0` |
| a mancha | registro do `NetSvrBloodstainManager` (`*(*(*(0x141616cf8)+0x30)+0x90)`): `+0x2c` tem, `+0x2d` já cobrada, `+0x30` almas, `+0x34` mapa, posição, ângulo e célula | depois da recarga, slot `+0x28` (`FUN_14026b0d0`) cria o sinal tipo 10 do `BloodstainSetCtrl` |
| hollow | nível em `PlayerParam+0x1ac` | `FUN_140202c30(PlayerParam, *(data+0x76d))`, chamado por `FUN_14037dcc0` no quadro em que o controlador entra no estado 2 |
| aparência e HP máximo | `*(chr+0xb0)+0x3e` (0 humano, 1 hollow, 2 muito hollow); máximo efetivo `chr+0x174` | `FUN_1402026e0(chr)` na carga seguinte |
| Estus | `+0x24` da entrada do Estus Flask (item 60155000) no inventário `*(*(ctx+0xa8)+0x10)` | `FUN_1401ac370(inventário)`, o que o SpEffect do descanso na fogueira chama |

Detalhes que custaram medição:

- **A posição da mancha é a última posição segura**, não onde o personagem
  está: o registro a toma de `IBloodstainSetCtrl` slot `+0x80`, um anel de
  posições em chão firme. Por isso a cobrança tem de vir antes do teleporte, e
  por isso numa queda a mancha fica na beira.
- **A mancha antiga não some sozinha.** Na morte comum quem a tira é a recarga;
  `FUN_14020e4e0` só expulsa um sinal quando o conjunto está cheio. O hook
  percorre os dois conjuntos do `BloodstainSetCtrl` (`+0x18`, `+0x20`; contagem
  no slot `+0x18`, entrada no `+0x10`, ativa quando `+0x14` é negativo, tipo no
  nibble baixo do handle) e remove os sinais tipo 10 pela interface (slot
  `+0x20`, que recebe o ponteiro da entrada) antes de criar o novo.
- **O nível de hollow sozinho não muda nada.** O multiplicador do HP máximo
  (`FUN_140202820`) vale 1,0 enquanto `*(chr+0xb0)+0x3e` diz humano. Chamar só
  o recálculo (`FUN_140202ca0`) deixou o máximo em 915 com hollow 1; quem
  converte o nível em estado, troca o modelo e recalcula é `FUN_1402026e0`, o
  inverso de `FUN_140203d50` (o que a Human Effigy chama).
- **As checagens de hollow** são as de `FUN_14037dcc0`: sem hollow se
  `FUN_14031c850(data)` (dois bits de efeito em `+0x4b8`), se `+0x4c8` bit 55,
  se `FUN_14016f740(chr)` (tipo NPC ou fantasma), ou se `+0x4b8` bit 58. A
  quinta, `thunk_FUN_140014b03`, pula para código ofuscado e ficou de fora.

### O que o modo `respawn` faz

Na primeira recusa de uma morte (as seguintes, enquanto a recuperação corre,
são a mesma morte sendo segurada):

1. almas para a mancha (`FUN_14026af40`), se o registro não estiver marcado;
2. hollow com as checagens, e `FUN_1402026e0`;
3. manchas tipo 10 removidas, e a nova criada (`FUN_14026b0d0`, que também
   desmarca o registro para a próxima morte);
4. Estus recarregado;
5. a recuperação da queda: teleporte para o nascimento da fogueira do registro,
   e, quando o controle de queda diz que pousou, bits e câmera limpos e o HP
   cheio no máximo novo.

Cada chamada ao jogo fica atrás de um `__try` próprio, e os bytes de cada
função são conferidos na instalação.

### O resultado

| teste | log do hook | conferido |
| --- | --- | --- |
| HP zerado a 6 m da fogueira, 4321 almas, Estus em 0 | `almas 4321 -> 0 (registradas: 4321 ..., 10000 perdidas da anterior); hollow 1 -> 2; 1 antiga removida, nova criada, agora 1 no mundo; estus recarregado` | de pé na fogueira; a mancha verde onde ele morreu, a da fogueira sumida; ao tocar, 4321 almas de volta e o registro vazio |
| queda no vazio | `almas 4321 -> 0; hollow 0 -> 1; nova criada`, recuperação da queda em 1 quadro | a mancha na última posição segura; câmera normal |
| segunda morte antes de recuperar | `registradas: 1000 almas para a mancha, 4321 perdidas da anterior; 1 antiga removida` | uma mancha só no mundo |
| humano (efígie) morrendo | `hollow 0 -> 1, hp maximo 915 -> 869`, `renascer concluido ... hp 915 -> 869` | `*(chr+0xb0)+0x3e = 1`, HP 869/869 |
| servidor | — | nenhum `RequestNotifyDeath` nem `RequestNotifyKillEnemy` na conexão; nenhum warp |

O Estus apareceu recarregado no próprio inventário do jogo (1 carga depois de
zerado à mão).

## Com sessão (passo 6, 13–14/09)

Samuel em Heide, Chico invocado por marca branca, os dois no modo `respawn`.
Duas sessões, cada uma terminada pelo caminho legal (modo `observe` no Chico e
uma morte comum de fantasma: `RequestNotifyLeaveSession` e
`RequestNotifyLeaveGuestPlayer` em um segundo). Saves fotografados antes
(`pre-passo6`) e restaurados depois.

### Quem paga o quê, pelas checagens do jogo

A cobrança do passo 5 cobrava de qualquer um. O que o jogo decide está na
sequência da morte e em `FUN_14037dcc0`, e o hook agora passa pelas mesmas
portas:

| custo | quem decide | fantasma branco (papel 1) |
| --- | --- | --- |
| almas | `FUN_14018fbc0`, passo da sequência `EventResult` para a morte tipo 1: há gerenciador de sessão (`ctx+0x22f0`) e `NetSvrManager`, e `FUN_14018fd70` concorda — `*(ctx+0x70)+0x1b9` limpo e, se o papel é de convidado (segundo byte da linha do papel na tabela `0x1410c0050`, 16 bytes por papel), o param do papel (`FUN_14016f540`) com `+0x2e == 1` | `+0x2e = 0`: **as almas ficam** |
| mancha | só quando almas entraram agora, e só onde `FUN_14026b0d0` poria uma (slot `+0x58` do contexto) | intocada |
| hollow | as cinco checagens de `FUN_14037dcc0`; a que o decompilador não mostra é `call 0x14016f7d0`, um salto para código ofuscado, `bool(chr)` | a ofuscada diz **isento** |
| contador de mortes, anel | `call 0x140203be0` (outro salto ofuscado) escolhe o ramo do jogador desta máquina: `FUN_140203ad0` soma em `PlayerParam+0x104+papel*8` e `+0x1a4`, `FUN_1401ac240` quebra o anel de proteção vestido, e um menu aberto de `ctx+0x22e0` é fechado | soma |
| Estus | o renascer comum recarrega para todos | recarregado |

O papel é o byte `*(chr+0xb0)+0x3c` (0 dono do mundo, 1 fantasma branco); o
estado de hollow é o `+0x3e` ao lado. Os dois saltos ofuscados foram chamados
como o jogo chama, com o personagem em `rcx`, e os bytes do salto são
conferidos na instalação.

### A cópia do outro jogador morria

A primeira rodada (build `1424e7c8`) acertou tudo do lado de quem morre e
errou do outro lado. Com o HP do Chico zerado:

```
Chico   custos da morte (papel 1, convidado 1): almas 1234 -> 1234 (convidado que nao paga ...);
        hollow isento (... ofuscada=1 ...); mortes 69 -> 70 ...; renascer concluido em 1 quadros
Samuel  outro controlador ... personagem ... (vftable +0x10e4bb8, papel 1) estado 0 -> 2 hp=0
```

Na tela do Samuel, **"Phantom Chico has been vanquished."**; no servidor,
`RequestNotifyKillEnemy` do Samuel. O HP 0 do Chico já tinha saído pela rede
antes de o hook devolvê-lo, e a cópia dele no mundo do Samuel (`PlayerCtrl`,
tipo de personagem 2 em `chr+0x54`) morreu pelo próprio controlador — o mesmo
formato de uma morte local: causa 10, bits `0x4000/0x8000` em `+0x4c8`. As
máquinas de sessão continuaram em `0x10` e 7, mas o host não via mais o
fantasma.

É uma corrida: aconteceu uma vez em duas mortes com o HP zerado por fora do
quadro, e nenhuma vez nas sete seguintes (seis com o HP zerado, uma queda).

A morte de um jogador pertence à máquina que joga o personagem. O build
`69a68af9` recusa, nos modos `cancel` e `respawn`, a morte pendente de uma
cópia de `PlayerCtrl` com o mesmo teste do controlador local (estado 0,
`+0x5fc` zero, `+0x759` ligado): limpa o byte, devolve o HP e os bits, e não
deixa o controlador rodar naquele quadro. Medido com o byte ligado à mão na
cópia do Chico dentro do Samuel:

```
morte da copia RECUSADA #1 ... personagem ... tipo 2 papel 1 hp=853 -> 853 ...
```

e o Chico continuou de pé ao lado dele. Zerar o HP da cópia à mão **não**
serve de teste: a rede o regrava antes do quadro seguinte.

### "YOU DIED"

A sequência da morte (`EventResult` slot `+0x20`, `FUN_14018f830`) monta jobs
a partir de uma linha de parâmetro: id `papel + tipo*100` na tabela do registro
da fogueira (`FUN_14044ed10`), ou `tipo*100 + 99`. O primeiro byte é um tipo
FE, e a tabela de dez ints em `0x1410c3580` o traduz em banner para
`FUN_1405012e0(*(ctx+0x22e0), id)`. Lido ao vivo: a linha 100 (dono do mundo)
e a 199 (fantasma branco) dizem FE 1, banner 3.

O banner 3 esconde o HUD (`+0x46c` de `*(frontend+0xd8)`), e só uma carga o
devolve: `FUN_1404fffb0` liga `+0x468`, que o update do HUD (`FUN_140507360`)
lê como "mostrar tudo de novo". O hook chama a mesma função quando
`FUN_140500b10` diz que o front end terminou. Medido no Chico e no Samuel, em
sessão: o letreiro, o HUD sumido, e `banner 3 acabou em 203 quadros: HUD
devolvido`. Ligado por padrão desde `22880bf2`, conferido solo com esse build
(só `respawn` escrito, `banner=1` no status). O som da morte (`FUN_1401905c0`,
uma sobreposição de BGM que ninguém desfaz sem carga) ficou de fora.

Para fantasma, a morte comum mostra depois do banner "You have been
vanquished. Returning to your world..." (mensagem `0x129da1` da linha 199). O
hook não a mostra: o fantasma não volta para casa.

### A mancha online não sai

`FUN_14026c0b0` (ou `FUN_14026c1b0` para morte tipo 3) é o que o update do
`NetSvrBloodstainManager` chama no quadro em que o personagem local está com
HP 0 e o bit `0x4000`. Rastreado com hora numa morte comum: a chamada no mesmo
quadro da morte, o job (`FUN_14026bd10`) **cinco segundos depois**, e aí
`FUN_14019f520` lê o gravador de fantasma e `FUN_140269650` envia; o servidor
registrou `RequestCreateBloodstain` cinco segundos depois da morte.

Chamada pelo hook, a mesma cadeia passa pelas três checagens de
`FUN_14026bc50`, cria o job, o job roda cinco segundos depois — e não envia.
`FUN_14019f520` só aceita se entre os últimos 16 quadros do gravador houver um
marcado `0x1000` (`FUN_1401a25e0`): um quadro gravado com o personagem morto.
Com a morte recusada no mesmo quadro, esse quadro nunca é gravado. A chave
`mancha_online` fica desligada.

### O resultado

| teste | quem morreu | conferido |
| --- | --- | --- |
| HP zerado a 7,6 m da fogueira (build 1) | Chico, fantasma | lado dele certo; **cópia morta no host**, "vanquished" |
| HP zerado (build 1) | Samuel, host | almas 4321 para a mancha, hollow 0→1, máximo 915→869, mortes 55→56, teleporte; na tela do Chico, o Samuel na fogueira; 60 s depois: nenhum `NotifyDeath`/`LeaveGuestPlayer`, host `0x10`, convidado 7 |
| HP zerado (build 2) | Chico | **o host vê o Chico na fogueira**, sem "vanquished"; 60 s depois host `0x10`, convidado 7, nenhum `LeaveGuestPlayer` |
| byte de morte ligado na cópia (build 2) | cópia do Chico no Samuel | `morte da copia RECUSADA`, cópia de pé |
| quatro mortes seguidas (build 2) | Chico | todas recusadas, nenhuma cópia morta |
| queda no mar (build 2) | Chico | causa 90, câmera de queda desligada, fogueira em 1 quadro |
| queda no mar (build 2) | Samuel | almas para a mancha na beira, hollow 0→1, fogueira; 60 s depois host `0x10`, convidado 7 |
| banner (build 2) | Chico e Samuel | "YOU DIED", HUD devolvido em 203 quadros |
| servidor | — | o primeiro `RequestNotifyDeath` da conexão do Chico só apareceu na morte comum da saída, depois de sete mortes recusadas na sessão |

### Duas armadilhas desta rodada

**O modo volta a `observe` a cada boot.** Um `goto --to-instance 2` feito
antes de escrever `respawn` levou o Samuel para fora da laje de Heide: morte
por queda de verdade, sem sessão aberta, custando a efígie e as almas de teste.
Escreva o modo nos dois `DS2_Death.req` antes de mexer em qualquer personagem.

**Um breakpoint armado no meio de uma instrução derruba o jogo** quando o fluxo
chega nele: o `0xCC` corta a instrução. `bp` precisa do início exato de uma
instrução; confira no objdump antes de armar.

## A fogueira do host (passo 7, 14/09)

Até o passo 6 o convidado renascia na fogueira do **próprio** registro, e só
deu certo porque o Samuel e o Chico tinham a mesma (`0x7ba7`). O registro de um
convidado é o do mundo dele, e o jogo não grava outro enquanto ele está no
mundo do host: `FUN_1401caf50` (acender) e `FUN_1401cb950` (sentar) só gravam
quando quem interagiu é o personagem local **e** o slot `+0x58` do contexto diz
que ele não está no mundo de outro. O registro do host existe só na máquina do
host.

### Um canal P2P, e não o servidor

A lista de tarefas previa um canal "provavelmente pelo servidor". Não precisou:
a sessão entre os dois já é P2P da Steam, e o jogo usa **um canal só** dela. Das
oito chamadas a `SteamNetworking()` no binário, as seis que levam canal passam
0 — `FUN_140a75800` pergunta e `FUN_140a73de0` lê, `FUN_140a7a410` e
`FUN_140a76d90` enviam —, e as outras duas aceitam e fecham a sessão com um
usuário. Um pacote em outro canal viaja pela mesma sessão P2P e fica esperando
na outra máquina, sem que o jogo o toque, até alguém ler aquele canal. O
servidor não participa e nada muda na VPS.

| peça | onde |
| --- | --- |
| o poll da sessão | `FUN_140a75800`, slot `+0x108` de `DLNRD::SteamSessionLight` (vftable `0x1411b1058`), na thread do gerenciador de sessões; cerca de 107 chamadas por segundo em sessão (74 224 em 11,5 min) |
| os membros | vetor em `+0x68..+0x70`; cada um é um `SteamSessionMemberLight` (vftable `0x1411b35e8`) com o CSteamID em `+0xc8`, onde o jogo procura o remetente de um pacote. O próprio jogador está na lista |
| quem é o host | `+0xad` do membro, ligado por `FUN_140a72740` ao acrescentá-lo quando o id dele é o `GetLobbyOwner` do lobby (`+0x3f0` da sessão); o log de depuração do jogo chama de "Host". Lido ao vivo: 1 para o Samuel e 0 para o Chico, nas duas máquinas |
| aceitar pacotes de alguém | `FUN_140a735f0` (`P2PSessionRequest_t`) só aceita quem está no lobby da sessão |
| enviar e ler | `ISteamNetworking` slots `+0x00`, `+0x08`, `+0x10`, com os mesmos argumentos que o jogo passa; o próprio SteamID sai de `ISteamUser` slot `+0x10`, por ponteiro |
| fora de sessão | varredura solo em 14/09: nenhum objeto com a vftable de `SteamSessionLight` |

### O que o mod faz

`DS2_CoopChannelHook` desvia o poll: deixa o jogo rodar e depois lê o canal 7.
Se o jogador local é o host da sessão e o dono do mundo em que está (papel 0),
anuncia `{mapa, tipo, id}` do registro dele para cada outro membro, a cada 2 s
e na hora em que muda. O anúncio tem 24 bytes (`JMJC`, versão, tipo, papel de
quem envia) e só é guardado se vier do host de uma sessão vista nos últimos
5 s. A thread de rede não lê o mundo do jogo: o `DS2_DeathInterceptHook`
publica o papel e o registro a cada quadro, na thread do jogo. `DS2_Channel.req`
com `status` escreve o que o canal viu em `DS2_Channel.log`.

Na morte de quem não é dono do mundo, o renascer procura a fogueira anunciada
no mapa carregado; sem anúncio de menos de 30 s, ou com a fogueira do host fora
do mapa, cai na do próprio registro e, sem ela, na última posição no chão. A
procura agora confere o mapa além do id: `*(*(obj+0x28)+8)`, o mesmo campo que
`FUN_1401caf50` grava no registro (`FUN_1403ba320`), `0x0a1f0000` nas três
fogueiras de Heide, lido em 14/09. O registro do convidado **não** é tocado —
gravar ali a fogueira do host a levaria para o save dele — e a chave
`fogueira_do_host` do `DS2_Death.req` desliga a escolha sem build.

### O resultado

Encenação: o Chico descansou sozinho na fogueira "Tower of Flame" (`0x7ba2`,
69 m ao norte da de Heide) e foi levado de volta à fogueira do Samuel; o
registro dele ficou em `0x7ba2`, o do Samuel em `0x7ba7`. Os dois humanos,
marca branca, sessão formada (`RequestNotifyJoinGuestPlayer` às 01:48:08,
`RequestNotifyJoinSession` às 01:48:10), os dois em `respawn`. Build
`d3cd29e5`.

| teste | quem | log do hook | conferido |
| --- | --- | --- | --- |
| HP zerado na Tower of Flame | Chico | `levando para fogueira do host (6.186, -18.517, 209.053) mapa=0a1f0000 tipo=0 id=00007ba7; papel 1, anunciada por 011000010afd1a3a ha 1050 ms` | de (13.08, 276.66) para (6.27, 209.91); na tela do Samuel, o Chico na fogueira dele |
| HP zerado ao lado do Samuel, `fogueira_do_host off` | Chico | `levando para fogueira do registro (13.056, -6.167, 276.660) ... id=00007ba2` | o controle: sem o anúncio, 69 m para a própria |
| HP zerado na Tower of Flame, chave de volta | Chico | `fogueira do host ... id=00007ba7 ... ha 1064 ms` | ao lado do Samuel de novo |
| queda no mar | Chico | `morte CANCELADA #4 queda ... causa=90`, `fogueira do host ... ha 1778 ms`, `renascer concluido em 1 quadros ... camera de queda desligada` | na fogueira do Samuel |
| HP zerado na Tower of Flame | Samuel | `levando para fogueira do registro ... id=00007ba7`, sem nota de host; hollow 0→1, máximo 915→869, mortes 55→56, mancha trocada | de volta à própria fogueira; na tela do Chico, o Samuel lá |
| 60 s depois da última morte | — | — | host `0x10`, convidado 7; nenhum `RequestNotifyDeath`, `KillEnemy` ou `Leave` no servidor |
| canal | — | host: 347 enviados, 0 falhas; convidado: 346 recebidos, 0 recusados | um anúncio a cada 2 s durante 11 minutos |
| saída | Chico em `observe`, `copias off` no Samuel | — | o primeiro `RequestNotifyDeath` da conexão do Chico às 02:00:09, `LeaveSession` e `LeaveGuestPlayer` às 02:00:21/22; o Chico voltou para casa na **própria** fogueira, `0x7ba2` |

A última linha é a outra metade da prova: o registro do convidado continuou
sendo dele.

### O convidado que ainda está entrando

O mesmo teste mostrou um erro do primeiro build. Nos 10 segundos entre o lobby
se formar e ele chegar ao mundo do Samuel, o Chico ainda era dono do próprio
mundo (papel 0) e anunciou a **própria** fogueira cinco vezes — e o Samuel
aceitou. Com dois jogadores não teve efeito, porque o host não usa anúncio; com
três, um convidado já dentro iria para a fogueira de quem está entrando. Papel 0
não identifica o host da sessão. O que identifica é a marca que o jogo põe no
membro (`+0xad`), e o build `4862d723` só anuncia e só aceita com ela.

Conferido numa segunda sessão com esse build, a mesma encenação (saves com o
Chico ainda em `0x7ba2`):

- os dois logs listam `011000010afd1a3a (host) 0110000140d6d6d1`;
- o Chico **não anunciou nada** na entrada (`enviados=0`), e o Samuel não
  recebeu nada (`recebidos=0`); o Chico recebeu 22 anúncios do Samuel em 40 s,
  nenhum recusado;
- HP do Chico zerado na Tower of Flame: `levando para fogueira do host ...
  id=00007ba7 ...; papel 1, anunciada por 011000010afd1a3a ha 753 ms`, e na
  tela do Samuel o Chico na fogueira;
- 60 s depois, host `0x10` e convidado 7; saída pelo caminho legal
  (`RequestNotifyDeath` às 02:12:41, `LeaveSession` e `LeaveGuestPlayer` às
  02:12:53/54), e o Chico em casa na `0x7ba2`.

Saves devolvidos a `pre-passo6` depois das duas sessões.

### Uma armadilha de encenação

**O teleporte não gira o personagem, e a fogueira só oferece "Rest" a quem está
virado para ela.** Posto no ponto exato de nascimento da Tower of Flame, o
Chico não recebeu prompt nenhum; e andar com o analógico mexe a câmera junto,
de modo que "para baixo" muda de sentido a cada passo. O que funcionou foi
medir o deslocamento de um toque curto no analógico, converter a direção do
mundo para a da tela e dar o último passo *em direção* à fogueira: o prompt
apareceu a 0,16 m do ponto de nascimento.
