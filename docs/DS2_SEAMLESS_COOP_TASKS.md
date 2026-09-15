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
