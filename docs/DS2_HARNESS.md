# The test harness: ds2os-dev

The harness drives two local DS2 installations and records what it managed to
prove. The implementation lives in `Source/LoaderLinux/crates/ds2os-dev/src`.
The JSON contract described here is version **1**.

## Building and preparing

At the repository root:

```bash
~/.cargo/bin/cargo build --manifest-path Source/LoaderLinux/Cargo.toml -p ds2os-dev
~/.cargo/bin/cargo test --manifest-path Source/LoaderLinux/Cargo.toml -p ds2os-dev
```

In the examples below, `ds2os-dev` means
`Source/LoaderLinux/target/debug/ds2os-dev`. The CLI uses Steam, Proton, X11,
`curl`, `xwininfo` and `/dev/uinput` depending on the operations it runs.

Configure each account's **17-digit decimal** Steam ID64. The numbers below are
examples; replace them with the IDs of the two accounts used in the test:

```bash
ds2os-dev game identity --instance 1 76561198000000001
ds2os-dev game identity --instance 2 76561198000000002
ds2os-dev game character --instance 1 Samuel
ds2os-dev game character --instance 2 Chico
ds2os-dev doctor --json
ds2os-dev up --seamless --auto-rematch --json
```

The association lives in `~/.local/share/ds2os-dev/config.json`, in the
`steamIds` field. It is an explicit operator setting, not a discovery of the
active Steam account. `loginusers.vdf` does not prove who is logged in now.
The harness uses that association to select the connection in the API and the
hexadecimal save directory. The same ID on both accounts is refused by the
configuration command and by the scenarios.

`up` requires both installations. For only one, use `server up`,
`game prepare`, `pad start`, `game launch --instance 1` and
`game enter --instance 1`. The pad has to exist **before** the game starts.

`up` rewrites `Injector.config` from that invocation's flags. The injector
reads configuration and DLL when the process launches; changing the file with
the game open does not update the loaded hooks. Use a relaunch to apply a new
DLL or configuration. `reload` is for server changes.

## Hygiene: logs, rescue copies, Wine orphans and screenshots

**Hook logs.** The injector does not rotate its own logs; the timer's log
reached 412 MB. `game prepare` (and therefore `up`) renames every `DS2*.log` above
8 MB to `<nome>.1` in the installation whose game is **closed**, replacing the
previous `.1` (one generation only). With the game open nothing is moved and
the item comes out as `skipped_running`. Each installation appears in the
`logs_rotated` event (`name`, `bytes`, `action`). A log rotated during the run
itself appears in the log index as `absent`.

**Rescue copies.** `save restore` keeps the replaced save as
`antes-de-<label>-<runId>`, with the id of the run that did it (the evidence
directory). The old ones had a `AAAAMMDD-HHMMSS` stamp there instead.

```bash
ds2os-dev save prune --keep 5 --dry-run    # o que sairia
ds2os-dev save prune --keep 5              # apaga
```

`prune` deletes, per account, the rescue copies beyond the `--keep` newest by
modification date. Only a name `conta<N>-antes-de-<label>-` followed by a
stamp or a runId counts as a rescue: a chosen label that starts with
`antes-de-` (like `antes-de-nivel1`) never goes. `--pattern` has to start with
`antes-de-` (`pattern_not_rescue`). `save list` shows the chosen labels and
summarises the rescues per account; in the JSON each snapshot has `rescue`.

**Wine orphans.** `up`, when no `DarkSoulsII.exe` is open, ends by pid the
orphans `doctor` lists under `wine_orphans` (SIGTERM, SIGKILL after 3 s, and
it only counts as ended when the pid is no longer that process) and records
`orphans_cleared` with the X11 connections before and after. An orphan is
`xalia.exe` in a prefix with no game, or `winedevice.exe` in a prefix
**without `services.exe`**. A `winedevice.exe` whose Wine session is alive
hosts that session's drivers, the pad's `winebus.sys` among them, and
`services.exe` does not bring it back: on 14/09 the first version of this
cleanup killed both of prefix 1's (alive since 13/09, with the main Steam
hanging off it) and the game launched next sat at "PRESS START" without seeing
the pad.

**Screenshots.** `game shot --scale 0.5` records at half resolution (the
average of each 2×2 block; an odd row/column is discarded); the default for
`game shot` and for `pad seq --shot` is still `1`. In scenarios the default is
half scale; `"screenshots": "full"` records at the window's resolution.

## The injector from CI: `injector fetch`, `check` and `status`

`Injector.dll` only builds with MSVC, so it only exists on CI
(`.github/workflows/injector-linux.yml`, artifact `injector`). The cycle for a
change in a hook is:

```bash
ds2os-dev injector check              # sintaxe dos .cpp alterados, antes do push
git push                              # o CI constrói
ds2os-dev injector fetch --json       # espera o run, baixa, manifest
ds2os-dev game stop --instance both   # (sem sessão viva)
ds2os-dev up --seamless ...           # prepare copia a DLL, relança
ds2os-dev injector status             # cada jogo diz o build que carregou
```

**The receipt says the commit.** CI passes `-DDS2OS_BUILD_SHA=${{ github.sha }}`;
`DS2_Harness.json` gains `"build"` and `observe` shows it in `hooks.build`. A
local build says `"unknown"`; a DLL from before the field has no `build`.

**`fetch`** picks, by default, the branch's newest run whose commit has the
same injector sources as HEAD (`git diff --quiet <sha> HEAD --` over
`Source/Injector`, `Source/InjectorLauncher`, `Source/Shared`,
`Source/ThirdParty/detours` and the workflow). That way a harness-only commit
on top of an injector push still finds the right run; with no such run it is
`no_run_for_head`. `--latest` takes the branch's newest and `--run ID` a
specific one; both report `sameCodeAsHead`. A run in progress is polled every
15 s up to `--seconds` (default 1500); a conclusion other than `success` is
`injector_build_failed`.

It downloads into `~/Downloads/injector.new`, checks that `Injector.dll` and
`Injector.exe` exist and start with `MZ`, writes `manifest.json`
(`runId`, `headSha`, `branch`, `fetchedAt`, `files` with each one's SHA-256)
and only then rotates: `injector.prev` is deleted, `injector` becomes
`injector.prev`, `injector.new` becomes `injector`. A run that is already
there, with the same hashes, is not downloaded again (`alreadyFetched`). It
cancels the `ci.yml` runs for the same commit (and for HEAD) and confirms they
reached `completed` (`ciCancelled`; whatever it could not confirm becomes a
warning).

**`fetch` never installs**: `game prepare` overwrites the DLL of an open game.
`data.installs[]` carries, per instance, `installedSha256`, `equalsSource`,
`running`, `bootId`, `build` and `buildState`; `needsPrepare` means some
installed DLL differs from the downloaded one, `needsRelaunch` means some open
game has a different DLL or a `buildState` other than `current`.

| `buildState` | Meaning |
| --- | --- |
| `current` | This boot's receipt says the manifest's commit |
| `other` | Another commit |
| `unknown` | `"build": "unknown"`, a local build |
| `unrecorded` | The receipt has no `build`: a DLL older than the field |
| `noReference` | No manifest, or no receipt for this boot (game stopped) |

**`status`** (not exclusive) shows the source directory, the manifest (or
`null` when the directory did not come from `fetch`), whether the source DLL
is the manifest's, and the same `installs`, `needsPrepare` and `needsRelaunch`.

**`check`** is **syntax with mingw, not an MSVC build**.
`x86_64-w64-mingw32-g++ -fsyntax-only -fpermissive -w` on each `.cpp`, with:
`__try` rewritten as `if (1)`, `__except (filtro)` as `else` (the filter's line
breaks kept, so the line numbers line up),
`_ReturnAddress()` as `__builtin_return_address(0)`, a `detours.h` with
declarations only and a `Windows.h` that includes `<windows.h>`. An `-include`
that redefines `__try` does not work: libstdc++ uses `__try` in its own macros.

With no arguments, it checks the `Source/Injector` `.cpp` files changed against
HEAD or untracked, plus the ones that include a changed `.h`; nothing changed
is `nothing_to_check` (inconclusive). Files on the command line, or `--all`
(27 files, ~10 s in parallel). A file with an error is also compiled in HEAD's
version (with the tree's headers) and only the errors whose message HEAD does
not have count: `Entry.cpp` (`::main`), `DS2_LogProtobufsHook.cpp` (missing
`<atomic>`, which MSVC brings along) and `ReplaceServerPortHook.cpp` already
fail under mingw and show up as `ok` with `preexisting`. A new error is
`syntax_errors` (code 1) with `files[].errors[]` `{line, message}`.

`check --self-test` is the positive signal: it checks `DS2_CrashHook.cpp`
intact (it has to come out `ok`) and with a broken line at the end (it has to
point at the error on that line). A clean `check` without the self-test does
not prove the compiler was looking.

`doctor` gained `injector_build` per open instance: the receipt's `build`
against the source manifest's `headSha`, `warning` for another commit, a local
build or a DLL older than the field.

## Results for the LLM

`--json` is global: it works before or after the subcommand. stdout contains a
single object at the end; progress messages go into the events file.

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

| Exit code | Result | Interpretation |
| --- | --- | --- |
| 0 | `passed` | The command met its contract; in a scenario, every assertion passed |
| 1 | `failed` | An action, assertion, configuration, cancellation or persistence failure |
| 2 | `inconclusive` | The observation/assertion could not obtain the evidence it needed |

`harnessBuild` says which code produced the result: `commit` is HEAD at build
time and `dirty` marks uncommitted edits in `ds2os-dev` or `ds2os-core` in
that build (`unknown`/`false` when built without git). The binary in
`target/debug` survives checkouts and edits; a result without this does not say
whether it came from the current code.

`errorCode` extracts the stable prefix of errors such as `busy`, `timeout`,
`instance_unresolved`, `wrong_character`, `partial_failure` and
`cleanup_failed`. Errors with no prefix use `operation_failed`. The `error`
field keeps the human context. Clap's own syntax errors, `--help` and
`--version` happen before the run is recorded and follow the parser's normal
interface.

**Migration:** the old fields of `status --json` and `doctor --json` are now
inside `data`. `doctor --json` returns code 1 when there are problems.
`status` is an inventory: code 0 does not mean there is a co-op session.
`up` aggregates the accounts' failures and returns code 1 if any of them does
not reach the required state. `up --no-enter` waits for confirmation of the
title screen; a PID created on its own does not approve the operation.
Screenshots that fail also produce an error.

## `doctor`: the chain of proof

```bash
ds2os-dev doctor --json
```

`doctor` does not only list what is installed: it exercises every link a test
depends on, with whatever is running. On 13/09 it said "all ready" while every
`game enter` failed, because it checked files and both defects were one step
further on (the executable read from the wrong directory, and the account
written in another notation by the API).

`data` carries `environment`, `problems` (the environment's, in the old
format), `checks`, `summary` and `ok`. Each item in `checks` has `name`,
`instance` (when it belongs to an account), `status`, `detail`, `fix` and
`data`:

| `status` | Meaning |
| --- | --- |
| `ok` | The link was exercised and answered as expected |
| `warning` | It works, but something will bite: an installed DLL different from the source one, huge logs, Wine orphans |
| `problem` | The link is broken; `doctor` ends with code 1 and `errorCode` `environment_not_ready` |
| `skipped` | It could not be exercised (instance stopped, server stopped, probe lock busy); it **never** counts as `ok` |

| `name` | What it exercises |
| --- | --- |
| `environment` | Each problem from `environment.problems()` (Steam, game, Proton, server, injector) |
| `harness_build` | The binary is from the commit the repository is on and no crate source is newer than it; otherwise `warning` |
| `server_api` | The API answers and every Steam ID listed is 17-digit decimal |
| `identities_distinct` | The two configured accounts are different |
| `identity` | The account has a decimal SteamID64 configured |
| `install` | The account's installation was resolved |
| `game_exe` | The executable the probe checks (`installs[].gameExe`) is 1.03 |
| `injector_installed` | SHA-256 of the installed `Injector.dll` against the source one (`~/Downloads/injector`) |
| `injector_build` | This boot's receipt `build` against the source `manifest.json`'s `headSha` (`injector fetch`) |
| `logs_size` | The installation's `DS2*.log` logs; `warning` above 256 MB |
| `game_processes` | Exactly one `DarkSoulsII.exe` in the account's prefix |
| `memprobe` | A real `locate` query: `data.state`, `data.reason`, `data.latencyMs`; `warning` above 1500 ms |
| `receipt` | `DS2_Harness.json` is from the same boot as `DS2_Nav.txt` and every hook installed |
| `config_drift` | What the open boot received (the receipt's `configured`) against the current `Injector.config`: `autoRematch`, `forceZone`, `removeFog`, `seamless`, `timer` |
| `api_identity` | The API lists the account; in the world with no record it is `problem` (offline or another account) |
| `extra_game_processes` | No `DarkSoulsII.exe` outside the instances' prefixes |
| `pad` | The pad is up; an open game with no pad is `warning` |
| `wine_orphans` | `xalia.exe` from prefixes with no game and `winedevice.exe` from prefixes with no `services.exe` |
| `x11_clients` | X11 connections in `/proc/net/unix`; `warning` from 200 on (Xorg refuses above 256) |

`config_drift` is the trap in `up`, which rewrites `Injector.config` with its
own flags: the file changes, the open game keeps the config it launched with.
`doctor` is not exclusive and runs alongside a controller; with the game open
it writes a read MemProbe request, like `observe`.

## Observation and identity

```bash
ds2os-dev observe --instance both --json
ds2os-dev where --instance 2 --json
ds2os-dev players --json
```

`observe` combines the API with each installation's local readings. In
`data.instances[]` you get:

| Field | Source and meaning |
| --- | --- |
| `instance`, `steamId`, `identitySource` | The account declared in the harness |
| `processes[].pid`, `startTicks` | The process in the Proton prefix and its start instant in Linux ticks |
| `state` | `title`, `loading`, `world` or `unknown` |
| `stateReason` | Why `state` is what it is; see the table of reasons below |
| `pose`, `poseAgeMs` | Position, orientation, tick and archetype published by the injector |
| `bootId` | Identifier of the injector's load; `null` with an old DLL |
| `player`, `serverConnected` | The API record filtered by Steam ID; `null` when not verifiable |
| `hooks` | Receipt of the hooks installed in the same boot as the telemetry |
| `p2pSessionVerified` | Only with `--session`: `true` when the session exists and data crossed between the peers; see "The session" |
| `session` | Only with `--session`: role, members, machine states and this instance's channel counters |
| `problems` | Reasons why some observation could not be confirmed; an `unknown` appears as `state_unknown: <motivo>` |

`observedAtMs`, `serverObservedAtMs` and `durationMs` make it visible when the
sources were queried. The sources are sampled one after another, not at one
atomic instant. The age of the API's answer is not the age of the last packet
the game sent.

The title byte is only read for the 1.03 executable known by the SHA-256 of
`ds2os-core::exe::DS2_SOTFS_1_03`. The executable checked is the one the
environment resolved for the installation (`installs[].gameExe`, which in
Scholar of the First Sin lives in `Game/`); the request files sit at the root,
next to the injector. Byte 1 confirms the title; zero with a valid, advancing
position confirms the world; zero with no player means loading. Everything else
is `unknown`, and the reason comes with it:

| Reason | State | Meaning |
| --- | --- | --- |
| `title_flag_set` | `title` | The title byte read as 1 |
| `telemetry_advancing` | `world` | Byte 0 and the published pose's tick advanced |
| `no_telemetry` | `loading` | Byte 0 and no pose published |
| `instance_stopped` | `unknown` | No game process in the account's prefix |
| `instance_missing` | `unknown` | No installation resolved for the account |
| `exe_missing` | `unknown` | The installation resolved no executable, or it cannot be read; no request is written |
| `unsupported_exe` | `unknown` | An executable other than 1.03; no request is written |
| `probe_busy` | `unknown` | Another reader held `DS2_MemProbe.lock` for the whole deadline |
| `request_write_failed` | `unknown` | The request or the lock could not be written; the error goes in `detail` |
| `request_not_consumed` | `unknown` | The request is still on disk: nothing in the game is reading requests (game still starting, DLL without the probe) |
| `no_answer` | `unknown` | The request was consumed, but no answer with the label appeared within the deadline |
| `malformed_answer` | `unknown` | An answer with the label, in a format that is not the byte dump |
| `unreadable` | `unknown` | The injector answered that the address is unreadable |
| `unexpected_byte` | `unknown` | A byte other than 0 and 1 |
| `telemetry_stalled` | `unknown` | Byte 0 with a published pose, but the tick did not move |
| `stale_telemetry` | `unknown` | Only in `observe`: no fresh sample from the same process and boot |
| `timeout`, `cancelled` | `unknown` | The deadline ran out, or the operation was cancelled, before asking |

The names are part of the contract: they can gain values, they do not change
meaning.

Between 13/09 and 14/09 the check looked for `DarkSoulsII.exe` at the root,
found nothing and every `locate` answered `unknown`: `game enter`,
`game leave`, `up`, `reload` and the scenarios waited out the deadline without
pressing a button.

Every MemProbe request gets a unique label. An earlier request's answer does
not satisfy it, and only the stretch of `DS2_MemProbe.log` written after the
request is read. There is one lock per installation for the harness's
requests; a busy lock is waited on until the query's deadline, and only then
becomes `probe_busy`. External tools that write straight into
`DS2_MemProbe.req` have to respect the same lock so they do not overwrite
requests.

### Navigation events

Every question to the game, every button and every wait on the API goes into
`events.jsonl`, so that a failure says where it stopped:

| `kind` | `data` |
| --- | --- |
| `locate` | `instance`, `state`, `reason`, `exe` (the executable checked), `label`, `requestWritten`, `byte`, `tickBefore`, `tickAfter`, `elapsedMs` and, when present, `detail` |
| `press` | `instance`, `command`, `ok`, `error` |
| `enter` | `phase: "world_without_api_record"` with `expected`, `listed` (the Steam IDs the API listed) and `offlineForMs`; `phase: "offline_recovery"` with `attempt` |

`world_without_api_record` is normal in the first seconds in the world: the
server takes 2 to 5 seconds to list someone who has just arrived (measured on
14/09: six events in a row, and the arrival right after). Only at 45 seconds
without the account does `enter` give up on the world and do
`offline_recovery`; `listed` with the account in another notation, or empty for
a long time, is what deserves attention.

A `timeout` or `leave_failed` from `game enter`, `game leave`, `reload` and
`up --no-enter` adds to the error the last state and reason read, how many
buttons were sent and how many API readings came back without the account —
for example `timeout: prazo da operação esgotado; último estado unknown
(request_not_consumed) há 2.0s; 0 tecla(s); 0 leitura(s) da API sem a conta`.
The `errorCode` is still `timeout`.

A pose requires complete, finite fields and a file at most 2 seconds old.
`observe` requires the tick to advance in the same boot and the processes to be
unchanged. `goto` also stops when the process/boot changes. Window mapping is
exclusively by process/prefix; there is no fallback to the window's position in
the X11 list.

`game enter` combines the local state with the expected account's API record
and, when configured, checks the character's name. It no longer takes the last
global line about a loaded character as evidence for the instance being driven.
The server publishes the account twice, `steamId` in hexadecimal
(`011000010afd1a3a`) and `steamId64` in decimal; the harness's `player.steamId`
is the decimal one, the same format as `game identity`. Comparing the
hexadecimal against the decimal found nobody: in the world, `enter` took a
player the API was already listing for offline, and burnt the deadline going
out to the title and back.
`game leave` is idempotent at the title and refuses to act when the world was
not confirmed. Its path assumes the menu opens on the Equipment tab, which is
where it opens after every load; a menu used by hand in the same boot leaves
another tab remembered (see `human`).

Stopping an instance the harness itself launched (a scenario's `launch` step)
left `proton run` as a zombie of the harness process, which did not `wait`:
`/proc/<pid>` was still there, and the scenario's cleanup failed with
"a instância 1 não morreu" until the harness exited. A zombie process now
counts as ended, and the harness reaps its own children. `reload` aborts before
restarting the server if it cannot confirm that some client left.

## The game's memory: `probe`

```bash
ds2os-dev probe --instance 1 "mod title 1614804 1" "chain chr 16148f0 d0 376" --json
ds2os-dev probe --instance 1 "pokeabs hp 7fffeb7b9fc8 00000000 64030000" --json
```

Each argument is a MemProbe command with a **name** in place of the label:
`abs|mod <nome> <endereço hex> <comprimento>`,
`chain <nome> <offset hex> <offsets hex separados por vírgula> <comprimento>`,
`pokeabs|pokemod <nome> <endereço> <bytes hex> [<bytes esperados>]`,
`pokechain <nome> <offset> <offsets> <bytes> [<esperados>]` and
`scan <nome> <valor hex> <largura 1|2|4|8> [<máximo>]`. The harness swaps the
name for a unique label, sends every line **in a single request** (each round
trip costs about half a second) and waits for all of their answers.

Two rules of the injector's parser that fail silently, and that the harness
checks before writing:

- the **length is decimal**: `200` is 0xc8 bytes. It sits between 1 and 16384;
  outside that the injector reads 0x100 bytes without saying so.
- the chain already starts by dereferencing the module address, so the list
  carries only the offsets after it: `chain chr 16148f0 d0 376` reads from
  `*(*(base+0x16148f0)+0xd0)`. A `0,` in front dereferences the context's
  vtable.

`data.answers[]` carries `name`, `label` and `kind`: `bytes` (`address`,
`bytes` in hex), `unreadable`, `chain_unresolved`, `poked` (`address`,
`before`, `wrote`), `poke_refused` (`expected`, `found`), `poke_invalid`,
`scan` (`hits`) or `malformed` (`line`). An answer only counts with every line
complete, and only the stretch of the log written after the request is read.

A write only passes when the injector says it wrote (`poked` with `wrote`
true); a refusal, invalid bytes or a failure end in `poke_not_written`.
Without a complete answer, the error is `locate`'s reason
(`request_not_consumed`, `no_answer`, `probe_busy`, `unsupported_exe`...).
Only reads run alongside a controller; a `poke*` line takes `control.lock`.
Every request goes into `events.jsonl` as a `probe` event, with the lines sent
and the answers.

## The event flags: `flags`

```bash
ds2os-dev flags                          # as duas contas: categorias e o que difere
ds2os-dev flags --flag 131000022         # uma flag em cada conta
ds2os-dev flags --instance 1 --group 13100 --json
```

It reads the `EventFlagManager` (`*(*(*0x1416148f0+0x70)+0x20)`, vftable
checked) in three or four MemProbe requests: the manager, the nodes of the 31
buckets level by level, and the bytes of each loaded category.
`data.instances[].categories` carries `category`, `bytes` and the ids that are
on in `set`; with `--flag`, `data.flag` says `true`/`false`/`null` (category
not loaded) per account; with both accounts, `data.differences` lists per
category what only one of them has on. Only the loaded map's categories and
the global ones (`10`, `20`) exist in memory. The layout and what the session
does with them are in `docs/DS2_WORLD_STATE.md`.

## The character: `character`

```bash
ds2os-dev character --instance both --json
ds2os-dev observe --instance 1 --character --json
```

It reads the local character in a single MemProbe request (four chains) and
returns, per instance, `character` with `address` (the character object, which
changes on every load), `hp`, `hpMax` (after hollowing), `souls`, `deaths`,
`hollow` (0 to 32), `hollowState` (0 human, 1 hollow), `role` (0 world owner, 1
white phantom), `position` and `bonfire` (`map` and `id` of the record death
sends you to; `null` when the record does not resolve). The addresses are the
ones `DS2_DeathInterceptHook` reads and writes and the ones measured in
`docs/DS2_SEAMLESS_COOP.md`, only for the 1.03 executable.

At the title or while loading there is no character: the error is
`no_character`. An object with HP outside `0..=hpMax` or a non-finite position
is `implausible_character`, not a character full of zeros.

`observe` only reads the character with `--character`, because it costs another
round trip; a reading problem appears as `character_unread: <motivo>`.
Scenarios accept `/character/hp`, `/character/hpMax`, `/character/souls`,
`/character/deaths`, `/character/hollow`, `/character/hollowState`,
`/character/role`, `/character/bonfire/id` and `/character/bonfire/map`; the
observation for those assertions already includes the character, and outside
`state: world` they are inconclusive.

## Human effigy: `human`

```bash
ds2os-dev human --instance both --json
ds2os-dev scenario run docs/scenarios/human.json --json
```

It burns a Human Effigy through the **Inventory** and only passes when the
character's memory says human: `hollow` (`param+0x1ac`) and `hollowState`
(`roles+0x3e`) at 0. The belt count never counts. A character that is already
human comes out with `verdict` `already_human` and **no** buttons. Outside the
world it is `not_in_world`.

The path is blind: start (1200 ms), right (Inventory), A (categories,
consumables first), A (the grid, which always opens on the first item, the
Estus), right (Human Effigy), A (submenu with Use selected), a half-scale
capture into `human-<N>/` (evidence, not an oracle), A. It waits up to 5 s for
the bytes to flip. On success it closes with B, B, left, B, which puts the tab
back on Equipment; on failure it leaves with B ×3 only and the error is
`still_hollow`. `data` carries `before`, `after` (`hollow`, `hollowState`,
`hp`, `hpMax`) and `presses`.

**The path only holds from the default tab.** The start menu remembers the tab
while the game runs and wraps in both directions (Inventory → Equipment →
System → …), so no sequence of buttons reaches a known tab starting from an
unknown one. A load (going to the title and back) puts the tab back on
Equipment, and the inventory grid always opens on the first item. Measured on
14/09 with Samuel and Chico, whose saves have the effigy as the second
consumable. A menu opened by hand after the last load, or a different inventory
order (the Y button sorts), sends the same buttons somewhere else.
`game leave` depends on the same thing: its path (start, RB ×5, down ×2, A,
left, A) assumes the menu opens on Equipment. That is why `human` ends by
putting the tab back.

`docs/scenarios/human.json` does the same with `input` steps over the
`pre-passo6` baseline: it launches and enters both accounts,
`assert /character/hollowState 1` before pressing anything (if the character is
already human the scenario fails without touching anything), the buttons with
`afterMs`, `screenshot`, `wait /character/hollowState 0` and
`assert /character/hollow 0`. With a baseline, a wrong button does not survive
the cleanup.

## The death hook: `death` and `kill`

```bash
ds2os-dev death --instance both status --json
ds2os-dev death --instance 1 mode respawn --json
ds2os-dev death --instance 1 feature copias off --json
ds2os-dev death profile set --mode respawn --feature copias=off
ds2os-dev kill --instance 1 --json
```

`death` writes orders into `DS2_Death.req` and only confirms on finding the
**exact echo** in the stretch of `DS2_Death.log` written after the order:
`=== modo: renascer na fogueira, pagando a morte ===`, `=== cobranca: feature
copias off ===`, or the `status` line. `=== nao entendi: ... ===` is
`death_order_refused`, never success. With no echo, `request_not_consumed`
(the hook did not read it: the game is starting, or the DLL has no hook) or
`no_answer`. The harness uses its own `DS2_Death.lock` per installation and
waits for an earlier request to disappear before writing its own.

The modes are `observe` (the game's death), `cancel` (the death does not
happen, nothing is billed) and `respawn` (the death is billed and the character
comes back to the bonfire in the same session). The parts of the bill are
`almas`, `hollow`, `contador`, `anel`, `mancha_online`, `estus`, `banner`,
`copias`, `fogueira_do_host` and `outro_mapa`. `status` returns `mode`,
`counters` and `features`.

**Every launch starts in `observe` with the whole bill on.** The profile
(`death profile set|show|clear`, kept in `config.json`) is reapplied by
`game enter` on every arrival in the world, after the boot's receipt shows
`DS2 Death Intercept` installed, and it only counts with the echo; failing here
makes the `enter` fail with that reason. The `enter` event with
`phase: "death_profile"` records the result.

`kill` requires the instance in a confirmed world, reads the mode with `status`
and the character, and zeroes the HP with `pokeabs` **passing the expected
bytes**. It passes only with the hook's lines for that death:

| Mode | Positive signal |
| --- | --- |
| `respawn` | exactly one `custos da morte (...)` followed by `renascer concluido em N quadros`; the `morte CANCELADA` between them is the hook holding the game's death back |
| `cancel` | `morte CANCELADA`, with no `custos da morte` |
| `observe` | `morte vista`; it requires `--real-death`, because it is the game's death (souls on the ground, a load and, in a session, the end of it) |

`data` carries `mode`, `before` and `after` (the character before and after),
`poke`, `deathLines` and `otherSide` (the `morte da copia RECUSADA` lines the
other installation wrote in the same window). A refused write is
`poke_not_written`; HP zeroed without the lines within 15 s is `inconclusive`.
Scenarios gain the `{"action": "kill", "instance": N}` action, which refuses
the `observe` mode.

## The session: `session`

```bash
ds2os-dev session --json
ds2os-dev observe --instance both --session --json
```

It samples both instances in parallel: `DS2_Channel.req`'s `status` twice, with
at least 2.5 s between them, and both session machines by vtable scan
(`NetSummonAcceptMultiplayCtrl` `0x1410d7998`, state at `+0x150`;
`NetSummonJoinMultiplayCtrl` `0x1410d7bd8`, state at `+0xf8`). It takes 4 to 5
seconds, which is why it only enters `observe` with `--session` and in the
assertions that ask for `/session/*` or `/p2pSessionVerified`.

The decision has two levels, and `data.assessment` says which one passed:

| Field | Requires |
| --- | --- |
| `objectsAgree` | the host with its controller at `0x10`, the guest with its own at 7, and both machines' channels listing the same two members, with the same one marked `(host)`, which are the two configured accounts |
| `peersExchange` | between the samples, the host's `enviados` rises with `falhas` unchanged **and** the guest's `recebidos` rises (the host announces the bonfire every 2 s over Steam P2P) |

`p2pSessionVerified` is `true` only with both; `false` only when neither
instance has a session on the channel nor an active controller; `null` in
everything else, with the reason in `problems`:

| Problem | Means |
| --- | --- |
| `session_objects_only` | the session exists and no data crossed; the host's controller has been measured at `0x10` minutes after the session ended |
| `channel_counters_stalled`, `host_loading` | counters stopped; while loading the host stops announcing |
| `members_disagree`, `not_our_pair` | the channels disagree, or there are more than the two accounts (three players are not testable on this machine) |
| `host_controller_inactive`, `guest_controller_inactive` | the session machine is not in the active-session state |
| `roles_incomplete` | one side shows a session or a controller and the other does not |
| `identity_mismatch` | the account the channel says it is running is not the configured one |

Two things measured on 14/09 that the harness handles:

- outside a session the channel publishes `eu=0000000000000000`: the hook only
  learns its own account when it queries a session. `samples[].second.me` stays
  `null`, and that is not a wrong account.
- the scan finds the injector's own needle, at a low address that appears in
  **both** scans and, after the second, contains the guest's vtable. An address
  present in both lists is discarded, and every candidate has its vtable
  re-read before the state counts.

`data.samples[]` keeps the two raw channel samples and each instance's
controllers; `data.assessment.instances[]` carries `role` (`host`, `guest`,
`none`, `unknown`), `members`, `hostState`, `guestState` and the deltas `sent`,
`failed`, `received`, `refused`. The command ends `passed` with a decision
(`true` or `false`) and `inconclusive` with `null`. The channel request uses a
harness `DS2_Channel.lock`, like `DS2_Death`.

### Ending the session: `session end`

```bash
ds2os-dev session end --json
```

Killing a client while the session is live is an illegal disconnect, and the
game counts it in the save. `session end` ends the session the way the game
accepts, a recipe measured on 14/09:

1. `session` decides who is host and who is guest. If no channel shows
   members, it ends `passed` with `data.ended: false`, `reason: no_session`.
   Incomplete roles are `inconclusive` (`session_unverified`).
2. It saves the host's `copias` and the guest's mode. Then it turns `copias`
   off on the host, puts the guest in `observe` and kills the guest
   (`kill --real-death`).
3. It passes when **both** instances' channels are left with no live session
   across two readings at least 2.5 s apart. The deadline is `--seconds`,
   default 60 (`session_still_live` when it runs out). It does not depend on
   the host's controller, which stays at `0x10` for minutes.
4. It puts `copias` and the mode back to their earlier values, even on
   failure, each confirmed by its echo. `data.restored` has one boolean per
   setting, and the errors go in `data.restoreErrors` (`restore_failed` if it
   does not go back).

The guest pays a real death: souls on the ground and hollow. Burn an effigy
before the next staging. `data.serverLines` carries the server's
`LeaveSession` and `LeaveGuestPlayer` lines, as information only: the server
logs only the first message of each type per connection, and without them comes
the `server_census_silent` note. With the current server, which logs every
`RequestNotify*` (see "The timeline"), the `Notify ...` lines appear in any
session. Measured: a session ended in 19.5 s, with both lines on the server.

### The `session_live` guard

`game stop`, `down` and `save restore --stop` ask each open instance's channel,
before closing **any** of them, whether there is a live session (members seen
within the last 5 s). With a session, they refuse with `session_live` and touch
nothing. The server does not go down on `down` either. `--force` closes anyway
and records the `stop` event with `phase: forced_with_session`.

A channel that does not answer (game starting, frozen, a DLL without the hook)
does **not** block. The stop goes ahead with the
`phase: session_check_unknown` event. The cleanup of a scenario with a
`baseline` does not refuse either: the save is restored right after, and the
disconnect goes away with it. That is recorded as the
`cleanup_kill_with_session` event.

## The hooks back to the arrival state: `hooks reset`

```bash
ds2os-dev hooks reset --instance both --json
```

A test that refuses a session reason, forces a map, arms a breakpoint or
changes the death bill leaves that in the open game, and the next test inherits
it. `hooks reset` undoes each item and only passes with the hook's echo. A
hook missing from this boot's receipt is `skipped`, and a missing receipt is
`receipt_missing`.

| Hook | Request | Confirmation |
| --- | --- | --- |
| `DS2 Seamless Session` | `clear`, `role any`, `status` | `=== recusando a mascara 00000000, papel -1 ===` |
| `DS2 Backread` | `unfocus`, `clear`, `status` | `pedido 00000000` and `foco 00000000` in the status header |
| `DS2 Trace` | `wpclear`, `clear`, `report` | `=== limpo ===` and then `=== 0 armados, ... ===` (`wpclear` writes nothing with no watchpoint armed, so it is not expected) |
| `DS2 Death Intercept` | `observe`, the ten bill parts at their boot defaults (all on except `mancha_online`), the saved `death profile` on top, `status` | the status's mode and bill parts equal to what was expected |

The result is the state a `game enter` leaves. That includes the death
profile, because the arrival applies it.

What cannot be undone is written down. `DS2_Backread` has no verb that
releases a `keep` before its deadline, so maps still with `forcado` other than
zero come back in `data.forced` with the `keeps_remain` warning. In a session,
those are the remote copy's keeps and they are expected. Hooks that did not
answer give `inconclusive` (`hooks_unanswered`). Hooks that answered something
else give `failed` (`hooks_not_reset`).

## The server's signs: `server wait --signs`

```bash
ds2os-dev server wait --signs 0 --seconds 90 --json
```

A killed client leaves its sign in the server's cache until the connection
expires, and that sign looks real. `server wait` reads the `Sign poll` lines
the server writes **after** the command started and passes when the most recent
one shows `N signs cached`. That number is the whole cache, not one player's.
`data.byPlayer` keeps each one's last poll.

The line only exists with `DS2_StickySigns` on. The server's default is off,
and the local `Saved/default/config.json` is on. With it off, the command is
always `inconclusive`. The server limits it to one per player every 10 s. In
practice it shows up every ~60 s per player (measured on 14/09), so the default
deadline is 90 s, and less than 60 s can miss the only poll of whoever is left.
No poll within the deadline gives `inconclusive`, never `passed`. A poll with
another number at the end of the deadline gives `failed` (`signs_remain`). The
command is read-only and runs alongside a controller.

## Moving the character: `teleport`, `bonfires`, `backread` and `goto-map`

```bash
ds2os-dev bonfires --instance 1 --json
ds2os-dev teleport --instance 1 --to-bonfire 0x7ba2 --json
ds2os-dev teleport --instance 1 --to 6.186,-18.517,209.053 --json
ds2os-dev goto-map --instance 1 --map 0a040000 --to 10.53,5.92,-16.25 --json
ds2os-dev backread --instance 1 load|focus <mapa> x y z|unfocus|clear|keep <i> <ms>|status --json
ds2os-dev scenario run docs/scenarios/teleport-roundtrip.json --json
```

`teleport-roundtrip.json` uses Samuel's coordinates in Heide and Majula: it
goes to the Cathedral, to Majula and back to Heide's first bonfire.

`bonfires` reads, in a single round trip, the record of the last bonfire and
the nodes of the loaded map's list: id, map and spawn point (the translation
minus 1.1 × the Z axis, the way respawning does it). The nodes are asked for as
paths from the context (`70,58,8`, `70,58,8,60`, …, up to 16), and the ones
past the end of the list come back `cadeia nao resolveu`.

`teleport` writes what the death hook's `TeleportLocal` writes:
`chr+0x90` and `+0xa0`, `motion+0x50`, the physics (`+0x80`, velocities `+0x60`
and `+0x70` zeroed, `+0x1c0`), the Havok body (`+0x250`, `+0x260`, `+0x1b0`,
`+0x1c0`, `+0x1a0`, 5 cm above the feet) and, last, the floor of the fall
controller (`*(chr+0xe0)+0xb0` `+0x20`). Without that last write, a 24.5 m fall
after the teleport has already killed the character. There are 13 writes:

1. One round trip reads the five objects and checks the vtables of the
   character (`0x1410e4bb8`) and of the body (`0x141126578`). A divergence
   gives `vtable_mismatch`, and nothing is written.
2. Another round trip does the 13 writes, **each with the bytes the read
   found**. The injector refuses the write if they changed (an address reused,
   or the character walking), and a refusal gives `teleport_refused`.
   With the character standing still, the 13 targets were measured identical
   across two reads.
3. It passes with 13/13, the character **settled** within 1.5 m horizontally
   and 1 m vertically of the target after 3 s (`not_arrived` outside that) and
   no `death_cost`, `death_cancelled` or `death_seen` line in `DS2_Death.log`
   in those 3 s (`died_after_teleport`).

The radius is what it is because the physics pushes the character out of
whatever it was put inside, and a bonfire's spawn sits inside the bonfire.
Measured on 14/09: Heide's Cathedral at 0.84 m, bonfire `0x7ba7` at 0.82 m,
12 m lower and with no fall damage.

`--to-bonfire` only accepts a bonfire from the loaded list
(`bonfire_not_loaded`: use `goto-map`).

`backread` talks to `DS2_Backread.req` and always returns the status with it.
`keep <i> <ms>` is not reversible: no verb releases a keep before its deadline,
and `keep i 0` on many indices fills the 8 slots (see `hooks reset`). An
echo only says the request was read, so `goto-map` waits for the effect:

1. `load <mapa>` until the hook's status lists the map at `estado 5` (in the
   log, `estado 4 -> 5`; ~505 ms measured). If it was already loaded, it comes
   out `alreadyLoaded`.
2. `focus <mapa> x y z` until `foco no mapa X em (...): celula N` with N other
   than -1 and -2.
3. `teleport` (without adding anything to y).
4. The physical contact under the character (`*(*(chr+0x100)+0x10)+0xe0`: type
   in the low nibble, 7 collision and 1 map object, the map index in bits 4..9)
   equal to the map's `[i]` in the status, across two readings in a row.
   Without that within 10 s it gives `inconclusive`, and the map and the focus
   stay requested.
5. `unfocus` + `clear`. After 4 s, the contact still has to be on the map
   (`left_map`).

Position is no proof because maps share coordinates: Heide's first bonfire sits
where Majula's sea rocks are. Measured on 14/09, Heide → Majula: contact
`0x3c17` (index 1), and Majula → Heide: `0xc7` (index 12), the same values as
the hook's respawn.

`where` shows, in `pose.live`, the character's feet as published by
`DS2_NavHook` (see below), and in `data.navLag` the instances whose published
pose is more than 1 m away from them. After the teleport to the Cathedral, the
old pose was 68 m behind, and only `live` was right. `goto` and
`goto --to-instance` still read that pose, so after a teleport they walk from
an old position.

`teleport` on its own proves position, not map: over coordinates that two maps
share, it can pass standing on the wrong map's floor. The one that checks the
contact is `goto-map`. The 3 s death check is negative and only holds with
`DS2 Death Intercept` in this boot's receipt.

## The timeline: `timeline`

```bash
ds2os-dev timeline --last 10m --json
ds2os-dev timeline --since 17:39:50 --kind death_cost,respawn_done,server_notify
ds2os-dev timeline --run <runId> --instance 2 --all
```

It merges, in order, the server log, both installations' hook logs and the
harness's `events.jsonl` into `data.entries[]`: `{atMs, at, resolutionMs,
source, instance, kind, line, more}`. Indented lines continue the entry above
(`more`). With no window, `--last 10m` applies. `--limit` (default 2000)
keeps the newest ones.

| Source | Clock | Resolution |
| --- | --- | --- |
| `server` | `AAAA-MM-DD HH:MM:SS`, local time, sanitised | 1000 ms |
| `death`, `channel`, `backread` | `HH:MM:SS.mmm` with no date | 1 ms |
| `session`, `seamless`, `respawn`, `crash`, `trace`, `rematch` | none | `atMs: null` |
| `harness` | `atMs` from each `events.jsonl` | 1 ms |

The date for the clocks with no date comes from the file's mtime, walking
backwards, or from the start of the run under `--run`, walking forwards. A
clock that goes back more than 12 h is a day rollover. Within the same server
second, the order is by source: a server line at `18:12:39.000` may have
happened after a hook line at `18:12:39.930`.

Lines with no clock do not fit in a window of hours. `--last`/`--since` leave
them out and say which sources wrote in the window
(`notes: untimed_sources`). `--run <id>` reads the stretches the run kept in
`logs/` and includes them at the end, with `at: null`. The timers, MemProbe and
the area probes are left out, because their clock is the process uptime or does
not exist.

By default only classified lines show up, without `sign_poll`,
`channel_status`, order echoes (`*_order`) or harness events. `--kind a,b`
picks types, `--all` shows everything and `--instance N` filters the hook logs
(the server's and the harness's stay).

| `kind` | Line |
| --- | --- |
| `death_cost`, `death_cancelled`, `death_seen`, `copy_refused`, `respawn_step`, `respawn_done`, `death_order` | `DS2_Death.log`: costs, death cancelled, death seen, copy refused, respawn steps and end, order echoes |
| `session_end_request`, `host_state`, `session_order` | `DS2_Session.log`: `fim de sessao pedido\|RECUSADO`, the host machine's states, echoes |
| `channel_members`, `channel_announce`, `channel_received`, `channel_status` | `DS2_Channel.log` |
| `backread_state`, `backread_release`, `backread_keep`, `backread_focus`, `backread_order` | `DS2_Backread.log` |
| `warp`, `warp_accepted`, `seamless_order` | `DS2_Seamless.log` |
| `crash`, `trace_hit`, `trace_order`, `rematch`, `respawn_order`, `hook_boot` | the other hook logs; `hook_boot` is the `=== ds2os ...` of each launch |
| `server_notify` | `Notify RequestNotify<tipo>: ...`, one per message |
| `server_notify_census` | `First DS2_Frpg2RequestMessage.RequestNotify*`, only the first per connection |
| `sign`, `sign_poll`, `disconnect`, `login` | signs created, removed and summoned; polls; connections ended; logins |

The DS2 server logs **every** `RequestNotify*` it receives
(`DS2_LoggingManager.cpp`) and one line when a lost client's sign finally
leaves the cache. A server older than that change has only the census, and in
that case `server_notify` stays empty. The server builds locally: `make Server`
in `intermediate/make`, applied with `server restart` with no players.

Measured on 14/09, on the same connection's second summon: `Notify
RequestNotifyJoinGuestPlayer` and `JoinSession` with no census line at all. A
death with a respawn inside the session gave `death_cost` → `death_cancelled` →
`respawn_step` → `respawn_done` in 17 ms, with no `RequestNotifyDeath` on the
server. A cancelled death does not notify, and that absence is the signal, not
a hole.

In a scenario, `step_started` and `step_finished` carry `logBytes`: the size of
each captured log at that instant, so each step's lines can be cut out.

## Scenarios

```bash
ds2os-dev scenario validate world-ready --json
ds2os-dev scenario run world-ready --json
ds2os-dev scenario validate docs/scenarios/menu-roundtrip.json --json
ds2os-dev scenario run docs/scenarios/menu-roundtrip.json --json
```

`world-ready` checks that both accounts are in the world and appear in the API.
It does not start the game and it **does not prove P2P, summoning or co-op
respawn**. `menu-roundtrip.json` tests going out to the title and back into the
world on both already-open instances. End the multiplayer session before that
test: the game may block Quit Game while a session is active.

File format:

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

The whole file is validated before the steps run. Unknown fields, undeclared
accounts, invalid values and scenarios with no assertions are refused.
`instances` accepts `[1]`, `[2]` or `[1,2]`; up to 200 steps and a
`timeoutSeconds` from 1 to 3600 are allowed.

`hookState` is `keep` (the default) or `reset`. With `reset`, `hooks reset`
runs on the declared instances that are open before the first step. Without
`baseline`, it runs again after the last step, because with `baseline` the
games are closed during cleanup. A stopped instance is skipped, since it comes
back clean when it starts. That is why, together with `baseline`, which
requires the games stopped, `reset` does nothing. Each reset produces the
`scenario_hooks_reset` event. The default is `keep` so as not to change the
scenarios that already exist.

| `action` | Additional fields |
| --- | --- |
| `launch` | `instance`; requires an already prepared environment and an existing pad |
| `enter` | `instance`, optional `character`; uses the configured character if omitted |
| `leave` | `instance` |
| `input` | `instance`, `command`, for example `press a 90` or `stick l 0 -1 500`; an optional `afterMs` (0 to 5000) waits after the button |
| `goto` | `instance`, `x`, `z`, optional `radius`, default 2 metres |
| `assert` | `instance`, `pointer`, `equals` |
| `wait` | The fields of `assert`, plus `seconds` |
| `observe` | None |
| `screenshot` | None; it fails if the requested capture fails |
| `kill` | `instance`; refuses the death hook's `observe` mode |
| `teleport` | `instance`, `x`, `y`, `z`; see "Moving the character" |
| `goto_map` | `instance`, `map` (hex), `x`, `y`, `z`; see "Moving the character" |

The pointers are relative to the observation of the **instance identified by
its number**, not to an array index. `/state`, `/serverConnected`,
`/player/name`, `/player/location`, `/pose/archetype`, `/p2pSessionVerified`,
`/session/role`, the `/character/*` listed above and
`/hooks/hooks/<nome exato do hook>` are accepted. Check `observe` for the
hooks' names. `equals` uses JSON equality; a missing or `null` field is
inconclusive. Waiting for `null` or `unknown` is not a valid assertion. A known
divergence is a failure. `wait` repeats until it reaches the value or its
deadline runs out.

The deadlines are propagated to navigation, menus, waiting and API calls. The
deadline is cooperative: X11 operations, disk access and process launches are
not interrupted in the middle of a system call. There is no guarantee of
real-time interruption of a frozen X server.

## Saves and restoring

```bash
ds2os-dev save backup --instance both --label majula-ready --json
ds2os-dev save list --json
ds2os-dev save restore majula-ready --instance both --stop --json
```

`backup` requires the clients stopped, the save to exist and the association to
be unambiguous. `both` requires both installations. Labels accept ASCII
letters, numbers, underscore and hyphen; do not include `conta1-` or `.ds3os`
when restoring. The JSON listing returns the exact `label`. A backup does not
overwrite an existing label. The retail `.sl2` save is not used.
`restore --stop` goes through the `session_live` guard (see "The session");
`--force` only applies together with `--stop`.

The copies go through a temporary file, `sync_all` and a rename; a copy failure
does not truncate the previous save. `restore` keeps a rescue copy before
replacing each destination. The two files do not make up one joint atomic
transaction; disk failures may require recovery from the preserved snapshots.

A scenario can include `"baseline": "majula-ready"`. In that mode:

1. The selected instances must be stopped before it runs.
2. The harness preserves the original saves under a `run-...` label.
3. It restores the baseline and runs the steps, including `launch` when needed.
4. On finishing or failing, it closes the selected instances and restores the originals.
5. A cleanup failure makes the result `failed`; `originalSnapshot` reports the rescue.

SIGINT/SIGTERM are handled cooperatively so the cleanup is reached. SIGKILL,
power loss and an abrupt end to the process do not run that step; the rescue
label is in `fixtures.json`. Without `baseline`, the scenario keeps its effects
on the clients and the saves; there is no implicit restore.

## Exclusivity, evidence and limitations

A control operation holds `control.lock` for the whole sequence, including
focus and cleanup. Another controller gets `busy` immediately. Observations
can run at the same time; MemProbe requests are serialised per installation.
The lock is released by the system when the process dies. Do not send input
straight to the socket outside the harness during a scenario.

The pad requires an explicit `ok`, has a read/write timeout and limits each
hold to 1–5000 ms. `pad seq` validates every step before emitting the first;
`wait` accepts up to 30000 ms and `gap` up to 5000 ms. The sequence can be
cancelled. The daemon neutralises the controls between connections and the CLI
tries to neutralise when it finishes controlling. Restart an old daemon to use
the new protocol.

Each invocation creates `~/.local/share/ds2os-dev/runs/<id>/`, respecting
`XDG_DATA_HOME` when it is set:

| Artifact | Contents |
| --- | --- |
| `command.json` | The exact arguments and the instant of the invocation |
| `environment.json` | Environment, declared identity, processes and the configuration on disk |
| `events.jsonl` | Progress and events; scenarios include observations and a verdict per assertion |
| `result.json` | The same contract emitted on stdout with `--json` |
| `logs/` | Stretches written during the operation, with offsets and a note of rotation/truncation in `index.json`: the server's log, each instance's and **every** `DS2_*.log`/`DS2OS_*.log` from both installations |
| `manifest.json` | In scenarios: `harnessBuild`, the SHA-256 of the binaries on disk and the receipts available |
| `scenario.json`, `fixtures.json` | The scenario that ran and the identification of the snapshots, when used |
| `step-*/`, `failure/`, PNGs | Per-instance captures with unique names |

Each log stretch is limited to 1 MiB. A missing or unreadable file appears in
the index. Rotation by a change of inode or by the size shrinking is detected;
truncating and rewriting a file past the offset between two readings may not
be. Automatic evidence captures record their own errors; a state assertion does
not depend on a reachable X server. Use a `screenshot` step when the image's
existence is part of the test's contract.

To use `bootId` and the receipts, build the **Windows injector**, install the
new DLL into both folders and relaunch the games. The `DS2_Harness.json`
receipt contains that boot's installation and configuration results; the last
column of `DS2_Nav.txt` identifies the same boot. From the 14/09 injector on,
the Nav line gains three fields after the boot: `x y z` from
`*(ctx+0xd0)+0x90`, the local character's feet. Readers that count fields from
the start do not change. Earlier DLLs still answer MemProbe with unique labels
and publish a position, but `hooks` stays `null`. The SHA-256 in the manifest
identifies the file on disk; it does not prove that a process opened before the
copy loaded that file.

`players.souls`, `deathCount` and `multiplayCount` are `null`: the DS2 server
does not implement those measurements. `watch` reads, with `timeline`'s table,
both installations' hook logs and the server's, and reports a death and its
cost, a death cancelled or refused, a respawn, a warp and its reason, a session
end request, channel members, a crash, a sign and every `RequestNotify*`. It
also identifies players by Steam ID and announces arrival, area change and API
disconnect/loss/recovery. Hook lines with no clock get the time of the reading,
with 1.5 s granularity. Without the death hook, a death cannot be proven.

The complete respawn scenario depends on a positive source for the death
(`kill`), a respawn **in the host's world** and peer interaction after coming
back (`p2pSessionVerified`). A HUD, a phantom role, a live PID, a position at
the bonfire or presence in the API, on their own, do not approve that test.

The crate's automated tests exercise parsing, correlation, assertions, locks,
pad protocol failures, save copying and the CLI contract. They do not replace
running `menu-roundtrip` with the real games, nor validating the hooks under
Windows/Proton.
