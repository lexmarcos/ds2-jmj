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
| `+0x08` | `registro+0x164` | id do mapa (`0x0a1f0000` = Majula) |
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

Morte comum (Samuel caiu no mar em Majula), capturada com
`bp 1c2a80 deref rdx 56`:

    +0x00  03 00 00 00   família 3
    +0x04  01 00 00 00   motivo 1 - última fogueira
    +0x08  00 00 1f 0a   mapa 0x0a1f0000, Majula
    +0x0c  ff ff ff ff
    +0x10  00 00 00 00
    +0x14  03 eb b0 c1   o byte 3; o resto é lixo de pilha
    +0x18  a7 7b 00 00   ponto 0x7ba7
    +0x1c  48 6e 3d 41   lixo de pilha daqui para baixo

Volta forçada de um convidado, capturada com `bp 2c3bdb deref rdx 32` numa
morte de fantasma no mundo do host:

    +0x00  03 00 00 00
    +0x04  04 00 00 00   motivo 4 - de volta ao próprio mundo
    +0x08  00 00 1f 0a   mesmo mapa: o duelo foi em Majula
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
quem pediu — o `de=+0x...` é o que identifica o caminho.

## O que isto **não** faz

Ser honesto aqui importa mais que a feature:

- **A sessão continua acabando.** O `RequestNotifyLeaveSession` sai antes do
  warp, e a demolição em `FUN_1402c3900` não é tocada. O que muda é só **onde
  o jogador aterrissa dentro do próprio mundo**: na última fogueira em vez do
  ponto de retorno.
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

## O que ainda não se sabe

- Que motivos existem além de 1 e 4, e o que o portão em `0x140248940` cobra.
- O que significam `+0x00` (família 3/4/2) e `+0x14`.
- Se a fogueira de um convidado é alcançável a partir do mundo do host — isto
  é, se o mapa e o ponto que o registro guarda ainda são os dele enquanto ele
  é fantasma. É a primeira coisa que o log responde.
- Se o host que morre com fantasma dentro emite motivo 4 para alguém, e por
  qual caminho.
