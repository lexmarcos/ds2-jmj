# Co-op seamless: o ponto de partida

O objetivo: dois jogadores atravessando o jogo juntos de ponta a ponta, e uma
morte — de qualquer um dos dois — mandando o jogador para a **última fogueira
dentro da mesma sessão**, em vez de desfazer a sessão e mandá-lo para o
próprio mundo.

Este documento é o que já se sabe quando o trabalho começa, em 12/09. Quase
tudo veio do caminho levantado para a revanche por red sign; ver
[DS2_SESSION_END_CLIENT.md](DS2_SESSION_END_CLIENT.md) para a cadeia completa.

## O que já está mapeado

A morte de um fantasma passa por três coisas distintas, e confundi-las custou
uma madrugada:

1. **Encerrar as sessões.** Uma tabela em `0x1410c0050`, vinte entradas de 16
   bytes indexadas por um papel lido de `objeto+0xe0`, e o byte `+1` de cada
   entrada decide se aquele papel desfaz as sessões ao morrer. O fantasma é o
   **índice 7**; o byte dele mora em `0x1410c00c1`.
2. **Demolir a sessão.** `FUN_1402c3900`, o handler do estado 8 da máquina de
   estados da sessão. Manda o `RequestNotifyLeaveSession` e põe o estado em 9.
3. **Mandar o jogador para casa.** Um **warp**, e é aqui que mora o co-op.

## O warp é a peça

Medido: zerar o byte da tabela para o índice 7 **não** impede o retorno. O
fantasma vê "You have been vanquished. Returning to your world…" do mesmo
jeito; o que muda é só se as sessões são desfeitas junto. Ou seja, suprimir a
tabela deixaria o jogador indo para casa com a sessão pendurada — pior que o
comportamento original.

O warp sai de duas chamadas ao mesmo slot virtual `+0x40` do contexto global
do jogo (`DAT_1416148f0`):

| sítio | quem chama | forma do pedido |
| --- | --- | --- |
| `0x14044fe1f`, em `FUN_14044fde0` | o ramo "não encerre as sessões" | destino vazio: dois qwords `-1` e zeros |
| `0x1402c3bdb`, em `FUN_1402c3900` | a demolição normal da sessão | ver abaixo |

O pedido da demolição, capturado em execução com
`bp 2c3bdb deref rdx 32` no cliente do fantasma, numa morte por queda dentro
do mundo do host:

    +0x00  03 00 00 00      3
    +0x04  04 00 00 00      4
    +0x08  00 00 1f 0a      0x0a1f0000
    +0x0c  ff ff ff ff      -1
    +0x10  00 00 00 00
    +0x14  03 7f 00 00      0x7f03
    +0x18  a7 7b 00 00      0x7ba7
    +0x1c  00 00 00 00

Os campos ainda não foram decifrados. O caminho óbvio para decifrá-los é
capturar o **mesmo** struct numa morte comum, no próprio mundo, onde o jogo
leva o jogador à última fogueira — a diferença entre os dois pedidos é o que
diz como se pede "vá para a fogueira" em vez de "vá para casa".

## O que ainda não se sabe

- Qual sítio constrói o warp de uma morte comum (a que renasce na fogueira).
  Não é `FUN_14044fde0` — o breakpoint lá não disparou numa morte de fantasma.
- O que cada campo do pedido significa.
- Se o host aceita continuar a sessão com um convidado que "morreu": o jogo
  pode ter outras contas a acertar (o fantasma sumindo da tela do host, a
  bloodstain, os itens).
- Se a sessão sobrevive ao carregamento da fogueira. Um carregamento de área
  dentro do mundo do host é coisa que o jogo nunca faz para um convidado.

## O que já existe e ajuda

O `DS2_RematchHook` prova que dá para chamar código do jogo a partir do
injector, na thread certa, com os argumentos certos — e a técnica é a mesma
que o co-op vai precisar. Ver
[DS2_REMATCH_AFTER_DEATH.md](DS2_REMATCH_AFTER_DEATH.md).
