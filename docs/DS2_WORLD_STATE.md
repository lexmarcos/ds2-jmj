# Estado de mundo numa sessão (M4)

O M4 pede que portas, alavancas, elevadores, atalhos, illusory walls e Pharros
abertos pelo host apareçam abertos para quem entrou, e que isso **não** vá para
o mundo do convidado. Antes de construir qualquer coisa, a pergunta é o que o
jogo já faz. Este arquivo é o que foi medido em 15/09.

## Estado: pausado em 15/09 — como retomar

**Por que parou.** Tudo o que dá para medir sem jogar está medido. O que falta
precisa de um mecanismo que nenhum dos dois personagens acionou, e em Heide
não há nenhum: flags e estado de objetos são iguais nos dois mundos. Achar e
acionar um exige teste manual, e isso não estava disponível.

**O que já está feito** (detalhes nas seções abaixo):

1. O convidado recebe **todas** as event flags do host na entrada, no lugar das
   próprias, e recupera as próprias em casa — medido com bits ligados só num
   lado.
2. A entrada é um **instantâneo** que o host exporta e o convidado importa no
   warp (`FUN_1402bf8f0` → `FUN_1402c2fa0`, um hit de cada, medido), e ele traz
   também `EventValueManager`, `EventBonfireManager`, `MapStateActManager` e
   `EnemyGeneratorDeadCounter` (lido).
3. Em sessão o host propaga cada flag que muda pelo pacote P2P `0x20`
   (`FUN_140474a60` → `FUN_14051e6b0`); o convidado só consegue mudar flags de
   mapa (`FUN_14025cdb0`) (lido; nenhuma flag mudou nas sessões medidas).
4. O save do convidado grava o estado de objetos do próprio mundo, não o do
   host (`SaveDataObj`, lido).
5. Ferramentas: `ds2os-dev flags` (lê e compara as flags das duas contas) e
   `up --party --party-host 2` (o Chico hospeda).

**O próximo passo, exatamente.** Com um mecanismo não acionado no mundo do
host (alavanca, porta, elevador, parede ilusória):

1. `up --seamless --keep-fog --party [--party-host N]`, `death --instance
   <host> mode respawn` se houver luta;
2. traço nas duas instalações: `bp 474a60` (flag em `rdx`, valor em `r8`),
   `bp 25ce10 deref rdx 8`, `bp 25cec0 deref rdx 8` e `bp 240270` (troca de
   estado de um `StateActCtrl`, estado novo em `rdx`);
3. `ds2os-dev flags` e o estado dos objetos (abaixo) nos dois, antes;
4. **(a)** o host aciona em sessão → hits e diferenças no convidado;
   **(c)** `session end` → o convidado em casa volta ao próprio estado;
   **(b)** `retoma` → nova entrada → o convidado vê acionado sem `0x20`.

Mecanismos são de uso único no save; a ordem (a), (c), (b) aproveita o mesmo.

## Estado de objetos de mapa, pela memória

Os objetos com máquina de estado são `MapObjStateActComponent` (vftable
`0x1410c6d78`, com `0x1410c6dc8` em `+0x30`): `scan 1410c6d78 8 400` acha uns
120 em Heide. Em cada componente, `+0x08` é o `MapEntity` (posição em
`+0x70`, `float` x, y, z; `*(+0x28)+8` o id do mapa) e `+0x48` o
`StateActCtrl` (vftable `0x1410cf668`), onde `+0x1c` é o estado atual, `+0x1d`
o índice e `+0x1f` uma marca. As fogueiras são objetos destes: a 1,1 m do
ponto de nascimento de cada uma, estado `30` acesa.

**Uma leitura errada, para não repetir.** A fogueira `0x7bac`, a 170 m de
Heide's Ruin, leu `10` num cliente e `30` no outro, e pareceu um mecanismo que
um só acionou. Não era: quem **voltou de uma sessão** lê `10` nos objetos
distantes (com `+0x1f = 2`) até eles carregarem de novo, e quem carregou o mapa
do zero lê o estado salvo. Os dois personagens têm as três fogueiras de Heide
acesas. Compare só objetos com `+0x1f = 0`, ou depois de um carregamento
limpo nos dois.

`SetState` do `StateActCtrl` é o slot `+0xe8` (`FUN_14023ff60` →
`FUN_1402410d0` no gerenciador `*(*0x1416148f0+0xa0)+0x280`); o estado atual
sai do slot `+0x50`. A importação do instantâneo aplica pares `(índice,
estado)` por esse slot (`FUN_1401f30e0`).

**Cuidado com teleporte.** A origem de um `MapEntity` não é chão: um teleporte
para a grade de objetos na água a oeste de Heide's Ruin matou o Samuel (hollow
3 → 4, cobrado pelo modo `respawn`). Use `teleport --to-bonfire` ou `goto`.

## O jogo já sincroniza event flags por P2P

Os pacotes P2P são registrados em `FUN_14051e3d0`, e o de tipo `0x20` é
`NetP2pPacketEventFlag` (vftable `0x1410fb688`, 7 slots). Os vizinhos: `0x2a`
AiFlag, `0x2b` AiData, `0x2c` AiGroup, `0x31` SpEffect.

| função | papel |
| --- | --- |
| `FUN_140474a60(mgr, flag, valor)` | **o setter do jogo**. Em sessão, um cliente que não é o host (`FUN_1405135f0`) só grava as flags que `FUN_14025cdb0` aceita; se a flag mudou e há sessão, `FUN_14051e6b0` a manda |
| `FUN_14051e6b0` | envia 8 bytes (`uint32 flag`, `byte valor`) pelo slot `+0x18` do pacote, `FUN_14025cec0` → `NetSvr` slot `+0x78`, tipo `0x20` |
| `FUN_14025ce10` | recebe: 8 bytes; se quem mandou não é o host, passa pelo mesmo filtro; grava com `FUN_1404750b0` |
| `FUN_1404750b0(mgr, flag, valor)` | grava o bit e chama os ouvintes (`FUN_140184ff0`) quando ele muda |
| `FUN_14025cdb0(flag)` | o filtro: recusa `flag < 1e9` (e o bloco `1e9..`) cujo `(flag/10000) % 100 > 2`, ou seja, as **globais**; aceita as de mapa (`1310xxxxx` em Heide) |

Ou seja, em sessão **o host manda qualquer flag**, e o convidado só consegue
mudar flags de mapa.

## Onde as flags moram

`mgr = *(*(*0x1416148f0 + 0x70) + 0x20)` (`EventFlagManager`, vftable
`0x1410eff58`). Em `mgr + 0x20` há uma tabela de 31 baldes indexada por
`((flag / 10000) * 0x89) % 0x1f`; cada nó tem `+0x00` ponteiro para os bytes,
`+0x08` o tamanho, `+0x0c` a categoria (`flag / 10000`) e `+0x10` o próximo. O
bit é `flag % 10000`, do mais alto para o mais baixo dentro de cada byte.

Pela harness: `chain a 16148f0 70,20 312` lê o gerenciador; os nós e os bytes
saem com `abs`. Em Heide só existem cinco categorias carregadas: `10` e `20`
(1250 bytes cada, as globais) e `13100`–`13102` (25 bytes cada, o mapa). Samuel
e Chico tinham exatamente os mesmos bits (34 em `10`, 18 em `20`, 2 em
`13100`), então comparar os dois não diz nada sozinho.

`FUN_1404744b0` esvazia a tabela (troca de mapa) e grava em `mgr + 0x118` se o
cliente está em sessão como não-host.

## O que foi medido

Com os dois em Heide, `up --seamless --party`, traço em `25ce10` e `25cec0`
(`deref rdx 8`) nas duas instalações:

1. **Entrada: nenhum pacote de flag.** Nem o host mandou nem o convidado
   recebeu um `0x20` ao formar a sessão.
2. **O convidado recebe as flags de mapa do host ao entrar.** Com a sessão
   desfeita, o bit `131000199` (sem uso conhecido) foi ligado **só na memória
   do host**. Formada a sessão, o convidado tinha o bit. Como nenhum `0x20`
   passou, a cópia na entrada vem de outro caminho, ainda não encontrado.
3. **O convidado não leva a flag para casa.** Depois de `session end`, no
   próprio mundo, o bit estava desligado no Chico e ligado no Samuel (e foi
   desligado de volta na memória do Samuel em seguida).
4. **As globais vêm junto.** O mesmo teste com o último bit da categoria `10`
   (`109999`): ligado só no host, presente no convidado na sessão, ausente no
   convidado em casa, desligado de volta no host.

5. **É troca, não mescla.** O inverso: `109999` ligado só no convidado, em
   casa. Na sessão o convidado tinha o bit **desligado**, como o host; de volta
   em casa, ligado de novo (e desligado na memória do Chico em seguida).

O convidado **carrega as flags do host** ao entrar, no lugar das próprias, e
recupera as próprias ao voltar.

Isso é o comportamento que o design pede para portas e alavancas, **se** elas
forem flags de mapa: o convidado vê o mundo do host, e o mundo dele não muda.

## De onde vem a cópia na entrada

Não é o `0x20`: é um **instantâneo do mundo** que o host serializa e o
convidado importa no warp de entrada.

O `EventFlagBuffer` (`*(mgr + 0x18)`, vftable `0x1410c2fa8`) guarda **duas**
cópias, zeradas separadamente por `FUN_140185320(buf, 0|1)`:

| cópia | globais | mapas (3 × 25 bytes, com o id do mapa em `+0xe7c` / `+0x1930`) |
| --- | --- | --- |
| 0, o próprio mundo | `+0x008` e `+0x4ea` (1250 bytes cada, categorias `10` e `20`) | `+0xde6`… |
| 1, o mundo de outro | `+0xe88` e `+0x136a` | `+0x184c`… |

`FUN_1404744b0`, ao esvaziar o gerenciador numa troca de mapa, grava em
`mgr + 0x118` se o cliente é convidado em sessão, e `FUN_1404746b0` usa esse
byte para dizer em qual cópia o mapa que sai é descartado (`FUN_140186480`).

- **Exporta** (host): `FUN_140185ac0` copia a cópia 0 para um blob, via
  `FUN_140474570`, chamado de `FUN_1402b6880` e `FUN_1402bf8f0`.
- **Importa** (convidado): `FUN_140185da0` copia o blob para a cópia 1 e
  `FUN_140184f70` avisa os ouvintes, via `FUN_140474590`, chamado de
  `FUN_1402b9ad0` e `FUN_1402c2fa0` — métodos virtuais vizinhos do construtor
  do warp de entrada (`FUN_1402c2a80`).

Medido numa entrada (15/09, 08:36, traço em `474570` e `474590`): o host passou
por `FUN_140474570` vindo de `+0x2bfbfc` (`FUN_1402bf8f0`) e o convidado por
`FUN_140474590` vindo de `+0x2c3080` (`FUN_1402c2fa0`), os dois com
`r8 = 0a1f0000`, o mapa de Heide. Um hit de cada, e nenhum `0x20`.

`FUN_1402c2fa0`, quando `+0xf8 == 4`, importa do mesmo blob tudo o que o design
chama de mundo do host. Os destinos, nomeados pela RTTI da vftable lida na
memória (`ctx = *0x1416148f0`):

| blob | destino | classe |
| --- | --- | --- |
| `+0x3108` | `*(ctx+0x70)+0x20` | `EventFlagManager` — as flags, medido acima |
| `+0x3b18` | `*(ctx+0x70)+0x28` | `EventValueManager` (`FUN_14047a350`); `FUN_14047a430` o enche com as flags `10010000`–`10019999` |
| `+0x59ec`, 16 bytes | `*(ctx+0x70)+0x58` | `EventBonfireManager` (`FUN_14017ea40`) |
| `+0x3000` / `+0x3004` | `*(ctx+0x38)+0x1f8` | `MapStateActManager` (`FUN_1401f3000`, `FUN_1401f30e0`) — estado de objetos de mapa |
| `+0x4018` | `*(ctx+0x40)+0x10` | `EnemyGeneratorDeadCounter` (`FUN_1401f69e0`) — o despawn de inimigos |

`*(ctx+0x70)` é o `EventManager`; `+0x10` nele é o `EventTaskManager`.

Ou seja, a entrada do jogo original já é **um instantâneo autoritativo do
host**: flags, valores de evento, fogueiras, estado de objetos e mortes de
inimigos. O que o M4 precisa medir é quanto disso chega ao objeto na tela e o
que muda **depois** da entrada, que só o `0x20` (flags) cobre até onde se viu.

## O estado de objetos no save do convidado

O save guarda o estado de objetos de mapa num `SaveDataObj` (vftable
`0x1410da378`): até três mapas, 0x6008 bytes cada, versão `0x69`.

- **Grava** (`FUN_1402e5a10`, slot `+0x10`): para cada mapa guardado em
  `*(*(ctx+0x38)+0x200)+0x18`, se o cliente **não** é convidado em sessão e o
  mapa está carregado, serializa os objetos vivos (`FUN_1401f22c0`); se é
  convidado, grava a cópia guardada (`FUN_1401e7450`) — o mundo dele, não o
  do host. Depois `FUN_1401f2ea0` no `MapStateActManager`.
- **Lê** (`FUN_1402e5890`, slot `+0x18`): até 3 blocos para `FUN_1401e7a10`,
  depois `FUN_1401f2ce0`.

Lido, não medido: é o jogo garantindo que um convidado não salva portas e
alavancas do host no próprio save, que é o que o design pede.

## O que ainda não se sabe

- **Se portas, alavancas, elevadores e illusory walls são flags de mapa.**
  `MapObjStateActComponent` (vftable `0x1410c6d78`) pode guardar estado por
  objeto fora do `EventFlagManager`. Nenhum mecanismo real foi acionado ainda.
- ~~**Uma mudança de flag feita durante a sessão.**~~ Medida no M7 (15/09):
  o setter do host (`DS2_Carry.req`, `flag <id> 1`) chegou ao convidado em
  20 ms, global e de mapa. Falta um **objeto** mudando de estado ao vivo.
- **O que o convidado vê de uma alavanca que ele mesmo puxa** no mundo do host:
  uma flag de mapa passa pelo filtro, mas o objeto pode ter outra trava.
- **Mudanças depois da entrada fora das flags.** `MapStateActManager`,
  `EnemyGeneratorDeadCounter` e `EventBonfireManager` chegam no instantâneo;
  não se sabe se têm pacote próprio durante a sessão.
- **O lado oposto do design.** "O que vocês fizerem juntos é salvo para todos"
  (um chefe morto na sessão fica morto no mundo do convidado) é exatamente o
  que o jogo **não** faz: o mundo do convidado volta como estava. Isso é
  trabalho de um marco posterior, não do M4.
