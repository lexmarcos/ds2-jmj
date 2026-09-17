# Viagem por carregamento nativo (alternativa D): o plano

Levantamento feito em 16/09 por um agente Fable, em Ghidra `-readOnly` e
`objdump`, sem executar nada nos jogos. Versão 1.03 Calibrations 2.02.

É a resposta à decisão registrada em `DS2_SEAMLESS_COOP_TASKS.md` de refazer a
viagem do zero, depois de três famílias de queda distintas provarem que forçar
o mapa ao lado e teleportar é inseguro por construção.

Leia com `DS2_PRESENCE_ASTRA_REVIEW.md` (que define as alternativas A a D) e
`DS2_PRESENCE_REBUILD_PLAN.md` (as primitivas de presença).

## A conclusão que decide o rumo

**D puro — carregamento nativo nos dois lados preservando a mesma sessão — não
está disponível hoje**, e por dois motivos independentes. **O primeiro deles
foi medido em 16/09 e caiu** (ver a caixa na fase 1); o segundo continua de pé:

1. **A viagem nativa do host provavelmente encerra a própria sessão.**
   `FUN_1402bd0d0` (slot `+0xe0` do host ctrl) é chamado por `FUN_1402c7ec0` a
   cada warp daquela máquina, com o motivo. O **case 2** — que é a viagem de
   menu do host — faz:

       *(ctrl+0x1b8) |= 2;
       if (3 < *(int*)(ctrl+0x30) - 1U) {     // ou seja, (ctrl+0x30) >= 5
           FUN_1402be230(ctrl, 0x15);          // manda 0x15 ao par
           FUN_1402be1e0(ctrl, 9);             // +0x150 = 0x12: sessão encerrada
       }

   `ctrl+0x30` é o papel do convidado invocado, escrito por `FUN_1402bd560` a
   partir da mensagem de join, indexado na tabela de 20 papéis de
   `0x1410c0050` — onde o **fantasma branco é o índice 7**. Se for 7, `3 < 6` é
   verdade e a sessão morre antes de o carregamento começar.

   **Isto nunca foi exercido**: o M8 só faz o host viajar nativo depois de os
   convidados saírem legalmente, então o case 2 nunca rodou com convidado vivo.

2. **A re-entrada nativa do convidado já estava bloqueada** — o pacote `0xd` só
   é aceito pelo host em `0xe` e pelo convidado em `0xc`, e a sessão
   estabelecida veta (ver `DS2_PRESENCE_REBUILD_PLAN.md`).

Portanto D fecha de uma de duas formas, e nenhuma é "só trocar o transporte":

- **D-3a:** host viaja nativo, **convidado é re-invocado** (sessão nova no
  destino). Totalmente nativo, zero ponteiro pendurado, já provado ponta a
  ponta. Custa o ritual de invocação (~75 s) e não é seamless.
- **D-3b:** host viaja nativo, convidado faz carregamento nativo **e a mod
  reconstrói a presença** — isto é, **D = carregamento nativo + alternativa A**.
  Preserva a sessão; é onde está o custo real.

## 1. O caminho nativo

Todo warp passa por `FUN_1401c2a80` (`+0x1c2a80`), slot `+0x40` do contexto
global `*(0x1416148f0)`. A **viagem de fogueira** é a cadeia acima dele:

| passo | função | offset |
| --- | --- | --- |
| menu escolhe a fogueira | `FUN_14017fdb0` | `+0x17fdb0` |
| monta o pedido | `FUN_1401843b0(&req, id, motivo)` | `+0x1843b0` |
| inicia a viagem | `FUN_140184830(travel, &req)` | `+0x184830` |
| máquina de fases | `FUN_140184a10(travel)` | `+0x184a10` |
| **o warp** | `FUN_1401c2a80(ctx, &req, flag)` | `+0x1c2a80` |
| grava renascimento | `FUN_14044fe30` | `+0x44fe30` |

Prólogos conferidos contra o executável:

    +0x17fdb0  4c 8b dc 56 48 81 ec 10 01 00 00 48 8b
    +0x1843b0  48 89 5c 24 08 48 89 74 24 20 57 48 83 ec 60
    +0x184830  40 53 48 83 ec 60 8b 02 48 8b d9 89 01
    +0x184a10  40 53 48 83 ec 20 48 8b d9 48 8b 0d
    +0x44fe30  48 89 5c 24 10 57 48 83 ec 60 83 7a 04 01
    +0x1c2a80  40 55 41 54 41 56 48 8d 6c 24 e0 48 81 ec
    +0x2bd0d0  83 fa 05 0f 87 4b 01 00 00 53 48 83 ec 20

**Campos do pedido:** `+0x00` família do destino (3 fogueira, 4 mundo próprio,
2 player-start), `+0x04` **motivo** (1 morte, 2 viagem, 4 entrada/volta),
`+0x08` id do mapa, `+0x18` ponto de nascimento.

**O portão de motivo** em `FUN_1401c2a80`: aceita se `ctx+0x24ac == 0x1e` e
`!(ctx+0x24b1 & 2)`; depois motivo 1 sempre passa, motivo 4 com flag 0 passa
sem portão, e motivos 2 e 4-flag-1 passam pelo portão `FUN_140248940`, que é
`*(mgr+0x168) > 0` — o contador de tempo do multiplay, positivo em sessão viva.

**Host** viaja com motivo 2 flag 0. **Convidado entrando no mundo do host** usa
motivo 4 flag 1, e essa forma **só é construída pelo handler do estado 2 da
sessão** a partir de um payload de rede — não pela cadeia de fogueira.
**Convidado voltando para casa**: motivo 4 flag 0.

**Por que o carregamento nativo resolve a corrupção:** o loader em `ctx+0x24ac`
passa pelo estado `0x14`, que destrói mapa e personagens e zera `ctx+0xd0`, e
pelo `0xb`, que recria. O mundo é remontado, não deslizado, então as três
famílias de queda deixam de ter onde existir.

## 2. O mundo emprestado tem quatro componentes, e o warp reconstrói um

| componente | onde mora | um warp flag 1 reconstrói? |
| --- | --- | --- |
| bit "em outro mundo" | `ctx+0x24b1 & 0x40` | **Sim** |
| registro de volta para casa | `session+0x1a0..+0x1c8` no join ctrl | Não é regravado; sobrevive enquanto o objeto existir |
| snapshot do mundo do host (flags, fogueiras, objetos) | importado por `FUN_1402c2fa0`, **só no estado join 4** | **Não** |
| presenças remotas (a cópia do outro) | registradas no **estado join 5**, materializadas por `0xd→0xe→0xf` | **Não** |

Isto explica de vez a tentativa 4, em que o convidado voltou para o próprio
mundo: reemitir só o warp move o jogador, liga o bit, e não refaz nem o
snapshot nem a presença.

**Aviso que vale ouro:** o convidado **nunca** deve chamar `FUN_14044fe30` —
ela grava o registro de renascimento do **próprio** jogador, que vai para o
save e para o registro de volta-para-casa. Um convidado que grave ali a
fogueira do host contamina o save dele. Só o host grava.

## 3. Os watchdogs — são dois

**300 s** (`FUN_1402be090`, `DAT_1410d7b40 = 300.0f`, bytes `00 00 96 43`):
dispara com o bit `0x10` de `ctrl+0x1b8` armado e `ctrl+8 - ctrl+0x1b4 > 300`.
O case 0 de `FUN_1402bd0d0` arma **e renova** `+0x1b4`; o case 4 arma e **não
renova**. Tratamento dentro da transação: guardar `+0x1b8` e `+0x1b4`, renovar
`+0x1b4 = ctrl+8` se o bit estiver armado, restaurar no fim. É escrita de dados
num campo por quadro, reversível — não desligar globalmente.

**~23 s, o do silêncio** — e este é **o matador mais provável**. Medido na
tentativa 9: o host largou um convidado silencioso ~23 s depois de ele parar de
mandar. Durante um carregamento nativo o convidado tem `ctx+0xd0 = 0` e não
manda nada por segundos. Rankeia à frente do de 300 s.

## 4. O que sobrevive

| peça | sobrevive | como muda |
| --- | --- | --- |
| contrato `Idle/Moving/Arrived/Failed` | sim | `Arrived` passa a vir do **fim do carregamento nativo** (`ctx+0x24ac` de volta a `0x1e`, `ctx+0xd0 != 0`), não do streamer. "Nunca por tempo" continua |
| barreira + `TravelRelease` + host primeiro | sim, intacta | o gatilho do recibo muda; host primeiro fica **mais** necessário, porque o convidado importa o snapshot do host |
| provas de tipo | sim | |
| guardas `__try` | sim, como rede | devem passar a **nunca** disparar; um disparo vira sinal de que o carregamento não aconteceu |
| `DropDeadRigidBody` | vira **medidor** | deve parar de disparar |
| canal entre as máquinas | sim, central | carrega a transação e, em D-3b, o blob da presença |
| transporte (backread forçado, teleporte, contato) | **não** | sai inteiro; é a fonte da corrupção |
| cortina desenhada por nós | **em aberto** | provavelmente vira a tela do próprio jogo; `FUN_140b06270` é inerte fora de um warp real e **não foi testado dentro de um** |

## 5. As fases

> ## Fase 1 medida em 16/09: **passou**
>
> Sessão de co-op viva e verificada, sem viajar. O controlador do host
> (`0x7ffffe5bf120`, vftable `0x1410d7998`, `+0x150 = 0x10`) tem
>
>     ctrl+0x30 = 1
>
> A previsão estática era 7. **Está errada.** Com 1, a condição do case 2 é
> `3 < (unsigned)(1 - 1)` = `3 < 0` = falsa, e o ramo que manda `0x15` ao par e
> encerra a sessão **não roda**. A viagem nativa do host **não mata a sessão**.
>
> A aritmética foi conferida no binário e não só na decompilação do relatório:
> em `FUN_1402bd0d0`, `param_1` é `longlong*`, então `param_1[6]` é o byte
> `+0x30` e `param_1+0x37` como `uint*` é `+0x1b8`. Confere. E vale registrar o
> outro lado da comparação: ela é verdadeira para `x >= 5` **e também para
> `x == 0`**, que dá wrap em unsigned — então zero é tão perigoso quanto sete.
>
> A varredura pela vftable devolve dois endereços; o segundo (`0xa38fb08`) é
> falso positivo — o `+0x150` dele é um ponteiro, não um estado.
>
> **Escopo desta medição:** uma amostra, um convidado, fantasma branco invocado
> pelo sistema de party, logo depois da invocação. Não cobre invasor nem outros
> papéis, e não foi observada ao longo de uma sessão inteira.
>
> **Consequência:** D-mesma-sessão **não** precisa do hook host-side de
> supressão de fim de sessão. Esse risco, que era o mais caro do plano, está
> fora. O caminho segue para a fase 2.
>
> **De brinde, para a fase 2:** no mesmo controlador, `+0x1b8 = 0x261` — o bit
> `0x10` **não** está armado — e `+0x1b4 = 0.0` com o relógio `+0x08` em 65,9 s.
> Ou seja `+0x1b4` nunca foi renovado; se algo armar aquele bit depois de 300 s
> de sessão, o watchdog dispara **na hora**, sem os 300 s de folga.

**Fase 1 — custo zero, e é a que pode matar D inteiro.** Ler `+0x30` do host
ctrl (`NetSummonAcceptMultiplayCtrl`, vftable `0x1410d7998`) **numa sessão de
co-op viva, sem viajar**. Se for >= 5 (previsão estática: 7, o fantasma
branco), a viagem nativa do host encerra a sessão, e D-mesma-sessão passa a
exigir um hook novo do lado do **host** suprimindo o fim de sessão no case 2 —
uma classe de intervenção que nunca existiu aqui (toda supressão até hoje é do
convidado). Se for < 5, o host viaja nativo sem encerrar e D fica barato.

> ## Fase 2 medida em 16/09: o warp passa, e **mata o host pelas presenças**
>
> Duas corridas, mesmo destino (`0a1f0000`, fogueira `7ba7`), mesma função
> `StartTravel`. A diferença entre elas é uma só, e por isso valem como
> controle e experimento.
>
> **Controle — convidado fora da sessão.** Pelo caminho de fallback
> (`junta desliga` → votação → `TravelLeave` → `StartTravel`): o host viaja e
> **chega** em Heide (`-18.5, 209`). O objeto do controlador some no mesmo
> segundo, mas por causa da saída dos convidados, não do warp.
>
> **Experimento — convidado dentro da sessão.** Pelo pedido `nativo`, que
> chama `StartTravel` sem mandar ninguém embora:
>
>     21:36:58  estado=0x10 papel=1 flags=0x261 armado=False relogio=54.2
>     21:36:59  host: viagem iniciada para a fogueira 7ba7
>     21:37:01  estado=0x10 papel=1 flags=0x271 armado=True  relogio=57.2
>     21:37:03  excecao c0000005 em +0x5180a8, lendo 0xffffffffffffffff
>
> **Três coisas medidas, em ordem de importância.**
>
> **1. A fase 1 se confirmou em ação.** No instante do warp o estado seguiu
> `0x10` e o papel seguiu 1: o ramo do case 2 que encerra a sessão **não
> rodou**. O host pode warpar sem que a sessão seja encerrada por aquele
> caminho.
>
> **2. O warp arma o watchdog de 300 s e não renova a marca.** As flags foram
> de `0x261` para `0x271` — o bit `0x10` ligou — com `+0x1b4` ainda em `0.0` e
> o relógio em 57,2 s. É exatamente a configuração que o levantamento previu
> como perigosa, e agora está medida: a partir daí a sessão está num
> cronômetro que dispara quando o relógio passar de 300.
>
> **3. E o host caiu 3,7 s depois, dentro do código de presença.**
>
>     1405180a0:  sub  $0x28,%rsp
>     1405180a4:  mov  0x60(%rcx),%rax     rcx = 0x7fffe81ed980
>     1405180a8:  mov  0x18(%rax),%edx     <- aqui
>
>     rax = 3f800000293988ec  -> dois floats (1.0 e 4.1e-14), nao um ponteiro
>     retornos: +0x5184b0 +0xa480d2 +0x10c5118 +0x517154 +0x5140c0
>
> A pilha inteira está em `0x51xxxx`, a região do registro de presenças
> remotas. O campo `+0x60` do objeto devolveu dados de ponto flutuante onde o
> código espera um ponteiro.
>
> **O que o par controle/experimento isola.** Mesmo warp, mesmo destino, mesma
> fogueira: **sem presenças, chega limpo; com presenças, mata o host em 3,7 s.**
> O warp não é o problema — as presenças remotas sendo desmontadas debaixo de
> uma sessão viva são.
>
> **Consequência para o plano.** O que era inferência vira medição: "presenças
> remotas **não** são reconstruídas por um warp" é fraco demais. Elas são
> desmontadas, e a maquinaria de sessão continua andando por cima do que sobrou.
> Portanto **D-3b exige retirar as presenças antes do warp** — `FUN_14051c820`
> por jogador, a primitiva do `DS2_PRESENCE_REBUILD_PLAN.md` — e recriá-las
> depois. Isto é, **D = carregamento nativo + alternativa A**, agora com apoio
> empírico e não só por leitura.
>
> **Custo.** O host caiu (Samuel estava em 30 antes). O convidado **não pagou**:
> ficou em 80, e foi parado sem `--force` depois.
>
> **E um erro de método que custou caro, registrado para não repetir:** antes
> deste experimento eu parei os jogos com `game stop --force` achando que não
> havia sessão; o party tinha reinvocado e havia. **Vinte pontos**, dez em cada
> personagem (Samuel 20→30, Chico 70→80). O `--force` existe justamente para
> atropelar a checagem que evita isso. Nunca usar `--force` sem antes ler o
> estado da sessão.

**Fase 2 — viagem nativa só do host, sem convidado, custo ~zero.** Instrumentar
`FUN_1402be090` e a cadeia do warp; confirmar que o controlador do host
sobrevive além de 300 s e que o watchdog não arma. Aqui também se responde se a
tela de carregamento do jogo serve de cortina.

> ## A primeira metade de 3b, medida em 16/09: **a hipótese caiu**
>
> A regra decidida é viagem em conjunto com **sessão preservada**, que é a 3b.
> O experimento mais barato para a primeira metade: se o host morre 3,7 s depois
> do warp porque as presenças são desmontadas debaixo de uma sessão viva, então
> retirá-las antes deve fazer a queda sumir.
>
> **Retirar funciona.** `presenca` leu o registro `0x7FFFFE479D80` com 1 viva,
> entrada 0, estado 2, papel 1, net id 257, personagem `0x7FFFEB7B9E60`.
> `presenca retira` chamou `FUN_14051c820` e três segundos depois o registro
> lia **0 vivas**.
>
> **E a sessão sobreviveu à retirada** — `p2pSessionVerified: true`, host em
> `0x10`, convidado em 7, pacotes cruzando. Esta é a primeira **medição** da
> premissa central do `DS2_PRESENCE_REBUILD_PLAN.md`, que até aqui era leitura
> estática: `FUN_14051c820` tira uma presença sem tocar na sessão.
>
> **Mas o host caiu do mesmo jeito**, no mesmo endereço e no mesmo tempo:
>
>     22:03:27.531  presencas (antes da viagem nativa): 0 viva(s)
>     22:03:27.531  host: viagem iniciada para a fogueira 7ba7
>     22:03:31.200  excecao c0000005 em +0x5180a8, rax=00000000bf7c1c5d
>
> **Onde eu errei.** O "controle" da fase 2 — o caminho de fallback que chegou
> limpo — tirava as presenças **e** encerrava a sessão. Eu atribuí às presenças
> o que podia ser de qualquer uma das duas. Com presenças fora e sessão viva a
> queda continua, então **as presenças não são a causa**. É a terceira hipótese
> minha derrubada por medição hoje, e as três tinham a mesma forma: uma
> explicação plausível sustentada por um teste que não separava as variáveis.
>
> **O que a queda é, com precisão.** `FUN_1405180a0(param_1)`:
>
>     uVar4 = *(param_1 + 0x60);      // devolveu bf7c1c5d, um float
>     iVar1 = *(int *)(uVar4 + 0x18); // <- +0x5180a8, aqui
>     ... &DAT_1410c0050 + papel * 0x10
>
> É uma consulta de **papel**, na mesma tabela de 20 papéis que o case 2 de
> `FUN_1402bd0d0` indexa. Chamada por `FUN_140518230(objeto, float)`, que é uma
> **atualização por quadro**. Ou seja: o warp invalida um objeto e a máquina de
> rede continua rodando por cima dele no quadro seguinte.
>
> **O que isso muda no plano.** A 3b não começa com "retirar as presenças". Ela
> começa com **parar a máquina de rede durante o warp**, ou impedir que ela
> ande sobre o objeto que o warp derruba. Retirar presença continua sendo
> necessário para a reconstrução, mas não é o que evita esta queda.
>
> **Custo.** O host caiu (Samuel 30 → 40, contando a queda da fase 2). O
> convidado ficou em 80 e foi parado sem `--force`.

> ## Quem é o `param_1` e quem o invalida (16/09, leitura estática)
>
> **O despachante.** `FUN_140514020(param_1, delta)` é a batida por quadro da
> camada de rede, e ela percorre os slots do objeto global `DAT_141616cf8`:
>
>     raiz[0] = *raiz        -> FUN_140520ed0      (e lido em +0xa4 e +0xb4)
>     raiz[1] = raiz+0x08    -> FUN_14051e860
>     raiz[2] = raiz+0x10    -> FUN_14025b460
>     raiz[3] = raiz+0x18    -> FUN_1402c9540
>     raiz[4] = raiz+0x20    -> FUN_14051c940      o registro de presencas
>     raiz[5] = raiz+0x28    -> FUN_1405170e0      <- o que quebra
>     raiz[6] = raiz+0x30    -> FUN_140291170
>
> Então **`param_1` é `*(0x141616cf8 + 0x28)`**: um irmão do registro de
> presenças, no mesmo objeto de rede, batido todo quadro. Ele tem estado em
> `+0x08` (0, 1 ou 2), uma trava em `+0x78` que `FUN_1405170e0` fecha no começo
> e abre no fim, e só chama a função que quebra quando o estado é 1 ou 2.
>
> **O objeto que morre.** Dentro de `FUN_140518230` existe um vetor:
>
>     registros = *(param_1 + 0x10)     contagem em param_1 + 0x0c
>     cada registro tem 0x18 bytes:
>       +0x00  etiqueta (o caminho que quebra exige 0x7f00)
>       +0x04  float que acumula o delta do quadro
>       +0x0a  bandeiras; bit 0 "em uso", bit 1 "ja tratado"
>       +0x10  **um ponteiro para um objeto do jogo**
>
> É esse `registro+0x10` que vai para `FUN_1405180a0`, que lê `+0x60` dele,
> depois `+0x18` daquilo, e indexa a tabela de papéis `DAT_1410c0050`. Nas duas
> quedas o `+0x60` devolveu ponto flutuante (`3f800000293988ec` e `bf7c1c5d`),
> e os dois endereços eram vizinhos (`0x7fffe81ed980` e `0x7fffe81ed160`):
> elementos de uma mesma coleção, reaproveitados.
>
> `FUN_1405180a0` também chama `FUN_14017b7e0(param_1 + 0x10)`, que é da mesma
> família dos `GetComponent<T>` que já nos morderam — o objeto é do tipo
> personagem.
>
> **Quem invalida:** o próprio warp. O carregador passa pelo estado `0x14`, que
> destrói mapa e personagens e zera `ctx+0xd0`. O objeto de `registro+0x10`
> morre ali, e **nada limpa o registro** — o bit "em uso" continua ligado, a
> etiqueta continua `0x7f00`, e no quadro seguinte a camada de rede anda por
> cima dele. É a mesma doença de sempre, numa lista nova.
>
> **Isto é leitura, não medição.** O que falta medir é quem escreve por cima do
> objeto, e agora há um alvo com endereço: a cadeia
> `*(*(0x141616cf8 + 0x28) + 0x10) + i*0x18 + 0x10` dá o objeto **antes** do
> warp, e a vigia de página (`wp` em `DS2_Trace.req`) pode ser armada sobre ele
> enquanto ele ainda existe. É a ferramenta que achou o corpo rígido, e é a
> primeira vez nesta investigação que ela tem um endereço para vigiar.
>
> **E sugere um conserto mais barato que reconstruir presença:** limpar o bit
> "em uso" dos registros com etiqueta `0x7f00` antes do warp, ou segurar o
> estado de `*(raiz+0x28)` em 0 durante a transação de viagem, que é o que
> impede `FUN_1405170e0` de chamar a função que quebra. Nenhum dos dois foi
> testado.

> ## O teste do sync, e o que ele revelou sem querer (16/09, 22:24)
>
> A leitura ao vivo tinha fechado bonito: o subsistema `*(raiz+0x28)` guarda o
> **id do mapa corrente** em `+0x18`, estado em `+0x08`, e trinta registros de
> `0x18` bytes com ponteiro para personagem em `+0x10`. **Os dois endereços das
> duas quedas eram os registros 11 e 24 desse vetor**, ambos com etiqueta
> `0x7f00` e bandeiras `0x11` — exatamente a combinação que chega na consulta
> de papel. O encaixe era perfeito.
>
> O conserto: escrever 0 no estado, que é o estado ocioso do próprio jogo e
> desliga o ramo que quebra.
>
> **Não funcionou, por dois motivos, e o primeiro é o que importa.**
>
> **1. A escrita não gruda.** `sync para` levou o estado de 1 para 0 às
> 22:23:59. Treze segundos depois, no momento da viagem, o log da própria
> viagem leu **estado 1**: a máquina de estados do jogo tinha restaurado
> sozinha. A intervenção não estava em vigor quando importava. Qualquer
> conserto aqui tem que ser um detour em `FUN_1405170e0`, não uma escrita.
>
> **2. E eu mudei duas variáveis de novo.** O destino desta corrida foi Majula
> (`0a040000`) e não Heide (`0a1f0000`), porque o host já estava em Heide.
> Então esta corrida não é comparável às duas anteriores.
>
> **A queda mudou de endereço**, de `+0x5180a8` para `+0x3ce81c`:
>
>     1403ce810:  mov 0x10(%rbx),%rax      o vetor
>     1403ce814:  mov (%rax,%rdi,8),%rsi   indice 0x4e
>     1403ce818:  mov 0x30(%rsi),%rcx      devolveu NULO
>     1403ce81c:  mov 0x58(%rcx),%eax      <- aqui
>
> Com `rax = 0x7fffe81ecfd0`, dentro da mesma vizinhança dos objetos do sync.
> **Não dá para saber se mudou por causa do destino ou da intervenção**, e a
> intervenção nem estava ativa. É mais uma lista com referência pendurada.
>
> ## A conclusão que eu tiraria, e ela é desagradável
>
> Este é o terceiro consumidor distinto a morrer pelo mesmo motivo hoje: as
> listas de componentes das entidades (74 cópias do mesmo laço), a lista de
> sincronia de personagens do mapa, e agora este vetor. **Consertar consumidor
> não termina** — é a mesma lição da manhã, cobrada outra vez à noite.
>
> O warp derruba o mundo **assumindo que nada mais aponta para ele**. Uma
> sessão viva aponta, por várias estruturas ao mesmo tempo. Não parece haver
> um ponto único onde intervir.
>
> Isso empurra a leitura para: **"host warpa enquanto a sessão vive" pode
> simplesmente não ser viável por remendo.** O que funciona hoje, e funcionou
> limpo nas duas vezes que foi testado, é o warp com a sessão **encerrada** —
> que é a D-3a.
>
> A regra do projeto é sessão preservada, então a saída, se existir, não é
> impedir que a sessão toque o mundo durante o warp: é **suspender a sessão**
> durante a travessia e retomá-la do outro lado, sem que ela seja encerrada do
> ponto de vista do servidor nem dos saves. Isso não foi investigado, e não sei
> se existe.

> ## O silêncio da rede: funcionou, e a queda esperou (16/09, 22:50)
>
> O detour em `FUN_140514020` — a batida única de onde toda a camada de rede é
> percorrida — foi armado antes do warp e solto quando o mundo voltasse.
>
> **A primeira corrida não provou nada e o log disse por quê:** a janela abriu e
> fechou em três milissegundos, com **zero** batidas puladas, porque no instante
> de armar o mundo ainda estava de pé. A condição de saída tinha uma borda só.
> Corrigida para duas — o mundo cair e depois voltar.
>
> **A segunda corrida funcionou:** 192 batidas puladas em 5,3 s, com o mundo
> caindo e voltando. Nenhum subsistema de rede andou enquanto não havia mundo.
>
> **E o host caiu assim mesmo, no mesmo milissegundo em que a janela fechou:**
>
>     22:50:42.945  rede: parei a batida ate o mundo voltar
>     22:50:48.278  rede: o mundo voltou; 192 batida(s) puladas
>     22:50:48.279  excecao c0000005 em +0x5180a8
>
> Em `+0x5180a8`, que é a consulta de papel do sync — a mesma de antes.
>
> **O que isso diz, com precisão.** O silêncio não era pouco: era cedo demais
> para acabar. Quando o carregador volta ao ocioso e o personagem existe, a
> **lista de sincronia ainda é a do mapa velho** — no momento da viagem ela
> lia `mapa 0a1f0000` e o destino era `0a040000`. A primeira batida depois do
> silêncio anda a lista velha e morre.
>
> **O conserto que a evidência aponta**, e ele é de uma linha: ao soltar a
> janela, **pôr o estado do sync em 0 antes** de deixar a batida passar, para
> que `FUN_1405170e0` tome o ramo que reconstrói em vez do que anda. A escrita
> avulsa de 22:23 não colou porque foi treze segundos antes e a máquina
> restaurou; no instante exato da soltura, a batida seguinte é a que lê.
>
> **Mas é a quinta intervenção**, e as quatro anteriores tiveram esta mesma
> forma: evidência boa, conserto plausível, e a queda aparecendo no lugar
> seguinte. Vale dizer isso antes de gastar mais.
>
> **Duas correções ao que eu já tinha escrito aqui.** A queda **não** é
> determinística, e também **não** é imediata: a corrida das 22:41 pareceu
> limpa, e o host morreu minutos depois, o que só se descobriu quando o
> `session end` falhou por falta de processo. Qualquer medição daqui para
> frente precisa vigiar minutos, não os 3,7 s do primeiro caso.
>
> **Custo até aqui:** Samuel de 10 a 70 pontos ao longo da noite. Chico intacto
> em 80 — o convidado nunca pagou por esta linha de trabalho.

> ## O veredito da noite de 16/09: a viagem nativa com sessão viva não se
> conserta por remendo
>
> O último conserto — zerar o estado do sync no instante exato em que o
> silêncio da rede termina — **funcionou uma vez e falhou na seguinte**.
>
>     23:01:46  sync (o mundo voltou): estado 1 -> 0
>     23:01:46  rede: o mundo voltou; 193 batida(s) puladas
>     ... 120 s vigiados, nenhuma queda. Host em Majula, sessao verificada.
>
>     23:05:04  sync (o mundo voltou): estado 1 -> 0
>     23:05:04  rede: o mundo voltou; 153 batida(s) puladas
>     23:06:08  excecao c0000005 em +0x1e5c30      <- 64 s depois
>
> **Cinco intervenções, cinco endereços diferentes:**
>
> | intervenção | onde a queda foi parar |
> | --- | --- |
> | guarda no pré-desenho (`FUN_1403f4f10`) | `+0x17b260`, um `GetComponent<T>` |
> | varredura das listas de entidade | `+0xfd8592`, busca que devolve nulo |
> | retirada das presenças | `+0x5180a8`, consulta de papel do sync |
> | silêncio da batida da rede | `+0x5180a8`, no ms em que a janela fecha |
> | zerar o sync ao soltar a janela | `+0x1e5c30`, 64 s depois |
>
> Cada conserto foi tecnicamente correto sobre o que o anterior revelou, e
> nenhum terminou. O warp derruba o mundo assumindo que **nada** aponta para
> ele; uma sessão viva aponta por estruturas demais, e elas não estão
> enumeradas em lugar nenhum.
>
> **O que está provado e vale guardar:**
>
> - o host **pode** warpar nativo sem que a sessão seja encerrada pelo case 2
>   (`ctrl+0x30 = 1` medido, e a sessão seguiu verificada nas duas corridas);
> - `FUN_14051c820` retira uma presença **sem tocar na sessão** (medido, não
>   mais leitura);
> - a batida da rede **pode** ser silenciada durante o carregamento
>   (192 e 153 batidas puladas, mundo caindo e voltando);
> - e uma viagem nativa completa com sessão preservada **é possível** — ela
>   aconteceu, uma vez, com o host chegando em Majula e o convidado seguindo
>   fantasma em Heide.
>
> **O que não está:** que ela seja repetível. É uma em duas com o último
> conserto, e as quedas são intermitentes e às vezes tardias, o que torna
> qualquer corrida curta inconclusiva.
>
> **Recomendação:** parar de remendar consumidor. Os caminhos que sobram, em
> ordem de honestidade:
>
> 1. **D-3a** — os dois saem da sessão, viajam nativo, o party reinvoca do
>    outro lado. Funciona hoje, é inteiramente nativo, e custa o ritual de
>    invocação. Não é seamless, e é o único caminho com zero quedas medidas.
> 2. **Achar a enumeração**, se existir: o que o jogo faz quando *ele* encerra
>    uma sessão antes de um warp, que é o caminho limpo das duas corridas de
>    controle. Se houver uma função que desfaz tudo o que a sessão pendurou no
>    mundo, chamá-la antes do warp e refazer depois é um conserto de raiz e não
>    um remendo. Não foi procurada.
> 3. Continuar remendando, sabendo que a sexta intervenção tem a mesma forma
>    das cinco anteriores.
>
> **Custo da noite:** Samuel de 10 a 90 pontos. Chico intacto em 80. O convidado
> nunca pagou por esta linha de trabalho — todas as quedas foram do host.

**Fase 3 — convidado no mundo. É aqui que há risco de ponto.** Com baseline dos
dois saves. **3a**: host viaja nativo, convidado é re-invocado. **3b**: host
viaja nativo, convidado carrega nativo e a mod reconstrói a presença antes do
`TravelRelease`. Régua de 3b: **uma** geração por peer, movimento e ação nos
dois sentidos depois da reconstrução (tráfego no canal não conta — prova
comunicação da mod, não replicação), contadores inalterados, saída legal.

## 6. Riscos e o que detecta cada um

- host encerra a própria sessão no warp → a leitura da fase 1; em teste,
  `ctrl+0x150` indo a `0x12`/`0x13` e `0x15` no par
- watchdog de silêncio derruba o convidado em carregamento → cronometrar do
  warp do convidado até o `Leave*` no servidor
- watchdog de 300 s → detour de leitura gravando o delta contra 300.0
- convidado reaparece no próprio mundo → mundo do host não aplicado, o outro
  não visível
- save do convidado contaminado → nunca chamar `FUN_14044fe30` no convidado
- penalidade: qualquer queda na fase 3 custa 10 pontos, e Chico está em 70
- duplicação de presença ao recriar → `FUN_14051ce20` sobrescreve `E+0x40` sem
  destruir o antigo

## 7. O que é leitura e o que precisa medição

**Lido estaticamente:** a cadeia da viagem e os campos do pedido; o portão de
motivo; os quatro componentes do mundo emprestado; o case 2 de `FUN_1402bd0d0`;
os dois watchdogs; que `FUN_14044fe30` grava o registro do próprio jogador; os
prólogos acima.

**Precisa medição, e não pode ser apresentado como fato:** o valor de `+0x30`
numa sessão viva (previsão 7, mas é previsão); se suprimir o case 2 deixa a
máquina do host consistente, já que ela também escreve `+0x1b8 |= 2`; quem no
host conta o silêncio de ~23 s e se um carregamento cabe dentro dele; se o
convidado consegue carregar preservando o join ctrl; e a tela de carregamento
dentro de um warp real.
