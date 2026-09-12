# Seamless Co-op: o plano, em ordem

O enunciado está em [DS2_SEAMLESS_COOP_DESIGN.md](DS2_SEAMLESS_COOP_DESIGN.md).
O que já foi medido do cliente está em
[DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md). Este arquivo é a lista de
trabalho, e existe para ser **editada**: quando um marco fecha, ele vira uma
linha de "feito" com o endereço e a prova, não some.

**M0 entrou na frente de tudo**, a pedido: sem levar um personagem até o outro
sem mão humana, cada teste destes custa meia hora de pilotagem. Está feito —
ver [DS2_INVESTIGATION_TOOLS.md](DS2_INVESTIGATION_TOOLS.md) para o que ele faz
e o que não faz.

A ordem não é preferência, é dependência. **Tudo de M2 para baixo pressupõe
M1.** Enquanto uma morte desfizer a sessão, respawn, espectador, party wipe,
viagem em grupo e reset de fogueira não têm onde acontecer.

Regra que vale para todo item: **sucesso é sinal positivo**. Para sessão, é
`RequestNotifyJoinGuestPlayer` seguido de `RequestNotifyJoinSession` chegando
ao servidor. Ausência de erro não prova nada.

---

## Feito

| o quê | onde | prova |
| --- | --- | --- |
| Multiplayer nas áreas fechadas | `DS2ForceMultiPlayZone` + `DS2_InvadeAnywhere` | marca vermelha em Majula |
| Sem limite de 12 min na sessão | `DS2PatchPhantomTimers` | [DS2_PHANTOM_TIMER_PATCH.md](DS2_PHANTOM_TIMER_PATCH.md) |
| Host reinvoca a placa sozinho | `DS2_RematchHook` | `Summoning sign` sem ninguém apertar nada |
| …inclusive placa **branca** de co-op | o hook não olha o tipo | `Sign … type 1` invocada sozinha |
| Todo warp do jogo mapeado e interceptável | `DS2_SeamlessCoopHook`, `+0x1c2a80` | [DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md) |
| Morte de convidado vai para a última fogueira | motivo 4/força 0 → `FUN_14044fde0` | medido com o interruptor ligado e desligado |
| **M0 — o harness anda sozinho** | `DS2_NavHook` + `ds2os-dev where` / `goto` | 9 m da fogueira até o outro personagem, escada acima, em 17 passos |

---

## M1 — a sessão sobrevive a uma morte — **FEITO em 12/09**

Recusar o pedido de fim de sessão mantém a sessão viva. Medido, com a recusa
armada só do lado do convidado (`block 2`, `role 1` em `DS2_Session.req`) e o
fantasma branco morto por queda:

    fim de sessao RECUSADO papel=1 estado=7 motivo=2 de=+0x2c9246

- **nenhum warp aconteceu** — o `DS2_Seamless.log` do convidado ficou vazio, ou
  seja ele não foi mandado para casa;
- **o host não pediu nada**, o log dele continuou vazio;
- e a sessão continuou de pé **nos dois lados**: o HUD do host seguia listando
  "Chico" com a barra vazia, e o HUD do convidado seguia mostrando a barra do
  "Samuel".

O que **não** está resolvido, e é o M2: o convidado fica morto onde caiu.
Nada o levanta. Recusar o fim impede o desmonte; não faz renascer.

Também não está medido quanto tempo a sessão aguenta assim, nem o que acontece
se o host andar para outra área com um convidado morto pendurado.

### Como era antes

**O marco que destrava o resto.** Hoje: o convidado morre, `FUN_1402c3900`
(estado 8) manda `RequestNotifyLeaveSession`, a sessão acaba e ele vai para o
próprio mundo.

O que já se sabe:

- a sessão é uma máquina de estados, com o estado em `objeto+0xf8`;
- **estado 2** = `FUN_1402c2a80`, leva o convidado para dentro do mundo do
  host; monta um pedido de warp com `motivo 4`, terceiro argumento **1**, e o
  destino é mapa + posição + orientação;
- **estado 8** = `FUN_1402c3900`, desmonta; warp com `motivo 4`, terceiro
  argumento **0**;
- a transição de estado passa pelo slot virtual `+0x30` do objeto de sessão;
- a tabela de papéis em `0x1410c0050` decide se uma morte desfaz as sessões —
  o fantasma é o índice 7, byte em `0x1410c00c1`. **Zerar esse byte não
  impede o retorno**: a sessão fica pendurada e o jogador vai embora do mesmo
  jeito. Medido.

A hipótese a testar, nessa ordem:

1. numa morte de convidado, impedir a ida ao estado 8 (hook no `+0x30`);
2. reemitir um warp no formato do estado 2 — `motivo 4`, terceiro argumento 1,
   mapa do host, posição de destino — em vez do warp de volta;
3. ver se o host continua enxergando o fantasma e se a sessão continua viva.

### Medido em 12/09: quem pede o fim, e com que motivo

Com um co-op de verdade formado (`RequestNotifyJoinGuestPlayer` seguido de
`RequestNotifyJoinSession`) e o convidado morto por queda, o hook registrou,
**no cliente do convidado**:

    fim de sessao pedido  papel=1  estado=7  motivo=2  motivo_anterior=0  de=+0x2c9246

E no **host, nada**. O log dele ficou vazio.

Três coisas saem daí:

- o estado era **7**, como o caminho estático previa, e o motivo **2** é o que
  vai parar em `+0x1cc` e empurra a máquina para o estado 8;
- o papel do fantasma branco de co-op é **1** (o invasor de vermelho é 7);
- **o desmonte nasce inteiramente do lado de quem morreu.** O host não pede
  nada. Isso é o que torna o M1 plausível com um hook só: recusar do lado do
  convidado pode bastar, sem tocar no cliente do host.

Quem pede é `+0x2c9246`, ainda não decifrado.

Como provar o resto: sem novo `RequestNotifyLeaveSession` no servidor, e o
fantasma visível na tela do host depois do respawn.

Risco conhecido: o warp **devolve um byte**, e o estado `0x13` é para onde a
máquina vai quando ele recusa. Um retorno ignorado esconde a recusa.

---

## M2 — respawn dentro da sessão

Começado em 12/09. O problema está definido com precisão agora, e a peça que
falta tem nome.

**O estado depois do M1:** o convidado fica **morto onde caiu**, dentro da
sessão do host. Os dois HUDs continuam listando o outro. Nada o levanta.

Três coisas medidas ao chegar aqui:

- **o pedido de fim de sessão é de uma vez só.** Recusado, não é repetido:
  escrever `clear` depois não faz o jogo tentar de novo, e o convidado
  continua morto e na sessão. Ou seja, recusar não adia o desmonte, cancela.
- **nenhum warp é emitido**, então não há pedido a reescrever — a abordagem de
  trocar o destino, que resolveu a volta para casa, não tem onde pegar aqui.
- **`jogador+0x64` é o arquétipo**, o mesmo número que o servidor chama de
  `archetype`. Achado diffando o struct do host contra o do convidado, e agora
  publicado pelo `DS2_NavHook`; `ds2os-dev where` mostra como `papel`.

**O que falta é um revive**, e é uma primitiva que o jogo tem de ter: o **Ring
of Life Protection** ressuscita em pé, com a vida cheia, **sem recarregar
área** — é o único revive do DS2 que não passa por um carregamento. Achar o
que esse anel dispara é o caminho mais curto para o M2.

### O que a varredura achou, e o que ela não achou

Varredura de breakpoints sobre as 385 funções de `0x140185000`–`0x140195000`,
com o jogo ocioso primeiro e depois uma morte comum: dispararam mais de 130.
A faixa inteira é o subsistema do personagem e uma morte toca quase tudo, então
a lista em si não aponta nada. Os **chamadores**, sim: `+0x44e9c8`, `+0x44ee7c`,
`+0x44eec6`, `+0x44eee7`, `+0x44ef6e`, `+0x44ef80` — todos colados no construtor
do pedido de warp.

Isso estabelece que `0x44e9b0`–`0x44fde0` é o **gerenciador de estado do
jogador**, e que ele opera sobre o mesmo `*(contexto+0x70)` que o renascimento
já usa:

| função | o que faz |
| --- | --- |
| `FUN_14044e9b0(obj, x)` | repassa `x` para sete sub-objetos |
| `FUN_14044ed40(obj, saida)` | monta o pedido de warp a partir do registro |
| `FUN_14044ee60(obj)` | **reseta** dez sub-objetos (`+0x40`…`+0xb8`) e limpa `*(obj+0x10)+0x21` |
| `FUN_14044fde0(obj)` | renascer na última fogueira |

`FUN_14044ee60` é chamado em `0x1401bf730` como `mov rcx,[rbx+0x70]; call`, o
que confirma o objeto — mas o código em volta é de **desmontagem**, uma fila de
"se o ponteiro existe, reseta", não de ressurreição.

**O revive não foi encontrado.** A evidência acumula na direção de que o DS2
não tem um, fora do caminho do Ring of Life Protection: o levantar-se está
embutido no carregamento de área que vem depois da morte.

### A hipótese que o M1 abriu, e que é o próximo teste

Se o revive vem junto do carregamento de área, a pergunta deixa de ser "como
ressuscitar em pé" e passa a ser:

> **a sessão sobrevive a um carregamento de área do convidado?**

O M1 provou que o objeto da sessão sobrevive a uma morte. Falta saber se ele
sobrevive a um load. Dá para medir sem escrever uma linha de código: com uma
sessão de pé e o convidado **vivo**, mandá-lo usar uma Homeward Bone — que é um
carregamento de área voluntário — e ver se a sessão continua listada nos dois
HUDs. Se sobreviver, o M2 vira "deixe a morte seguir o caminho normal e reentre
no estado 2", e não precisa de revive nenhum.

Onde **não** procurar, já verificado: os primeiros `0x200` bytes do objeto do
jogador não têm HP nem bandeira de morte. Um diff vivo-contra-morto ali só
mostra nome, arquétipo e posição, e uma varredura de 2 KB não achou nenhum par
de inteiros iguais que pareça (vida atual, vida máxima). O HP mora atrás de
algum sub-objeto.

### O plano original, para referência

Depende de M1. O convidado morre, perde as almas, deixa a bloodstain onde
morreu e reaparece na fogueira **do mundo do host**, ainda na sessão, e volta
andando.

Aberto: qual fogueira. O registro de renascimento do convidado
(`*(contexto+0x70)`, campos `+0x164` mapa / `+0x168` tipo / `+0x16c` ponto)
aponta para a fogueira **dele**. Para respawnar no mundo do host é preciso ou
trocar esse registro enquanto ele está na sessão, ou montar o pedido com o
mapa e o ponto do host.

---

## M3 — entrar uma vez, sem ritual

Tirar da entrada tudo que hoje é cerimônia:

- **sem soapstone**: uma forma de entrar por senha, não por placa no chão. O
  caminho da placa já está localizado — `FUN_14029ec00`, o job em `param_1`,
  o construtor acima em `+0x286239` — e serve enquanto a entrada por senha não
  existir;
- **sem efígie**: hoje a placa vermelha é recusada hollow e a branca não;
  nenhuma das duas deveria bloquear entrada;
- **sem Soul Memory**: o servidor já tem os parâmetros por item
  (`DS2_WhiteSoapstoneMatchingParameters` e companhia em
  `Source/Server/Config/RuntimeConfig.h`). Abrir é configuração, não código.
  Ver [DS2_SOUL_MEMORY_MATCHMAKING.md](DS2_SOUL_MEMORY_MATCHMAKING.md).

---

## M4 — estado de mundo autoritativo

Portas, alavancas, elevadores, atalhos, illusory walls e mecanismos de Pharros
abertos pelo host aparecem abertos para quem entrou. Nada disso foi
investigado ainda. O trabalho vizinho mais próximo é
[DS2_FOG_GATES.md](DS2_FOG_GATES.md), que já achou a classe e o teste por
quadro das barreiras de área.

---

## M5 — loot individual

Cada jogador abre o próprio baú e recebe o próprio item. Não investigado.
Pré-requisito honesto: entender como o jogo decide que um pickup já aconteceu,
e se essa decisão é local ou vem do host.

---

## M6 — recompensa de boss para todos

Almas, alma do boss e item para cada participante, sem a redução do fantasma.
Não investigado.

---

## M7 — o que foi feito junto entra nos dois saves

Bosses mortos e quests feitas juntos passam para o save de quem entrou; o
progresso **anterior** do host não passa. Esta é a segunda metade do problema
e provavelmente o maior trabalho depois de M1: exige o cliente do convidado
escrever no próprio save flags de um mundo que não é o dele.

Começar por boss, que é uma flag só e fácil de verificar, antes de qualquer
quest.

---

## M8 — fogueira e viagem

- descansar reseta o mundo para todos, com aviso antes
  ("A player is resting at a bonfire");
- fast travel move o grupo, com votação;
- Bonfire Ascetic **não** sincroniza: a intensidade é do mundo do host.

---

## M9 — espectador e party wipe

Morte dentro de luta de boss vira modo espectador; todos mortos é party wipe
com o boss resetado e a sessão viva; a vitória de quem sobrou vale para quem
morreu antes. Depende de M1 e M2.

**Não testável por completo aqui**: espectador com dois vivos e party wipe de
três precisam de três jogadores, e esta máquina tem duas contas Steam.

---

## M10 — quests de NPC

Lucatiel, Benhart, Pate, Creighton, Navlaan, Cale, Gilligan, Carhillion,
Felkin. Estado segue o host durante a sessão; o que acontece junto sincroniza.
O item mais caro da lista e o mais fácil de corromper save — deixar por último,
atrás de M7.

---

## M11 — invasões, covenants, Company of Champions

- `allow_invasions` liga invasão contra o grupo (3v1, ou mais de um invasor);
- covenant é individual e não muda ao entrar;
- Company of Champions do host impõe a dificuldade a todos sem trocar o
  covenant de ninguém;
- despawn de inimigos segue o host e não marca no save de quem entrou.

---

## O que bloqueia, e não é falta de trabalho

- **Três jogadores.** Duas contas Steam nesta máquina, sessão peer to peer por
  Steam id. Tudo de 3+ jogadores fica sem verificação local.
- **Endereços presos a uma versão.** Tudo aqui é 1.03 Calibrations 2.02. Uma
  atualização do jogo move tudo; por isso todo hook confere os bytes antes de
  escrever.
