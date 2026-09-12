# Seamless Co-op no DS2: o desenho

Este documento é o **enunciado**, não o relatório. Ele diz o que o mod deve
ser; o que já foi medido do jogo está em
[DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md), e o que falta construir, em
ordem, está em [DS2_SEAMLESS_COOP_TASKS.md](DS2_SEAMLESS_COOP_TASKS.md).

As decisões abaixo são do dono do projeto, registradas em 12/09. Onde eu
acrescentei alguma coisa, está marcado como **nota de engenharia** — isso é
observação minha sobre custo ou risco, e não muda a decisão.

## O princípio

> **O host define o estado do mundo da sessão, mas cada jogador continua com
> seu próprio personagem e seu próprio save.**

Não existe save compartilhado. O convidado entra no mundo do host com o
personagem dele — mesmas armas, magias, atributos, itens, almas, nível, Soul
Memory — e o que muda é só **em qual mundo ele está jogando naquele momento**.

A diferença para o multiplayer original é essa: o segundo jogador deixa de ser
um fantasma temporário e passa a ser um personagem persistente participando da
mesma campanha.

    antes:  summon → área → boss → fantasma some → efígie → novo summon
    depois: entra uma vez → Forest → Bastille → Iron Keep → ... → Nashandra

## O que é do host e o que é de cada jogador

| pertence ao **mundo do host** | pertence a **cada jogador** |
| --- | --- |
| bosses já mortos antes da sessão | personagem, nível, atributos |
| portas, alavancas, elevadores, atalhos | inventário, armas, anéis, magias |
| illusory walls, mecanismos de Pharros | almas e bloodstain |
| estado de NPC e quests | hollowing e forma humana |
| intensidade de fogueira (Bonfire Ascetic) | covenant |
| despawn de inimigos | Soul Memory |
| Company of Champions (dificuldade) | loot recolhido |

## Progresso: o que atravessa para o save de quem entrou

**O progresso anterior do host não é copiado.** Entrar no mundo de alguém mais
avançado não pode completar retroativamente o jogo de quem entrou.

    A: Pursuer morto        B entra no mundo de A
    B: Pursuer vivo         → B não encontra o Pursuer na sessão
                            → o save de B continua com o Pursuer vivo
                            → de volta ao mundo dele, o Pursuer está lá

**O que vocês fizerem juntos é salvo para todos.** Se os dois matam a Lost
Sinner na sessão, os dois saves ficam com a Lost Sinner morta, e ela continua
morta quando o convidado volta para o próprio mundo. É isso que permite fazer
a campanha inteira sem repetir cada chefe no mundo de cada um.

O mesmo vale para quests feitas juntos. Para estátuas e mecanismos
secundários, sincroniza quando os dois estavam presentes; para quests
importantes, sincroniza.

**Loot é individual.** Um baú com um Estus Flask Shard entrega o item a cada
jogador que o abre. Vale para titanite, armas, anéis, Estus Shards, Sublime
Bone Dust, Fragrant Branches e Pharros Lockstones — sem isso a campanha
cooperativa quebra.

**Almas de boss são de todos.** Cada participante recebe as almas, a alma do
boss e o item. Não existe a recompensa reduzida do multiplayer original.

**Bonfire Ascetic não sincroniza.** A intensidade é do mundo do host. O
convidado joga na intensidade do host durante a sessão, mas o mundo dele não
sobe de intensidade por causa disso.

**Despawn de inimigos segue o host** durante a sessão, e não marca as mortes
no save de quem entrou.

## Morte

Fora de boss:

    jogador morre → perde as almas carregadas → bloodstain no lugar da morte
                  → respawna na última fogueira → continua na sessão
                  → volta andando até o grupo

O host continua jogando normalmente. Não é preciso soapstone, Name-engraved
Ring nem novo summon.

Hollowing continua valendo: morrer reduz o HP máximo até os limites normais do
DS2, o Ring of Binding continua atenuando, e a Human Effigy continua servindo
para restaurar humanidade e HP — **sem desconectar da sessão**. O que a efígie
deixa de fazer é liberar o multiplayer; isso passa a ser sempre permitido.

Em luta de boss:

    jogador morre → modo espectador, assistindo quem ainda está vivo
    todos morrem  → party wipe: o boss volta ao estado inicial,
                    todos reaparecem, a sessão continua
    alguém mata   → vitória vale para todos, inclusive para quem morreu antes

O host morrer **não** encerra a sessão: ele também vira espectador e o
convidado pode terminar a luta.

## Mundo e deslocamento

Descansar numa fogueira **reseta o mundo para todo mundo**: inimigos voltam,
objetos quebráveis voltam, invasões de NPC podem reiniciar, o estado de
combate é limpo. O grupo deve ser avisado antes ("A player is resting at a
bonfire").

Fast travel move o grupo inteiro, com votação:

    Travel to King's Gate?   A ✓  B ✓  C ✓   → party inteira viaja

Portas, alavancas, elevadores, atalhos, illusory walls e mecanismos de Pharros
são sincronizados: o mundo da sessão tem um estado autoritativo. Quem usa a
Pharros Lockstone gasta a própria; ninguém mais perde uma. Se o convidado
voltar ao próprio mundo, o mecanismo lá continua fechado — Pharros conta como
interação de mundo, não como progresso de boss.

## Matchmaking, covenants e invasões

**Soul Memory deixa de limitar quem joga com quem.** Continua existindo no
save, mas não decide conexão: a entrada é por senha.

Covenant é individual e não muda ao entrar numa sessão; as recompensas também
continuam individuais. Se o host estiver em Company of Champions, o mundo
assume a dificuldade dele para todos, sem alterar o covenant de ninguém.

Invasões são opcionais (`allow_invasions`). Com elas ligadas, um Dark Spirit
invade a sessão e enfrenta o grupo — 3v1, ou com balanceamento para mais de um
invasor.

## Ciclo de vida da sessão

    A abre o jogo, cria a sessão
    B entra por senha
    o mundo passa a ser o estado de A
    jogam
    B sai  → volta ao próprio mundo levando tudo que conquistou
    A sai  → a party é desfeita e todos voltam aos próprios mundos

**Não existe migração de host.** Se o host sai, a sessão acaba: o estado de
mundo de outro jogador é diferente, e promover alguém a host produziria
inconsistência. Pelo mesmo motivo, uma campanha deve manter **sempre o mesmo
host** — trocar de host entre sessões faz flags de NPC andarem para trás.

## Notas de engenharia

Estas não mudam o desenho; dizem o que ele custa.

- **O buraco arquitetural é a sessão, não o warp.** Hoje uma morte desfaz a
  sessão e o jogo nunca carrega área nenhuma para um convidado dentro do mundo
  do host. Quase todo o resto desta lista depende de resolver isso primeiro —
  respawn, espectador, party wipe, fast travel em grupo e reset de fogueira só
  fazem sentido quando a sessão sobrevive.
- **Três jogadores não são testáveis nesta máquina.** São duas contas Steam, e
  a sessão é peer to peer por Steam id. Tudo sobre 3+ jogadores (espectador com
  dois vivos, party wipe de três, 3v1) fica sem verificação local até existir
  uma terceira conta.
- **Save é a segunda metade do problema.** "O que fizemos juntos entra nos dois
  saves" exige que o cliente do convidado escreva no próprio save flags de um
  mundo que não é o dele. Isso não foi investigado ainda, e é provavelmente o
  maior trabalho depois da sessão.
- **Quests de NPC são o item mais caro da lista** e o mais fácil de corromper
  save. Convém deixá-las para o fim, atrás de bosses e de flags de mundo, que
  são mais simples e mais fáceis de verificar.
