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
| **M3 — sem efígie**, placa branca | `DS2_HollowSummonHook` (`+0x2a1b1e`), com `--seamless` | Samuel e Chico hollow (estado 1): `Summoning sign` → `RequestNotifyJoinGuestPlayer` → `RequestNotifyJoinSession`, `p2pSessionVerified: true` (14/09) |
| **M3 — sem soapstone e sem toque**, primeira metade | `DS2_PartyHook` (convidado: `placa 1`) + `DS2_RematchHook` (host: `alvo <jogador> <tipo>`), com `--seamless --auto-rematch` | nenhuma tecla nos dois: `Sign 1015 created` → `Summoning sign 1015` → `JoinGuestPlayer` → `JoinSession`, `p2pSessionVerified: true`, os dois hollow (14/09) |
| **M3 — sem Soul Memory**, placa branca | `DisableSoulMemoryMatching` em `DS2_WhiteSoapstoneMatchingParameters` e `DS2_SmallWhiteSoapstoneMatchingParameters` | com tiers que separam 5130 de 2551: `refused by matching 1` desligado, `sent 1` e a invocação com ele ligado (14/09) |
| **M3 — entrar uma vez, sem ritual** | `DS2PartyGuest`/`DS2PartyAccept`/`DS2PartyPassword` (`DS2_PartyHook`), passada de party no `DS2_SignManager`, chegada de outro mapa no `DS2_DeathInterceptHook`; `up --seamless --party` | senha, quatro casos; Majula → Heide sem tecla nem queda, `p2pSessionVerified: true` (15/09) |

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

**E ela não aguenta indefinidamente.** Medido depois: deixado assim, o
convidado acabou recebendo *"Disconnected from multiplayer session."* e voltou
ao próprio mundo vivo, sem que nada fosse pedido de novo. Ou seja, recusar o
fim de sessão **adia** o desmonte enquanto alguém age, não o cancela para
sempre: um convidado morto que nunca levanta é uma sessão que expira.

Isso não desfaz o M1 — no momento da medição não houve warp, o host não pediu
nada e os dois HUDs listavam o outro — mas muda o que ele significa. O M1
compra tempo para o M2 acontecer; não substitui o M2.

Também não está medido o que acontece se o host andar para outra área com um
convidado morto pendurado.

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

## M2 — respawn dentro da sessão — **FEITO em 14/09, nos casos medidos**

**Os oito passos estão feitos (14/09).** O critério foi observado em todos os
casos medidos, com dois jogadores: a morte do host e a do convidado, por HP e
por queda, com a fogueira no mapa carregado, inclusive quando o registro do
convidado aponta para outra (passos 6 e 7), e por HP com a fogueira em outro
mapa, nas duas direções entre Heide e Majula (passo 8). O que não foi medido —
queda com a fogueira em outro mapa, outros pares de mapas, três jogadores — está
em [DS2_TO_VALIDATE.md](DS2_TO_VALIDATE.md). O critério, definido pelo dono do
projeto em 13/09:

> morre → respawna → continua na **mesma** sessão, e o host continua jogando
> normalmente. Vale para a morte de **qualquer** um dos dois, host ou fantasma.

A reinvocação automática medida em 12/09 — o convidado volta para casa, põe a
marca, e o `DS2_RematchHook` do host o traz de volta sozinho — é **um contorno**:
é outra sessão, com carregamento, e depende de o jogador pôr a marca de novo.
Fica registrada em [DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md) como plano de
reserva, não como entrega. Declarei o M2 fechado com ela em 12/09 e estava
errado.

**Por que costurar a sessão existente depois de um warp não fecha** — medido
em 12/09: a máquina do convidado completa o join até o estado 7; a do host fica
em `0x10` o tempo todo, sem notar a morte; e o host manda
`RequestNotifyLeaveGuestPlayer` **vinte e três segundos** depois — temporizador,
não reação.

**E por que nenhum warp vai fechar** — lido no binário em 13/09: todo warp, de
qualquer tipo, passa pelo teardown do loader, que destrói o personagem local
(`ctx+0xd0`), os personagens remotos e o mapa, e recria tudo. É isso que tira o
fantasma do mundo do host. O gerenciador de sessão (`ctx+0x22f0`) sobrevive à
recarga; a presença do outro jogador, não. Não existe no jogo um warp que não
recarregue.

### Próximo: não deixar a morte virar morte

Todas as fontes de morte medidas convergem num byte, `*(chr+0xb8)+0x759`, e o
jogador local o consome em `FUN_14013c720`, slot `+0x20` de `ChrDeadActionCtrl`
(medido em 13/09; o `+0x10` que o parecer apontava nunca roda para ele).
Interceptar ali — cancelar a morte, restaurar o HP, pôr o personagem 1,1 m à
frente da fogueira **sem warp** e aplicar as consequências à mão — não toca em
sessão nenhuma, e é o mesmo hook no host e no convidado.

Em ordem, cada passo com o seu sinal positivo. Os quatro primeiros são solo,
sem sessão, e não custam desconexão ilegal:

1. **Teleporte sem warp.** — **feito em 13/09.** A posição autoritativa é a do
   `hkpRigidBody` (`*(*(ChrPhysicsCtrl+0x320)+0x20)+0x1a0`), e o teleporte que
   funcionou escreve translação, swept transform e as cópias do jogo num pedido
   só: o Samuel foi da fogueira de Heide à Catedral de Blue (69 m) sem
   carregamento, de pé e controlável. Detalhes em DS2_SEAMLESS_COOP.md. Nota: o
   `where` **não** acompanha teleporte; a posição viva é `PlayerCtrl+0x90`.
2. **Coordenadas da fogueira ao vivo.** — **feito em 13/09.** A lista
   `*(*(ctx+0x70)+0x58)` tem as fogueiras do mapa carregado (3 em Heide); o id
   de cada uma é `**(*(*(obj+0xb8)+0x20)+0xe0)`, e o nó com o id do registro
   (`0x7ba7`) é o que tem o ponto de nascimento a 0,000 m de onde o jogo pôs o
   Samuel. A receita completa está em DS2_SEAMLESS_COOP.md.
3. **Interceptar a morte.** — **feito em 13/09.** `DS2_DeathInterceptHook`
   desvia `FUN_14013c720` só para o personagem local; `DS2_Death.req` escolhe
   `observe` (padrão) ou `cancel`. Com o HP zerado e `cancel`, a morte não
   acontece: HP de volta no mesmo quadro, controlador no estado 0, nenhum warp,
   e o personagem andou 2,5 m. A primeira morte deixada passar na mesma conexão
   imprimiu `First ... RequestNotifyDeath`, o controle de que as 2956
   canceladas antes dela não chegaram ao servidor. Detalhes em DS2_SEAMLESS_COOP.md, "A morte medida, e
   segurada".
4. **HP.** — **feito junto com o 3.** `PlayerCtrl+0x168` atual, `+0x16c`
   mínimo (-99999), `+0x170` máximo base, **`+0x174` máximo efetivo** (já com o
   desconto do hollow: 915 → 869 → 823 em duas mortes). O cancelamento devolve
   `+0x174`; sem devolver, `FUN_14016a650` religa o byte no quadro seguinte.
5. **Juntar solo.** — **feito em 13/09.** O modo `respawn` recusa qualquer morte
   do jogador local e a cobra com as funções do jogo, sem recarga: almas para
   uma mancha no local da morte (a antiga removida), hollow com aparência e HP
   máximo, Estus recarregado, e o personagem de pé no nascimento da fogueira do
   registro com o HP cheio. A queda passa pelo mesmo caminho e deixa a mancha
   na beira. Medido com HP zerado, queda no vazio, duas mortes seguidas e um
   personagem humano; nenhum `RequestNotifyDeath` nem warp. Detalhes em
   DS2_SEAMLESS_COOP.md, "Renascer pagando a morte".
6. **Com sessão.** — **feito em 14/09.** Chico invocado no mundo do Samuel,
   os dois em `respawn`. A morte do fantasma e a do host, por HP e por queda,
   renascem na fogueira sem carga e a sessão continua: 60 s depois nenhum
   `RequestNotifyLeaveGuestPlayer`, host em `0x10`, convidado em 7, e cada um
   **vê** o outro na fogueira. O primeiro `RequestNotifyDeath` da conexão do
   Chico só saiu na morte comum usada para encerrar, depois de sete recusadas.
   A cobrança segue as checagens do jogo para quem paga: o fantasma guarda as
   almas e não hollowa (a checagem ofuscada `0x14016f7d0` é essa isenção), e o
   contador de mortes soma para os dois. Custou uma descoberta: o HP 0 do
   fantasma chegou uma vez ao host antes de ser devolvido, e a cópia dele morreu
   lá ("Phantom Chico has been vanquished"); o hook agora recusa também a morte
   pendente da cópia de um jogador. "YOU DIED" aparece e o HUD volta. Detalhes
   em DS2_SEAMLESS_COOP.md, "Com sessão".
7. **Fogueira compartilhada.** — **feito em 14/09.** O jogo não grava fogueira
   para quem está no mundo de outro (`FUN_1401caf50` e `FUN_1401cb950` só gravam
   para o personagem local quando o slot `+0x58` do contexto diz que ele não
   está no mundo de outro). O canal não precisou do servidor: o jogo só usa o
   canal 0 da sessão P2P da Steam, e `DS2_CoopChannelHook` usa o 7. O host da
   sessão (a marca `+0xad` que o jogo põe no membro dono do lobby) anuncia
   `{mapa, tipo, id}` a cada 2 s, e a morte de um convidado renasce na fogueira
   anunciada. Medido com o registro do Chico em `0x7ba2` e o do Samuel em
   `0x7ba7`: HP zerado a 69 m e queda no mar levaram o Chico à fogueira do
   Samuel, com a chave desligada ele foi para a própria, a morte do host seguiu
   no registro do host, a sessão ficou em `0x10`/7 e, na saída, o Chico voltou
   para casa na própria `0x7ba2`. Detalhes em DS2_SEAMLESS_COOP.md, "A fogueira
   do host".
8. **Fogueira fora do mapa carregado.** — **feito em 14/09**, na forma que o
   dono do projeto escolheu: a última fogueira, com carregamento. Todo
   carregamento do jogo passa por warp, mas o carregamento por partes, a cada
   quadro, não. `DS2_BackreadHook` força o dono do mapa da fogueira
   (`MapAreaCtrlOwner+0x1e9`) com as partes e entrega ao streamer
   (`FUN_1403dc8e0`) a célula de navegação da fogueira no lugar da do jogador,
   que é de onde sai a busca das partes; sem a célula, o mapa chega sem chão. O
   renascer segura o personagem na última posição no chão, espera o estado 5
   (500 ms), leva-o à fogueira e solta quando o streamer o vê pisando no mapa
   novo. Em sessão, o mapa sob a cópia do outro jogador fica carregado com as
   partes em volta dela; na primeira sessão, antes disso, o jogo do Chico fechou
   logo depois de renascer, e a causa não foi lida. A direção inversa achou um
   defeito do teleporte: ele não movia a posição de onde o controlador de queda
   mede o pouso, e um renascer 24,5 m abaixo da morte cobrou duas mortes. Medido
   sozinho e em sessão, nas duas direções entre Heide e Majula: um custo por
   morte, sem warp, a sessão em `0x10`/7 mais de 60 s depois de cada morte, o
   outro jogador ainda na tela de cada um, e saída legal. Detalhes em
   DS2_SEAMLESS_COOP.md, "A fogueira de outro mapa".

---

### O histórico da abordagem que não fechou

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

### O teste do carregamento de área: o jogo não deixa nem tentar

Medido em 12/09, com uma sessão de co-op de pé e o convidado **vivo** no mundo
do host: abrir o inventário dele e escolher a Homeward Bone mostra o menu de
ação com **"Use" acinzentado**. Um fantasma invocado não pode usá-la.

E não é só o convidado. **O host também não pode**: com a mesma sessão de pé,
o inventário do Samuel mostra "Use" acinzentado na Homeward Bone exatamente
igual. A regra não é sobre ser fantasma — é sobre **haver sessão**. Enquanto um
grupo existe, ninguém dos dois lados carrega área por vontade própria.

Isso é mais forte que a suposição que estava no desenho, e mais útil: a porta a
abrir não é "deixe o fantasma usar itens", é **"permita carregar área com
sessão viva"**, um interruptor só, que vale para os dois lados.

Também ficou descartado o caminho mais óbvio para ela. A entrada do warp recusa
motivos fora de 1 e 4 consultando `FUN_140248940`, que devolve o inverso de um
virtual no slot `+0x1b0` de `*(contexto+0xd0)`. Esse virtual é
`0x140314440`, quatro instruções:

    xor eax,eax ; cmp dword [rcx+0x168],eax ; setg al ; ret

ou seja `*(mgr+0x168) > 0`. Medido nos dois clientes com a sessão de pé: **777
no host e 811 no convidado**, ambos positivos e crescendo — é um contador de
tempo, não uma permissão. O portão do warp deixa o motivo 5 passar nos dois
lados; quem barra é a camada de menu, antes de o warp chegar a ser pedido.

**O que isso faz com o M2.** As duas saídas viram uma só:

1. ~~deixar a morte seguir o caminho normal e reentrar no estado 2~~ — depende
   de um carregamento que o convidado não tem permissão de fazer;
2. **ressuscitar em pé, sem carregamento**, que é a única forma que sobra — e
   continua precisando da primitiva de revive que ainda não apareceu.

Ou seja: o M2 exige criar uma capacidade que o jogo remove de propósito do
convidado. Não é afinar um parâmetro, é abrir uma porta fechada.

### Duas tentativas, e o que cada uma provou

**Flag 1 (a forma da entrada): recusada.** O hook montou o pedido certo e o
warp disse não. As duas precondições estavam boas no instante da morte —
`ctx+0x24ac = 0x1e`, `ctx+0x24b1 = 0x40`, bit 2 limpo — então quem recusou foi
o portão `FUN_140248940`.

**Flag 0 (a forma do desmonte): aceita, e no lugar errado.** O warp passou
(`aceito=1`), **nenhum pedido de fim de sessão chegou a existir** — o log de
sessão ficou vazio nos dois lados —, o convidado voltou **vivo e em pé**, e a
sessão continuou em **estado 7 nos dois clientes** (o handler do estado 7
segue rodando com o mesmo ponteiro). O host continuou listando "Chico" no HUD.

Mas o convidado foi parar no **próprio mundo**: movi o host e ele não viu nada,
e o mapa e a posição que mandei no pedido foram ignorados.

Isso corrige uma inferência minha: **a flag não é só a chave do portão, ela
escolhe o significado do destino.** Com ela ligada o pedido é "vá para este
lugar neste mapa"; com ela desligada é "vá para casa", e o destino do pedido
não é olhado. Só a forma da entrada carrega destino — e só o portão a barra.

O saldo: saímos de "morto e travado" (M1) para "vivo, objetos de sessão
intactos nos dois lados, mundo errado". Falta uma coisa só, e ela é pequena:
o portão é `*(mgr+0x168) > 0`, com `mgr = *(contexto+0xd0)`. Medido positivo no
meio da sessão (777 e 811) e evidentemente **não positivo no instante da
morte** — que é o único instante que importa.

### A terceira tentativa: a flag da entrada é aceita — e o host trava

Encenado de novo com o hook em `flag 1` e o portão erguido se preciso. O que
disparou não foi a morte do convidado, foi a **morte do host** — que no cliente
do convidado passa pelo mesmo terminal, `FUN_140190950`. O registro:

    morte de fantasma: papel=1 flag=1 [+0x24ac]=1e [+0x24b1]=40 portao=811 aceito=1

Três coisas de uma vez:

- **a forma da entrada é aceita.** Com o portão aberto — 811 neste instante, e
  nem foi preciso erguer — o warp de motivo 4 com flag 1 passa. A recusa da
  primeira tentativa era mesmo o portão, e não a forma;
- **nenhum pedido de fim de sessão existiu** em nenhum dos dois lados;
- o convidado ficou **vivo, de pé e no lugar**, em vez de ser devolvido.

**Mas o host travou.** A tela dele parou no quadro da própria morte e não mudou
mais um pixel; o HUD continuou listando "Chico". O processo está vivo — o
publicador de posição segue contando, e a posição dele já é a da fogueira — ou
seja **a lógica rodou e a apresentação parou**.

A explicação que encaixa com tudo o que se sabe: o host morre, pede o fim da
sessão e fica esperando a saída do convidado, que o hook substituiu e nunca
mandou. É o risco que o parecer externo marcou como número um, só que pelo lado
oposto ao previsto — não é o cadáver do fantasma que não se levanta, é o host
que fica pendurado numa despedida que não vem.

**O que isso exige do M2:** a intervenção não pode ser só do lado de quem morre.
Ou o host precisa ser avisado por outro caminho, ou o hook precisa distinguir
"o convidado morreu" de "o host morreu" — no segundo caso não há nada a salvar,
a sessão acaba de qualquer jeito e substituir a despedida só trava os dois.

### A medição no caso certo, e a parede

Com o convidado morrendo (motivo 2, o caso do M2), o hook em flag 1 e o portão
erguido:

    morte de fantasma: papel=1 mapa=0a1f0000 sabor=1 destino=6.19,-18.52,209.05
    flag=1 [+0x24ac]=1e [+0x24b1]=40 portao=0 (erguido) aceito=1

- **`portao=0`** — o contador está **zerado no instante da morte do convidado**,
  e positivo (811) na morte do host. É exatamente por isso que a primeira
  tentativa com flag 1 foi recusada e a terceira não. O portão é o
  `*(mgr+0x168)` de `*(contexto+0xd0)`, erguido por uma chamada e devolvido.
- **`aceito=1`** com a forma da entrada.
- **Nenhum pedido de fim de sessão**, nos dois lados.
- O convidado ficou **vivo e de pé**, e o host continuou listando "Chico".

**Mas ele não está no mundo do host.** Movi o host e a tela do convidado não
mudou; ele tem a própria bloodstain aos pés. Reprodutível: aconteceu igual nas
duas medições que chegaram a este ponto.

**A parede, dita com precisão:** o pedido do estado 2 funciona *porque a máquina
de estados o emite enquanto transita para dentro do mundo do host*, com toda a
preparação de peer em volta. Reemitir só o warp reproduz **o movimento**, não a
**entrada**. O warp move o jogador dentro do mundo em que ele já está; quem
decide em qual mundo ele está é a sessão, não o destino do pedido.

Ou seja: o M2 não é um pedido de warp, é uma **reentrada no estado 2** — e o
handler desse estado recebe um `param_2` que vem da rede, com mapa, posição e
orientação. Sem esse payload (ou sem sintetizá-lo), não há entrada.

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

## M3 — entrar uma vez, sem ritual — **FEITO em 15/09**

**Feito:** efígie e Soul Memory não travam mais a entrada por placa branca, e
a entrada já acontece sem soapstone e sem toque quando os dois lados recebem a
ordem (ver "Sem efígie" e "Entrar sem soapstone" em
[DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md)).

**Falta, para a entrada sem ritual de verdade:**

1. ~~**Quem manda a ordem.**~~ **Feito 15/09**: `DS2PartyGuest` (o convidado
   põe a placa ao chegar e a repõe ao voltar, com 30 s de carência quando ela
   some no próprio mundo) e `DS2PartyAccept` (steam ids cujas placas o host
   invoca sozinho); `ds2os-dev up --seamless --party`. Entrada, `session end`
   e reentrada automática medidas sem tecla nem arquivo.
2. ~~**A senha.**~~ **Feito 15/09**: `DS2PartyPassword` vira o
   `name_engraved_ring` (bit 31 ligado) da placa e do poll; o servidor só casa
   party com party do mesmo código, antes de tipo e Soul Memory. Um jogador com
   senha que não é convidado invoca as placas brancas que chegam. Medidos a
   entrada só pela senha, senhas diferentes, host público contra placa com
   senha e host com senha contra placa pública.
3. ~~**Longe um do outro.**~~ **Feito 15/09**: o servidor oferece ao poll com
   senha as placas do mesmo código de qualquer área, com a posição reescrita
   para onde o host está; o host invoca; o convidado entra num ponto errado do
   mapa do host (a própria placa convertida, no vazio) e o hook de morte o
   leva à fogueira do host ao chegar, antes de cair e em qualquer modo de
   morte: papel de dono para fantasma, anúncio do host num mapa diferente do
   de onde veio, nada sob os pés. Medido duas vezes de Majula para Heide em
   `observe` (teleporte 10 s depois da invocação, `p2pSessionVerified: true`,
   nenhuma morte), com o controle no mesmo mapa sem disparo.
4. ~~**Uma revanche e uma entrada no mesmo hook.**~~ **Feito 15/09**: a
   entrada é toda do `DS2_PartyHook` (detour próprio do `AddSign`), medida com
   `DS2AutoRematch` desligado.

Medido antes de mexer: **o convidado hollow põe a placa branca** (`Sign created:
type 1` com estado 1) — a trava estava só no **host**, que hollow recebia a
placa do servidor e não ganhava o prompt "Touch Summon Sign". O `CLAUDE.md`
dizia que hollow não põe placa branca; o bloqueio de 12/09 que gerou isso era a
desconexão ilegal.

Resíduos da efígie e da Soul Memory, não medidos:

- `FUN_1402a1bf0` recusa o **uso de item** de alguns tipos quando hollow (a
  tabela `0x1410d64f4` com 1): provavelmente orb e soapstone vermelhas. Não
  mexido — PvP não é entrada de co-op;
- a Small White Sign Soapstone só teve a configuração aberta, não uma placa
  medida;
- a VPS continua com a Soul Memory ligada: a config fica em `Saved/`, fora do
  git, e o padrão do código não mudou.

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

## M4 — estado de mundo autoritativo — **pausado em 15/09** (falta teste manual; retomar por [DS2_WORLD_STATE.md](DS2_WORLD_STATE.md), "Estado: pausado")

Portas, alavancas, elevadores, atalhos, illusory walls e mecanismos de Pharros
abertos pelo host aparecem abertos para quem entrou. O trabalho vizinho mais
próximo é [DS2_FOG_GATES.md](DS2_FOG_GATES.md), que já achou a classe e o teste
por quadro das barreiras de área.

**Medido 15/09** ([DS2_WORLD_STATE.md](DS2_WORLD_STATE.md)): o jogo já faz a
metade das flags. O convidado recebe **todas** as event flags do host ao entrar
(de mapa e globais, provado com bits ligados só na memória do host) no lugar
das próprias (um bit só do convidado some na sessão e volta em casa), não as
leva para casa, e em sessão o host propaga cada mudança pelo pacote P2P `0x20`
(`FUN_140474a60` → `FUN_14051e6b0`), enquanto o convidado só consegue mudar as
de mapa (`FUN_14025cdb0`).

A cópia na entrada é um **instantâneo** que o host exporta e o convidado importa
no warp (`FUN_1402bf8f0` → `FUN_1402c2fa0`, medido), e além das flags traz
`EventValueManager`, `EventBonfireManager`, `MapStateActManager` (estado de
objetos de mapa) e `EnemyGeneratorDeadCounter` (despawn). `ds2os-dev flags`
lê e compara as flags das duas contas.

**Falta:**

1. **Acionar um mecanismo de verdade** e ver se o estado dele é flag: uma
   alavanca ou porta que um dos dois ainda não abriu, puxada pelo host antes e
   durante a sessão, com `25cec0`/`25ce10` no traço e o `EventFlagManager`
   lido nos dois. Se for, o M4 é teste e não código.
2. **O que não for flag** (`MapObjStateActComponent` guarda estado por objeto):
   achar onde o convidado o recebe, ou não recebe.
3. **Mecanismo acionado pelo convidado** no mundo do host: a flag de mapa passa
   pelo filtro; falta ver o objeto.

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

## M7 — o que foi feito junto entra nos dois saves — **mecanismo feito em 15/09**

**Feito 15/09**: `DS2_ProgressCarryHook` (com `--seamless`). O convidado vigia o
pacote P2P `0x20` (`FUN_14025ce10`), guarda as flags **globais** que o jogo
aplicou vindas do host (as que `FUN_14025cdb0` só aceita do host) em
`DS2_Carry.pending`, e ao voltar ao próprio mundo (papel 0, gerenciador sem a
marca de mundo de outro, 5 s parado) as grava pelo setter do jogo
(`FUN_140474a60`). Flags de mapa ficam no mundo do host. O progresso anterior
do host chega no instantâneo de entrada, não no `0x20`, e por isso não passa.
`DS2_Carry.req` aceita `flag <id> <0|1>`, `le <id>`, `status`, `limpa`.

Medido com Samuel host e Chico convidado em Heide:

    10:46:15.500  Samuel  flag 109999 <- 1 pelo setter do jogo: antes 0, depois 1
    10:46:15.500  Samuel  flag 131000199 <- 1 pelo setter do jogo: antes 0, depois 1
    10:46:15.520  Chico   recebida flag 109999 = 1 (papel 1): guardada para o meu mundo
    10:46:15.520  Chico   recebida flag 131000199 = 1 (papel 1): flag de mapa: fica no mundo do host
    (session end)
    10:47:16.854  Chico   no meu mundo: flag 109999 <- 1 (antes 0, depois 1)

`flags` com o Chico em casa: `109999` ligada, `131000199` desligada. Depois de
reiniciar o processo do Chico (boot novo), `109999` continuava ligada: está no
save. As duas flags foram desligadas de volta nos dois saves e conferidas
depois de sair para o título e entrar.

**Falta:**

1. **Uma flag de chefe de verdade.** Que a morte de um chefe seja uma flag
   global é hipótese; nenhum chefe foi morto numa sessão. Precisa de luta.
2. **O que mais é global e não deveria passar**: se pegar um item no mundo do
   host é flag global, o convidado perde o item no próprio mundo (vai contra o
   M5); o mesmo vale para estado de NPC (M10). Sem medição, o hook passa tudo o
   que é global.
3. As flags `100100` e `100110`, que o host liga e manda ao carregar um mapa
   (`FUN_1404747c0`), passam também; parecem inofensivas, não foram olhadas.

Lido para os itens 1 e 2, entre quem chama o setter: `FUN_14040fdb0` liga, na
morte de um personagem, a flag em `+4` do parâmetro dele (candidata a "chefe
morto"), e `FUN_1401826d0` liga duas flags de um registro (`+0x10`, `+0x14`) e
soma um valor de evento (`+0x18`), com cara de tesouro recolhido. As duas só
rodam em quem **não** é convidado, então no host, e saem pelo `0x20`. A
categoria das flags vem dos parâmetros e não foi lida.

Bosses mortos e quests feitas juntos passam para o save de quem entrou; o
progresso **anterior** do host não passa. Esta é a segunda metade do problema
e provavelmente o maior trabalho depois de M1: exige o cliente do convidado
escrever no próprio save flags de um mundo que não é o dele.

Começar por boss, que é uma flag só e fácil de verificar, antes de qualquer
quest.

---

## M8 — fogueira e viagem — **feito em 15/09, com dois jogadores**

**Feito 15/09**: `DS2_BonfireInSessionHook` (com `--seamless`). O dono do
mundo descansa na fogueira com fantasmas no mundo, e a sessão fica. Em sessão
o jogo recusava em **três** lugares, todos perguntando a `FUN_14025f690`
("sessão de multiplayer de pé", estado 1 ou 2):

1. `FUN_1401cb950`, o descanso: resposta sim → mensagem `0x453`, a caixa
   "Cannot use bonfire" (`0x453` é id de texto, não evento; a tabela de falhas
   de invocação `FUN_140212730` usa ids do mesmo tipo);
2. `FUN_14017ed90`, o job de descanso no estado 2: `FUN_14025ea40` diferente de
   0 → cancela o menu (`FUN_1401994e0`), e o personagem se levanta;
3. `FUN_140199a70`, a fila de menus no estado 10 (menu da fogueira): resposta
   sim → cancela o menu.

1 e 3 respondem "não" por um detour de `FUN_14025f690` que olha o endereço de
retorno (`+0x1cb9d9`, `+0x199c2e`) e só quando o jogador local é dono do mundo;
2 é patch de bytes (`74 19` → `eb 19` em `+0x17ee9d`), alcançável só com um
descanso já começado.

Achado por partes, cada uma medida: breakpoints nos ramos de `FUN_1401cb950`
(com sessão ele ia direto à mensagem; sem, iniciava o job), a leitura por
sonda da fila (`+0x54` de `*(ctx+0x70)+0x50`: 10 com o menu aberto sozinho, 0
em sessão) e uma vigia de escrita nesse campo. Com as três trocadas em memória
e depois com o build `2dc983c9`: Samuel host sentou, o menu "Heide's Ruin"
abriu com o fantasma do Chico ao lado, `p2pSessionVerified: true` antes,
durante e um minuto depois de fechar o menu. O fantasma não recebe a opção de
descansar (os prompts dele são "Light torch" e "Pick up item").

Ao entrar no estado 2 o job roda `FUN_14017fd70` (`FUN_140417210`,
`FUN_1403c1b50`, `FUN_14044f880`), o candidato ao reset do mundo do host.

**Reset e aviso no convidado — feitos 15/09.** Medido antes, com o Chico
host e o Samuel fantasma em Heide: um inimigo (550 HP, em (-55.9, -8.0,
260.0)) morto no host sumiu também do convidado — a morte replica —, mas
depois do descanso ele **voltou só no host**; no convidado continuou morto.
O descanso do jogo não conta nada à sessão.

Agora o `DS2_BonfireInSessionHook` fala pelo canal P2P próprio
(`DS2_CoopChannel::SendHostEvent`, tipos 2 e 3 do anúncio): `FUN_14017dc40`
(o descanso começou) manda `RestStarted`, e o convidado mostra a caixa **"A
player is resting at a bonfire."** com a função das mensagens de rede do jogo
(`FUN_1404fe2a0` em `*(ctx+0x22e0)`, título `FUN_140503620(0, 0xcc)`);
`FUN_14017fd70` (o reset: geradores de inimigos, objetos de mapa, eventos)
manda `WorldReset`, e o convidado roda o **mesmo** `FUN_14017fd70` na cópia
dele do mundo do host, na thread do jogo. Medido com o build `502fb6d0`:

    14:29:54.978  Chico   host: descanso na fogueira 00007ba7; aviso para a sessao
    14:29:54.992  Samuel  convidado: o host descansou (...ha 6 ms); aviso mostrado
    14:29:57.861  Chico   host: o mundo foi reiniciado pelo descanso
    14:29:57.876  Samuel  convidado: mundo do host reiniciado aqui tambem (ha 7 ms)

O inimigo morto de novo antes do descanso estava de volta, 550/550 na mesma
posição, **nos dois** clientes; a caixa apareceu na tela do Samuel e fechou com
A; `p2pSessionVerified: true` o tempo todo.

**Viagem com votação — feita 15/09.** O host escolhe a fogueira na lista de
viagem; `DS2_BonfireInSessionHook` segura a escolha **na lista**
(`FeGroupTestBonfireTransitionList` slot `+0x80`, `FUN_1400d5170`) e manda
`TravelVote` pelo canal; cada convidado recebe a caixa sim/não do jogo
(`FUN_1404fe1c0`, a mesma de `FeSubStateCommonWindow`) com "The host wants to
travel to another bonfire. Travel together?", e a resposta volta ao host
(anúncio de tipo `0x20`).

- **Não**, ou sem resposta em 30 s: o host recebe "Travel canceled: a player
  declined." (ou "...not every player answered.") e continua na lista, de pé,
  com o registro de renascimento intacto.
- **Todos sim**: `TravelLeave`; cada convidado sai da sessão pelo caminho que
  a viagem do host já usava — `+0x120 = 1` no `NetSummonJoinMultiplayCtrl`,
  que o estado 7 (`FUN_1402c3830`) transforma em fim de sessão motivo 3 —, o
  host espera o grupo sair (1,5 s sem membros), a escolha segue, e o party
  junta todos na fogueira nova com a chegada de outro mapa do M3.

Medido com o build `3235c402` (Chico host, Samuel convidado):

    16:28:24.347  Chico   escolha de fogueira segurada na lista; votacao 2
    16:28:24.361  Samuel  votacao 2 aberta (caixa 10)
    16:28:28.445  Samuel  votacao 2 respondida sim
    16:28:28.479  Samuel  saio da sessao para o host viajar (+0x120 0 -> 1)
    16:28:30.328  Chico   convidados fora da sessao em 366 ms; a escolha segue
    16:29:47.974  Samuel  chegada de outro mapa: levando para fogueira do host (The Far Fire)
    16:30:02      p2pSessionVerified: true, os dois em Majula, sem morte

Recusa medida às 16:31:16 (resposta em 6 s) e sem resposta às 16:27:34.

Três tentativas que **não** funcionaram, para não repetir: segurar o warp
(`FUN_1401c2a80` motivo 2) — a transição de carregamento já tinha começado e o
host ficou sentado sem menu; segurar a fase 1 da viagem (`FUN_140184a10`,
`*(*(ctx+0x70)+0x70)+0x40`) — o personagem já estava na animação de viagem e
ficou congelado nela; e o host viajar com o fantasma ainda no mundo — o jogo
do host **fechou duas vezes** (`c0000005` em `+0x3f510f`, `FUN_1403f4f60`, um
personagem já liberado dentro do `CharacterManager`), e numa delas foi o do
Chico, que perdeu 10 pontos de desconexão (40 → 50).

**A saída pela viagem não conta desconexão ilegal (medido, com controle).**
Contador em `*(*(*0x141616cf8+0x30)+0x68)+0x1c0` (`MultiPlayPenaltyCtrl`,
vftable `0x1410d0f00`): `+0x08` armado, `+0x0a` pontos, `+0x0c` punição
restante; no save em `+0x488` bit 0, `+0x47a` e `+0x47c` (`FUN_14024fe40`).
Entrar numa sessão arma; um fim legal desarma sem somar; um cliente que some
armado soma `+0x1c`/`+0x20` do parâmetro (10), bloqueio em `+0x22` (100),
punição de 36000 s. Nas quatro saídas por viagem medidas (13:53, 15:23,
15:33 e 16:28) o convidado foi de armado 1 para 0 com os pontos iguais (Samuel
10, Chico 40/50); o **controle** foi o crash do host às 13:53, que custou 0 →
10 ao Samuel. Os pontos antigos são de desconexões anteriores.

**O convidado descansa, e a viagem é de qualquer um — feitos 15/09.**
Decidido com o usuário: o descanso do host **não** cura o convidado (medido:
726/854 antes e depois); quem quer a cura senta na fogueira. O descanso do
convidado reinicia o mundo **para todos**; e uma proposta para uma fogueira que
o host não acendeu é cancelada com aviso.

O convidado nunca via "Rest at bonfire" por **duas** travas:

1. `FUN_140453ce0`, as entradas de ação de evento (a lista de prompts, que
   `FUN_1404554e0` alimenta e `FUN_140455f80` mostra): uma entrada de tipo 13
   ou 14 (a fogueira; textos `0x6d`/`0x6e` pela tabela `0x1410ef2a0`) é
   descartada, antes até da distância, quando o slot `+0x58` do contexto
   (`FUN_1405135f0`) diz que o jogador está no mundo de outro. Achada com um
   breakpoint na entrada: no Samuel fantasma, na fogueira, a entrada de tipo
   `0x0e` passava pela máscara `+0xa0` e morria ali. Com o `jne` de `+0x453dd8`
   trocado por `nop` o prompt apareceu na hora. O hook abre essa trava só
   enquanto o jogador local é fantasma branco (invasor continua sem nada), com
   bytes esperados nos dois sentidos;
2. a pergunta 130602 do script (`FUN_140513440`, "em sessão como convidado"),
   respondida "não" a um fantasma branco a até 3 m de uma fogueira carregada.
   Não isolado se ela ainda é necessária com a trava 1 aberta; fica.

O descanso do convidado não reinicia a cópia dele: manda `GuestEvent
RestStarted` ao host, que mostra "A player is resting at a bonfire.", repassa
o aviso aos outros convidados (menos quem descansou) e roda o próprio reinício,
que chega a todos como `WorldReset`.

A viagem: o convidado que escolhe uma fogueira na lista dele (a lista mostra
as fogueiras do mundo do host) fica segurado na escolha e manda `TravelPropose`.
O host recusa se já há votação (`Busy`) ou se não acendeu aquela fogueira
(`NotLit`, "Travel canceled: the host has not lit %ls."); senão abre a votação
com o convidado já contado como sim. As caixas agora nomeiam o destino, com o
nome da fogueira (texto categoria `0x12`) e a área (categoria 5): "The host
wants to travel to The Far Fire (Majula). Travel together?" ou "A player wants
to travel to ...". Aprovada, a saída e a viagem são as do host; o host viaja
pelas funções do jogo (`FUN_1401843b0` + `FUN_140184830` + `FUN_14044fe30`).

Medido com os builds `44e4f654` (trava 1 por sonda) e `618742e8` (no hook),
Chico host e Samuel convidado na The Far Fire:

    18:11:13.856  Samuel  descanso na fogueira 122a no mundo do host; aviso ao host
    18:11:13.870  Chico   o convidado descansou na fogueira 122a; aviso a todos e reinicio o mundo
    18:11:13.887  Samuel  mundo do host reiniciado aqui tambem (pedido ha 0 ms)
    (Samuel HP 400/915 -> 914 ao sentar; de novo com 618742e8 às 18:26:48)
    18:13:23.948  Samuel  proponho viajar para The Far Fire (Majula)
    18:13:23.961  Chico   votacao 1 ... proposta por um convidado; caixa aberta
    18:13:51.596  Chico   votacao 1 recusada; Samuel: "Travel canceled: a player declined."
    18:14:39.668  Samuel  proponho de novo; 18:14:55.099 Chico responde sim
    18:14:55.132  Samuel  saio da sessao para o host viajar (+0x120 0 -> 1)
    18:14:57.000  Chico   convidados fora em 383 ms; viagem iniciada para a fogueira 122a

Depois da viagem: penalidade desarmada nos dois com os pontos iguais (Samuel
10, Chico 50), o party juntou de novo e `p2pSessionVerified: true` com o
Samuel na The Far Fire do Chico. A caixa nomeada da votação do host apareceu
no Samuel às 18:28:26. Com o build `74247cca`, o convidado que deixa a pergunta
sem resposta também recebe o motivo quando ela fecha: "Travel canceled: not
every player answered." nos dois às 18:39:34.

**Viagem em grupo, sem sair da sessão — feita em 15/09.** A primeira forma —
os convidados saíam da sessão, o host viajava com a viagem do jogo e o party
juntava todos de novo — **não é viajar junto**: medido no uso real, o convidado
voltava ao próprio mundo com "Summoning canceled." e só reaparecia no mundo do
host cerca de 75 s depois (19:39:47 saiu, 19:41:06 voltou; três viagens
seguidas iguais). É o mesmo contorno que o M2 já tinha rejeitado.

Agora ninguém sai: cada máquina leva o próprio jogador até a fogueira pelo
caminho do passo 8 do M2 — `DS2_DeathIntercept::GoToBonfire`, que segura o mapa
do destino ao lado do atual (`DS2_BackreadHook`), espera o estado 5, foca na
célula da fogueira e teleporta, sem warp nenhum. O host grava o próprio
registro de renascimento na fogueira nova (`FUN_1401843b0` + `FUN_14044fe30`).
A viagem leva ~2,5 s por jogador e a sessão fica verificada o tempo todo.

Três coisas foram aprendidas fechando os jogos (quatro quedas, duas delas
fechando os dois jogos: 40 pontos de desconexão no Samuel, de 10 para 50, e 10
no Chico, de 50 para 60):

1. **Fechar o menu da fogueira por chamada direta mata o convidado.** Chamar
   `FUN_1401994e0` da tick do hook fechou o menu no host e derrubou o convidado
   duas vezes, no mesmo milissegundo da chamada (`c0000005` escrevendo em 0
   dentro do desmonte do menu). O que o jogo faz é outra coisa: o job do
   descanso cancela o menu quando há sessão. Então o patch `+0x17ee9d` sai por
   um instante e o **jogo** fecha o menu — medido ao vivo antes de virar
   código: menu fechado em menos de 1 s, personagem de pé, sessão verificada;
2. **Soltar o mapa do outro jogador durante a viagem mata quem viaja.** Uma
   tentativa de economizar memória parou de segurar o mapa sob a cópia do outro
   jogador enquanto a viagem carregava; o convidado fechou dentro do
   `CharacterManager` (`+0x3f4fac`, personagem já liberado) — o mesmo crash que
   a viagem com warp dava. O mapa do outro jogador continua mantido;
3. **As duas máquinas não podem trazer o mapa ao mesmo tempo.** Quando host e
   convidado carregavam juntos, os dois jogos fecharam (uma vez no alocador,
   outra no quadro em que o mapa velho foi solto). Agora o **host viaja
   primeiro** e só chama os convidados depois de chegar e ficar 1,5 s parado no
   mapa novo ("cheguei na fogueira %04x em %llu ms; os convidados podem vir").

E uma quarta, sem crash: a fogueira do destino pode **já estar na lista** por o
mapa estar carregado para a cópia do outro jogador — mas só com as partes em
volta dele. O convidado chegou a Heide sem chão e caiu para a morte. Qualquer
mapa que não seja o de baixo dos pés passa pelo mapa segurado e pelo foco.

Medido com o build `2cda8f19`, Chico host e Samuel convidado: proposta do
convidado em Heide → host aceita → host chega em Majula em 2,2 s → convidado
vai junto; os dois de pé na The Far Fire, cada um vendo o outro,
`p2pSessionVerified: true`. Com o build `9fccbc75`, três idas e voltas
seguidas Heide↔Majula pelo arquivo de pedido, com os dois chegando juntos e
sem queda.

A forma antiga fica como reserva: se o mapa do destino não puder ser trazido
(`DS2_DeathIntercept::MapReachable` falso), a votação aprovada volta a mandar
os convidados saírem e o host viaja pelo jogo.

**Lista de fogueiras do convidado — corrigida em 15/09.** A tabela de fogueiras
(`*(*(ctx+0x70)+0x58)+0x20`, 0x18 por entrada, id ushort) tem uma coluna de
"acesa" por mundo, e a que o convidado lê (`+0x44` = 1) só ganhava as fogueiras
dos mapas que ele carregou na sessão: com os dois em Heide, a lista de viagem
dele tinha 2 áreas, e o host tinha 13. Agora o host publica as próprias acesas
como um bitmap sobre a ordem da tabela (pacote `0x30` do canal, 96 bits, 77
entradas em 1.03) e o convidado escreve na coluna dele — 74 fogueiras alinhadas
no primeiro segundo, e a lista dele passou a ser a mesma do host. A tabela é
conferida antes de ser escrita (contagem, coluna e ids crescentes).

**O aviso de descanso saiu.** A caixa "A player is resting at a bonfire." era
modal e atrapalhava a luta; por decisão do dono do projeto (15/09) o descanso
não avisa mais ninguém. O reinício do mundo continua valendo para todos.

**Tela de carregamento na viagem — feita em 15/09.** A viagem sobe a cortina
que a viagem do próprio jogo usa: `ctx+0x1178 = 1` (que já desliga os prompts
de ação e o cronômetro da morte) e `FUN_140b06270(*(0x1416751f8)+0x80, 1)`, que
desliga o desenho do mundo; o jogo ainda escreve o nome da área por cima.
Desce 1,2 s depois de o personagem estar no lugar, e sempre antes de 25 s.
Ninguém vê mais o personagem pendurado entre os dois mapas.

**A viagem junta está desligada por padrão desde 16/09.** A votação aprovada
volta ao caminho medido estável (convidados saem pela saída legal, o host viaja
pela viagem do jogo, o party junta todos); `DS2_Bonfire.req` aceita
`junta liga` para tentar a sem-sair. O motivo está abaixo, e o custo de
descobrir isso foi o Samuel chegar aos **100 pontos** de desconexão ilegal — o
bloqueio — com o temporizador de punição em 36000 s.

**O convidado ainda cai depois de chegar — em aberto.** Com a cortina e com o
mapa de origem segurado por 15 s, o jogo do convidado ainda fechou duas vezes
de cinco viagens, sempre **depois** de ele chegar e ficar de pé:

- `+0x3f4fac` / `+0x3f510f`, na tarefa de pré-desenho de personagem do
  `CharacterManager` (`FUN_1403f4f60`, `mov 0x38(%rcx)` com `rcx` vindo de
  `chr+0xc8` apontando para memória já liberada, padrão `000b15..`), 1 a 3 s
  depois da chegada — e, das duas vezes, logo **depois** de a cortina descer;
- `+0x1cbf40` (`FUN_1401cbf20`), uma lista percorrida com um nó liberado,
  com o id do mapa novo em `r15`.

Sozinho nunca acontece: oito viagens seguidas de ida e volta, sem sessão, sem
uma queda.

**O que o vigia mostrou (16/09).** `DS2_TravelWatchHook` registra, numa janela
em volta da viagem, quem destrói o quê: o destrutor de `MapEntity`
(`FUN_1403b9ea0`), a soltura de `MapModelComponent` (`FUN_1403f6300`) e as duas
desmontagens por índice de mapa. Numa viagem que **não** caiu, o registro traz
centenas de solturas de componentes do mapa deixado para trás, todas por dentro
do quadro. Na que caiu (`+0x40d33b`), a lista percorrida pelo quadro
(`FUN_14040d2e0`, o update de todo componente registrado: nó em `dono+0x20`,
anterior em `+0x00` e próximo em `+0x08`) tinha um nó cujo dono **já estava
liberado** — vftable carimbada pelo alocador — e nenhuma das quatro portas
vigiadas tinha soltado aquele dono. Ou seja: **alguém libera sem desligar da
lista**.

O quadro passou a ser percorrido pelo hook, que confere a vftable do dono antes
de chamar e desliga da lista o nó morto. Isso tirou aquela queda e trouxe a
seguinte: `+0x3f687a`, **dentro** da soltura do componente, num sub-objeto já
liberado. Quer dizer: a referência morta não está só na lista do quadro; está
dentro de componentes vivos. É um problema de tempo de vida de memória do
streaming do jogo com dois mapas e a sessão de pé, e não se fecha com uma
trava pontual — por isso a viagem junta ficou desligada.

Duas correções desta rodada valem de qualquer forma:

- o `keep` mais longo vence (o pedido de 30 s da viagem estava sendo encurtado
  para 5 s pelo keep que a cópia do outro jogador renova a cada quadro);
- o mapa de chegada só pode ser segurado depois que o personagem pisa nele: o
  contato físico ainda responde o mapa de onde ele saiu no instante do salto.

E uma que era invisível: **o perfil de morte estava nulo**, então toda entrada
no mundo deixava o hook em `observe` e nenhuma morte em sessão era paga pelo
M2 — a morte do convidado virava o warp do jogo, que desmonta o mundo. Agora o
perfil é `respawn`. Com sessão, é sempre a máquina do **convidado**, e o que ela tem a
mais é a cópia do outro jogador, que trocou de mapa junto. A pista que sobra é
essa: quem guarda `chr+0xc8` para a cópia de outro jogador e o que acontece com
esse campo quando o mapa dela muda sem warp.

Custo até aqui: 80 pontos de desconexão ilegal no Samuel (de 10) e 10 no Chico.
O save `antes-viagem-junta` foi tirado antes de tudo isso.

**Segunda rodada de correções (16/09), a queda ainda em aberto.** Três
tentativas principais, cada uma medida com dois jogadores, e cada uma **moveu**
a queda em vez de fechá-la:

1. **O vigia passou a guardar o pré-desenho do próprio componente de modelo**
   (`FUN_1403f4f60`), não o update do personagem: antes de o jogo tocar no
   componente, confere o componente e os três objetos que ele aponta
   (`+0x40` a instância do modelo, `+0xc8` o registro no quadro, `+0xd0` o
   seguidor); um componente já liberado é pulado. Isso mostrou que, **em pé e
   parado**, todo mundo passa (0 pulados). A queda vem só quando um mapa é
   **desmontado**.
2. **A viagem passou a segurar os dois mapas inteiros** (`keep` com todas as
   partes), e os `keep` **somam** e nunca encolhem — antes o `keep` que a cópia
   do outro jogador renova por quadro rebaixava o mapa às partes debaixo dela,
   e o mapa saía em duas fases (as partes na chegada, o resto 30 s depois).
   Não fechou: `+0x40d2c7`, dentro do `unlink` do registro do quadro
   (`FUN_14040d2b0`), escrevendo `proximo->[0] = anterior` com `proximo` já
   liberado.
3. **A cortina passou a abrir a tela de carregamento do próprio jogo**
   (`FUN_1405014b0`, evento `0x67` ao objeto de front-end `0x4c5c574`) e a
   esconder o HUD (`FUN_1404ffef0(frontend, 0xffdffbff)`), além de desligar o
   desenho do mundo como antes. Medido na tela do convidado: o HUD some, as
   barras de letterbox e o rodinho de carregando aparecem — mas o mundo
   **ainda é desenhado** por trás (a chamada que desliga o desenho,
   `FUN_140b06270`, é no-op fora de um warp de verdade). É melhor que o
   personagem flutuando com o HUD cheio, mas não é a tela preta.

**O que está claro agora.** A queda é sempre durante a **desmontagem** dos
`MapModelComponent` de um mapa (`FUN_1403f6300`, chamada por `FUN_1403f4500`),
e o endereço muda a cada tentativa (`+0x3f4230`, `+0x40d2c7`, `+0x3f687a`,
`+0x3f4fac`). O padrão nos registradores é sempre o carimbo do alocador
(`00b540…`, `00b010…`): um nó do registro do quadro (`prev`/`next` em
`+0x20`/`+0x28`) foi **liberado sem ser desligado da lista** — a cópia do outro
jogador, que trocou de mapa junto. O vigia do quadro (`FUN_14040d2e0`) desliga
o nó morto **quando percorre aquele balde**, mas a desmontagem
(`FUN_14040cea0` → `FUN_14040d2b0`) tromba no vizinho morto **antes** disso,
quando um componente vivo tenta se desligar. E o `keep` pela força do dono não
impede a desmontagem por **índice de mapa** (`FUN_1401c5dd0`), que é por outro
caminho.

**A próxima pista, concreta:** ou guardar o próprio `FUN_14040d2b0` (conferir
que `prev`/`next` estão vivos antes de escrever neles — é minúsculo mas muito
chamado), ou impedir a desmontagem por índice de um mapa segurado durante a
janela da viagem (o vigia já intercepta `FUN_1401c5dd0`; hoje só registra).
Ambas mexem em caminho quente e precisam de medição com dois jogadores, que
custa ponto. Enquanto isso a viagem junta **continua desligada por padrão** e a
votação cai no caminho estável (convidados saem pela saída legal, o host viaja
pela viagem do jogo, o party junta todos).

**O que já vale, independente da queda:** o `keep` que soma e nunca encolhe; o
pré-desenho guardado (uma trava real contra a queda, não só um registro); a
tela de carregamento do jogo com o HUD escondido; e `DS2_Bonfire.req` aceita
`votar <mapa> <fogueira>` no host para exercitar a votação inteira sem a lista.

## A queda do convidado — fechada em 16/09

**Uma guarda só, no lugar certo.** Toda a vida de um mapa passa por uma
chamada: `FUN_1403cc3f0`, a atualização por quadro do `MapAreaCtrlOwner`, cuja
máquina de estados carrega o mapa, transmite as partes e, no fim, desmonta
tudo. **Todas** as quedas que sobraram vinham de dentro dessa desmontagem, num
endereço diferente a cada vez — `+0x3d8782`, `+0x3c1bf8`, `+0x40d2c7`,
`+0x40cee3`, `+0x3f4230`, `+0x3f647e`, `+0x3ece30`. Perseguir uma por uma só
mudava o endereço: foram quatro rodadas de guarda pontual, e a cada rodada a
queda reapareceu noutro lugar.

O `DS2_BackreadHook` agora envolve essa chamada inteira em `__try`. Uma falha
ali deixa o mapa **pela metade** — memória que a sessão não recupera — em vez
de fechar o jogo e custar dez pontos de desconexão ilegal ao convidado.

**Medido em 16/09, com os dois jogadores**: dez trechos seguidos Heide↔Majula
(cinco idas e voltas), host primeiro e convidado depois, com a sessão
verificada o tempo todo. **Nenhum jogo fechou.** A guarda aparou **60 falhas**
no ciclo do mapa e o jogo seguiu em todas; os dois terminaram de pé na mesma
fogueira de Heide, `p2pSessionVerified: true`, penalidade inalterada (Samuel
10, Chico 50, armado 1 porque a sessão estava viva).

Guardas menores, pelo mesmo princípio, cobrem o resto do caminho: o pré-desenho
(`FUN_1403f4f60`), a pós-física (`FUN_1403f41d0`), a soltura do componente
(`FUN_1403f6300`), a saída da lista do quadro (`FUN_14040cea0`) e a busca em
lista (`FUN_1401cbf20`). Uma falha em qualquer uma delas pula o quadro ou deixa
o componente vazar.

**A corrupção em si continua sem explicação, e está anotada aqui para quem
voltar.** A assinatura é sempre a mesma e é estranha: a metade **alta** de um
ponteiro vivo aparece escrita por cima, e a metade baixa continua certa.

    00b54001410e86d8   deveria ser 00000001410e86d8   (vftable +0x10e86d8)
    00b01001410eb518   deveria ser 00000001410eb518   (vftable +0x10eb518)
    00b010fff06b8588   deveria ser 00007ffff06b8588
    000b0010e81d77b0   deveria ser 00007fffe81d77b0

Os valores que aparecem são sempre da mesma família (`00b010`, `00b540`,
`000b0010`, e uma vez `a140a140a140a140` no qword inteiro). Acontece **só com a
sessão de pé** — sozinho, catorze viagens seguidas em dois builds diferentes,
nenhuma falha — e **em qualquer uma das duas máquinas**: em 15 e 16/09 quase
sempre no convidado, mas em 16/09 às 05:37 foi o **host** que fechou. O que as
duas têm em comum na viagem junta é segurar a cópia de outro jogador que trocou
de mapa.

Duas leituras continuam de pé e não dá para escolher entre elas com esta
amostra:

- **escrita perdida de passo 2 bytes**: alguma cópia de 16 em 16 bits invade
  objeto vivo, e os dois bytes trocados no meio de um ponteiro são a borda onde
  ela começa ou termina;
- **uso depois de liberar**: um bloco parcialmente reaproveitado dá exatamente
  o mesmo desenho.

Uma coisa já foi conferida e ajuda a separar: o `free` do `DLRegularHeap`
(`FUN_1408572d0`, slot `+0x68` da vftable do alocador) **não preenche o bloco
liberado com padrão nenhum** — ele grava ponteiros de lista livre nos vizinhos
(`+0x10`, `+0x18`). Então um valor de 16 bits repetido não é veneno **deste**
alocador; e a marca de um uso-depois-de-liberar aqui seria um endereço de heap
(`00007fff...`) aparecendo onde devia haver outra coisa, não `a140a140`.

**Duas teorias foram testadas e descartadas**, o que vale mais que as que
sobraram:

1. **"alguém libera um nó sem desligar da lista do quadro"** — era a conclusão
   de 15/09. O vigia passou a refazer os 32 baldes da lista a cada quadro,
   deixando só nós cujo dono ainda é um objeto vivo. Em dez trechos **nenhum
   balde precisou ser refeito**. A lista estava sempre íntegra; não é isso;
2. **pressão de memória pela viagem forçar o mapa inteiro** — a viagem pedia as
   128 partes do destino. Medido sozinho em 16/09, com a máscara em **zero** o
   mapa carrega igual e o chão vem do mesmo jeito, porque quem traz o chão é o
   **foco** (a célula de navegação entregue ao streamer), não a máscara. A
   pegada de componentes ficou idêntica (188 em Heide, ~570 em Majula), então a
   máscara nunca foi o gasto. A viagem deixou de pedi-la de qualquer forma.

**E uma lição de método:** a primeira versão das guardas tentava *adivinhar*
quais campos de um componente estavam saudáveis (`+0x40`, `+0xc8`, `+0xd0`, a
vftable embutida em `+0x50`) e pulava o componente quando algum não parecia um
objeto vivo. Isso jogou fora **600 atualizações por boot** de objetos de mapa
perfeitamente normais: aqueles campos legitimamente guardam coisas que não são
objetos com vftable. Adivinhar qual campo está são é como se quebra o jogo
tentando salvá-lo. O que ficou foi o `__try`, que só dispara numa falha de
verdade e não tem falso positivo.

**Corrigido no mesmo dia, antes que virasse conclusão errada.** A primeira
leitura desta seção dizia "dez trechos sem queda". Não era verdade, e o erro
era do método: quando a viagem desiste (30 s sem o mapa), ela escreve
**"viagem concluido" assim mesmo** e deixa o personagem onde estava. O teste
esperava essa linha, então uma viagem que não aconteceu contava como sucesso.
Relendo os registros daquela corrida: o host viajou 28 de 30 vezes, mas o
**convidado só viajou 5** — as outras 13 desistiram. A ausência de queda estava
inflada por viagens que nunca saíram do lugar. O teste agora só conta um trecho
quando os **dois** registram "o personagem esta nele" no mapa de destino.

E a causa daquelas desistências era minha: a viagem tinha passado a pedir o
mapa com a máscara de partes **zerada**. Medido com os dois jogadores, um dono
forçado com máscara vazia fica em estado 0 para sempre
(`estado 0 forcado 1 quer 1`, todas as máscaras zero): **o byte de força diz
"mantenha este mapa", não "carregue"; quem carrega é ter uma parte pedida.**
Sozinho parecia funcionar só porque o jogador acabava de vir daquele mapa e o
streamer ainda queria partes dele. Pior no convidado, porque o host vai
primeiro: a cópia do host no mundo do convidado forçava o destino sem partes, e
o pedido do próprio convidado encontrava um dono que nunca carregaria. As
máscaras voltaram.

**Com viagens de verdade, a queda continua.** Medido de novo depois de tudo
isso: quatro trechos com os **dois** chegando, e no quinto um jogo fechou —
desta vez o do **host**, em `+0x36f846`, sem nenhum guarda ter disparado. As
guardas aparam bastante (sessenta falhas num boot, todas sobrevividas) e não
são inúteis, mas não fecham a questão.

**A pista nova, e é a melhor até agora.** Nessa última queda o registrador
trazia `a140a140a140a140` — um valor de **16 bits repetido**. Isso encaixa com
a assinatura antiga: os dois bytes trocados no meio dos ponteiros (`b0 10`,
`b5 40`) são o mesmo fenômeno visto na borda, onde o preenchimento começa ou
termina. Ou seja, **algo escreve memória em passos de 2 bytes e invade objetos
vivos**; o valor muda a cada vez, então é dado sendo copiado, não um veneno
constante. Não é ASCII em UTF-16 (o byte alto seria 0x00), então não são as
caixas de texto deste hook.

### Achado em 16/09: é o corpo rígido do Havok, e é uso depois de liberar

**A vigia de escrita respondeu.** E a primeira coisa que ela corrigiu foi o
plano: eu vinha dizendo que faltava um breakpoint de **escrita em hardware**.
Ele **não funciona nesta máquina**, e o repositório já dizia isso em
`DS2_AREA_RESTRICTION.md` ("Hardware watchpoints do not work under Wine...
accepted by all sixty threads and never fires"), com o aviso de que vale saber
antes de pegar a ferramenta óbvia de novo. A ferramenta que funciona já estava
construída: a vigia por **proteção de página** do `DS2_TraceHook`
(`wp` em `DS2_Trace.req`).

**A cadeia, medida ao vivo no host:**

    PXCharacterRigidBody  (o corpo físico do personagem)
      +0x110  ->  hkpRigidBody  (o corpo rígido do Havok)
                    +0x18  ->  ponteiro para o heap dos mapas

A queda repetida do host lia exatamente esse caminho
(`FUN_140bd15b0`: `*(rcx+0x110)`, depois `+0x18`, depois `+0x8`).

**E a vigia mostrou quem escreve o `+0x18` do `hkpRigidBody`:** um `rep movsb`
(`+0x1c31725`) copiando **0x1d28 bytes**, vindo de `FUN_1404e00d0`, que
**aloca** um buffer e copia dentro dele. O destino da cópia caiu em cima do
campo. Ou seja, a memória do `hkpRigidBody` foi **reciclada** para virar outro
buffer enquanto o `PXCharacterRigidBody` do personagem ainda apontava para ela.

**Isso decide a pergunta que estava aberta.** Não é uma escrita perdida de
passo 2 bytes; é **uso depois de liberar com reaproveitamento de endereço**. Os
valores que pareciam dados soltos eram o novo ocupante do bloco visto através
do ponteiro velho, e por isso variavam tanto: `a140a140a140a140` e
`3e2a256f2b7fe62c` são floats do buffer novo, `005c003200470053` é texto UTF-16
de um caminho de arquivo, e `00b54001410e86d8` é um bloco reaproveitado só em
parte, com a metade baixa do ponteiro antigo ainda de pé.

**E fecha com a causa estrutural.** A viagem junta move o personagem entre
mapas **sem o carregamento** que reconstruiria as coisas. O corpo rígido do
Havok pertence ao mundo físico do mapa que está sendo desmontado; o
`PXCharacterRigidBody` do personagem continua com ele. Sozinho não morde; com
sessão, e com uma cópia remota atravessando junto, morde.

**O conserto passa a ter alvo.** Não é "reconstruir a presença" em geral: é
garantir que o corpo rígido do personagem seja recriado no mundo físico do
destino, ou que o ponteiro seja reassentado, em vez de ser carregado por cima
da desmontagem. Isso é bem menor que a reentrada coordenada inteira que
`DS2_SEAMLESS_TRAVEL_ARCHITECTURE.md` propõe, e é o primeiro experimento a
fazer.

**O que ainda não está provado:** o ponto exato onde o `hkpRigidBody` é
liberado. A cadeia e o reaproveitamento estão medidos; o sítio da liberação
não.

**Três correções de método nesta rodada**, todas por engano meu e todas úteis:

1. o detector de corrupção primeiro exigiu que `+0x50` fosse vftable **deste
   módulo**: 40 falsos positivos, porque o campo legitimamente aponta para
   fora dele;
2. depois foi ampliado para `+0x40`, `+0xc8` e `+0xd0`, que **não são
   ponteiros** em muitos componentes: ele passou a relatar texto UTF-16, o
   float 1.0 e pares de coordenadas como se fossem dano;
3. o que funciona é olhar **um** campo provadamente ponteiro e perguntar só se
   o valor **pode ser um endereço** (qualquer bit acima do 47 ligado não pode).

**Uma varredura que se provou desnecessária, e por que foi tirada.** Durante
uma dessas rodadas o vigia passou a refazer os 32 baldes da lista do quadro a
cada quadro. Em mais de quarenta trechos **nenhum balde precisou ser refeito** —
a lista estava sempre íntegra. Além de não servir, ela guardava o ponteiro do
registro de um quadro para usar noutro, que é um jeito de corromper memória
dizendo que está protegendo. Saiu.

**Por isso a viagem junta está desligada por padrão de novo.** Uma queda custa
dez pontos de desconexão ilegal ao convidado e não há Bone of Order sobrando
nesta jogatina; o padrão tem que ser o caminho que não custa nada.

### O que a rodada de 16/09 de manhã acrescentou

**Duas suposições minhas viraram prova, e a sequência mais que dobrou.** O
hook do backread escrevia no dono do mapa — o byte de força e **seis blocos de
dezesseis bytes** de máscara de partes — sem nunca conferir a vftable do
objeto, enquanto a leitura no mesmo arquivo sempre conferiu. E a máscara vinha
da cópia do outro jogador por uma corrente (`parte → dono → info → conjunto`)
que também não provava que o dono era um dono de mapa. As duas passaram a
exigir prova.

Resultado medido, com viagens de verdade contadas só quando os **dois** pisam
no destino: de **quatro** trechos antes da queda para **dez**.

E uma hipótese caiu, o que também vale: o aviso "não é um MapAreaCtrlOwner"
**nunca disparou**. O dono sempre era legítimo, então as máscaras nunca foram
parar num objeto alheio; o que melhorou foi a corrente da máscara, não o alvo.

**O host tem uma queda própria, e ela se repete.** Duas vezes (05:37 e 08:52),
sempre no host, com a **mesma pilha**: a tarefa de pós-física do
`CharacterManager` (`FUN_140359e80` monta os itens) descendo por
`FUN_140354e80` → `FUN_140314e90` → `FUN_140370bf0` → `FUN_14036dc50` →
`FUN_14036f800` → `FUN_140bd15b0`, onde um ponteiro esperado contém **dados de
ponto flutuante** (`a140a140a140a140`, depois `3e2a256f2b7fe62c` — dois floats
cada). É a cadeia de animação, e o host é quem guarda a cópia do convidado.

`FUN_140354e80` é o **executor de tarefa** genérico: chama o trabalho
(`tarefa[2](tarefa[1], arg, tarefa+3)`) e depois a conclusão (slot 0 da própria
tarefa). O vigia agora o reimplementa com o trabalho sob `__try` e a conclusão
**sempre** — um personagem perde um quadro de animação em vez de todo mundo
perder a sessão, e a contabilidade da tarefa continua fechando.

### Era isso. A viagem junta está ligada por padrão desde 16/09

Medido com dois jogadores, contando um trecho só quando os **dois** registram
"o personagem esta nele" no mapa de destino:

| build | trechos até um jogo fechar |
| --- | --- |
| antes das guardas | 1 a 3 |
| guarda do ciclo do mapa | 4 |
| + prova de tipo no dono e na máscara de partes | 10 |
| + guarda do executor de tarefa | **40, sem nenhuma queda** |

Quarenta trechos Heide↔Majula seguidos, **nenhuma viagem falha, nenhum jogo
fechado, nenhum ponto de desconexão gasto** (Samuel 10, Chico 50 do começo ao
fim), `p2pSessionVerified: true` o tempo todo. Ao longo deles o host aparou
**59** falhas no executor de tarefa e o convidado 63 no ciclo do mapa, e os dois
seguiram jogando. Quarenta foi onde o teste parou, não onde ele quebrou.

A corrupção de memória **continua existindo** — as guardas param de morrer, não
param de corromper. O que mudou é que agora os dois lugares onde ela chegava a
ser fatal estão cobertos, e o custo de uma falha é um quadro de animação
perdido num personagem. Quem quiser ir atrás da causa: a assinatura, as
teorias já descartadas e o caminho do breakpoint de escrita em hardware estão
logo acima.

**Uma ideia que ficou pronta e não foi precisa:** segurar as cópias dos outros
jogadores paradas durante a viagem (`DS2_DeathIntercept::HoldCopies`), já que é
a cópia que desliza entre mapas sem o carregamento que a reconstruiria. Com
quarenta trechos limpos ela não se justificava, e o preço seria o outro jogador
congelado por alguns segundos na tela. Fica anotada como a próxima alavanca se
a queda voltar.

**Falta:**

- **a queda do convidado depois da chegada, acima, é o próximo trabalho**;
- **as outras quedas não estão provadas como resolvidas**: cada correção foi
  medida uma vez, contra uma falha que não acontecia em toda viagem;
- **Majula → Heide pela votação** não foi medido no build final (só pelo `ir`,
  que não exercita o "host primeiro"); o registro de renascimento do host na
  fogueira nova foi conferido uma vez (`0a040000/0000122a` depois da viagem);
- `NotLit` e `Busy` não medidos: a lista do convidado só mostra fogueiras que
  o host acendeu, então o `NotLit` só aparece numa corrida;
- a caixa do aviso e a da votação são modais: é preciso apertar;
- três ou mais jogadores (uma conta Steam a mais): o repasse do aviso do
  descanso de um convidado aos outros não é testável;
- descanso com um invasor no mundo: o hook não distingue.

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
