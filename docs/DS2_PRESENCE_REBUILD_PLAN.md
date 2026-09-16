# Reconstruir a presença: o que existe, o que não existe, e o plano

Levantamento feito em 16/09 por um agente Fable, em Ghidra `-readOnly` e
`objdump`, sem executar nada nos jogos. Versão 1.03 Calibrations 2.02.

Este documento é a resposta a `DS2_SEAMLESS_TRAVEL_ARCHITECTURE.md`, e corrige
a premissa central dele. Leia os dois juntos.

## O que muda em relação ao parecer

O parecer dizia que **não existe primitiva comprovada para retirar a presença
de um jogador sem sair da sessão**, e chamava isso de primeiro trabalho de
engenharia reversa. Ela **existe**, é nativa e é por jogador.

O que **não** existe é a **reconstrução** pelo caminho nativo fora dos estados
de entrada: o pacote de dados do jogador (`0xd`) só é aceito pelo host em
estado `0xe` e pelo convidado em `0xc`; em `0x10`/`7` a sessão veta. Portanto a
"reentrada coordenada pelo carregamento nativo" **na forma pura não está
disponível** sem refazer o handshake que o M2 bateu nove vezes.

O que está disponível é um **híbrido**: retirada nativa mais reconstrução feita
pela mod, com o blob do jogador capturado na entrada.

Isso inverte o ranking do parecer: **alternativa 2 primeiro** (retirar as cópias
antes da travessia, manter o transporte atual que já fez 40 trechos limpos,
recriar na liberação da barreira), e a alternativa 1 (warps nativos) só como
escalada.

## O registro de presenças remotas

`R = *(*0x141616cf8 + 0x20)`, 0x2500 bytes, construído por `FUN_14051ad90`.

| campo | o que é |
| --- | --- |
| `R+0x08` | quantas entradas ativas vivas; com zero, `FUN_14051d9b0` desliga o sync |
| `R+0x174` | net id deste jogador |
| `R+0x1a8 .. +0x5b8` | 5 **entradas ativas** de 0xd0 bytes |
| `R+0x5c0 .. +0x2500` | 5 **slots pendentes** de 0x640 bytes |

Entrada ativa `E`: `+0x00` membro Steam, `+0x40` o `PlayerCtrl` da cópia,
`+0x48` estado (0 livre, 2 viva, 3 saindo), `+0x4c` papel, `+0x6a` net id,
`+0x78` cronômetro da saída, `+0x8c` nome.

Slot pendente `S`: `+0x00` membro Steam, `+0x40` o **blob de 0x5f0 bytes** do
jogador, `+0x630` flag, `+0x631` liberado para nascer.

## As primitivas

| passo | função | pré-condição que importa |
| --- | --- | --- |
| registrar | `FUN_14051b0e0(R, membro, blob, flag)` | o `membro` tem que vir da lista **viva** (`FUN_140520040`), não de uma cópia de 0x40 bytes |
| liberar | `FUN_14051c4d0(R, steamid)` | só o papel `0xe` precisa disso para nascer |
| nascer | `FUN_14051dbb0(R)` → `FUN_14051ce20(R, S)` | **se já existe entrada ativa para aquele id, sobrescreve `E+0x40` sem destruir o antigo**: é o modo de falha padrão, duplicação e órfão no `CharacterManager` |
| **retirar** | **`FUN_14051c820(E)`** | inicia o fade de 0.5 s e põe `E+0x48 = 3`; não toca sessão nem manda nada ao servidor |
| destruir | `FUN_14051c940` estado 3 → `FUN_14051d2a0(R, E)` | chama `FUN_140359890` no `CharacterManager`; a destruição real é **adiada** por uma lista, então "entrada em estado 0" não é "personagem destruído" |
| reset total | `FUN_140513340` → `FUN_14051bff0(R)` | é o que o **warp** chama: destrói todas as presenças sem encerrar sessão nenhuma |

Prólogos para os bytes esperados:

    +0x51c820  40 53 48 83 ec 20 8b 41 48 48 8b d9
    +0x51b0e0  48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18
    +0x51ce20  40 55 53 56 57
    +0x51d2a0  48 89 6c 24 20 57 48 83 ec 20 48 8b e9
    +0x51c4d0  48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20
    +0x51bff0  48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 60
    +0x51c940  40 53 48 81 ec 80 00 00 00 48 8b 1d a8 a3 0f 01
    +0x51dbb0  48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18
    +0x520810  48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18
    +0x2be090  40 53 48 83 ec 30 f6 81 b8 01 00 00 20 48 8b d9
    +0x2c2820  48 89 5c 24 18 48 89 7c 24 20 55 48 8d 6c 24 a9
    +0x359890  48 85 d2 0f 84 43 02 00 00 55 41 56
    +0x513340  40 53 48 83 ec 20 48 8b 05 ab 39 10 01 48 8b d9

## Por que a retirada parece neutra para a sessão

Nenhuma das duas máquinas de sessão lê o registro: o handler do estado 7 do
convidado (`FUN_1402c3830`) só olha `+0x120` e `+0x1cc`; o do `0x10` do host
(`FUN_1402bed10`) descreve a entrada uma vez e tenta de novo no quadro seguinte
se ela sumiu. **Isto é leitura, não medição** — o experimento abaixo é que
decide.

## Riscos nomeados

- **Watchdog de 300 s no host** (`FUN_1402be090`, `DAT_1410d7b40 = 300.0f`): o
  aviso de warp arma um bit sem renovar o cronômetro, então um warp do host
  muito depois do início pode encerrar a sessão. É a parede provável da
  alternativa 1. Mede-se sem custo pelo caminho legal de hoje.
- **Duplicação ao recriar**, acima.
- **Quem envia o `0xd`** não foi achado estaticamente; mede-se com
  `bp 520810` numa invocação normal.

## O plano, em fases

**A — instrumentar, sem custo.** Um `DS2_PresenceHook` só de observação sobre
registrar, nascer, retirar, destruir e reset, mais `status` das 5 entradas e
dos 5 slots. Rodar uma invocação normal, uma viagem pelo caminho legal e um
`session end`, e ler os sinais positivos de cada passo.

**B — o experimento que decide.** Com baseline dos dois saves: o host retira a
cópia do convidado, segura 60 s (a sessão tem que continuar verificada, sem
`Leave*` no servidor, sem ponto de penalidade), recria, e a cópia tem que
**se mexer quando o dono anda**. Repetir invertido, depois os dois ao mesmo
tempo, e só então um trecho com as cópias ausentes durante a travessia, lendo
os contadores de falha antes e depois.

**C — alternativa 2.** Votação aprovada, cada máquina retira as cópias, viaja
pelo transporte atual, e recria na liberação da barreira, antes de a cortina
descer. Régua: 40 trechos com os dois chegando, cópias recriadas e se movendo,
**contadores de falha inalterados** (qualquer disparo é falha arquitetural),
pontos iguais, sessão verificada o tempo todo, saída legal no fim.

**D — alternativa 1, só se C ainda acusar falhas com as cópias ausentes.**
Warps nativos nos dois lados, com o watchdog neutralizado e o warp do convidado
construído à mão. Sai o transporte inteiro; a barreira e o contrato ficam.

## O que preservar em qualquer caminho

O contrato `Idle/Moving/Arrived/Failed`, com `Arrived` escrito só quando o mapa
alcançado é o de destino e nunca por tempo (a frase "só por contato físico"
estava forte demais: quem decide é o mapa da parte que o streamer registrou sob
o jogador, e o contato entra em diagnóstico e retenção — ver a correção de
16/09 em `DS2_SEAMLESS_COOP_TASKS.md`); a
barreira e a `TravelRelease`; host primeiro; o `DropDeadRigidBody` (que vira um
**medidor**: deve parar de disparar para cópias); todas as guardas `__try` como
rede, com a regra de que disparo é falha arquitetural; as provas de tipo; o
`keep` que soma e nunca encolhe.

## O que refutaria a direção

A sessão cair depois da retirada; a cópia recriada não se mexer; queda dentro
da retirada ou logo depois; duas presenças do mesmo jogador; e o mais
importante para nós: **falha nos guardas mesmo com as cópias ausentes**, que
provaria que o problema é o jogador local carregado entre mapas, e não a cópia.
