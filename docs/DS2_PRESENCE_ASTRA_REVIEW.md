# Revisão do gpt-6-astra sobre a reconstrução de presença

Consulta feita em 16/09, com `codex exec` em sandbox somente leitura sobre este
repositório, modelo `gpt-6-astra`, esforço `low`. Thread para retomar:
`codex resume 01a0abfe-7d86-7d71-8888-1a40a0b85a6d`.

É a terceira opinião da cadeia: o parecer externo
(`DS2_SEAMLESS_TRAVEL_ARCHITECTURE.md`) propôs reconstruir a presença; um
agente Fable conferiu contra o binário e achou a premissa central errada
(`DS2_PRESENCE_REBUILD_PLAN.md`); esta revisão confere o Fable e desenha os
caminhos de implementação.

**O que ela mudou, em uma linha:** os 13 prólogos do Fable conferem todos, mas
a nossa falha restante estava atribuída à função errada — ver a seção "A
segunda dependência nunca esteve sob guarda" em `DS2_SEAMLESS_COOP_TASKS.md`.

O relatório abaixo é a saída literal do modelo, sem edição.

---

## 1. Verificação

**Recomendo antecipar o teste de travessia com as cópias ausentes e manter a reconstrução híbrida como primeira opção, condicionada ao resultado.** A análise encontrou, porém, três divergências relevantes entre documentação e implementação: o endereço da falha restante pertence a outra função, a chegada ainda não exige diretamente o contato físico, e faltam contadores publicados para executar o portão proposto com rigor.

Nenhum arquivo foi criado ou editado. Não executei o harness, jogos, servidor ou Steam; não acessei rede. `git status --porcelain=v1` estava vazio no início e permaneceu vazio no fim, na branch `feature/seamless-coop`.

### Conferência dos prólogos

Usei `x86_64-w64-mingw32-objdump -d`, com intervalos delimitados por `--start-address` e `--stop-address`, sobre o executável indicado. **Todos os 13 prólogos da tabela conferem integralmente**, incluindo os bytes das instruções que o objdump apresenta em linhas de continuação.

| Offset relativo ao módulo | Bytes conferidos | Resultado |
|---|---:|---|
| `+0x51c820` | 12 | Iguais |
| `+0x51b0e0` | 15 | Iguais |
| `+0x51ce20` | 5 | Iguais |
| `+0x51d2a0` | 13 | Iguais |
| `+0x51c4d0` | 15 | Iguais |
| `+0x51bff0` | 15 | Iguais |
| `+0x51c940` | 16 | Iguais |
| `+0x51dbb0` | 15 | Iguais |
| `+0x520810` | 15 | Iguais |
| `+0x2be090` | 16 | Iguais |
| `+0x2c2820` | 15 | Iguais |
| `+0x359890` | 12 | Iguais |
| `+0x513340` | 16 | Iguais |

O arquivo tem **28.200.992 bytes** e `ImageBase = 0x140000000`.

Uma ressalva factual: o cabeçalho PE contém `DYNAMIC_BASE` e `HIGH_ENTROPY_VA`. Portanto, **“sem ASLR” não é uma conclusão sustentada pelo cabeçalho**; pode descrever o comportamento medido nesse ambiente. Os hooks devem continuar resolvendo endereços pela base efetivamente carregada.

A coincidência dos prólogos confirma os pontos de entrada documentados, mas não certifica ABI, contexto de chamada ou segurança concorrente. Em particular, os cinco bytes de `+0x51ce20` são uma assinatura bastante curta para uma futura verificação de versão.

### O que conferi além dos prólogos

Li os documentos na ordem pedida, as partes pertinentes do histórico e os trechos de implementação dos hooks. Também consultei decompilações **já existentes** em `/tmp/ds2-presence-review/`, sem executar novamente o Ghidra, e confrontei os pontos decisivos com disassembly novo do executável.

**Confirmações importantes:**

- `FUN_14051ce20` procura uma entrada existente e grava o resultado da criação diretamente em `E+0x40`. A instrução em `0x14051d006` confirma a sobrescrita; esse caminho não retira previamente o personagem antigo.
- O construtor copia **`blob+0x22e → E+0x6a`**, estabelecendo o net id da presença.
- `FUN_14051ac20` procura uma entrada por net id, exige estado `2` e identidade válida.
- `FUN_14051b3c0` resolve net id para o `PlayerCtrl`; `FUN_14051a9c0` também resolve a entrada e usa `FUN_14051d360` para produzir uma descrição contendo seu personagem.
- `FUN_14051d2a0` chama `FUN_140359890`, zera `E+0x40`, decrementa `R+8` e libera a entrada.
- `FUN_14035b9c0` processa a lista adiada em `CharacterManager+0x30..+0x38`, incluindo etapas anteriores à liberação final. Isso confirma que a entrada livre é uma evidência insuficiente de destruição.
- O watchdog consulta o bit `0x10` em `sessão+0x1b8` e compara `sessão+8 − sessão+0x1b4` com `DAT_1410d7b40`. Os bytes dessa constante são `00 00 96 43`: **300,0 em float**.

**Correção importante sobre a falha restante:** `0x1403f4f2b` pertence a **`FUN_1403f4f10`**, não a `FUN_1403f4f60`. Nesse caminho:

1. O argumento aponta para o componente.
2. O jogo carrega `componente+0x40`.
3. Em `+0x3f4f2b`, lê a vftable desse objeto.
4. Depois chama seu slot virtual `+0x08`.

Isso localiza melhor o consumidor do ponteiro inválido. Ainda não identifica quem liberou o objeto nem se o componente pertence a um personagem remoto, local ou a outra entidade.

### Divergências no código atual

- **Chegada física:** em [ContinueSettle](/home/suel/projects/ds2-jmj/Source/Injector/Hooks/DarkSouls2/DS2_DeathInterceptHook.cpp:1780), `Arrived` depende de `CurrentMap() == destino`. [CurrentMap](/home/suel/projects/ds2-jmj/Source/Injector/Hooks/DarkSouls2/DS2_DeathInterceptHook.cpp:701) percorre o streamer até a parte onde o jogador esteve. O contato físico é lido para retenção e diagnóstico, mas **não participa daquela decisão**. O contrato escrito é mais forte que a implementação.
- **Contadores:** os acumuladores existem, mas `Backread::WriteStatus` não publica `s_caught_update`; `Death::status` não publica `s_bodies_dropped`; a interface de `TravelWatch` não oferece um snapshot completo dos seus contadores. Contar linhas limitadas do log não substitui isso.
- **Congelamento:** `HoldCopies` aparece no documento, mas não encontrei sua declaração ou implementação em `Source`.
- **Barreira:** já distingue `Arrived` de `Failed`, mas ainda libera pelo resultado do transporte, sem uma etapa de reconstrução. O código também registra a limitação de uma única caixa de recebimento por tipo de evento, que pode perder recibos com três ou mais jogadores.

**Não conferi integralmente:** todos os campos do blob, todos os consumidores da replicação, os vetos do `0xd` nas duas máquinas de sessão, nem todos os efeitos transitivos da retirada. Não houve medição ao vivo nesta consulta.

## 2. As opções

### Opção A — Retirada nativa, transporte atual e reconstrução pelo registro

**O que é:** implementar o híbrido do plano, com uma barreira antes da viagem e outra antes de devolver o controle.

A sequência seria:

**Preparar → retirar em todas as máquinas → confirmar destruição → host viaja → convidados viajam → reconstruir em todas → confirmar replicação → `TravelRelease`.**

As funções centrais são:

| Responsabilidade | Funções/estruturas |
|---|---|
| Retirada individual | `FUN_14051c820(E)` |
| Progressão da retirada | `FUN_14051c940` → `FUN_14051d2a0` |
| Retirada do CharacterManager | `FUN_140359890`; fila processada por `FUN_14035b9c0` |
| Registro pendente | `FUN_14051b0e0(R, membro, blob, flag)` |
| Materialização | `FUN_14051dbb0` → `FUN_14051ce20` |
| Liberação específica do papel `0xe` | `FUN_14051c4d0` |
| Resolução da identidade | `FUN_140520040`, `E+0x6a`, `FUN_14051ac20`, `FUN_14051b3c0` |

**Preferiria deixar `FUN_14051c940` executar normalmente o processamento dos pendentes**, após a mod registrar o pedido no contexto correto. Chamar `FUN_14051dbb0` diretamente acrescenta risco de reentrância e de executar a criação numa fase inadequada do quadro.

Há duas formas concretas de fornecer os dados:

#### A1. Blob capturado na entrada, atualizado apenas onde houver contrato conhecido

É o menor protótipo. Capturar o blob recebido por `FUN_14051b0e0`, guardar a identidade e a flag separadamente, e reutilizar a representação de criação.

Para uma reconstrução no mesmo mapa, isso reduz bastante o trabalho. Para viajar, **reproduzir o blob intacto é insuficiente como projeto definitivo**: ele contém posição e estado do personagem no instante da entrada. Pode recriar na origem, com equipamento ou HP antigos.

**Custo:** médio para demonstrar retirar/recriar; médio a alto para transformar em viagem robusta.

**Risco principal:** snapshot obsoleto, replicação que não retoma, destruição ainda pendente e duplicação por repetição do registro.

**Sinal positivo:** uma nova geração de personagem por peer, resolvida pelo mesmo net id, recebendo movimento e ações nativas atuais nos dois sentidos.

#### A2. Snapshot novo produzido pelo dono a cada viagem

Manter a captura inicial como referência, mas produzir dados atuais para reconstrução. Há um caminho concreto para investigar: **`FUN_14051ba70` e `FUN_14051d5a0`**, que exportam dados do personagem; a primeira também trata identidade, papel e net id.

O dono produziria o snapshot em contexto seguro do jogo, e o canal 7 transportaria os dados associados à transação. O receptor chamaria o registro com um membro obtido da lista atual.

**Isso evita depender de reenviar o pacote nativo `0xd` ou reabrir seu portão de sessão.** Não autoriza, porém, chamar esses exportadores arbitrariamente: suas pré-condições e o buffer completo ainda precisam ser conferidos.

**Custo:** maior que A1; exige exportação validada, transporte do snapshot, tratamento de versão, tamanho, repetição e validade temporal.

**Risco principal:** exportação incompleta ou incoerente; identidade correta associada a dados velhos; tratar uma estrutura interna como formato de rede sem validar sua representação.

**Sinal positivo:** mudar equipamento/estado antes da viagem, reconstruir com esse estado atual e observar a replicação posterior funcionando. A cópia inicial serve para comparar os campos, não como autorização para restaurar estado antigo.

**Minha avaliação:** A1 é o melhor protótipo; A2 é uma candidata melhor para uso contínuo. Nenhuma delas resolve automaticamente uma dependência pertencente exclusivamente ao mapa ou ao jogador local.

### Opção B — Corrigir a segunda dependência pelo ciclo de vida do modelo

**O que é:** identificar o proprietário de `componente+0x40` na falha de `FUN_1403f4f10` e corrigir sua retirada/recriação pelo mecanismo proprietário.

Há portas nativas concretas para estudar:

- `FUN_1403f4f10`: consumidor exato da falha informada.
- `FUN_1403f4f60`: outro caminho de atualização do modelo.
- `FUN_1403f6300`: soltura do recurso do componente.
- `FUN_1403f4c20`: reação a mudança de backread, com desligamento e chamadas virtuais de soltura/carregamento.
- `FUN_1403f4ce0`: desligamento de registro/proxy.
- `FUN_1403f4410` e `FUN_1403f4500`: construção e destruição do componente.

As últimas relações vêm das decompilações existentes; **não representam uma API de recarga já validada**.

Essa opção tem duas implementações possíveis: invalidar corretamente a dependência quando seu recurso morre, ou executar o ciclo nativo de desligar/recarregar o modelo enquanto o componente proprietário permanece vivo.

**Custo:** baixo se aparecer um vínculo único com uma operação nativa segura; alto se for necessário descobrir dependências espalhadas.

**Risco:** transformar um ponteiro inválido em vazamento, modelo permanentemente ausente ou atualização incompleta. **Zerar `+0x40` por analogia com `DropDeadRigidBody` não está justificado.** O teste de nulo desse consumidor não prova o contrato dos demais.

**Sinal positivo:** observar o recurso antigo desligado e destruído, o novo ligado ao componente correto e ambos os caminhos de atualização executando normalmente após o descarregamento da origem.

**Minha avaliação:** continua viável porque esta é a segunda dependência identificada. Eu daria à investigação uma pergunta delimitada: *quem é o dono do componente e qual operação nativa troca o recurso?* Não abriria outra sequência de guardas pontuais. Uma terceira dependência independente aciona a regra de reconstrução.

### Opção C — Congelar as cópias mantendo vivos seus recursos

**O que é:** conservar o `PlayerCtrl` e sua identidade de rede, impedir temporariamente sua travessia e manter as partes da origem necessárias à cópia. Depois, reassentar/religar a cópia ao destino antes de liberar a origem.

É diferente de simplesmente ocultar o personagem:

| Intervenção | O que resolve | O que permanece |
|---|---|---|
| Ocultar desenho | Aparência durante a transição | Física, animação, tarefas e referências antigas |
| Pular apenas pré-desenho | Um consumidor | Outros consumidores e a liberação dos recursos |
| Congelar posição | Movimento da cópia | Recursos podem morrer sob ela |
| Suspender consumidores e reter recursos | Pode preservar a vida do conjunto | Ainda precisa resolver a transferência ao destino |

As peças existentes incluem `DS2_Backread::KeepIndex`, o transporte e os pontos de tarefa observados pelo `TravelWatch`. A suspensão precisaria acontecer num ponto seguro de agendamento; não bastaria abandonar tarefas sem cumprir sua conclusão.

**Custo:** baixo para um ensaio limitado; médio a alto para garantir segurança em todos os consumidores.

**Risco:** apenas adiar o defeito até descongelar ou expirar o `keep`. Também pode reter progressivamente mapas e recursos.

**Sinal positivo:** depois de descongelar, a cópia retoma replicação e execução normal, e a origem é efetivamente desmontada sem referência sobrevivente aos recursos antigos.

**Minha avaliação:** é mais barata como experimento, **não está demonstrado que seja mais barata como solução**. A documentação já mostra por que “segurar por mais tempo” não equivale a estabelecer propriedade correta. Não existe ainda prova de um estado nativo em que a cópia viva deixe de tocar qualquer mapa.

### Opção D — Carregamento nativo coordenado, seguido de reconstrução híbrida

**O que é:** substituir backread forçado e teleporte pelo carregamento nativo, mas continuar reconstruindo as presenças pela mod. Não retomar como premissa a reentrada pura já bloqueada pelos estados da sessão.

Peças relevantes:

- Host: `FUN_1401843b0`, `FUN_140184830`, `FUN_14044fe30`.
- Convidado: construção do pedido que preserve destino e mundo do host, tomando `FUN_1402c2a80` como referência.
- Reset de presenças: `FUN_140513340` → `FUN_14051bff0`.
- Reconstrução: as mesmas primitivas da opção A.
- Watchdog: `FUN_1402be090`.
- Preservação do retorno original do convidado e do estado de mundo emprestado.

**Custo:** alto; é troca do transporte e revalidação do mundo, não apenas troca de uma chamada.

**Risco:** convidado reaparecer no próprio mundo, instantâneo errado, perda da ligação de replicação, timeout e alteração indevida do registro de retorno.

Eu **não neutralizaria globalmente o watchdog**. Primeiro observaria seu armamento; depois definiria um tratamento limitado à transação de viagem, com prazo próprio e recuperação. O fim da transação precisa restaurar o comportamento normal.

**Sinal positivo:** ambos materializam o destino no mundo do host, replicam ações, preservam personagem/save e, na saída legal, o convidado recupera seu mundo original. Deve passar também com viagem iniciada após mais de 300 segundos de sessão.

**Minha avaliação:** escalada apropriada se o transporte atual continuar produzindo falhas após retirada e destruição comprovadas das cópias.

## 3. Recomendação

**Adotar a opção A, mas investir primeiro no experimento capaz de refutá-la.** A ordem que proponho é:

1. **Fechar a observabilidade mínima.** Publicar contadores completos, identificar gerações dos personagens e observar a destruição adiada. Registrar corretamente a falha de `FUN_1403f4f10`. A prova de chegada do experimento deve incluir contato atual, destino e avanço da física.
2. **Observar uma entrada e uma saída normais em Heide.** Isso calibra os sinais de registro, materialização, retirada e destruição. Instrumentação passiva reduz o risco, mas não é literalmente “sem custo”: um detour incorreto também pode derrubar o cliente.
3. **Executar o portão causal antes de implementar reconstrução.** Com fixtures consistentes das duas contas, retirar as cópias nos dois lados, confirmar destruição e realizar um trecho pelo transporte atual, host primeiro.
4. **Manter as cópias ausentes até o descarregamento efetivo da origem.** Passar de 30 segundos é necessário, mas o cronômetro sozinho não prova que os recursos foram desmontados.
5. **Se houver falha relevante nesse intervalo, parar a implementação da opção A como solução suficiente.** Investigar a propriedade do componente e escolher B ou D conforme a evidência. A retirada pode continuar sendo parte da solução, mas perdeu a justificativa de resolver sozinha.
6. **Se o portão não refutar, testar reconstrução no mesmo mapa.** Primeiro um lado, depois o outro, depois ambos. Aqui entram o ensaio de ausência prolongada, o retorno da replicação e o controle de duplicação.
7. **Integrar preparação e reconstrução à barreira.** A reconstrução deve ocorrer **antes** de `TravelRelease`, com recibos próprios. Não fazer a mensagem que libera o controle também iniciar a criação.
8. **Validar sequência curta e depois pelo menos 40 trechos reais**, com intervalos que exponham a desmontagem da origem, inversão de host, votação completa, movimento/ações bilaterais e saída legal final.

Durante ausência deliberada, `R+8` pode chegar a zero e o portão nativo de sync ficar falso. Portanto, os critérios devem distinguir **sessão preservada durante a ausência** de **replicação funcional depois da reconstrução**. Tráfego no canal 7 prova comunicação da mod, não movimento dos personagens.

Um trecho com sucesso permite continuar a investigação; não aprova a arquitetura contra uma falha intermitente.

## 4. Respostas diretas

### 1. Os bytes conferem?

**Sim: todos os 13 prólogos da tabela conferem integralmente.**

Também confirmei a sobrescrita de `E+0x40`, o transporte do net id, parte importante da resolução identidade→personagem, a fila adiada e a constante do watchdog.

A principal correção é o sítio da queda: **`+0x3f4f2b` está em `FUN_1403f4f10`**, lendo o objeto de `componente+0x40`.

Não certifiquei toda a semântica do plano apenas por esses bytes.

### 2. A ordem das fases está certa? Qual é o menor experimento?

**Concordo com antecipar o trecho com cópias ausentes, depois da instrumentação necessária para provar que realmente foram destruídas.** Não exigiria implementar a recriação para fazer essa pergunta.

O menor ensaio interpretável tem:

1. Controle com cópias presentes, na mesma rota e condições, para estabelecer exposição ao defeito.
2. Ensaio em estado inicial equivalente, retirando ambas as cópias ainda na origem.
3. Recibo positivo de destruição de cada geração antiga.
4. Travessia serial dos jogadores locais.
5. Confirmação física de chegada e desmontagem efetiva da origem.
6. Snapshots dos contadores antes da retirada, depois da retirada, depois da chegada e depois da desmontagem.

Se o controle não reproduzir a falha, uma passagem limpa sem cópias tem pouco poder discriminante. Se a falha ocorrer **depois da destruição comprovada**, a retirada isolada não basta.

Duas cautelas causais:

- Uma falha durante a retirada precisa ser classificada separadamente da falha durante a travessia.
- Falhar sem cópias **não prova que o jogador local seja a causa**. Podem existir resíduos da presença anterior, recursos compartilhados, tarefas pendentes ou um defeito do streamer.

A limpeza desse ensaio precisa estar prevista para o caso de a saída legal não funcionar sem presenças. O experimento não deve depender de uma recriação ainda inexistente para proteger os saves.

### 3. Como capturar o blob e fazer a cópia voltar a se mexer?

**Capturaria no ingresso de `FUN_14051b0e0`, antes de chamar o original.**

Nesse ponto estão disponíveis os quatro elementos úteis: registro, membro, blob e flag. O procedimento seria:

- Copiar imediatamente os `0x5f0` bytes para memória pertencente à mod.
- Registrar identidade normalizada, geração da sessão, lado, flag, tamanho e origem da chamada.
- Após o original, confirmar o slot pendente efetivamente preenchido.
- Na criação, correlacionar slot, entrada ativa e nova geração de `PlayerCtrl`.
- Invalidar o cache ao sair/reentrar, trocar de peer ou mudar a geração da sessão.
- Resolver novamente o membro pela lista atual antes de registrar. **Não guardar uma cópia bruta do wrapper de 0x40 bytes como membro reutilizável.**

O jogo usa `FUN_14051b050` para copiar o payload ao slot. Comparações devem respeitar os campos que esse caminho copia; padding não deve virar uma falsa divergência.

Campos concretos relevantes, relativos ao **blob**, não ao slot:

| Campo | Uso observado estaticamente |
|---|---|
| `+0x00..+0x3f` | Dados de transformação; posição inicial lida de `+0x30..+0x3f` |
| `+0x40` | Papel usado na criação |
| `+0x5c` | Dados entregues à inicialização por `FUN_140338a50` |
| `+0x18c` | Bloco entregue a um subsistema do personagem |
| `+0x22e` | Net id copiado para `E+0x6a` |
| `+0x230` em diante | Seleções e dados de equipamento consumidos na criação |
| `+0x298` | Valor usado para inicializar HP, limitado pelos valores do personagem criado |
| `+0x29c` | Nome |
| `+0x2e0`, contador em `+0x5e0` | Bloco aplicado por `FUN_140228dc0` |

Isso **não é uma especificação completa do blob**.

A ligação que deve ser preservada é:

**membro atual ↔ entrada `E` ↔ net id em `E+0x6a` ↔ novo `PlayerCtrl` em `E+0x40`.**

O binário confirma os resolvedores dessa associação. A criação também restaura estado da entrada, incrementa contadores e chama `FUN_1405206a0`, que marca o registro correspondente do membro. Por isso, chamar só o construtor de personagem seria insuficiente.

**Não fechei a cadeia inteira do pacote de posição até sua aplicação ao novo personagem.** Ter o net id correto é necessário, mas não demonstra que buffers, portões de sync e estado de recepção retomaram corretamente.

O teste positivo deve mostrar uma sequência distinta — andar, parar, mudar de direção e executar uma ação — chegando à nova geração da cópia nos dois sentidos. Uma cópia visível ou posicionada no destino não basta.

Também evitaria reaplicar indiscriminadamente pacotes acumulados durante a ausência. É preciso distinguir estado substituível de eventos, manter o transporte ativo e estabelecer como descartar estado anterior à reconstrução sem quebrar o protocolo nativo.

### 4. Como evitar duplicação e confirmar destruição?

Usaria uma máquina de estados da mod por identidade e geração, sem adulterar os estados nativos de entrada.

A sequência correta é:

1. **Reservar a operação.** Impedir que duas solicitações da mesma transação registrem o mesmo peer.
2. **Capturar a geração antiga.** Registrar `E`, `PlayerCtrl`, identidade e net id enquanto estão válidos.
3. **Solicitar retirada por `FUN_14051c820`.**
4. **Deixar o jogo progredir.** Observar estado `3`, passagem por `FUN_14051d2a0` e entrada no caminho do `CharacterManager`.
5. **Confirmar retirada dos índices e conclusão do ciclo adiado.** A fila `+0x30..+0x38` é processada por `FUN_14035b9c0`; o caminho envolve `FUN_14035b920`, `FUN_14035b380` e liberação de referências.
6. **Confirmar destruição da geração antiga.** Na vftable de `PlayerCtrl` em `0x1410e4bb8`, conferi os alvos `FUN_14037ec60` no slot `0` e `FUN_14037f240` no slot `+0x10`. São portas concretas para observar destruição e limpeza.
7. **Revalidar membro, sessão e ausência de registro concorrente.**
8. **Registrar uma única vez**, deixar materializar e conferir cardinalidade e vínculo de rede.

A confirmação forte combina eventos positivos de retirada, limpeza/destruição e um ponto seguro após o processamento das tarefas. Se o objetivo incluir provar recuperação da memória, acrescentar recibo da liberação pelo alocador, filtrado pelos objetos acompanhados.

**Não usar como prova isolada:** `E.estado=0`, `E+0x40=0`, memória ilegível, endereço diferente ou esperar alguns frames.

O alocador pode devolver o mesmo endereço para o novo personagem. Portanto, a identidade de acompanhamento precisa ser **endereço + geração**, e a instrumentação não deve manter uma referência forte que impeça justamente a destruição que tenta medir.

Finalmente, `FUN_14051b0e0` retornar sucesso significa **pedido pendente aceito**. Repeti-lo porque a cópia ainda não apareceu pode criar vários pendentes para o mesmo jogador.

### 5. Existe alternativa mais barata?

**Sim: corrigir o ciclo de vida do recurso de `componente+0x40` pode ser mais barato**, se a investigação encontrar uma operação nativa de desligamento e recarga com escopo definido. Esse alvo agora é mais preciso que “pré-desenho do mapa”.

Além disso, `FUN_1403f4c20` sugere um caminho nativo de reagir ao backread e trocar o recurso sem reconstruir a presença inteira. É uma pista concreta, não uma solução demonstrada.

Ocultar desenho é barato, mas não resolve propriedade. Congelar atualização pode ser um bom controle experimental, desde que inclua tarefas pendentes e retenção dos recursos. Sem uma operação segura de retomada, apenas transfere o momento da falha.

Não recomendaria:

- Chamar `FUN_1403f6300` isoladamente e presumir que tudo ficou desligado.
- Zerar campos por semelhança com o conserto do Havok.
- Reabrir genericamente a aceitação do `0xd` em sessão estabelecida: isso não resolve duplicação, destruição adiada nem os demais efeitos da entrada.
- Manter mapas antigos indefinidamente como solução final.

### 6. Qual é a instrumentação mínima da fase A?

Separaria o núcleo necessário ao primeiro portão dos hooks necessários para explicar reconstrução e replicação.

| Pergunta | Porta de observação | Sinal positivo |
|---|---|---|
| Quais dados chegaram? | `FUN_14051b0e0`, entrada/saída | Blob capturado e slot pendente associado ao membro |
| Quem nasceu? | `FUN_14051ce20`, entrada/saída | Nova geração em `E+0x40`, estado `2`, net id correto e inclusão no manager |
| Quem começou a sair? | `FUN_14051c820` | Transição da geração acompanhada para estado `3` |
| Quem saiu do registro? | `FUN_14051d2a0` | Ponteiro retirado e contagem ajustada |
| Quem foi enfileirado? | `FUN_140359890` | Geração antiga aceita no caminho de retirada |
| Quem concluiu a destruição adiada? | `FUN_14035b9c0` e portas de limpeza/destruição | Processamento da geração antiga, desligamento dos índices e destrutor concluído |
| Houve reset global? | `FUN_14051bff0`; chamador registrado | Reset observado e motivo distinguido da retirada individual |
| O novo objeto está resolvível? | `FUN_14051b3c0` ou `FUN_14051a9c0` | Consulta pelo net id retorna a nova geração |
| A replicação retomou? | Aplicador de movimento ainda a identificar + amostras do personagem | Movimento/ação atual do dono aplicado à cópia |
| Quem envia o `0xd`? | `FUN_140520810`, filtrado pelo tipo | Envio observado com chamador, thread, tamanho e contexto |
| Quem possui o modelo que falha? | `FUN_1403f4f10` e instrumentação existente | Associação componente→entidade→geração, capturada enquanto válida |

Para o **primeiro portão**, captura, retirada, fila/destruição e contadores são suficientes; descobrir o emissor do `0xd` não precisa bloquear esse ensaio.

Para manter o hook pequeno e pouco invasivo:

- Registrar eventos binários em buffer limitado; formatar fora dos caminhos quentes.
- Incluir boot, sessão, viagem, peer, geração, thread e sequência.
- Publicar snapshots coerentes das cinco entradas, cinco slots e personagens acompanhados.
- Contar perdas de eventos: buffer saturado torna a prova incompleta.
- Usar os observadores de tarefas/mapas existentes, acrescentando correlação, sem outro executor.
- Não persistir ponteiros de objetos do jogo para dereferenciar posteriormente numa thread de log.

Os contadores devem cobrir pelo menos exceções de mapa, tarefas, modelo/pós-física, soltura/desregistro, nós pulados e corpos Havok descartados. **Contador inalterado é uma condição necessária; o sinal positivo de sucesso vem dos eventos de ciclo de vida, chegada, descarregamento e replicação.**

## 5. O que eu não sei

### O que foi verificado estaticamente nesta consulta

- Os 13 prólogos, tamanho do executável e cabeçalho PE.
- O consumidor exato de `+0x3f4f2b`.
- A sobrescrita de `E+0x40`.
- A transferência do net id e os resolvedores citados.
- A existência e o processamento da fila adiada.
- A comparação do watchdog com 300 segundos.
- As divergências entre contrato documentado, chegada implementada e contadores publicados.

### O que é medição anterior relatada pelo projeto

- Os 40 trechos, a diferença de 16–18 ms entre cortinas e os acionamentos de guardas.
- A queda de 39 exceções para zero no host após `DropDeadRigidBody`.
- O reaproveitamento do bloco Havok observado pela vigia.
- As limitações dos watchpoints de hardware e os resultados das tentativas de reentrada.

Não reproduzi essas medições nem auditei todos os seus artefatos.

### O que continua hipótese ou trabalho pendente

- Se retirar as cópias elimina a segunda dependência.
- Quem possui e quem libera o recurso acessado em `FUN_1403f4f10`.
- Se a retirada permanece neutra para a sessão em todas as condições relevantes.
- Se o blob capturado basta para uma reconstrução funcional numa sessão estabelecida.
- O aplicador final de posição, o tratamento de mensagens durante a ausência e a retomada do sync.
- A segurança de usar os exportadores para snapshots novos.
- A possibilidade de recarregar apenas o modelo.
- A segurança concorrente do ponto escolhido para retirar e registrar.
- O comportamento com três ou mais jogadores.

**O investimento inicial mais justificável é provar destruição completa e atravessar sem as cópias.** Esse teste responde à ressalva causal antes de o projeto assumir o custo da reconstrução.