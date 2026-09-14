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
| `server_api` | A API responde e todo Steam ID listado é decimal de 17 dígitos |
| `identities_distinct` | As duas contas configuradas são diferentes |
| `identity` | A conta tem um SteamID64 decimal configurado |
| `install` | A instalação da conta foi resolvida |
| `game_exe` | O executável que o probe confere (`installs[].gameExe`) é o 1.03 |
| `injector_installed` | SHA-256 do `Injector.dll` instalado contra o de origem (`~/Downloads/injector`) |
| `logs_size` | Logs `DS2*.log` da instalação; `warning` acima de 256 MB |
| `game_processes` | Exatamente um `DarkSoulsII.exe` no prefixo da conta |
| `memprobe` | Uma consulta real de `locate`: `data.state`, `data.reason`, `data.latencyMs`; `warning` acima de 1500 ms |
| `receipt` | `DS2_Harness.json` é do mesmo boot que `DS2_Nav.txt` e todos os hooks instalaram |
| `config_drift` | O que o boot aberto recebeu (`configured` do recibo) contra o `Injector.config` atual: `autoRematch`, `forceZone`, `removeFog`, `seamless`, `timer` |
| `api_identity` | A API lista a conta; no mundo sem registro é `problem` (offline ou outra conta) |
| `extra_game_processes` | Nenhum `DarkSoulsII.exe` fora dos prefixos das instâncias |
| `pad` | O pad está no ar; jogo aberto sem pad é `warning` |
| `wine_orphans` | `xalia.exe`/`winedevice.exe` de prefixos sem jogo |
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
| `p2pSessionVerified` | Reservado para uma prova de interação entre peers; atualmente `null` |
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

Os pointers são relativos à observação da **instância identificada pelo
número**, não a um índice de array. São aceitos `/state`, `/serverConnected`,
`/player/name`, `/player/location`, `/pose/archetype`, `/p2pSessionVerified` e
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
O save retail `.sl2` não é usado.

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
| `logs/` | Trechos escritos durante a operação, com offsets e indicação de rotação/truncamento em `index.json` |
| `manifest.json` | Nos cenários: SHA-256 dos binários em disco e recibos disponíveis |
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
`DS2_Nav.txt` identifica o mesmo boot. DLLs anteriores ainda atendem MemProbe
com rótulos únicos e publicam posição, mas `hooks` fica `null`. SHA-256 no
manifesto identifica o arquivo em disco; não prova que um processo aberto
antes da cópia carregou esse arquivo.

`players.souls`, `deathCount` e `multiplayCount` são `null`: o servidor DS2 não
implementa essas medições. `watch` usa os logs de warp disponíveis, identifica
jogadores por Steam ID e anuncia desconexão/perda/recuperação da API. Um hook
de warp desativado ou uma morte que não dispara warp não produz prova de morte.

O cenário de respawn completo depende de uma fonte positiva para morte,
renascimento **no mundo do host** e interação entre peers após a volta. O
campo `p2pSessionVerified` fica `null` enquanto essa fonte não existir. Um
teste que exigir `true` será inconclusivo; HUD, papel de fantasma, PID vivo,
posição na fogueira ou presença na API isoladamente não aprovam esse teste.

Os testes automatizados do crate exercitam parsing, correlação, assertions,
locks, falhas do protocolo do pad, cópia de saves e contrato CLI. Eles não
substituem a execução de `menu-roundtrip` com os jogos reais nem a validação
dos hooks em Windows/Proton.
