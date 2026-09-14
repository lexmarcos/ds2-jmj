# Harness de testes: ds2os-dev

O harness controla duas instalações locais de DS2 e registra o que conseguiu
comprovar. A implementação fica em `Source/LoaderLinux/crates/ds2os-dev/src`.
O contrato JSON descrito aqui é a versão **1**.

## Compilar e preparar

Na raiz do repositório:

```bash
~/.cargo/bin/cargo build --manifest-path Source/LoaderLinux/Cargo.toml -p ds2os-dev
~/.cargo/bin/cargo test --manifest-path Source/LoaderLinux/Cargo.toml -p ds2os-dev
```

Nos exemplos abaixo, `ds2os-dev` significa
`Source/LoaderLinux/target/debug/ds2os-dev`. O CLI usa Steam, Proton, X11,
`curl`, `xwininfo` e `/dev/uinput` conforme as operações executadas.

Configure o Steam ID64 **decimal de 17 dígitos** de cada conta. Os números abaixo
são exemplos; substitua pelos IDs das duas contas usadas no teste:

```bash
ds2os-dev game identity --instance 1 76561198000000001
ds2os-dev game identity --instance 2 76561198000000002
ds2os-dev game character --instance 1 Samuel
ds2os-dev game character --instance 2 Chico
ds2os-dev doctor --json
ds2os-dev up --seamless --auto-rematch --json
```

A associação fica em `~/.local/share/ds2os-dev/config.json`, no campo
`steamIds`. É uma configuração explícita do operador, não uma descoberta da
conta ativa da Steam. `loginusers.vdf` não comprova quem está logado agora.
O harness usa essa associação para selecionar a conexão na API e o diretório
hexadecimal do save. IDs iguais nas duas contas são recusados pelo comando de
configuração e pelos cenários.

`up` exige as duas instalações. Para uma só, use `server up`, `game prepare`,
`pad start`, `game launch --instance 1` e `game enter --instance 1`.
O pad precisa existir **antes** de iniciar o jogo.

`up` reescreve `Injector.config` usando as flags daquela invocação. O injector
lê configuração e DLL no lançamento do processo; mudar o arquivo com o jogo
aberto não atualiza os hooks carregados. Use um relançamento para aplicar uma
nova DLL ou configuração. `reload` serve para alterações do servidor.

## Higiene: logs, resgates, órfãos do Wine e capturas

**Logs dos hooks.** O injector não gira os próprios logs; o do timer chegou a
412 MB. `game prepare` (e portanto `up`) renomeia todo `DS2*.log` acima de
8 MB para `<nome>.1` na instalação cujo jogo está **fechado**, substituindo o
`.1` anterior (uma geração só). Com o jogo aberto nada é movido e o item sai
como `skipped_running`. Cada instalação aparece no evento `logs_rotated`
(`name`, `bytes`, `action`). Um log girado durante a própria execução aparece
no índice de logs como `absent`.

**Cópias de resgate.** `save restore` guarda o save substituído como
`antes-de-<label>-<runId>`, com o id da execução que o fez (o diretório de
evidências). As antigas tinham um carimbo `AAAAMMDD-HHMMSS` no lugar.

```bash
ds2os-dev save prune --keep 5 --dry-run    # o que sairia
ds2os-dev save prune --keep 5              # apaga
```

`prune` apaga, por conta, as cópias de resgate além das `--keep` mais novas
por data de modificação. Só conta como resgate um nome
`conta<N>-antes-de-<label>-` seguido de carimbo ou runId: um label escolhido que
começa com `antes-de-` (como `antes-de-nivel1`) nunca sai. `--pattern` tem de
começar com `antes-de-` (`pattern_not_rescue`). `save list` mostra os labels
escolhidos e resume os resgates por conta; no JSON cada snapshot tem `rescue`.

**Órfãos do Wine.** `up`, quando nenhum `DarkSoulsII.exe` está aberto, encerra
por pid os órfãos que `doctor` lista em `wine_orphans` (SIGTERM, SIGKILL
depois de 3 s, e só conta como encerrado quando o pid não é mais aquele
processo) e registra `orphans_cleared` com as conexões X11 antes e depois.
Órfão é `xalia.exe` num prefixo sem jogo, ou `winedevice.exe` num prefixo
**sem `services.exe`**. Um `winedevice.exe` com a sessão Wine viva hospeda os
drivers dela, entre eles o `winebus.sys` do controle, e o `services.exe` não o
traz de volta: em 14/09 a primeira versão desta limpeza matou os dois do
prefixo 1 (vivo desde 13/09, com o Steam principal pendurado nele) e o jogo
lançado em seguida ficou no "PRESS START" sem ver o pad.

**Capturas.** `game shot --scale 0.5` grava em meia resolução (média de cada
bloco 2×2; linha/coluna ímpar descartada); o padrão de `game shot` e de
`pad seq --shot` continua `1`. Nos cenários o padrão é meia escala;
`"screenshots": "full"` grava na resolução da janela.

## O injector do CI: `injector fetch`, `check` e `status`

`Injector.dll` só compila com MSVC, então só existe no CI
(`.github/workflows/injector-linux.yml`, artefato `injector`). O ciclo de uma
mudança num hook é:

```bash
ds2os-dev injector check              # sintaxe dos .cpp alterados, antes do push
git push                              # o CI constrói
ds2os-dev injector fetch --json       # espera o run, baixa, manifest
ds2os-dev game stop --instance both   # (sem sessão viva)
ds2os-dev up --seamless ...           # prepare copia a DLL, relança
ds2os-dev injector status             # cada jogo diz o build que carregou
```

**O recibo diz o commit.** O CI passa `-DDS2OS_BUILD_SHA=${{ github.sha }}`;
`DS2_Harness.json` ganha `"build"` e `observe` mostra em `hooks.build`. Build
local diz `"unknown"`; uma DLL de antes do campo não tem `build`.

**`fetch`** escolhe, por padrão, o run mais novo do branch cujo commit tem os
mesmos fontes do injector que HEAD (`git diff --quiet <sha> HEAD --` sobre
`Source/Injector`, `Source/InjectorLauncher`, `Source/Shared`,
`Source/ThirdParty/detours` e o workflow). Assim um commit só do harness em
cima de um push do injector ainda acha o run certo; sem run assim é
`no_run_for_head`. `--latest` pega o mais novo do branch e `--run ID` um
específico; os dois dizem `sameCodeAsHead`. Run em andamento é consultado a
cada 15 s até `--seconds` (padrão 1500); conclusão diferente de `success` é
`injector_build_failed`.

Baixa em `~/Downloads/injector.new`, confere que `Injector.dll` e
`Injector.exe` existem e começam com `MZ`, escreve `manifest.json`
(`runId`, `headSha`, `branch`, `fetchedAt`, `files` com o SHA-256 de cada um)
e só então gira: `injector.prev` é apagado, `injector` vira `injector.prev`,
`injector.new` vira `injector`. Um run que já está lá, com os mesmos hashes,
não é baixado de novo (`alreadyFetched`). Cancela os runs do `ci.yml` do mesmo
commit (e de HEAD) e confirma que chegaram a `completed` (`ciCancelled`; o que
não confirmou vira aviso).

**`fetch` nunca instala**: `game prepare` sobrescreve a DLL de um jogo aberto.
`data.installs[]` traz, por instância, `installedSha256`, `equalsSource`,
`running`, `bootId`, `build` e `buildState`; `needsPrepare` é alguma DLL
instalada diferente da baixada, `needsRelaunch` é algum jogo aberto com DLL
diferente ou com `buildState` diferente de `current`.

| `buildState` | Significado |
| --- | --- |
| `current` | O recibo deste boot diz o commit do manifest |
| `other` | Outro commit |
| `unknown` | `"build": "unknown"`, build local |
| `unrecorded` | O recibo não tem `build`: DLL anterior ao campo |
| `noReference` | Sem manifest, ou sem recibo deste boot (jogo parado) |

**`status`** (não exclusivo) mostra o diretório de origem, o manifest (ou
`null` quando o diretório não veio de `fetch`), se a DLL de origem é a do
manifest, e os mesmos `installs`, `needsPrepare` e `needsRelaunch`.

**`check`** é **sintaxe com mingw, não compilação MSVC**.
`x86_64-w64-mingw32-g++ -fsyntax-only -fpermissive -w` em cada `.cpp`, com:
`__try` reescrito como `if (1)`, `__except (filtro)` como `else` (as quebras de
linha do filtro mantidas, para os números de linha baterem),
`_ReturnAddress()` como `__builtin_return_address(0)`, um `detours.h` só com
declarações e um `Windows.h` que inclui `<windows.h>`. Um `-include` que
redefine `__try` não serve: a libstdc++ usa `__try` nas próprias macros.

Sem argumentos, verifica os `.cpp` de `Source/Injector` alterados contra HEAD
ou não rastreados, mais os que incluem um `.h` alterado; nada alterado é
`nothing_to_check` (inconclusivo). Arquivos na linha de comando, ou `--all`
(27 arquivos, ~10 s em paralelo). Um arquivo com erro é compilado também na
versão de HEAD (com os headers da árvore) e só contam os erros cuja mensagem
HEAD não tem: `Entry.cpp` (`::main`), `DS2_LogProtobufsHook.cpp` (falta
`<atomic>`, que o MSVC traz de carona) e `ReplaceServerPortHook.cpp` já falham
no mingw e aparecem como `ok` com `preexisting`. Erro novo é `syntax_errors`
(código 1) com `files[].errors[]` `{line, message}`.

`check --self-test` é o sinal positivo: verifica `DS2_CrashHook.cpp` intacto
(tem que dar `ok`) e com uma linha quebrada no fim (tem que apontar o erro
naquela linha). Um `check` limpo sem o autoteste não prova que o compilador
estava olhando.

`doctor` ganhou `injector_build` por instância aberta: o `build` do recibo
contra o `headSha` do manifest de origem, `warning` para outro commit, build
local ou DLL anterior ao campo.

## Resultados para a LLM

`--json` é global: funciona antes ou depois do subcomando. O stdout contém um
único objeto ao término; mensagens de progresso ficam no arquivo de eventos.

```json
{
  "schemaVersion": 1,
  "runId": "<identificador único>",
  "status": "failed",
  "ok": false,
  "durationMs": 840,
  "harnessBuild": {"commit": "<sha do commit>", "dirty": false},
  "data": {},
  "error": "assertion_failed: conta 2, /state esperado world",
  "errorCode": "assertion_failed",
  "artifacts": "/.../ds2os-dev/runs/<identificador único>"
}
```

| Código de saída | Resultado | Interpretação |
| --- | --- | --- |
| 0 | `passed` | O comando cumpriu seu contrato; num cenário, todas as assertions passaram |
| 1 | `failed` | Falha de ação, assertion, configuração, cancelamento ou persistência |
| 2 | `inconclusive` | A observação/assertion não conseguiu obter a evidência necessária |

`harnessBuild` diz que código produziu o resultado: `commit` é o HEAD no
momento do build e `dirty` indica edições não commitadas em `ds2os-dev` ou
`ds2os-core` naquele build (`unknown`/`false` quando compilado sem git). O
binário em `target/debug` sobrevive a checkouts e edições; um resultado sem
isso não diz se veio do código atual.

`errorCode` extrai o prefixo estável de erros como `busy`, `timeout`,
`instance_unresolved`, `wrong_character`, `partial_failure` e `cleanup_failed`.
Erros sem prefixo usam `operation_failed`. O campo `error` conserva o contexto
humano. Erros de sintaxe do próprio Clap, `--help` e `--version` acontecem antes
do registro de execução e seguem a interface normal do parser.

**Migração:** os campos antigos de `status --json` e `doctor --json` agora
estão dentro de `data`. `doctor --json` devolve código 1 quando há problemas.
`status` é um inventário: código 0 não significa que existe uma sessão co-op.
`up` agrega falhas das contas e devolve código 1 se alguma não alcançar o
estado exigido. `up --no-enter` espera confirmação do título; PID criado
sozinho não aprova a operação. Capturas que falham também produzem erro.

## `doctor`: a cadeia de prova

```bash
ds2os-dev doctor --json
```

`doctor` não só lista o que está instalado: exercita cada elo de que um teste
depende, com o que estiver rodando. Em 13/09 ele dizia "tudo pronto" enquanto
todo `game enter` falhava, porque conferia arquivos e os dois defeitos estavam
um passo adiante (o executável lido do diretório errado e a conta escrita em
outra notação pela API).

`data` traz `environment`, `problems` (os do ambiente, no formato antigo),
`checks`, `summary` e `ok`. Cada item de `checks` tem `name`, `instance`
(quando é de uma conta), `status`, `detail`, `fix` e `data`:

| `status` | Significado |
| --- | --- |
| `ok` | O elo foi exercitado e respondeu o esperado |
| `warning` | Funciona, mas algo vai morder: DLL instalada diferente da de origem, logs enormes, órfãos do Wine |
| `problem` | O elo está quebrado; `doctor` termina com código 1 e `errorCode` `environment_not_ready` |
| `skipped` | Não foi possível exercitar (instância parada, servidor parado, lock do probe ocupado); **nunca** conta como `ok` |

| `name` | O que exercita |
| --- | --- |
| `environment` | Cada problema de `environment.problems()` (Steam, jogo, Proton, servidor, injector) |
| `harness_build` | O binário é do commit em que o repositório está e nenhum fonte dos crates é mais novo que ele; senão `warning` |
| `server_api` | A API responde e todo Steam ID listado é decimal de 17 dígitos |
| `identities_distinct` | As duas contas configuradas são diferentes |
| `identity` | A conta tem um SteamID64 decimal configurado |
| `install` | A instalação da conta foi resolvida |
| `game_exe` | O executável que o probe confere (`installs[].gameExe`) é o 1.03 |
| `injector_installed` | SHA-256 do `Injector.dll` instalado contra o de origem (`~/Downloads/injector`) |
| `injector_build` | O `build` do recibo deste boot contra o `headSha` do `manifest.json` de origem (`injector fetch`) |
| `logs_size` | Logs `DS2*.log` da instalação; `warning` acima de 256 MB |
| `game_processes` | Exatamente um `DarkSoulsII.exe` no prefixo da conta |
| `memprobe` | Uma consulta real de `locate`: `data.state`, `data.reason`, `data.latencyMs`; `warning` acima de 1500 ms |
| `receipt` | `DS2_Harness.json` é do mesmo boot que `DS2_Nav.txt` e todos os hooks instalaram |
| `config_drift` | O que o boot aberto recebeu (`configured` do recibo) contra o `Injector.config` atual: `autoRematch`, `forceZone`, `removeFog`, `seamless`, `timer` |
| `api_identity` | A API lista a conta; no mundo sem registro é `problem` (offline ou outra conta) |
| `extra_game_processes` | Nenhum `DarkSoulsII.exe` fora dos prefixos das instâncias |
| `pad` | O pad está no ar; jogo aberto sem pad é `warning` |
| `wine_orphans` | `xalia.exe` de prefixos sem jogo e `winedevice.exe` de prefixos sem `services.exe` |
| `x11_clients` | Conexões ao X11 em `/proc/net/unix`; `warning` a partir de 200 (o Xorg recusa acima de 256) |

`config_drift` é a armadilha de `up`, que reescreve `Injector.config` com as
próprias flags: o arquivo muda, o jogo aberto continua com a config do
lançamento. `doctor` não é exclusivo e roda ao lado de um controlador; com o
jogo aberto ele escreve um pedido de MemProbe de leitura, como `observe`.

## Observação e identidade

```bash
ds2os-dev observe --instance both --json
ds2os-dev where --instance 2 --json
ds2os-dev players --json
```

`observe` combina a API com as leituras locais de cada instalação. Em
`data.instances[]` aparecem:

| Campo | Fonte e significado |
| --- | --- |
| `instance`, `steamId`, `identitySource` | Conta declarada no harness |
| `processes[].pid`, `startTicks` | Processo no prefixo Proton e instante de início em ticks do Linux |
| `state` | `title`, `loading`, `world` ou `unknown` |
| `stateReason` | Por que `state` é esse; ver a tabela de motivos abaixo |
| `pose`, `poseAgeMs` | Posição, orientação, tick e arquétipo publicados pelo injector |
| `bootId` | Identificador do carregamento do injector; `null` com DLL antiga |
| `player`, `serverConnected` | Registro da API filtrado pelo Steam ID; `null` quando não verificável |
| `hooks` | Recibo dos hooks instalados no mesmo boot da telemetria |
| `p2pSessionVerified` | Só com `--session`: `true` quando a sessão existe e dados atravessaram entre os peers; ver "A sessão" |
| `session` | Só com `--session`: papel, membros, estados das máquinas e contadores do canal desta instância |
| `problems` | Motivos pelos quais alguma observação não pôde ser confirmada; um `unknown` aparece como `state_unknown: <motivo>` |

`observedAtMs`, `serverObservedAtMs` e `durationMs` tornam visível quando as
fontes foram consultadas. As fontes são amostradas sequencialmente, não em um
instante atômico. A idade da resposta da API não é a idade do último pacote
enviado pelo jogo.

O byte do título só é lido para o executável 1.03 conhecido pelo SHA-256 de
`ds2os-core::exe::DS2_SOTFS_1_03`. O executável conferido é o que o ambiente
resolveu para a instalação (`installs[].gameExe`, que no Scholar of the First
Sin fica em `Game/`); os arquivos de pedido ficam na raiz, ao lado do injector.
Byte 1 confirma título; zero com posição válida e avançando confirma mundo;
zero sem jogador indica carregamento. Todo o resto é `unknown`, e o motivo vai
junto:

| Motivo | Estado | Significado |
| --- | --- | --- |
| `title_flag_set` | `title` | O byte do título lido como 1 |
| `telemetry_advancing` | `world` | Byte 0 e o tick da pose publicada avançou |
| `no_telemetry` | `loading` | Byte 0 e nenhuma pose publicada |
| `instance_stopped` | `unknown` | Nenhum processo do jogo no prefixo da conta |
| `instance_missing` | `unknown` | Nenhuma instalação resolvida para a conta |
| `exe_missing` | `unknown` | A instalação não resolveu executável, ou ele não pode ser lido; nenhum pedido é escrito |
| `unsupported_exe` | `unknown` | Executável diferente do 1.03; nenhum pedido é escrito |
| `probe_busy` | `unknown` | Outro leitor segurou `DS2_MemProbe.lock` durante o prazo inteiro |
| `request_write_failed` | `unknown` | Não foi possível escrever o pedido ou o lock; o erro vai em `detail` |
| `request_not_consumed` | `unknown` | O pedido continua no disco: nada no jogo está lendo pedidos (jogo ainda iniciando, DLL sem a sonda) |
| `no_answer` | `unknown` | O pedido foi consumido, mas nenhuma resposta com o rótulo apareceu no prazo |
| `malformed_answer` | `unknown` | Resposta com o rótulo, num formato que não é o dump de bytes |
| `unreadable` | `unknown` | O injector respondeu que o endereço é ilegível |
| `unexpected_byte` | `unknown` | Byte diferente de 0 e 1 |
| `telemetry_stalled` | `unknown` | Byte 0 com pose publicada, mas o tick não andou |
| `stale_telemetry` | `unknown` | Só em `observe`: sem amostra nova do mesmo processo e boot |
| `timeout`, `cancelled` | `unknown` | O prazo acabou, ou a operação foi cancelada, antes de perguntar |

Os nomes fazem parte do contrato: podem ganhar valores, não mudam de sentido.

Entre 13/09 e 14/09 a conferência procurava `DarkSoulsII.exe` na raiz, não
achava nada e todo `locate` respondia `unknown`: `game enter`, `game leave`,
`up`, `reload` e os cenários esperavam até o prazo sem apertar um botão.

Cada pedido de MemProbe recebe um rótulo único. A resposta de um pedido
anterior não o satisfaz, e só o trecho de `DS2_MemProbe.log` escrito depois do
pedido é lido. Há um lock por instalação para os pedidos do harness; um lock
ocupado é esperado até o prazo da consulta, e só então vira `probe_busy`.
Ferramentas externas que escrevam diretamente em `DS2_MemProbe.req` precisam
respeitar o mesmo lock para não sobrescrever pedidos.

### Eventos de navegação

Cada pergunta ao jogo, cada tecla e cada espera pela API ficam em
`events.jsonl`, para que uma falha diga onde parou:

| `kind` | `data` |
| --- | --- |
| `locate` | `instance`, `state`, `reason`, `exe` (o executável conferido), `label`, `requestWritten`, `byte`, `tickBefore`, `tickAfter`, `elapsedMs` e, quando houver, `detail` |
| `press` | `instance`, `command`, `ok`, `error` |
| `enter` | `phase: "world_without_api_record"` com `expected`, `listed` (os Steam IDs que a API listou) e `offlineForMs`; `phase: "offline_recovery"` com `attempt` |

`world_without_api_record` é normal nos primeiros segundos no mundo: o servidor
leva de 2 a 5 segundos para listar quem acabou de chegar (medido em 14/09: seis
eventos seguidos, e a chegada logo depois). Só aos 45 segundos sem a conta o
`enter` desiste do mundo e faz `offline_recovery`; `listed` com a conta em
outra notação, ou vazio por muito tempo, é o que merece atenção.

Um `timeout` ou `leave_failed` de `game enter`, `game leave`, `reload` e
`up --no-enter` acrescenta ao erro o último estado e motivo lidos, quantas
teclas foram enviadas e quantas leituras da API vieram sem a conta — por
exemplo `timeout: prazo da operação esgotado; último estado unknown
(request_not_consumed) há 2.0s; 0 tecla(s); 0 leitura(s) da API sem a conta`.
O `errorCode` continua `timeout`.

Uma pose exige campos completos e finitos e arquivo com até 2 segundos de
idade. `observe` exige avanço de tick no mesmo boot e processos inalterados.
`goto` também interrompe quando o processo/boot muda. O mapeamento de janela
é exclusivamente por processo/prefixo; não existe fallback para a posição da
janela na lista do X11.

`game enter` combina estado local com a API da conta esperada e, quando
configurado, confere o nome do personagem. Não considera mais a última linha
global de personagem carregado como evidência da instância sendo controlada.
O servidor publica a conta duas vezes, `steamId` em hexadecimal
(`011000010afd1a3a`) e `steamId64` em decimal; o `player.steamId` do harness é
o decimal, o mesmo formato de `game identity`. Comparar o hexadecimal com o
decimal não achava ninguém: no mundo, o `enter` tomava por offline um jogador
que a API já listava, e esgotava o prazo saindo para o título e voltando.
`game leave` é idempotente no título e recusa agir quando o mundo não foi
confirmado. `reload` aborta antes de reiniciar o servidor se não conseguir
confirmar a saída de algum cliente.

## Memória do jogo: `probe`

```bash
ds2os-dev probe --instance 1 "mod title 1614804 1" "chain chr 16148f0 d0 376" --json
ds2os-dev probe --instance 1 "pokeabs hp 7fffeb7b9fc8 00000000 64030000" --json
```

Cada argumento é um comando do MemProbe com um **nome** no lugar do rótulo:
`abs|mod <nome> <endereço hex> <comprimento>`,
`chain <nome> <offset hex> <offsets hex separados por vírgula> <comprimento>`,
`pokeabs|pokemod <nome> <endereço> <bytes hex> [<bytes esperados>]`,
`pokechain <nome> <offset> <offsets> <bytes> [<esperados>]` e
`scan <nome> <valor hex> <largura 1|2|4|8> [<máximo>]`. O harness troca o nome
por um rótulo único, manda todas as linhas **num pedido só** (cada ida e volta
custa cerca de meio segundo) e espera a resposta de todas.

Duas regras do parser do injector que falham em silêncio, e que o harness
confere antes de escrever:

- o **comprimento é decimal**: `200` são 0xc8 bytes. Fica entre 1 e 16384;
  fora disso o injector lê 0x100 bytes sem avisar.
- a cadeia já começa desreferenciando o endereço do módulo, então a lista traz
  só os offsets seguintes: `chain chr 16148f0 d0 376` lê a partir de
  `*(*(base+0x16148f0)+0xd0)`. Um `0,` na frente desreferencia a vtable do
  contexto.

`data.answers[]` traz `name`, `label` e `kind`: `bytes` (`address`, `bytes` em
hex), `unreadable`, `chain_unresolved`, `poked` (`address`, `before`, `wrote`),
`poke_refused` (`expected`, `found`), `poke_invalid`, `scan` (`hits`) ou
`malformed` (`line`). Uma resposta só conta com todas as linhas completas, e só
o trecho do log escrito depois do pedido é lido.

Uma escrita só passa quando o injector diz que escreveu (`poked` com `wrote`
verdadeiro); recusa, bytes inválidos ou falha terminam em `poke_not_written`.
Sem resposta completa, o erro é o motivo do `locate` (`request_not_consumed`,
`no_answer`, `probe_busy`, `unsupported_exe`...). Só leituras rodam ao lado de
um controlador; uma linha `poke*` toma o `control.lock`. Todo pedido fica no
`events.jsonl` como evento `probe`, com as linhas enviadas e as respostas.

## O personagem: `character`

```bash
ds2os-dev character --instance both --json
ds2os-dev observe --instance 1 --character --json
```

Lê o personagem local num pedido só de MemProbe (quatro cadeias) e devolve, por
instância, `character` com `address` (o objeto do personagem, que muda a cada
carregamento), `hp`, `hpMax` (depois do hollowing), `souls`, `deaths`, `hollow`
(0 a 32), `hollowState` (0 humano, 1 hollow), `role` (0 dono do mundo, 1
fantasma branco), `position` e `bonfire` (`map` e `id` do registro para onde a
morte manda; `null` quando o registro não resolve). Os endereços são os que o
`DS2_DeathInterceptHook` lê e escreve e os medidos em
`docs/DS2_SEAMLESS_COOP.md`, só para o executável 1.03.

No título ou carregando não há personagem: o erro é `no_character`. Um objeto
com HP fora de `0..=hpMax` ou posição não finita é `implausible_character`, não
um personagem com zeros.

`observe` só lê o personagem com `--character`, porque custa mais uma ida e
volta; um problema de leitura aparece como `character_unread: <motivo>`.
Cenários aceitam `/character/hp`, `/character/hpMax`, `/character/souls`,
`/character/deaths`, `/character/hollow`, `/character/hollowState`,
`/character/role`, `/character/bonfire/id` e `/character/bonfire/map`; a
observação dessas assertions já inclui o personagem, e fora de `state: world`
elas são inconclusivas.

## O hook de morte: `death` e `kill`

```bash
ds2os-dev death --instance both status --json
ds2os-dev death --instance 1 mode respawn --json
ds2os-dev death --instance 1 feature copias off --json
ds2os-dev death profile set --mode respawn --feature copias=off
ds2os-dev kill --instance 1 --json
```

`death` escreve ordens em `DS2_Death.req` e só confirma ao achar o **eco exato**
no trecho do `DS2_Death.log` escrito depois da ordem: `=== modo: renascer na
fogueira, pagando a morte ===`, `=== cobranca: feature copias off ===`, ou a
linha de `status`. `=== nao entendi: ... ===` é `death_order_refused`, nunca
sucesso. Sem eco, `request_not_consumed` (o hook não leu: jogo iniciando ou
DLL sem o hook) ou `no_answer`. O harness usa um `DS2_Death.lock` próprio por
instalação e espera um pedido anterior sumir antes de escrever o seu.

Os modos são `observe` (a morte do jogo), `cancel` (a morte não acontece, nada
é cobrado) e `respawn` (a morte é cobrada e o personagem volta à fogueira na
mesma sessão). As partes da cobrança são `almas`, `hollow`, `contador`, `anel`,
`mancha_online`, `estus`, `banner`, `copias`, `fogueira_do_host` e
`outro_mapa`. `status` devolve `mode`, `counters` e `features`.

**Todo lançamento começa em `observe` com a cobrança inteira ligada.** O perfil
(`death profile set|show|clear`, guardado em `config.json`) é reaplicado pelo
`game enter` a cada chegada ao mundo, depois de o recibo do boot mostrar
`DS2 Death Intercept` instalado, e só conta com o eco; falhar aqui faz o
`enter` falhar com o motivo. O evento `enter` com `phase: "death_profile"`
registra o resultado.

`kill` exige a instância num mundo confirmado, lê o modo com `status` e o
personagem, e zera o HP com `pokeabs` **passando os bytes esperados**. Passa só
com as linhas do hook para aquela morte:

| Modo | Sinal positivo |
| --- | --- |
| `respawn` | exatamente um `custos da morte (...)` seguido de `renascer concluido em N quadros`; o `morte CANCELADA` entre eles é o hook segurando a morte do jogo |
| `cancel` | `morte CANCELADA`, sem `custos da morte` |
| `observe` | `morte vista`; exige `--real-death`, porque é a morte do jogo (almas no chão, carregamento e, em sessão, o fim dela) |

`data` traz `mode`, `before` e `after` (o personagem antes e depois),
`poke`, `deathLines` e `otherSide` (as linhas `morte da copia RECUSADA` que a
outra instalação escreveu no mesmo intervalo). Escrita recusada é
`poke_not_written`; HP zerado sem as linhas em 15 s é `inconclusive`. Cenários
ganham a ação `{"action": "kill", "instance": N}`, que recusa o modo `observe`.

## A sessão: `session`

```bash
ds2os-dev session --json
ds2os-dev observe --instance both --session --json
```

Amostra as duas instâncias em paralelo: o `status` do `DS2_Channel.req` duas
vezes, com pelo menos 2,5 s entre elas, e as duas máquinas de sessão por
varredura de vtable (`NetSummonAcceptMultiplayCtrl` `0x1410d7998`, estado em
`+0x150`; `NetSummonJoinMultiplayCtrl` `0x1410d7bd8`, estado em `+0xf8`). Leva
de 4 a 5 segundos, por isso só entra no `observe` com `--session` e nas
assertions que pedem `/session/*` ou `/p2pSessionVerified`.

A decisão tem dois níveis, e `data.assessment` diz qual passou:

| Campo | Exige |
| --- | --- |
| `objectsAgree` | host com o controlador em `0x10`, convidado com o seu em 7, e os canais das duas máquinas listando os mesmos dois membros, com o mesmo marcado `(host)`, que são as duas contas configuradas |
| `peersExchange` | entre as amostras, `enviados` do host sobe com `falhas` inalterado **e** `recebidos` do convidado sobe (o host anuncia a fogueira a cada 2 s por Steam P2P) |

`p2pSessionVerified` é `true` só com os dois; `false` só quando nenhuma das
duas instâncias tem sessão no canal nem controlador ativo; `null` em todo o
resto, com o motivo em `problems`:

| Problema | Significa |
| --- | --- |
| `session_objects_only` | a sessão existe e nenhum dado atravessou; o controlador do host já foi medido em `0x10` minutos depois de a sessão acabar |
| `channel_counters_stalled`, `host_loading` | contadores parados; em carregamento o host para de anunciar |
| `members_disagree`, `not_our_pair` | os canais divergem, ou há mais que as duas contas (três jogadores não são testáveis nesta máquina) |
| `host_controller_inactive`, `guest_controller_inactive` | a máquina de sessão não está no estado de sessão ativa |
| `roles_incomplete` | um lado mostra sessão ou controlador e o outro não |
| `identity_mismatch` | a conta que o canal diz rodar não é a configurada |

Duas coisas medidas em 14/09 que o harness trata:

- fora de sessão o canal publica `eu=0000000000000000`: o hook só aprende a
  própria conta ao consultar uma sessão. `samples[].second.me` fica `null`, e
  isso não é conta errada.
- a varredura acha a própria agulha do injector, num endereço baixo que aparece
  **nas duas** varreduras e, depois da segunda, contém a vtable do convidado.
  Endereço presente nas duas listas é descartado, e todo candidato tem a
  vtable relida antes de o estado contar.

`data.samples[]` guarda as duas amostras cruas do canal e os controladores de
cada instância; `data.assessment.instances[]` traz `role` (`host`, `guest`,
`none`, `unknown`), `members`, `hostState`, `guestState` e os deltas `sent`,
`failed`, `received`, `refused`. O comando termina `passed` com uma decisão
(`true` ou `false`) e `inconclusive` com `null`. O pedido de canal usa um
`DS2_Channel.lock` do harness, como `DS2_Death`.

### Terminar a sessão: `session end`

```bash
ds2os-dev session end --json
```

Matar um cliente com a sessão viva é uma desconexão ilegal, e o jogo conta
isso no save. `session end` termina a sessão do jeito que o jogo aceita,
receita medida em 14/09:

1. `session` decide quem é host e quem é convidado. Se nenhum canal mostra
   membros, termina `passed` com `data.ended: false`, `reason: no_session`.
   Papéis incompletos são `inconclusive` (`session_unverified`).
2. Guarda o `copias` do host e o modo do convidado. Depois desliga `copias` no
   host, põe o convidado em `observe` e mata o convidado (`kill --real-death`).
3. Passa quando os canais **das duas** instâncias ficam sem sessão viva em
   duas leituras com pelo menos 2,5 s entre elas. O prazo é `--seconds`,
   padrão 60 (`session_still_live` ao vencer). Não depende do controlador do
   host, que continua em `0x10` por minutos.
4. Devolve `copias` e o modo aos valores de antes, mesmo em falha, cada um
   confirmado pelo eco. `data.restored` tem um booleano por ajuste, e os
   erros ficam em `data.restoreErrors` (`restore_failed` se não voltar).

O convidado paga uma morte de verdade: almas no chão e hollow. Queime uma
efígie antes da próxima marca. `data.serverLines` traz as linhas
`LeaveSession` e `LeaveGuestPlayer` do servidor, apenas como informação: o
servidor só registra a primeira mensagem de cada tipo por conexão, e sem elas
vem a nota `server_census_silent`. Com o servidor atual, que registra cada
`RequestNotify*` (ver "A linha do tempo"), as linhas `Notify ...` aparecem em
qualquer sessão. Medido: sessão terminada em 19,5 s, com as duas linhas no
servidor.

### O guarda de `session_live`

`game stop`, `down` e `save restore --stop` perguntam ao canal de cada
instância aberta, antes de fechar **qualquer** uma delas, se há sessão viva
(membros vistos há até 5 s). Com sessão, recusam com `session_live` e não
mexem em nada. O servidor também não cai no `down`. `--force` fecha mesmo
assim e registra o evento `stop` com `phase: forced_with_session`.

Canal que não responde (jogo iniciando, travado, DLL sem o hook) **não**
bloqueia. A parada segue com o evento `phase: session_check_unknown`. A
limpeza de um cenário com `baseline` também não recusa: o save é restaurado
logo depois, e a desconexão vai embora com ele. Isso fica registrado como o
evento `cleanup_kill_with_session`.

## Os hooks de volta ao estado de chegada: `hooks reset`

```bash
ds2os-dev hooks reset --instance both --json
```

Um teste que recusa um motivo de sessão, força um mapa, arma um breakpoint ou
muda a cobrança da morte deixa isso no jogo aberto, e o próximo teste herda.
`hooks reset` desfaz cada item e só passa com o eco do hook. Hook ausente do
recibo deste boot fica `skipped`, e recibo ausente é `receipt_missing`.

| Hook | Pedido | Confirmação |
| --- | --- | --- |
| `DS2 Seamless Session` | `clear`, `role any`, `status` | `=== recusando a mascara 00000000, papel -1 ===` |
| `DS2 Backread` | `unfocus`, `clear`, `status` | `pedido 00000000` e `foco 00000000` no cabeçalho do status |
| `DS2 Trace` | `wpclear`, `clear`, `report` | `=== limpo ===` e depois `=== 0 armados, ... ===` (`wpclear` não escreve nada sem vigia armada, então não é esperado) |
| `DS2 Death Intercept` | `observe`, as dez cobranças no padrão de boot (todas ligadas menos `mancha_online`), o `death profile` salvo por cima, `status` | modo e cobranças do status iguais ao esperado |

O resultado é o estado que um `game enter` deixa. Isso inclui o perfil de
morte, porque a chegada o aplica.

O que não dá para desfazer vem escrito. O `DS2_Backread` não tem verbo que
solte um `keep` antes do prazo, então mapas ainda com `forcado` diferente de
zero voltam em `data.forced` com o aviso `keeps_remain`. Numa sessão, esses
são os keeps da cópia remota e são esperados. Hooks que não responderam dão
`inconclusive` (`hooks_unanswered`). Hooks que responderam outra coisa dão
`failed` (`hooks_not_reset`).

## Os signs do servidor: `server wait --signs`

```bash
ds2os-dev server wait --signs 0 --seconds 90 --json
```

Um cliente morto deixa o sign no cache do servidor até a conexão expirar, e
esse sign parece real. `server wait` lê as linhas `Sign poll` que o servidor
escreve **depois** de o comando começar e passa quando a mais recente mostra
`N signs cached`. Esse número é o cache inteiro, não o de um jogador.
`data.byPlayer` guarda o último poll de cada um.

A linha só existe com `DS2_StickySigns` ligado. O padrão do servidor é
desligado, e o `Saved/default/config.json` local está ligado. Com ele
desligado, o comando é sempre `inconclusive`. O servidor limita a uma por
jogador a cada 10 s. Na prática aparece a cada ~60 s por jogador (medido em
14/09), então o prazo padrão é 90 s, e menos que 60 s pode perder o único poll
de quem ficou. Nenhum
poll dentro do prazo dá `inconclusive`, nunca `passed`. Poll com outro número
no fim do prazo dá `failed` (`signs_remain`). O comando é só leitura e roda ao
lado de um controlador.

## Mover o personagem: `teleport`, `bonfires`, `backread` e `goto-map`

```bash
ds2os-dev bonfires --instance 1 --json
ds2os-dev teleport --instance 1 --to-bonfire 0x7ba2 --json
ds2os-dev teleport --instance 1 --to 6.186,-18.517,209.053 --json
ds2os-dev goto-map --instance 1 --map 0a040000 --to 10.53,5.92,-16.25 --json
ds2os-dev backread --instance 1 load|focus <mapa> x y z|unfocus|clear|keep <i> <ms>|status --json
ds2os-dev scenario run docs/scenarios/teleport-roundtrip.json --json
```

`teleport-roundtrip.json` usa as coordenadas do Samuel em Heide e Majula: vai à
Catedral, a Majula e volta à primeira fogueira de Heide.

`bonfires` lê, numa só ida e volta, o registro da última fogueira e os nós da
lista do mapa carregado: id, mapa e ponto de nascimento (translação menos
1,1 × eixo Z, como o renascer faz). Os nós são pedidos como caminhos a partir
do contexto (`70,58,8`, `70,58,8,60`, …, até 16), e os que passam do fim da
lista voltam `cadeia nao resolveu`.

`teleport` escreve o que o `TeleportLocal` do hook de morte escreve:
`chr+0x90` e `+0xa0`, `motion+0x50`, a física (`+0x80`, velocidades `+0x60` e
`+0x70` zeradas, `+0x1c0`), o corpo Havok (`+0x250`, `+0x260`, `+0x1b0`,
`+0x1c0`, `+0x1a0`, 5 cm acima dos pés) e, por último, o chão do controlador de
queda (`*(chr+0xe0)+0xb0` `+0x20`). Sem essa última escrita, uma queda de
24,5 m depois do teleporte já matou o personagem. São 13 escritas:

1. Uma ida e volta lê os cinco objetos e confere as vtables do personagem
   (`0x1410e4bb8`) e do corpo (`0x141126578`). Divergência dá
   `vtable_mismatch`, e nada é escrito.
2. Outra ida e volta faz as 13 escritas, **cada uma com os bytes que a leitura
   achou**. O injector recusa a escrita se eles mudaram (endereço
   reaproveitado, ou personagem andando), e uma recusa dá `teleport_refused`.
   Com o personagem parado, os 13 alvos foram medidos idênticos entre duas
   leituras.
3. Passa com 13/13, o personagem **assentado** a menos de 1,5 m na horizontal
   e 1 m na vertical do alvo depois de 3 s (`not_arrived` fora disso) e nenhuma
   linha `death_cost`, `death_cancelled` ou `death_seen` no `DS2_Death.log`
   nesses 3 s (`died_after_teleport`).

O raio é esse porque a física empurra o personagem para fora do que ele foi
posto dentro, e o nascimento de uma fogueira fica dentro da fogueira. Medido
em 14/09: Catedral de Heide a 0,84 m, fogueira `0x7ba7` a 0,82 m, 12 m abaixo
e sem dano de queda.

`--to-bonfire` só aceita fogueira da lista carregada
(`bonfire_not_loaded`: use `goto-map`).

`backread` fala com o `DS2_Backread.req` e sempre devolve o status junto.
`keep <i> <ms>` não é reversível: nenhum verbo solta um keep antes do prazo, e
`keep i 0` em muitos índices ocupa as 8 vagas (ver `hooks reset`). Um
eco só diz que o pedido foi lido, então `goto-map` espera o efeito:

1. `load <mapa>` até o status do hook listar o mapa em `estado 5` (no log,
   `estado 4 -> 5`; ~505 ms medidos). Se ele já estava carregado, fica
   `alreadyLoaded`.
2. `focus <mapa> x y z` até `foco no mapa X em (...): celula N` com N diferente
   de -1 e -2.
3. `teleport` (sem somar nada a y).
4. O contato físico sob o personagem (`*(*(chr+0x100)+0x10)+0xe0`: tipo no
   nibble baixo, 7 colisão e 1 objeto de mapa, índice do mapa nos bits 4..9)
   igual ao `[i]` do mapa no status, em duas leituras seguidas. Sem isso em
   10 s dá `inconclusive`, e mapa e foco ficam pedidos.
5. `unfocus` + `clear`. Depois de 4 s, o contato ainda precisa estar no mapa
   (`left_map`).

A posição não serve de prova porque mapas compartilham coordenadas: a primeira
fogueira de Heide fica onde estão as pedras do mar de Majula. Medido em 14/09,
Heide → Majula: contato `0x3c17` (índice 1), e Majula → Heide: `0xc7`
(índice 12), os mesmos valores do renascer do hook.

`where` mostra, em `pose.live`, os pés do personagem publicados pelo
`DS2_NavHook` (ver abaixo), e em `data.navLag` as instâncias cuja pose publicada
está a mais de 1 m deles. Depois do teleporte para a Catedral, a pose antiga
ficou 68 m atrás, e só o `live` estava certo. `goto` e `goto --to-instance` ainda leem essa
pose, então depois de um teleporte andam a partir de uma posição velha.

`teleport` sozinho prova posição, não mapa: sobre coordenadas que dois mapas
compartilham, ele pode passar em pé no chão do mapa errado. Quem confere o
contato é o `goto-map`. A checagem de morte por 3 s é negativa e só vale
com `DS2 Death Intercept` no recibo deste boot.

## A linha do tempo: `timeline`

```bash
ds2os-dev timeline --last 10m --json
ds2os-dev timeline --since 17:39:50 --kind death_cost,respawn_done,server_notify
ds2os-dev timeline --run <runId> --instance 2 --all
```

Junta, em ordem, o log do servidor, os logs de hook das duas instalações e o
`events.jsonl` do harness em `data.entries[]`: `{atMs, at, resolutionMs,
source, instance, kind, line, more}`. Linhas indentadas continuam a entrada
de cima (`more`). Sem janela, vale `--last 10m`. `--limit` (padrão 2000)
guarda as mais novas.

| Fonte | Relógio | Resolução |
| --- | --- | --- |
| `server` | `AAAA-MM-DD HH:MM:SS`, hora local, sanitizado | 1000 ms |
| `death`, `channel`, `backread` | `HH:MM:SS.mmm` sem data | 1 ms |
| `session`, `seamless`, `respawn`, `crash`, `trace`, `rematch` | nenhum | `atMs: null` |
| `harness` | `atMs` de cada `events.jsonl` | 1 ms |

A data dos relógios sem data vem do mtime do arquivo, andando para trás, ou do
início da execução no `--run`, andando para frente. Um relógio que volta mais
de 12 h é virada de dia. Dentro do mesmo segundo do servidor, a ordem é por
fonte: uma linha do servidor às `18:12:39.000` pode ter acontecido depois de
uma de hook às `18:12:39.930`.

Linhas sem relógio não cabem numa janela de hora. `--last`/`--since` as
deixam de fora e dizem quais fontes escreveram na janela
(`notes: untimed_sources`). `--run <id>` lê os trechos que a execução guardou
em `logs/` e as inclui no fim, com `at: null`. Os timers, o MemProbe e as
sondas de área não entram, porque o relógio deles é o uptime do processo ou
não existe.

Por padrão só aparecem linhas classificadas, sem `sign_poll`, `channel_status`,
ecos de ordem (`*_order`) nem eventos do harness. `--kind a,b` escolhe tipos, `--all` mostra tudo e
`--instance N` filtra os logs de hook (servidor e harness ficam).

| `kind` | Linha |
| --- | --- |
| `death_cost`, `death_cancelled`, `death_seen`, `copy_refused`, `respawn_step`, `respawn_done`, `death_order` | `DS2_Death.log`: custos, morte cancelada, morte vista, cópia recusada, passos e fim do renascer, ecos de ordem |
| `session_end_request`, `host_state`, `session_order` | `DS2_Session.log`: `fim de sessao pedido\|RECUSADO`, estados da máquina do host, ecos |
| `channel_members`, `channel_announce`, `channel_received`, `channel_status` | `DS2_Channel.log` |
| `backread_state`, `backread_release`, `backread_keep`, `backread_focus`, `backread_order` | `DS2_Backread.log` |
| `warp`, `warp_accepted`, `seamless_order` | `DS2_Seamless.log` |
| `crash`, `trace_hit`, `trace_order`, `rematch`, `respawn_order`, `hook_boot` | os demais logs de hook; `hook_boot` é o `=== ds2os ...` de cada lançamento |
| `server_notify` | `Notify RequestNotify<tipo>: ...`, uma por mensagem |
| `server_notify_census` | `First DS2_Frpg2RequestMessage.RequestNotify*`, só a primeira por conexão |
| `sign`, `sign_poll`, `disconnect`, `login` | placas criadas, removidas e invocadas; polls; conexões encerradas; logins |

O servidor DS2 registra **cada** `RequestNotify*` que recebe
(`DS2_LoggingManager.cpp`) e uma linha quando a placa de um cliente perdido
enfim sai do cache. Um servidor anterior a essa mudança só tem o censo, e
nesse caso `server_notify` fica vazio. O servidor compila local: `make Server`
em `intermediate/make`, aplicado com `server restart` sem jogadores.

Medido em 14/09, na segunda invocação da mesma conexão: `Notify
RequestNotifyJoinGuestPlayer` e `JoinSession` sem nenhuma linha de censo. Uma
morte com renascer dentro da sessão deu `death_cost` → `death_cancelled` →
`respawn_step` → `respawn_done` em 17 ms, sem `RequestNotifyDeath` no servidor.
Morte cancelada não notifica, e essa ausência é o sinal, não um buraco.

Num cenário, `step_started` e `step_finished` trazem `logBytes`: o tamanho de
cada log capturado naquele instante, para recortar as linhas de cada passo.

## Cenários

```bash
ds2os-dev scenario validate world-ready --json
ds2os-dev scenario run world-ready --json
ds2os-dev scenario validate docs/scenarios/menu-roundtrip.json --json
ds2os-dev scenario run docs/scenarios/menu-roundtrip.json --json
```

`world-ready` verifica que as duas contas estão no mundo e constam na API.
Não inicia o jogo e **não comprova P2P, invocação ou respawn co-op**.
`menu-roundtrip.json` testa sair para o título e voltar ao mundo nas duas
instâncias já abertas. Encerre a sessão multiplayer antes desse teste: o jogo
pode bloquear Quit Game enquanto uma sessão está ativa.

Formato de arquivo:

```json
{
  "schemaVersion": 1,
  "name": "meu-teste",
  "instances": [1, 2],
  "timeoutSeconds": 300,
  "hookState": "keep",
  "screenshots": "half",
  "steps": [
    {"action": "observe"},
    {"action": "wait", "instance": 2, "pointer": "/state", "equals": "world", "seconds": 30},
    {"action": "assert", "instance": 2, "pointer": "/player/name", "equals": "Chico"}
  ]
}
```

O arquivo inteiro é validado antes dos passos. Campos desconhecidos, contas
não declaradas, valores inválidos e cenários sem assertions são recusados.
`instances` aceita `[1]`, `[2]` ou `[1,2]`; são permitidos até 200 passos e
`timeoutSeconds` de 1 a 3600.

`hookState` é `keep` (padrão) ou `reset`. Com `reset`, o `hooks reset` roda
nas instâncias declaradas que estiverem abertas antes do primeiro passo. Sem
`baseline`, roda de novo depois do último passo, porque com `baseline` os
jogos são fechados na limpeza. Instância parada é pulada, já que volta limpa
ao iniciar. Por isso, junto com `baseline`, que exige os jogos parados, o
`reset` não faz nada. Cada reset gera o evento `scenario_hooks_reset`. O padrão é `keep`
para não mudar os cenários que já existem.

| `action` | Campos adicionais |
| --- | --- |
| `launch` | `instance`; requer ambiente já preparado e pad existente |
| `enter` | `instance`, `character` opcional; usa o personagem configurado se omitido |
| `leave` | `instance` |
| `input` | `instance`, `command`, por exemplo `press a 90` ou `stick l 0 -1 500` |
| `goto` | `instance`, `x`, `z`, `radius` opcional, padrão 2 metros |
| `assert` | `instance`, `pointer`, `equals` |
| `wait` | Os campos de `assert`, mais `seconds` |
| `observe` | Nenhum |
| `screenshot` | Nenhum; falha se a captura solicitada falhar |
| `kill` | `instance`; recusa o modo `observe` do hook de morte |
| `teleport` | `instance`, `x`, `y`, `z`; ver "Mover o personagem" |
| `goto_map` | `instance`, `map` (hex), `x`, `y`, `z`; ver "Mover o personagem" |

Os pointers são relativos à observação da **instância identificada pelo
número**, não a um índice de array. São aceitos `/state`, `/serverConnected`,
`/player/name`, `/player/location`, `/pose/archetype`, `/p2pSessionVerified`,
`/session/role`, os `/character/*` listados acima e
`/hooks/hooks/<nome exato do hook>`. Consulte `observe` para os nomes dos hooks.
`equals` usa igualdade JSON; campo ausente ou `null` é inconclusivo. Esperar
`null` ou `unknown` não é uma assertion válida. Divergência conhecida é falha.
`wait` repete até atingir o valor ou vencer seu prazo.

Os prazos são propagados a navegação, menus, espera e chamadas da API. O
deadline é cooperativo: operações X11, acesso a disco e lançamento de
processos não são interrompidos no meio de uma chamada do sistema. Não há
garantia de interrupção em tempo real de um servidor X travado.

## Saves e restauração

```bash
ds2os-dev save backup --instance both --label majula-ready --json
ds2os-dev save list --json
ds2os-dev save restore majula-ready --instance both --stop --json
```

`backup` exige os clientes parados, save existente e associação não ambígua.
`both` exige as duas instalações. Labels aceitam letras ASCII, números,
underscore e hífen; não inclua `conta1-` nem `.ds3os` ao restaurar. A listagem
JSON devolve o `label` exato. Um backup não sobrescreve um label existente.
O save retail `.sl2` não é usado. `restore --stop` passa pelo guarda de
`session_live` (ver "A sessão"); `--force` só vale junto com `--stop`.

As cópias passam por arquivo temporário, `sync_all` e rename; uma falha de
cópia não trunca o save anterior. `restore` guarda uma cópia de resgate antes
de substituir cada destino. Os dois arquivos não constituem uma transação
atômica conjunta; falhas de disco podem exigir recuperação a partir dos
snapshots preservados.

Um cenário pode incluir `"baseline": "majula-ready"`. Nesse modo:

1. As instâncias selecionadas devem estar paradas antes de executar.
2. O harness preserva os saves originais em um label `run-...`.
3. Restaura o baseline e executa os passos, incluindo `launch` quando necessário.
4. Ao terminar ou falhar, fecha as instâncias selecionadas e restaura os originais.
5. Falha de limpeza torna o resultado `failed`; `originalSnapshot` informa o resgate.

SIGINT/SIGTERM são tratados cooperativamente para chegar à limpeza. SIGKILL,
queda de energia e encerramento abrupto do processo não executam essa etapa;
o label de resgate fica em `fixtures.json`. Sem `baseline`, o cenário conserva
os efeitos sobre os clientes e saves; não há restauração implícita.

## Exclusividade, evidências e limitações

Uma operação de controle mantém `control.lock` durante a sequência inteira,
incluindo foco e limpeza. Outro controlador recebe `busy` imediatamente.
Observações podem rodar simultaneamente; os pedidos de MemProbe são
serializados por instalação. O lock é liberado pelo sistema ao morrer o
processo. Não execute entradas diretas no socket por fora do harness durante
um cenário.

O pad exige `ok` explícito, tem timeout de leitura/escrita e limita cada hold
a 1–5000 ms. `pad seq` valida todos os passos antes de emitir o primeiro;
`wait` aceita até 30000 ms e `gap` até 5000 ms. A sequência pode ser cancelada.
O daemon neutraliza os controles entre conexões e o CLI tenta neutralizar ao
terminar o controle. Reinicie o daemon antigo para usar o novo protocolo.

Cada invocação cria `~/.local/share/ds2os-dev/runs/<id>/`, respeitando
`XDG_DATA_HOME` quando definido:

| Artefato | Conteúdo |
| --- | --- |
| `command.json` | Argumentos exatos e instante da invocação |
| `environment.json` | Ambiente, identidade declarada, processos e configuração em disco |
| `events.jsonl` | Progresso e eventos; cenários incluem observações e verdicts por assertion |
| `result.json` | Mesmo contrato emitido em stdout com `--json` |
| `logs/` | Trechos escritos durante a operação, com offsets e indicação de rotação/truncamento em `index.json`: o log do servidor, o de cada instância e **todo** `DS2_*.log`/`DS2OS_*.log` das duas instalações |
| `manifest.json` | Nos cenários: `harnessBuild`, SHA-256 dos binários em disco e recibos disponíveis |
| `scenario.json`, `fixtures.json` | Cenário executado e identificação dos snapshots, quando usados |
| `step-*/`, `failure/`, PNGs | Capturas por instância com nomes únicos |

Cada trecho de log é limitado a 1 MiB. Arquivo ausente ou ilegível aparece no
índice. Rotação por troca de inode ou redução do tamanho é detectada; truncar
e reescrever um arquivo além do offset entre duas leituras pode não ser
detectado. Capturas automáticas de evidência registram seus próprios erros;
uma assertion de estado não depende de um servidor X acessível. Use uma etapa
`screenshot` quando a existência da imagem for parte do contrato do teste.

Para usar `bootId` e os recibos, compile o **injector Windows**, instale a DLL
nova nas duas pastas e relance os jogos. O recibo `DS2_Harness.json` contém
resultados de instalação e configuração daquele boot; a última coluna de
`DS2_Nav.txt` identifica o mesmo boot. A partir do injector de 14/09, a linha do
Nav ganha três campos depois do boot: `x y z` de `*(ctx+0xd0)+0x90`, os pés do
personagem local. Leitores que contam campos a partir do começo não mudam. DLLs anteriores ainda atendem MemProbe
com rótulos únicos e publicam posição, mas `hooks` fica `null`. SHA-256 no
manifesto identifica o arquivo em disco; não prova que um processo aberto
antes da cópia carregou esse arquivo.

`players.souls`, `deathCount` e `multiplayCount` são `null`: o servidor DS2 não
implementa essas medições. `watch` lê, com a tabela do `timeline`, os logs de
hook das duas instalações e o do servidor, e avisa morte e custo, morte
cancelada ou recusada, renascer, warp e motivo, pedido de fim de sessão,
membros do canal, crash, placa e cada `RequestNotify*`. Também identifica
jogadores por Steam ID e anuncia entrada, troca de área e
desconexão/perda/recuperação da API. Linhas de hook sem relógio recebem a hora
da leitura, com granularidade de 1,5 s. Sem o hook de morte, morte não se prova.

O cenário de respawn completo depende de uma fonte positiva para morte
(`kill`), renascimento **no mundo do host** e interação entre peers após a
volta (`p2pSessionVerified`). HUD, papel de fantasma, PID vivo, posição na
fogueira ou presença na API isoladamente não aprovam esse teste.

Os testes automatizados do crate exercitam parsing, correlação, assertions,
locks, falhas do protocolo do pad, cópia de saves e contrato CLI. Eles não
substituem a execução de `menu-roundtrip` com os jogos reais nem a validação
dos hooks em Windows/Proton.
