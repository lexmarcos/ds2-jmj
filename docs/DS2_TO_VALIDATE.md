# Still to validate

Multiplayer works in the closed areas, but "works" currently means: two
players, almost always on one machine, with two item types, in three of
the game's thirty-one areas, up to the moment the phantom appears.
Everything below is untested rather than known good.

Ordered by what would hurt most if it turned out to be wrong.

## Blocking a release

### The duel is never played out

Every successful test ends at the phantom arriving. Nothing has checked
that a duel in Majula behaves like a duel: damage exchanged, one player
dying, the session ending cleanly, souls awarded, the loser going home,
both clients returning to a sane state. A session that forms and then
misbehaves is worse than one that never forms, because it costs the
player their run.

Test it end to end, and then test the ugly endings too: the host quits
mid-fight, the phantom quits, one client is killed.

### Only three areas have been tried

Tested: Things Betwixt (`0x0098e4a0`, mask 0), Majula (`0x009932c0`,
mask 4), Heide (`0x009d5170`, mask 7). `NETWORK_AREA_PARAM` has 31 rows
and the unlock hook now raises **every** one below 63.

Nothing has checked what raising a mask does in areas that were already
open, or in the arena, or in boss rooms. The permission byte has six
bits and only one is understood; the others are being set wholesale.

Suspects worth a look before shipping: boss arenas, the Undead Purgatory
and the Belfries, which have their own invasion rules, and anywhere with
a scripted NPC invader.

### The forced zone is Heide's

`DS2ForcedZoneId` is `103110`, borrowed from Heide. Every area now
claims to be inside Heide's multiplay zone.

That zone record carries whatever settings Heide has, and a zone is
plausibly where phantom limits, session length and matchmaking ranges
live. If those turn out to be per-zone, the whole game is silently
running on Heide's rules. Nobody has read that record.

## Likely fine, but unverified

### One pair across the internet

Until 2026-09-11 both clients always ran on the same box, so every Steam
peer-to-peer session was loopback. That day the first pair on two
machines met: a Linux client and a Windows client on two different home
connections, both started by `loader-main` against the production
server. Both reported Majula inside the forced zone, so the area patch
reached the Windows client too. The Linux player's Cracked Red Eye Orb
found the Windows player as the only candidate, and the invasion landed:
the phantom arrived in the Windows player's world.

The server could not have shown that last part. The peer-to-peer
session goes through Steam rather than the server, and the server logs
nothing when one forms (`LogFirstMessageOfEachType` was off). The two
clients reporting positions a few metres apart afterwards was suggestive
and no more; the confirmation is the player's own account.

Still untried: stricter NAT, worse latency, a summon rather than an
invasion, any pair but that one.

### Only two item types

Tested: Red Sign Soapstone (sign type 4) and Cracked Red Eye Orb
(break-in type 0). Untested: White and Small White Soapstone and their
Sunlight variants, the Dragon Eye, the Blue Eye Orb, and the Mirror
Knight sign, which has its own manager and its own message set.

Co-op through a white sign in Majula is the obvious thing a user will
try first and it has never been run once.

### Two players only

Never tested with three or more. DS2 allows more than one phantom, the
server has a max-player cap that the original brief wants removed, and
the counters this project patches are per-client. Nothing is known about
how they behave with a crowd.

### The hook's failure and uninstall paths

`DS2_UnblockMultiPlayHook` refuses to patch when the bytes are not what
it expects, and restores them on uninstall. Neither path has ever run.
The refusal path is worth forcing deliberately, by pointing it at a
wrong offset, to confirm it declines cleanly rather than taking the game
down.

### One game build

Every offset in this project is hardcoded against version 1.03,
Calibrations 2.02. A patch moves them all. The mask hook finds its param
by type name and survives; the two code patches do not, though the new
one at least refuses rather than corrupting.

### The fog patch, in one area and one pair

`DS2RemovePhantomFog` takes away the barrier that pens a phantom into the
host's area, by holding every fog wall in the state it has when nobody is
visiting. Confirmed in Heide, one invasion, one pair of players: the barrier
does not appear, both move freely, and the boss gate still works.

Not tried: any other area, a white sign summon rather than an invasion, a
session that lasts, more than one guest, and what the server makes of a guest
who walks somewhere its own area filtering did not expect. It is off by
default and off in the loader for exactly that reason.

## Changes made along the way that nobody has checked

### Partial protobuf parsing

`Frpg2ReliableUdpMessageStream` now uses `ParsePartialFromArray`. That
was the right call: our `required` fields are guesses from captured
traffic and a wrong guess was silently dropping a client's message.

But it is in **shared** code, so it changes Dark Souls III too, and it
weakens a check that used to catch genuinely malformed messages. Nothing
has tested DS3 since. At minimum, confirm a DS3 server still runs.

### Sticky signs and the debug invasion trigger

Both are still in the tree. `DS2_StickySigns` is retired and off, and
was never validated in the state it was left in. The `debug_invade.req`
trigger was proven not to be a usable oracle and is dead weight; it
should probably be removed rather than left for someone to trust.

### The message census only logs firsts

`LogFirstMessageOfEachType` reports the first of each type per client,
which hid a sign being created and removed repeatedly and cost a wrong
diagnosis. Anyone reading that log should know it is a census, not a
trace.

## Understood incompletely

### Heide reads 7 and accepts a sign anyway

Bit 3 of the permission byte is tested in two places and refuses the
item when clear. Heide has it clear and places signs regardless. Either
it reaches the permission by another route, or the byte read by the
param decode is not the byte those sites consult for that area.

This blocks nothing, and every area measured now behaves. But it means
the model is wrong somewhere, and a wrong model is exactly what made the
mask look innocent for so long.

### The root cause is untouched

`FUN_1403c0890` fails in a closed area because the per-map multiplay
record it looks up is missing, not because its other term fails. Filling
that record in would explain the behaviour instead of overriding it, and
would not need a code patch at all.

Worth finding where that table lives and what a present record contains.

### The rematch after a death

Measured in [DS2_REMATCH_AFTER_DEATH.md](DS2_REMATCH_AFTER_DEATH.md), with
two gaps left open:

- whether the Red Sign Soapstone can be used hollow. The test that
  looked like it proved yes was run on a character who turned out to be
  human, so it proves nothing. The orb is the only item measured.
- whether a fall death and a kill death produce the same message chain.
  The fall showed no `RequestNotifyDeath`, but the server census only
  logs the first message of each type per client, so that is not
  evidence either way. A staged kill needs the two characters next to
  each other, and the terrain around the Heide bonfire kept killing the
  phantom on the way over.

The first reading of these measurements was wrong in a way worth
remembering: a death as an invader looked like it cost human form,
because an accidental second death **in the invader's own world** sat
between the duel and the test. The fix was to run the loop again with
nothing in between.

### A revanche por red sign, e o que o servidor tem sem entregar nada

`DS2_AutoRematch` e o gatilho `debug_summon.req` estão no servidor e
funcionam no que prometem: o par é lembrado e o push é reenviado. Mas
**sozinhos não formam sessão nenhuma** — está medido em
[DS2_REMATCH_AFTER_DEATH.md](DS2_REMATCH_AFTER_DEATH.md). A flag nasce
desligada e deve continuar assim até existir a metade do cliente; caso
contrário ela vira a mesma armadilha que o `debug_invade.req` virou.

Em aberto, em ordem de quanto bloqueiam:

- ~~O `SignHandle` sobrevive a uma placa nova?~~ **Não**, e o hook já
  resolve: ele lê o handle novo no parâmetro de saída da função que
  registra a placa. A revanche por red sign funciona ponta a ponta.
- **O hook invoca qualquer placa que chegue**, não só a do par. Com dois
  jogadores dá no mesmo; com três, está errado. Falta ler o campo do
  item que identifica o dono.
- **Qual índice da tabela em `0x1410c0050` é cada papel.** Zerar todos
  trava a morte; para o co-op seamless é preciso saber qual entrada
  mexer, e o tipo vem de `rcx+0xe0` num objeto transitório.
- ~~A Red Sign Soapstone pode ser usada hollow?~~ **Respondido em 12/09:
  não.** Hollow, o X não coloca placa; com uma Human Effigy, no mesmo
  ponto e sem andar, a placa sai. Vale para os dois itens online.
- **O host hollow não vê placa** foi medido uma vez só, com o controle
  no mesmo ponto (efígie, prompt aparece). Vale repetir num outro lugar
  antes de virar regra.

### O warp, e o co-op seamless

O caminho do warp está mapeado e o hook existe
([DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md)). O que ele entrega é
**onde o jogador aterrissa**, não a sessão: o
`RequestNotifyLeaveSession` continua saindo e a sessão continua
acabando. Em aberto:

- **A sessão pode sobreviver a uma morte?** O jogo nunca carrega área
  dentro do mundo do host para um convidado, então isto pode
  simplesmente não existir. É a pergunta que decide se o co-op seamless
  de verdade é possível ou se o caminho é morrer e se reencontrar pela
  revanche.
- **Que outros motivos de warp existem** além de 1 (fogueira) e 4 (fim
  ou começo de sessão), e o que o portão em `0x140248940` cobra de quem
  não é nenhum dos dois. O `DS2_Seamless.log` responde sozinho com uso.
- ~~O registro de renascimento de um convidado ainda aponta para a
  fogueira dele enquanto ele é fantasma?~~ **Sim**: o redirecionamento
  de um fantasma de co-op devolveu `mapa=0a1f0000 ponto=00007ba7`, que é
  o mesmo ponto de uma morte comum dele no próprio mundo.
- ~~A morte do host com fantasma dentro~~ **medida em 12/09**: o host faz
  uma morte comum (motivo 1, para a própria fogueira) e o convidado passa
  pelo mesmo `+0x2c3bde` com a mesma forma posição de quando é ele quem
  morre. Um hook só cobre os dois casos.
- **Os três bytes de param** (`+0x2c/+0x2d/+0x2e` da linha do papel) só
  foram lidos por dedução do comportamento. Falta ler a linha na memória
  e conferir papel por papel, e falta saber o que é o motivo de fim 4,
  que devolve `2` sem consultar a linha.
- **Quando a placa do convidado não está em cima da fogueira dele**, o
  redirecionamento deveria mudar o lugar de pouso visivelmente. As duas
  medições foram feitas com a placa no mesmo ponto da fogueira, então a
  troca está provada no pedido, não na tela.

### A morte segurada (`DS2_DeathInterceptHook`)

Medido só solo, só com o Samuel, em Heide, em 13/09
([DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md), "A morte medida, e segurada").
Em aberto:

- **Só duas das dez fontes de `+0x759` foram exercitadas**: HP zerado e queda.
  Dano letal de inimigo (`FUN_14013a9b0`), as três de `FUN_14013cc30` (entre
  elas o evento de animação `0x19`, que agarrões usam), `+0x145f3f` (causa
  `0x6e`), `+0x31b753`, `+0x37046b` (aterrissagem) e `+0xd1c7f8` não. As de
  `FUN_14013cc30` ligam o byte **dentro** do consumidor e passariam pela
  checagem; o log diz `SEM +0x759 ANTES` quando isso acontecer.
- **A morte instantânea** (`FUN_14013c500`, tipos 1 e 2) é só registrada. Não
  apareceu nenhuma vez: nem na queda nem na morte por HP.
- **Um personagem de pé com HP zerado por um quadro**: o hook devolve o HP no
  mesmo quadro em que o byte aparece, mas o que roda entre `FUN_14016a650` e o
  controlador naquele quadro vê HP 0. Ninguém olhou o que isso dispara.
- **Com sessão, nada.** Se o outro lado vê a morte pela replicação e não pelo
  aviso, cancelar só no cliente que morre não basta. A cópia do bloco de morte
  em `+0x8d615` é a candidata a ler.
- **A queda desfeita foi medida num volume só**: a água de Heide, bit 51. Os
  volumes que ligam o bit 52 (tipos 3, 4, 7, 8), a morte por dano ao aterrissar
  (`FUN_140372c00`, causas `0xa0`/`0x3c`) e o tipo 10, que só pede a câmera,
  não foram exercitados.
- **A volta para "a última posição no chão"**, usada quando a fogueira do
  registro não está no mapa carregado, nunca rodou. Cair perto da beira pode
  pôr o personagem de volta na beira.
- **Ficar dentro de um volume de morte sem estar no ar** (se algum mapa tiver
  isso) liga a câmera de queda sem nenhuma morte para o hook recusar, e a câmera
  ficaria presa.
- **A orientação não é escrita** no teleporte: o personagem chega à fogueira
  virado para onde estava.

### O renascer do passo 5

Medido só solo, em Heide, com o Samuel. Ficou de fora ou sem medir:

- **Contadores de morte**: a morte comum soma em `PlayerParam+0x104+tipo*8` e
  `+0x1a4` (`FUN_140203ad0`, só quando online). O renascer não soma.
- **Usos de magia e estados** (veneno, maldição, etc.) não são restaurados; só
  o Estus e o HP. A morte comum recarrega tudo pela recarga.
- **A checagem ofuscada** `thunk_FUN_140014b03`, uma das cinco antes do hollow,
  não é chamada.
- **A mancha online** (`NetSvrBloodstainManager::_StartCreateBloodstainJob`,
  `RequestCreateBloodstain`) não é enviada; outros jogadores não veem a morte.
- **O pecado** (`FUN_140202ae0`, que a sequência "YOU DIED" chama em sessão) não
  é tocado.
- **Nenhum aviso de morte** aparece: nem "YOU DIED", nem fade.
- **Fogueira fora do mapa carregado**: a volta para "a última posição no chão"
  num renascer por HP deixaria o personagem onde morreu, pagando a morte.
- **Como fantasma, na sessão**, a morte comum não tira almas nem hollowa. O
  renascer ainda não distingue, e as checagens do jogo (`FUN_140203b90`, o slot
  `+0x58` do contexto) podem recusar parte do custo. É o passo 6.

## O login que resolve o hostname oficial, depois de um reboot

Medido em 13/09, no primeiro lançamento depois de reiniciar a máquina: o jogo
mostrou "The DARK SOULS II service is not available" e **nunca conectou ao
servidor local**. Com `ss` amostrado a cada 10 ms, as tentativas eram para
`44.235.83.177:50050` e `44.235.102.125:50050` — que é exatamente o que
`frpg2-steam64-ope-login.fromsoftware-game.net` resolve —, ou seja, a porta já
trocada pelo injector e o **hostname oficial**. Ao mesmo tempo, a string UTF-16
do módulo (`0x1410d4ab0`) já dizia `127.0.0.1`, e havia cópias ASCII do
hostname oficial no heap (uma com o ponto final de FQDN).

No título, antes de apertar START, essas cópias ASCII não existem: elas são
feitas no login. Relançar o jogo resolveu na hora, e o login seguinte foi para
`127.0.0.1`. O que fez aquele processo usar o nome antigo não foi descoberto.
Duas condições estavam presentes e podem importar: a Steam da conta tinha
acabado de ser aberta pelo próprio jogo (código de saída 53, `steam://run`), e
o servidor tinha subido antes de a Steam logar. Para o loader isto importa: um
jogador veria o mesmo erro e não saberia que é só relançar.

## Not started

From the original brief, and unrelated to any of the above: arena
selection in the loader, and removing the max-player cap.
