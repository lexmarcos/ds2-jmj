# Revanche depois da morte

O que acontece entre uma morte em PvP e a próxima invasão do mesmo par, medido
nas duas instâncias desta máquina (Samuel, conta 1; Chico, conta 2), em Heide's
Tower of Flame, servidor local.

Este documento existe para responder uma pergunta de projeto: **quanto do
caminho de volta o servidor consegue percorrer sozinho?** A resposta curta é
"quase nada", e o motivo é o item.

## O servidor não consegue iniciar uma invasão

Já estava registrado em [DS2_STICKY_SIGNS.md](DS2_STICKY_SIGNS.md) e continua
valendo: um `PushRequestBreakInTarget` fabricado, enviado sem que o invasor
tenha usado o orbe, não produz nada — nem em Majula, nem em Heide, que é o
controle onde a invasão comum funciona.

> Uma invasão precisa que o cliente **do próprio invasor** esteja em estado de
> invasão. Nada que o servidor mande falsifica isso.

Portanto uma revanche automática não é uma funcionalidade de servidor. O
servidor pode tornar o pareamento determinístico; quem recomeça a sessão é o
cliente do invasor.

## Usar o orbe exige forma humana

Descoberto aqui, e é o obstáculo central da revanche.

Com o personagem **hollow**, apertar X com o Cracked Red Eye Orb selecionado no
cinto não faz absolutamente nada: sem animação, sem mensagem na tela e sem
nenhum pedido chegando ao servidor. O mesmo vale para a Red Sign Soapstone.
Parece um botão que não funciona, e foi assim que custou tempo.

O controle que separa as duas explicações possíveis:

| estado | lugar | X com o orbe |
| --- | --- | --- |
| hollow | colado na fogueira | nada |
| hollow | longe da fogueira | nada |
| humano | longe da fogueira | "Attempt to invade another world?" |
| humano | **no mesmo ponto**, colado na fogueira | "Attempt to invade another world?" |

A última linha é a que importa: a efígie foi usada sem o personagem dar um
passo, e o diálogo passou a aparecer. O bloqueio é a forma humana, não a
fogueira.

**Consequência:** quem morre fica hollow. Um invasor que morreu precisa de uma
Human Effigy antes de poder invadir de novo. Quem sobrevive continua humano —
mas isso ainda não foi medido (ver
[DS2_TO_VALIDATE.md](DS2_TO_VALIDATE.md)).

## A cadeia da morte, medida

Invasor morto por queda no mundo do host (relógio do servidor):

    01:18:36  3:Chico    última posição dentro do mundo do host
    01:18:40  1:Samuel   RequestNotifyKillEnemy
    01:18:52  3:Chico    RequestNotifyLeaveSession          morte + 12s
    01:18:53  1:Samuel   RequestNotifyLeaveGuestPlayer      morte + 13s
    01:18:57  3:Chico    Location: de volta ao próprio mundo morte + 17s

Os 12 segundos entre a morte e o `LeaveSession` batem com os 12,4s medidos em
[DS2_LEAVE_SESSION_BY_KILL.md](DS2_LEAVE_SESSION_BY_KILL.md) numa morte por
kill. O cliente do morto controla esse tempo; nada no servidor o encurta.

Note o `RequestNotifyKillEnemy` do **host**: o jogo contabiliza o invasor morto
como inimigo abatido, mesmo quando a morte foi queda e o host estava parado na
fogueira. Não confunda com `RequestNotifyKillPlayer`.

## O ciclo completo da revanche, dirigido à mão

    morte                               t+0
    LeaveSession                        t+12s
    de volta ao próprio mundo, hollow   t+17s
    Human Effigy pelo menu              ~t+30s
    orbe + confirmar                    ~t+35s
    pedido de invasão no servidor       t+185s (medido: 01:21:45)
    fantasma no mundo do host           ~t+205s

O número grande é ruído meu, não do jogo: inclui uma segunda morte acidental e
navegação de menu lenta. O piso real, com a efígie já no cinto, é **cerca de um
minuto**, e ele se divide assim:

| trecho | quem controla | dá para encurtar? |
| --- | --- | --- |
| morte → LeaveSession, 12s | cliente do morto | não sem mexer no cliente |
| volta ao próprio mundo, 5s | carregamento | não |
| efígie | jogador | sim: cinto, ou remover a exigência |
| orbe + confirmar | jogador | sim: seria o que um patch automatizaria |
| pareamento + carregar, ~20s | rede e carregamento | não |

Do minuto, aproximadamente 35 segundos são carregamento e a espera fixa do
cliente. O que um patch pode eliminar é o resto.

## Pareamento: com dois jogadores já é determinístico

    3:Chico | Break-in target list: type 0, 1 candidates of 2 clients.
    3:Chico |  candidate '1:Samuel': invadable yes.
    3:Chico | Break-in target request: target 1
    3:Chico | Invading '1:Samuel' across areas.

Com dois clientes no servidor a lista de alvos tem exatamente um candidato, e é
sempre o mesmo par. Um "lembrar o último oponente" no servidor não muda nada
aqui; só passaria a valer com três ou mais jogadores, que esta máquina não
consegue testar.

O host não precisa fazer nada: não há consentimento numa invasão. Toda a
revanche depende de **uma ação de um jogador** — o invasor usar o orbe.

## O que isso deixa como projeto

Em ordem de custo:

1. **Tirar a exigência de forma humana do item de invasão** (cliente). É um
   predicado, do mesmo tipo que o `DS2_UnblockMultiPlayHook` já trata. Sem
   isso, "automático" é impossível: o morto sempre precisa de uma efígie.
2. **Usar o orbe sozinho** quando a sessão terminou em morte e o par ainda está
   no ar (cliente). Precisa achar o ponto de entrada que o uso do item chama.
3. **Lembrar o par no servidor**, para quando houver mais de dois jogadores.
   Barato, mas sozinho não entrega nada.

## Ainda não medido

- Se quem **vence** continua humano (o invasor que mata o host, e o host que
  mata o invasor). Muda se a revanche custa uma efígie de um lado só.
- Se a morte por queda e a morte por kill produzem a mesma cadeia: aqui a
  queda não mostrou `RequestNotifyDeath`, mas o censo do servidor só registra a
  **primeira** mensagem de cada tipo por cliente, então isso não é evidência.
- A arena (`DS2_QuickMatchManager`, implementado no servidor) é o laço de
  revanche do próprio jogo, em mapas fixos e com registro por partida. Nunca
  foi exercitada neste fork.
