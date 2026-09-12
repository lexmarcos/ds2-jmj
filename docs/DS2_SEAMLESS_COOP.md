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
`+0x30` da sessão — "encerre, e este é o motivo". Ele confere um virtual
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
if (*(char*)(sessao[0x21] + 8) != 0)      { slot30(sessao, 0x13);     return; }
if (!FUN_1402c6570(..., papel))           { sessao[0x1f] = 0xb; ... return; }

sessao+0x19c = param_2[0];                // mapa
sessao+0x198 = param_2[7];

/* o bloco "de onde ele veio": lido do jogador VIVO e dos contadores VIVOS */
sessao+0x1a0 .. +0x1c8 = posição atual do jogador, ctx+0x70 +0x164/168/16c,
                         ctx+0xd0 +0x168   /* o portão */

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
bloqueio depois não adianta, e o convidado fica presoentre estados. Matar o
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
