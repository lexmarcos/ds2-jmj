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

E a Red Sign Soapstone é igual, medida em 12/09 com o mesmo controle: hollow,
apertar X não coloca placa nenhuma; uma Human Effigy sem o personagem dar um
passo e a placa sai na hora (`Sign 1002 created`). **Forma humana vale para os
dois itens.**

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

**Ressalva, de 12/09:** numa noite de testes o mesmo jogador apareceu hollow
depois de mortes que só aconteceram como fantasma, e precisou de efígie para
recolocar a placa. Ou existe um caso em que a morte de fantasma hollowa, ou
uma das mortes daquela sequência foi no próprio mundo sem eu perceber. As duas
observações estão registradas de propósito; o que decide é uma rodada isolada,
com o personagem humano, uma única morte como fantasma e o item testado logo
depois.

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

## Qual push o host obedece, e qual ele ignora

Três pushes, o mesmo host parado no mesmo lugar, medidos em 12/09 no servidor
local. O que muda entre eles é só quem recebe e o que o outro lado está
fazendo:

| push | para quem | o que acontece |
| --- | --- | --- |
| `PushRequestBreakInTarget` | host ocioso | **o host reage em 1s**: manda `RequestSendMessageToPlayers`, e se o invasor estiver esperando a sessão forma |
| `PushRequestVisit` | host ocioso | **nada**, nos quatro tipos (0 Blue Sentinels, 1 Bell Keepers, 2 Rat, 3) |
| `PushRequestSummonSign` | fantasma com placa no chão | o fantasma retira a placa e tenta entrar, e falha porque ninguém abriu sessão |

O controle rodou no meio disso, logo depois das quatro visitas ignoradas: uma
invasão comum por orbe, `RequestSendMessageToPlayers` às 02:48:24,
`RequestNotifyJoinGuestPlayer` às 02:48:34 e `RequestNotifyJoinSession` às
02:48:36. A máquina formava sessão; a visita é que não faz nada.

O provável motivo da visita ser ignorada: ela é o mecanismo dos covenants, e o
cliente do host checa se uma visita **daquele tipo** é legal onde ele está.
Heide não é área de Bell Keeper nem de Rat, e o patch de zona força a área de
atividade para 103110 de qualquer jeito. Não foi investigado além disso,
porque o resultado já bastava para descartar o caminho.

**O que isso ensina sobre a revanche:** o servidor tem as duas metades
separadas e elas nunca se encontram. O push de invasão abre a sessão no host;
o push de summon faz o fantasma aceitar. Falta provar que servem um ao outro —
é o próximo experimento, e é barato.

## Nenhuma combinação de pushes fecha a sessão

Depois de um duelo de red sign de verdade — com o blob do host guardado, o
host humano e o fantasma recolocando a placa — o servidor tentou reconectar
sozinho de três maneiras. Medido em 12/09, sempre com o par no mesmo lugar:

| tentativa | fantasma | host | resultado |
| --- | --- | --- | --- |
| só o summon | retira a placa, tenta entrar | nada na tela | "Unable to join multiplayer session" |
| só a visita | — | **inerte**, nos 4 tipos | nada acontece |
| só a invasão | — | abre sessão: `RequestSendMessageToPlayers` em 1s, e a própria placa dele some ("Your summon sign has disappeared") | nada acontece |
| invasão + summon, no mesmo tick | retira a placa, tenta entrar | abre sessão | falha |
| invasão, 4s, summon | retira a placa, tenta entrar | abre sessão | falha |

As duas metades existem e **não se encontram**. A leitura mais simples é que
elas não são a mesma sessão: o host abre uma sessão de *invasão* e o fantasma
tenta entrar numa de *summon*. O jogo não tem push que diga a um cliente "você
invocou fulano" — esse estado nasce quando o jogador encosta na placa, e só.

Um detalhe que veio de graça: o host **recusa um segundo push de invasão**
enquanto o primeiro está pendente, com `RequestRejectBreakInTarget`, reason 1.

### O blob não é intercambiável

O `player_struct` que o summon carrega tem que ser o que o host mandou **num
summon**. Tentei emprestar o blob que o mesmo jogador tinha mandado ao criar
uma placa sua: o fantasma ignorou o push por completo, nem retirou a placa.
Com o blob genuíno, ele age todas as vezes. Então a revanche por placa só é
possível para um par que já duelou uma vez — o que, para a funcionalidade
pedida, é exatamente o caso.

### Um host hollow não vê placa nenhuma

Custou uma hora até aparecer. Com o personagem hollow, a placa vermelha do
outro jogador simplesmente **não existe** no mundo dele: nenhum prompt, nada
no chão, e o servidor mandando a placa na lista normalmente. Uma Human Effigy
sem o personagem dar um passo e o prompt "Touch Summon Sign" aparece no mesmo
ponto.

É o espelho do que já estava medido para o orbe, e junto formam a regra:
**forma humana é exigida dos dois lados de um duelo por placa** — de quem
invoca para ver a placa, e de quem coloca para usar o item.

## Onde o cliente manda cada mensagem de placa

Achado procurando os ids do protocolo como imediatos
(`FindImmediate.java`, em `/home/suel/tools/scripts`). Todos caem na mesma
vizinhança dos envios de invasão, o que reforça que ali é a camada de rede do
jogo e não código de jogabilidade:

| função | imediato | mensagem |
| --- | --- | --- |
| `FUN_1406a2610` | `MOV EDX,0x398` | `RequestSummonSign` — **a ação do host**, o botão A na placa |
| `FUN_1406a24f0` | `MOV EDX,0x396` | `RequestRemoveSign` |
| `FUN_1406a1170`, `FUN_1406a1de0` | `MOV EDX,0x394` | `RequestCreateSign` |
| `FUN_1406a0910` | `MOV EDX,0x39b` | `PushRequestSummonSign` |
| `FUN_1406a1840` | `SUB EBX,0x39b` | despacho por id |
| `FUN_1406a6300` | `MOV EDX,0x3d2` | `RequestGetBreakInTargetList` |
| `FUN_1406a6fb0` | `MOV EDX,0x3d3` | `RequestBreakInTarget` |

`FUN_1406a2610` é o alvo se a revanche precisar de patch no cliente: é o envio
que só acontece depois que o host encosta na placa.

## O caminho do cliente quando o host encosta na placa

Levantado em 12/09 com a varredura de breakpoints (ver
[DS2_INVESTIGATION_TOOLS.md](DS2_INVESTIGATION_TOOLS.md)): 520 entradas
armadas na faixa `0x140270000–0x1402a0000`, jogo parado quinze segundos para o
que roda por quadro se desarmar, log apagado, e então o toque.

O confirme do diálogo "Summon this dark spirit?" leva a uma classe com nome
de verdade, vinda do RTTI:

    FUN_1402a5970   construtor de NetSvrSummonSignSummonJob
                    vftables em 0x1410d63a8 e 0x1410d6448

E, ao contrário de tudo o mais neste caminho, ela **tem grafo de chamadas
estático**:

    FUN_1402a14c0  ->  FUN_1402a41b0  ->  FUN_1402a2ca0  ->  FUN_1402a5970

O topo é pequeno o bastante para caber aqui:

```c
void FUN_1402a14c0(undefined8 param_1, undefined4 *param_2)
{
    undefined4 local[6];
    local[0] = *param_2;
    FUN_1402a41b0(param_1, local);
}
```

`FUN_1402a41b0` pega o gerenciador de placas (`FUN_1402128d0`), resolve o
**dword** que recebeu pelo slot virtual `+0x98` — ou seja, o argumento é um
*handle de placa*, não a placa — e, passando as checagens, monta o job.

**Isto é o ponto de entrada que a revanche precisa.** Os dois argumentos foram
capturados em execução, com `bp 2a14c0 deref rdx 4`, em dois summons
diferentes:

    summon da placa 1000   rcx=0x7ffffe591000  [rdx]=25000080  -> handle 0x80000025
    summon da placa 1003   rcx=0x7ffffe591000  [rdx]=45000080  -> handle 0x80000045

Duas coisas ficam decididas:

- **`param_1` é o `NetSvrSummonSignManager`**, e o ponteiro foi o mesmo nos
  dois summons — inclusive depois de fechar e reabrir o jogo. Um hook pode
  guardá-lo com segurança dentro de uma sessão.
- **O handle muda a cada placa.** 0x80000025 e 0x80000045 para placas
  diferentes do mesmo jogador, no mesmo lugar. O bit alto é uma tag; o resto
  parece índice mais geração. Então **repetir o handle guardado não serve**: o
  hook tem que descobrir o handle da placa nova, enumerando pelo gerenciador
  ou interceptando o ponto em que uma placa é registrada.

Esse é o próximo bloqueio de verdade da revanche, e é um bloqueio pequeno:
`NetSvrSummonSignInterface` tem um `GetSummonSignListJob`, e o gerenciador
resolve handles pelo slot virtual `+0x98`.

## A revanche funcionando, do lado do host

12/09, 05:32–05:34, servidor local, com `DS2AutoRematch` ligado nos dois lados:

    05:32:06  1:Samuel  Summoning sign 1000            o duelo normal, o jogador tocou a placa
              hook      "o jogador invocou a placa 80000015"
    05:33:03  3:Chico   morre por queda no mundo do host
    05:33:2x  host      DS2_Rematch.req -> "revanche armada"
    05:33:37  3:Chico   Sign 1001 created              o fantasma recoloca a placa
    05:33:40  hook      "revanche: invocando a placa 80000025 que acabou de chegar"
    05:33:40  1:Samuel  Summoning sign 1001            <- sem ninguem encostar em nada
    05:33:46  3:Chico   Sign 1001 removed by its owner  o fantasma sendo puxado
              tela      Chico de volta como fantasma vermelho no mundo do Samuel

O host não apertou nada. O detour no "adicionar placa" viu a placa nova
chegar, leu o handle do parâmetro de saída (`0x80000025` — diferente do
`0x80000015` do primeiro duelo, como esperado) e chamou o mesmo caminho que o
botão A chama.

**É a metade que faltava.** O servidor sabe lembrar o par, e agora o cliente do
host sabe recomeçar.

### O sinal canônico, com o censo zerado

A rodada acima aconteceu num servidor que já tinha gasto o "primeiro de cada
tipo" nas mensagens de entrada, então ela não prova sozinha. Repetida às 05:39
logo depois de um `reload`:

    05:39:13  3:Chico   Sign 1000 created
    05:39:37  hook      "revanche: invocando a placa 80000015 que acabou de chegar"
    05:39:37  1:Samuel  Summoning sign 1000
    05:39:43  3:Chico   Sign 1000 removed by its owner
    05:39:47  1:Samuel  First RequestNotifyJoinGuestPlayer
    05:39:49  3:Chico   First RequestNotifyJoinSession

As duas últimas linhas são o sinal que este projeto exige, e elas aparecem
depois de um summon que **nenhum jogador pediu**. Vinte e quatro segundos entre
a placa ir ao chão e a sessão formar, dos quais vinte são o intervalo do poll
de placas do cliente — é o que dá para encurtar depois, se valer a pena.

### O que ainda não é automático

A revanche é armada por um arquivo de pedido (`DS2_Rematch.req`), não pela
morte. O gatilho de verdade é o cliente perceber que a sessão terminou numa
morte, e o caminho para isso já está mapeado em
[DS2_SESSION_END_CLIENT.md](DS2_SESSION_END_CLIENT.md): o estado da sessão vai
para 8 e o motivo `2` chega ao pedido de encerramento. Falta ligar uma coisa na
outra.

E falta escolher quando **não** revanchear: o par saiu de perto, o jogador
quer parar, ou a placa que chegou é de outra pessoa. Para dois jogadores no
servidor a placa que chega é sempre do par; para mais, o hook precisa
identificar o dono, que é o campo do item que ainda não foi lido.

## O ciclo fechado, sem ninguém armar nada

Última rodada, 12/09 às 06:04, com a build em que o próprio summon liga a
revanche:

    05:58:11  1:Samuel  Summoning sign 1003          o unico toque humano
              hook      "o jogador invocou a placa 80000015; revanche ligada"
    06:03:xx  sessao termina
    06:04:05  3:Chico   Sign 1004 created            a placa volta ao chao
    06:04:58  hook      "revanche: invocando a placa 80000025 que acabou de chegar"
    06:04:58  1:Samuel  Summoning sign 1004
    06:05:04  3:Chico   Sign 1004 removed by its owner
    06:05:11  3:Chico   First RequestNotifyJoinSession

O `JoinSession` da última linha vem de um cliente que tinha acabado de ser
reiniciado, então o censo dele estava limpo e a linha é honesta.

Entre o primeiro summon e esse, **ninguém apertou nada no host**.

## Como ligar, e o que a funcionalidade é hoje

Duas peças, uma de cada lado, e as duas nascem desligadas:

| onde | flag | o que faz |
| --- | --- | --- |
| servidor | `DS2_AutoRematch` | lembra o último summon de cada dono de placa e sabe reenviar o push; sozinho **não** forma sessão |
| cliente | `DS2AutoRematch` | o `DS2_RematchHook`: invocar uma vez liga a revanche, e cada placa que chegar depois é invocada sozinha |

No harness: `ds2os-dev up --auto-rematch`, ou
`ds2os-dev game prepare --auto-rematch` e relançar (a config é lida na
injeção).

O hook escreve em `DS2_Rematch.log`, ao lado da DLL. Escrever `0` em
`DS2_Rematch.req` desliga; qualquer outra coisa liga.

**O que é hoje:** depois de um duelo por red sign, o fantasma que voltou
recoloca a placa e o host o invoca sozinho, sem apertar nada. Vinte e quatro
segundos entre a placa ir ao chão e a sessão formar, dos quais vinte são o
intervalo do poll de placas do cliente.

**O que não é:** o hook invoca **qualquer** placa que chegue, não só a do par.
Com dois jogadores no servidor dá no mesmo; com três, invocaria o primeiro que
aparecesse. O campo do item que identifica o dono ainda não foi lido, e é o que
falta para isso ficar certo.

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
