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

Com o personagem **hollow**, apertar X com o Cracked Red Eye Orb selecionado no
cinto não faz absolutamente nada: sem animação, sem mensagem na tela e sem
nenhum pedido chegando ao servidor. Parece um botão que não funciona, e foi
assim que custou tempo.

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

## Mas morrer como invasor não tira a forma humana

Esta é a descoberta que muda o projeto, e ela quase passou batida porque a
primeira leitura dos dados estava errada.

O que hollowa é morrer **no próprio mundo**. Uma morte como invasor, dentro do
mundo do host, devolve o jogador para casa **ainda humano**.

A confusão: na primeira rodada o invasor morreu como fantasma, voltou, e o
orbe não funcionou — mas entre as duas coisas ele tinha caído de um penhasco
**no próprio mundo**, e foi essa segunda morte que o deixou hollow. A efígie
seguinte pareceu ser o que consertou a morte do duelo. Não era.

A medição limpa, sem nenhuma efígie envolvida:

    01:42:17  3:Chico   última posição dentro do mundo do host
      (morte por queda como fantasma)
    01:42:52  3:Chico   Location: de volta à própria fogueira
    01:42:54  3:Chico   Break-in target request: target 1
    01:42:54  servidor  Invading '1:Samuel' across areas.

Duas invasões seguidas, a segunda dois segundos depois de chegar em casa, sem
Human Effigy no meio. O contador de efígies não se moveu.

**Consequência:** a revanche não custa item nenhum além do próprio orbe, e a
exigência de forma humana **não** precisa ser removida para o invasor. Ela só
apareceria se o mesmo jogador morresse também no mundo dele.

## O ciclo completo da revanche, medido

    morte como invasor                  t+0       (±10s: a queda só é visível
                                                   na tela, e a última posição
                                                   no mundo do host é de 01:42:17)
    LeaveSession                        t+12s     (medido na primeira rodada)
    de volta ao próprio mundo           ~t+35s
    X, esquerda, A                      ~t+37s
    pedido de invasão no servidor       ~t+37s
    fantasma no mundo do host           ~t+55s

| trecho | quem controla | dá para encurtar? |
| --- | --- | --- |
| morte → LeaveSession, 12s | cliente do morto | só mexendo no cliente |
| carregar de volta, ~20s | carregamento | não |
| **três toques de botão, ~2s** | **o jogador** | **é o que a automação elimina** |
| pareamento + carregar, ~20s | rede e carregamento | não |

Vale dizer isso claramente: de aproximadamente 55 segundos, o jogador responde
por **dois**. Uma revanche automática não deixa o duelo mais rápido; ela tira a
atenção do jogador do laço. Quem quiser cortar o resto teria que atacar a
espera de 12s ou o próprio fim da sessão — fazer o fantasma renascer no mundo
do host em vez de voltar para casa, que é como a arena funciona.

## O sinal positivo, para o caminho do orbe

As medições acima terminam num fantasma visível na tela, que é a evidência
fraca contra a qual este projeto vive avisando. O handshake completo ficou
registrado em 12/09, num servidor recém reiniciado — o que importa, porque o
censo só registra a **primeira** mensagem de cada tipo por cliente, e por isso
essas linhas somem numa sessão longa:

    02:21:02  3:Chico   Break-in target request: target 1
    02:21:02  servidor  Invading '1:Samuel' across areas.
    02:21:12  1:Samuel  First RequestNotifyJoinGuestPlayer
    02:21:14  3:Chico   First RequestNotifyJoinSession

São as duas linhas que o CLAUDE.md chama de sinal de sucesso: dez segundos
entre o pedido e o convidado dentro do mundo.

Vale anotar o que quase estragou essa rodada. Por um tempo **nenhuma** sessão
se formava, nem por orbe nem por placa, porque havia dois jogos abertos na
mesma conta. Tudo antes do fim parecia certo — o servidor roteava o push, o
alvo respondia `RequestSendMessageToPlayers` — e então um lado dizia
"Summoning failed. Timed out." e o outro "Disconnected from multiplayer
session." Fechar o cliente sobrando resolveu na hora.

## Sair para o título é recusado durante uma sessão

O item **Quit Game** aparece no menu de sistema e fica selecionável, mas apertar
A não faz nada enquanto existe uma sessão PvP — nos dois lados, host e
fantasma. Foi assim que `game leave` falhou por 180s sem dizer o motivo.

Pelo que foi visto aqui, resta a morte, o temporizador e a desconexão. Os
itens de saída (Separation Crystal, Homeward Bone) não foram testados.

## O push de summon é aceito — e mesmo assim a sessão não forma

Medido em 12/09 com o gatilho `debug_summon.req` e o par já tendo duelado uma
vez (é o duelo anterior que deixa no servidor o id do host e o blob opaco que
ele mandou).

    02:22:16  1:Samuel  Summoning sign 1003              o duelo de verdade
    02:25:27  3:Chico   Sign 1004 created: type 4        a placa de volta ao chão
    02:25:43  servidor  Rematch: replayed the summon of sign 1004 by player 1
    02:25:43  3:Chico   Sign 1004 removed by its owner
    02:25:5x  3:Chico   "Summoning canceled. Unable to join multiplayer session."
              1:Samuel  nada. nenhum diálogo, nenhuma mensagem

As duas linhas do meio são a parte boa, e é um resultado novo: **o cliente que
está com uma placa no chão age num push que ninguém pediu.** Ele retira a
placa e tenta entrar, que é exatamente o que faz num summon legítimo. O push
de invasão fabricado não produz nem isso — o invasor parado não está
esperando nada.

A parte ruim é a última linha. O host nunca soube de coisa alguma. Quem abre a
sessão é o cliente do host quando ele aperta o botão na placa; o servidor não
consegue criar esse estado, e repetir o `player_struct` guardado não basta —
ou ele é de uso único, ou falta o lado que escuta. O fantasma tenta conectar
num host que não está esperando ninguém, e o jogo diz isso com todas as
letras.

**O que isso significa para a funcionalidade:** a revanche pela red sign
também precisa de um patch no cliente, só que do outro lado. No caminho do
orbe quem precisa agir é o invasor; no caminho da placa é o **host**, que
teria que reemitir sozinho o summon da placa do mesmo par. O servidor faz a
metade dele: lembra o par e sabe reenviar o push.

Uma versão intermediária que não precisa de patch nenhum: o fantasma recoloca
a placa (um toque), o host aperta A nela (um toque). Nada é consumido, e a
placa reaparece no mesmo lugar, ao lado do host.

## O que isso deixa como projeto

Em ordem de valor:

1. **Usar o orbe sozinho** quando a sessão terminou em morte e o par ainda está
   no ar (cliente). É a funcionalidade pedida, e agora ela é pequena: não
   precisa de efígie, não precisa mexer na forma humana, só disparar o mesmo
   caminho que os três toques disparam. Economiza dois segundos de relógio e
   toda a atenção do jogador.
2. **Não terminar a sessão na morte** (cliente). É o único caminho que corta
   os ~50 segundos de verdade, e é muito mais fundo: o fantasma teria que
   renascer no mundo do host em vez de voltar para o seu.
3. **Lembrar o par no servidor**. Com dois jogadores a lista de alvos já tem um
   candidato só; isso só passa a valer com três ou mais, que esta máquina não
   consegue testar.

O ponto de entrada para (1): os envios estão em `FUN_1406a6300`
(`RequestGetBreakInTargetList`, 0x3d2) e `FUN_1406a6fb0` (`RequestBreakInTarget`,
0x3d3), achados procurando os ids do protocolo como imediatos.
`getCallingFunctions` não achou chamador para nenhum dos dois — o que num
projeto aberto com `-noanalysis` não prova que não existam. Se o grafo de
chamadas não levar ao botão X, o caminho é um breakpoint em execução
(`DS2_Trace.req`) em `FUN_1406a6fb0`, que é o envio que só acontece **depois**
do YES; o da lista de alvos pode sair antes, para o jogo decidir se mostra o
diálogo.

## Ainda não medido

- Se a Red Sign Soapstone pode ser usada hollow. O teste que parecia provar
  que sim foi feito com o personagem humano sem que eu soubesse, então não
  vale nada. O orbe é o único item medido.
- Se o **host** que morre continua podendo ser invadido sem fazer nada. Ele
  hollowa, mas ninguém precisa de forma humana para ser invadido.
- Se a morte por queda e a morte por kill produzem a mesma cadeia: aqui a
  queda não mostrou `RequestNotifyDeath`, mas o censo do servidor só registra a
  **primeira** mensagem de cada tipo por cliente, então isso não é evidência.
- A arena (`DS2_QuickMatchManager`, implementado no servidor) é o laço de
  revanche do próprio jogo, em mapas fixos e com registro por partida. Nunca
  foi exercitada neste fork.
