# Working in this repo

A fork of [ds3os](https://github.com/TLeonardUK/ds3os) taught to run **Dark
Souls II: Scholar of the First Sin** (Steam appid 335300) against a private
server. The fork was detached, so there is no upstream remote unless you add
one.

The work is half server, half reverse engineering of the game client. Most of
what costs time here is the second half, so most of this file is about that.

Written material lives in `docs/`; `docs/README.md` is the index.
`docs/DS2_MAJULA_MULTIPLAYER.md` is the closest thing to a reference for what
the injector actually does and why.

**Everything written into the repo is in English**: documentation, commit
messages, code comments, log strings and the harness's own output. Talking to
the user is in Portuguese; what gets committed is not. Parts of the injector
and of `docs/` are still in Portuguese from earlier work — leave them be unless
you are already editing that passage, and write the new text in English.

**The standing goal is seamless co-op**, and it is written down rather than
carried in anyone's head. `docs/DS2_SEAMLESS_COOP_DESIGN.md` is the brief —
the host owns the world, each player keeps their own save, and everything that
follows from that. `docs/DS2_SEAMLESS_COOP_TASKS.md` is the work left, in
dependency order; keep it true as pieces land, and read both before starting
anything co-op. Nothing below M1 there is reachable until a death stops
tearing the session down.

## The harness: `ds2os-dev`

The current CLI and scenario contract is documented in
[docs/DS2_HARNESS.md](docs/DS2_HARNESS.md). Read it before changing the harness
or writing a new automated test. A runnable menu regression lives in
[docs/scenarios/menu-roundtrip.json](docs/scenarios/menu-roundtrip.json).

`Source/LoaderLinux/target/debug/ds2os-dev` drives everything. Reach for it
before writing any ad-hoc script for screenshots, input, window focus or
server control — it already solves the fiddly parts, and there is no `xdotool`
or `wmctrl` on this machine.

```
cargo build -p ds2os-dev      # from Source/LoaderLinux
```

`cargo` is **not on PATH** by default; it is at `~/.cargo/bin/cargo`.

| command | what it does |
| --- | --- |
| `doctor` | exercises the chain of proof — executable, a real MemProbe round trip, hook receipt against `Injector.config`, accounts against the API — as `ok`/`warning`/`problem`/`skipped` checks |
| `status` | inventory of server, ports, processes and logs; not a gameplay assertion |
| `observe --instance both --json` | fresh per-instance state, pose, Steam identity, API record, hook receipts and unknowns |
| `scenario validate <file\|world-ready> --json` | validates the entire scenario without executing steps |
| `scenario run <file\|world-ready> --json` | assertions, step evidence and optional save baseline/cleanup |
| `game identity --instance N <SteamID64>` | binds an instance to its expected account; required for `enter` and scenarios |
| `up` / `down` | `up` requires both installations and confirms arrival, and with no game open ends the Wine orphans first; `down` stops the server and second instance |
| `reload` | restarts the server and puts everyone back in the world, without closing the game |
| `server up\|down\|restart\|status` | the local server on its own |
| `server wait --signs N` | passes on a `Sign poll` line written after it started saying `N signs cached`; no poll is inconclusive |
| `up --seamless --party [--party-password X] [--party-host 2]` | the guest account (2 by default, 1 with `--party-host 2`) keeps its white sign down and the host summons it by itself (`DS2PartyGuest`/`DS2PartyAccept`/`DS2PartyPassword`); a session forms and re-forms with no input, and the server matches party signs only with the same password. `DS2_Party.req` takes `pausa`/`retoma` |
| `session end` | ends the session the legal way (host `copias off`, guest dies in observe), waits for both channels to drop it, restores both settings; pauses `DS2_Party` first so the session stays ended |
| `hooks reset --instance both` | Session, Backread, Trace and Death back to a fresh arrival's state, each confirmed by its echo; scenarios opt in with `"hookState": "reset"` |
| `game prepare` | writes `Injector.config`, the wrapper, and copies the injector binaries into **both** installations; with that game closed, rotates any `DS2*.log` above 8 MB to `.1` |
| `injector fetch` | waits for the CI run that built HEAD's injector sources, downloads it into `~/Downloads/injector` (previous kept as `injector.prev`), writes `manifest.json`, cancels `ci.yml`; says `needsPrepare`/`needsRelaunch`, never installs |
| `injector check [files] \| --all \| --self-test` | mingw **syntax** check of injector `.cpp` changed against HEAD, counting only errors HEAD's version lacks; not an MSVC build |
| `injector status` | the manifest, each installation's DLL and the `build` commit each running game's receipt announces |
| `game launch\|stop --instance 1\|2\|both` | starts or stops an instance, through Proton, without Steam |
| `game enter\|leave --instance <1\|2>` | walks the menus from the title into the world, and back out |
| `game focus <1\|2>` / `game shot [--scale 0.5]` | window focus and per-window PNG capture; scenarios capture at half scale unless `"screenshots": "full"` |
| `players --json` | API records; unsupported souls/death/session counters are `null` |
| `watch` | deaths and what they cost, respawns, warps, session end requests, channel members, crashes, signs and each `RequestNotify*`, from every hook log and the server, plus API presence |
| `timeline --last 10m \| --since HH:MM \| --run <id>` | server, hook and harness logs merged, ordered and classified; hook logs without a clock only appear with `--run` |
| `where` | where each character is standing, from the game's own memory |
| `character --instance N` | the local character from memory: HP, souls, hollowing, deaths, role, bonfire |
| `flags [--flag ID] [--group G]` | the event flags each game has loaded (`EventFlagManager`), and what differs between the two; a guest in a session carries the host's, see `docs/DS2_WORLD_STATE.md` |
| `human --instance N` | burns a Human Effigy through Inventory and passes only when hollowing and the hollow state read 0 in memory; presses nothing if already human |
| `death --instance N mode\|feature\|status` / `death profile set` | the death hook's mode and bill, confirmed by its echo; the profile is reapplied on every `game enter` |
| `session` | both instances: roles, members, session machine states, channel counters, and `p2pSessionVerified` |
| `kill --instance N` | zeroes HP with expected bytes and passes only on the hook's death lines |
| `probe --instance N "<kind> <name> <args>"...` | raw MemProbe lines in one request, every reply parsed; lengths are decimal |
| `goto --instance N --to x,z` / `--to-instance M` | walks a character there, unattended |
| `teleport --instance N --to x,y,z \| --to-bonfire <id>` | 13 writes with expected bytes after both vtables check; passes on the character settled within 1.5 m and no death for 3 s |
| `bonfires --instance N` | the bonfire record and the loaded map's bonfires with their spawn points |
| `goto-map --instance N --map <hex> --to x,y,z` | another map without a warp: load (estado 5), focus (a real cell), teleport, the physics contact on that map, release |
| `backread --instance N load\|focus\|unfocus\|clear\|keep\|status` | the backread hook's orders, each confirmed by its echo, status included |
| `game options` | the line to paste into Steam's launch options |
| `game watch` | asks the injector to watch the area address for a few seconds |
| `save prune --keep N [--dry-run]` | deletes each account's `antes-de-*` rescue copies beyond the newest N; never a label somebody chose |
| `pad start\|press\|dpad\|trigger\|stick\|seq\|status` | a virtual gamepad over `/dev/uinput` |
| `steam2 init\|run\|show` | the second Steam client, which gives instance 2 its own account |
| `logs <server\|instance2\|injector\|timer\|cli\|death\|backread\|channel\|crash\|trace\|session\|seamless\|respawn\|rematch\|carry\|memprobe>` | with `--instance N`, `-g <pattern>`, `-n <lines>`, `-f` |

### Contract for LLM-driven tests

Use `--json` for automation. It is global and emits **one result object**;
progress is in `events.jsonl`. Read `status`, `errorCode`, `error`, `data` and
`artifacts`. Exit codes: **0 passed, 1 failed, 2 inconclusive**. `doctor --json`
now fails when the environment has problems. The old doctor/status fields
are nested under `data`; update scripts that parsed the previous shape.
A successful inventory command does not prove the game or P2P session works.

Configure each account once with `game identity --instance N <SteamID64>`
(the decimal 17-digit ID), and set its expected character with
`game character --instance N <name>`. Do not infer the logged-in account from
Steam login history. The configured ID selects the API connection and save
folder; it must actually be the account launched in that prefix.

Use this loop:

1. `observe --json` to inspect the current state and available evidence.
2. `scenario validate <file> --json` before running a new scenario.
3. `scenario run <file> --json` to execute assertions and preserve evidence.
4. Read `result.json`, `events.jsonl` and screenshots from the returned
   `artifacts` directory. Report the run ID and exactly which assertions passed.

`scenario run world-ready` checks world state and server presence for both
accounts. It does **not** prove summoning, peer interaction or guest respawn.
`p2pSessionVerified` comes from `session` (or `observe --session`): `true`
needs the session objects and both coop channels to agree **and** packets to
cross from host to guest between two samples; the objects alone stay `null`,
because the host's controller outlives the session. Never convert missing
observations, no error logs, an installed hook, a live PID or a phantom HUD
into proof of a working co-op session.

Each invocation records command, environment, events, result and bounded log
excerpts in `~/.local/share/ds2os-dev/runs/<id>/` (or under `XDG_DATA_HOME`).
Scenarios also record binary hashes, scenario contents and fixture hashes.
Screenshots have unique names and instance ownership; they are not overwritten
by the next capture. Two controllers cannot run concurrently: one action owns
`control.lock` for its complete sequence. Observers can run alongside it.

The updated Windows injector publishes a boot ID in `DS2_Nav.txt` and hook
installation receipts in `DS2_Harness.json`. **Rebuild/copy the injector and
relaunch both games** to obtain receipts. Old DLLs can still answer unique
MemProbe labels and publish positions, but do not prove installed hooks for
the current boot. A changed `Injector.config` is configuration intent, not
proof that an open process loaded it. Restart an old pad daemon too, before
launching games, to obtain bounded holds and strict acknowledgements.

**`up` rewrites `Injector.config` from its own flags**, so a flag set on a
previous `game prepare` is gone the moment `up` runs. Repeat every flag you
want on the `up` itself — `up --seamless --auto-rematch`, not
`prepare --auto-rematch` followed by `up --seamless`. The symptom is a hook
that installs on one boot and silently is not there on the next, and its log
keeps the old boot's lines, which read as if it were alive.

`game prepare` takes the flags that end up in `Injector.config`:
`--force-zone` (multiplayer in the closed areas), `--no-timer`,
`--timer-seconds`, `--probe-area`, `--watch-reads`, `--area-address`,
`--probe-zone`. The config is read **when the injector is injected**, so
changing it means closing and reopening the game.

### Getting into the world

`up` requires both installations and returns failure if any requested launch
or arrival fails. `up --no-enter` confirms the title screen. `reload` quits to
title before restarting and refuses to restart if an instance is unknown or
cannot leave. These commands do not guarantee a particular area: use an
explicit area/position assertion for a test that requires Majula.

`game enter` uses a confirmed local world state plus the API record filtered
by the configured Steam ID and checks the expected character when supplied.
It no longer accepts the last character announced in the global server log
as evidence for whichever instance is being driven.

The memory probe uses a unique request label under a per-installation lock.
It verifies the known game executable before using version-specific offsets.
Title byte 1 means title; zero is only world when a valid position advances.
Otherwise the state is loading or unknown. Unknown never authorizes blind
button presses. Menu/navigation loops propagate deadlines and cancellation;
`game leave` succeeds immediately if already at title.

The server log uses bytes that are not UTF-8. Use the harness to read it.
Log messages remain diagnostic evidence, not an instance identity oracle.

**A server restart has one correct order**, which `reload` follows: quit to
the title **first**, then restart, then come back in. The client asks for a
session on its way *into* the title screen, so leaving first means it is
already holding one when the server returns — and the server keeps its tokens
across a restart (`PersistAuthTokens`, on by default, in
`Saved/<server>/auth_tokens.txt`), so that session is still good.

Restarting first costs a minute and looks like something else entirely. The
client keeps presenting a token the new process has never seen, is refused,
retries, and eventually drops to the title with "the connection to the game
server was lost" — which reads like a Steam failure and is not. It also puts
up "lost connection to game server, switching to offline mode" over the world,
and that dialog eats the first button of any menu walk.

Persisted tokens do not let a client keep playing through a restart: the
reliable UDP stream's sequence numbers live in the connection, so the server
answers `Received sequenced packet (type 4) before connection is established`
and the client is connected in name only. The trip to the title is what
rebuilds the stream.

The game happily enters the world **offline** when its login failed, and looks
perfectly normal there. `game enter` notices — nothing renames the connection
— quits to the title and tries once more.

**A long session runs the X server out of clients.** After some hours of
`game shot` / `game focus` and repeated launches, `game enter` fails with
`X11 setup failed: 'Maximum number of clients reached'` and nothing else
explains it. The connections are not the harness's: each Wine prefix leaves
`xalia.exe` (Steam's accessibility helper, which talks X11) and two
`winedevice.exe` behind when a game stops, and they pile up across launches —
22 hours' worth were still connected when this first bit.

**`winedevice.exe` is not always an orphan, and killing the wrong one costs
the gamepad.** It hosts the Wine session's drivers — `winebus.sys` among them,
which is how the game sees the virtual pad — and `services.exe` does not start
it again. A prefix's Wine session can outlive its game: prefix 1's has run
since 13/09 with the main Steam client hanging off it. On 14/09 killing its two
`winedevice.exe` with no game open left the next game at "PRESS START", deaf to
every press, while account 2 on the same pad walked in. `sc query winebus`
said `STOPPED`, exit 1067. So a `winedevice.exe` is an orphan only when its
prefix has no `services.exe`; `xalia.exe` with no game in its prefix always is.

`doctor` lists exactly those under `wine_orphans`, with the pids, and counts the
connections under `x11_clients`; `up` ends them when no game is open. By hand,
kill by pid, never `pkill -x winedevice.exe`, and never `pkill -f`, which
matches the shell running it too.

Two smaller rules, both learned by losing an afternoon:

- **The pad has to exist before the game starts.** `up` orders it that way.
- **Steam takes the pad for the games it starts.** Account 2 comes up through
  its own Steam client, and while Steam is bringing a game up it holds the
  virtual pad; presses aimed at the other instance during that window are
  simply lost, and the only symptom is a game sitting at its title screen
  while every command reports success. Account 1 therefore still starts
  through Proton directly — which is also the only way, since Steam refuses
  `-applaunch` for a while after it has seen the app running.
- **Focus is not a formality.** The game ignores the pad while another window
  is active, so every press focuses first and checks that the focus landed.

### Driving the game

The game reads controller buttons, not the keyboard. **X uses the belt item,
A confirms, Y cycles overlapping prompts**, and on the character list **X is
Delete** — only ever press A there.

```
pad seq "press start; wait 900; dpad right; wait 250; press a" --focus 1 --shot
```

Take a screenshot after every menu step rather than firing a long blind
sequence: a `dpad left` sent when no dialog is open moves the character
instead.

**A hollow character cannot use the Cracked Red Eye Orb**: X does nothing at
all — no animation, no message, nothing reaching the server — and it reads
exactly like a dead button. Standing on a bonfire does **not** block the item;
that was the first guess and it was wrong. Dying **in your own world** hollows
you; dying as an invader in someone else's world does not, so a duel loss
costs no effigy.

**A hollow guest *can* place a white sign** (measured 14/09: `Sign created:
type 1` in hollow state 1). What hollowing blocked was the **host**: its client
received the sign and never offered "Touch Summon Sign". With `--seamless`,
`DS2_HollowSummonHook` removes that, and a summon between two hollow characters
reaches `RequestNotifyJoinSession`. Without `--seamless`, the host still needs
the effigy.

**So burn a Human Effigy before every staging, rather than checking first** —
and burn it through **Inventory**, not the belt: `ds2os-dev human --instance
both`, right after `up`. Both characters carry 80+ of them. Pressing X on the
effigy in the belt has repeatedly done nothing while the menu path worked a
minute later on the same character, so a belt count that does not move proves
nothing. `human` believes only the character's memory: `hollow` and
`hollowState` both 0 (and the maximum HP comes back, 777 → 915 on Samuel).
Burn it **after** the instance is in the world. The game saves when the item
is used, so a burn survives a killed client.

**The start menu remembers its tab until the next load, and wraps.** Every
blind menu walk here — `human`, and `game leave`'s way to Quit Game — assumes
it opens on Equipment, which is true after every trip through the title
screen. Opening the menu by hand in the world and leaving it on another tab
sends the next walk somewhere else; `human` puts the tab back when it is done.

**Killing a client during a live session is an *illegal disconnect*, and the
game counts them.** After enough of them it puts up

> Due to repeated illegal multiplayer disconnects, your connection to other
> worlds was lost. Only a Bone of Order can restore your connection.

and from then on that character can do **nothing** multiplayer: the white
soapstone, the red soapstone and the Cracked Red Eye Orb are all a dead X,
with no animation, no message and no `RequestCreateSign` reaching the server.
It reads exactly like hollowing, and it is not.

It is **character state in the save**, so it survives a full `down`/`up` of
the server and both instances, and burning effigies does nothing for it. The
only cure is a **Bone of Order**. On 12/09 this cost most of an afternoon:
hollowing, an offline client, the seamless hooks, the arrival guard byte, the
local character's counter at `ctx+0xd0 +0x168` (`ctx+0xd0` is the local character, rebuilt on every load), position and X delivery were each
measured and cleared before the game finally said what was wrong — and it only
said it on the client's own screen, never in any log.

So: **`game stop` while a session is live costs a strike.** End the session
first: `ds2os-dev session end`. `game stop`, `down` and `save restore --stop`
ask each open instance's channel first and refuse with `session_live`, before
stopping any of them. `--force` overrides. A channel that does not answer does
not block. A scenario's baseline cleanup kills anyway, because the save is
restored right after, and records `cleanup_kill_with_session`. If a character suddenly cannot place a sign, look
at its screen before measuring anything.

**A co-op test can cost a strike, and the guest is who pays.** Measured
12/09: Chico placed sign 1001 without trouble at 19:26, a seamless-respawn
test ran at 19:31, and by 19:40 he could not place a sign and had stopped
polling for them entirely. From the game's side that is fair — a guest who
refuses the session teardown and then leaves *is* an illegal disconnect.

It is **not** every run, though: a later test that ended with the session
collapsing on its own left him able to place signs immediately afterwards. The
runs that burned a character were the ones that left a session wedged and were
cleaned up by killing the client. Still, assume a test can cost one, because
there are only so many Bones of Order in a playthrough and we spent both.

The cure is not an item, it is the save. The private server keeps its own
(`EnableSeperateSaveFiles`), so the whole thing is one file per account:

```
<prefix>/drive_c/users/steamuser/AppData/Roaming/DarkSoulsII/<steamid>/DS2SOFS0000.ds3os
```

`ds2os-dev save backup|restore|list` keeps snapshots in
`~/.local/share/ds2os-dev/saves`. **Close the selected clients before backup**:
a live copy is not a consistent fixture. Missing/ambiguous saves, incomplete
`both` selections and reused labels fail. Restore takes the label returned in
`save list --json`, without `contaN-` or `.ds3os`, and uses a temporary file
plus rename after preserving a rescue copy. `restore --stop` closes the
selected clients before replacing their saves.

Prefer a scenario with `"baseline": "<label>"` for repeatable experiments.
Start it with the selected clients stopped. It preserves originals, restores
the baseline, executes the declared launch/actions/assertions, then stops the
clients and restores originals even after action failure or cooperative
cancellation. A cleanup error is a failed run with the rescue label recorded;
SIGKILL or power loss still needs manual restore. Without `baseline`, a
scenario leaves its effects in place.

**The `.bak` files sitting next to the live saves are not what they look
like.** The ones dated 9/9 are from the moment `EnableSeperateSaveFiles`
created the private save out of the retail one: a level 1 character in Things
Betwixt with five minutes on the clock. Restoring one throws the character
away. Check the Data List screen before pressing A on a restored save.

**Two games open on the same account break every session, and the symptom
points somewhere else.** After a failed relaunch left a second client running
on account 1, no session would form at all — not an invasion, not a sign
summon — while everything upstream looked healthy: the server routed the push
and the target answered `RequestSendMessageToPlayers`. Then one side said
"Summoning failed. Timed out." and the other "Disconnected from multiplayer
session." Closing the extra client fixed it immediately. `status` shows the
game processes; there must be exactly one per account. Before reading any PvP
result as a finding, **run the invasion control** — two button presses, and it
says whether the machine can form a session at all.

**Quit Game is refused while a PvP session is live**, on both sides. The menu
entry highlights and A does nothing, so `game leave` sits there pressing
buttons until it times out. End the session first: `session end`, a death or
the timer.

`game focus <n>`, `pad seq --focus <n>` and `game shot` all mean the **instance**,
resolved by the owning process. They used to index the window list, which put
`shot-1.png` on either account depending on boot order, and sent presses to the
wrong game. There is now no positional fallback: an unresolved process
returns `instance_unresolved`. Captures use `shot-<instance>-<unique-id>.png`.

### Two installations, two accounts

**Never start the second instance from the first Steam.** The session
between two players is peer to peer over Steam and keyed on the account's
steam id, so two instances on one account can never reach each other: the
server marks the second connection `<id>_1`, phantoms never arrive, and the
test proves nothing while looking like a genuine negative. The whole reason
there are two Steam clients is to avoid this. Instance 1 is the main account
(Samuel, Marcos); instance 2 is the second client's account (Chico).

`ds2os-dev game launch --instance 2` gets it right by asking the **second
client** to launch the game (`steam.sh -applaunch 335300` with its own
`HOME`), which is also why that account's launch options must carry the
wrapper — the harness refuses to launch without it, because a game started
with no injector reaches FromSoftware's servers instead.

Running Proton directly is not enough, and fails quietly: `HOME` points the
Linux side at the second client, but Proton overwrites
`STEAM_COMPAT_CLIENT_INSTALL_PATH` with the installation it was launched
from, and that is the path the Windows side of steamclient follows. The game
logs in as the **first** account while every other sign looks right. The
server refuses the duplicate; `game enter` cannot pass without the expected
account in the API and a locally confirmed world.

The two instances have **separate game installations**:

```
/mnt/ssd/SteamLibrary/steamapps/common/Dark Souls II Scholar of the First Sin
/home/suel/steam2/.steam/debian-installation/steamapps/common/Dark Souls II Scholar of the First Sin
```

Each has its own `Injector.dll`, `Injector.config`, logs and request files.
`Injector.config` sits in the install root; the executable is in `Game/`.
A request file written to one never reaches the other — write to both. This
once looked like one instance hogging a shared file, because repeated writes
kept being answered by the same client.

### The injector is built on CI

`Injector.dll` needs MSVC (Detours), which cannot be built on this machine.
`.github/workflows/injector-linux.yml` builds it on push to
`Source/Injector/**`. Run `ds2os-dev injector check` before pushing (syntax
only, with mingw), then `ds2os-dev injector fetch` to wait for the run and
download it into `~/Downloads/injector`; `game prepare` (or `up`) installs it
from there, with the games stopped. The receipt carries the commit the DLL was
built from (`observe` → `hooks.build`), and `injector status` / `doctor` say
whether each open game runs the fetched one.

`.github/workflows/ci.yml` is broken and unrelated to our code: `Build Linux`
pins the retired `ubuntu-20.04` and queues forever, and the two nix jobs use
the shut-down `magic-nix-cache-action`. Cancel its runs (`gh run cancel`)
rather than waiting for them.

## Reverse engineering

Every offset in this project is hardcoded against **version 1.03,
Calibrations 2.02** (`DarkSoulsII.exe`, 28,200,992 bytes). Preferred base
`0x140000000`, and under Wine the image has always landed there, so a file
offset and a runtime address differ by a constant. A patch moves everything.

**That constant is a measurement, not a guarantee.** This line used to say "no
ASLR", and the header does not support it: `DllCharacteristics` is `0x8160`,
which carries both `DYNAMIC_BASE` and `HIGH_ENTROPY_VA`, and `.reloc` is
present with 0x40200 bytes of fixups. The binary can be relocated, and on real
Windows it will be. The hooks are safe because they resolve everything from the
base they are loaded at (`s_base + offset`); what is **not** safe is an
absolute address written as a literal — `0x141616cf8`, a vftable like
`0x1410e4bb8`, or a MemProbe `abs` line. Those are correct here and wrong the
moment the image moves. Prefer `mod`/`chain` over `abs`, and keep offsets
relative in new code.

### Ghidra, first

The binary is already analysed. Reach for Ghidra before spending long on
linear disassembly: `objdump` gives 5.6M lines with no cross references, and
the binary is full of functions split into chunks for unwinding, so walking
outward by grepping for `call` runs out and the reading turns into guesswork.

- Ghidra `/home/suel/tools/ghidra`, JDK 21 `/home/suel/tools/jdk`
- project `/home/suel/tools/proj`, name `ds2`, program `DarkSoulsII.exe`

```
JAVA_HOME=/home/suel/tools/jdk \
/home/suel/tools/ghidra/support/analyzeHeadless /home/suel/tools/proj ds2 \
  -process DarkSoulsII.exe -noanalysis -readOnly \
  -scriptPath <dir with the .java> -postScript Script.java <args>
```

`-noanalysis -readOnly` keeps the existing analysis intact. Headless wraps
every `println` as `INFO  Script.java> ... (GhidraScript)`, so strip that
before reading the output; a decompiled function's continuation lines arrive
unwrapped, which makes naive filtering drop half the answer.

What earns its keep: `getFunctionContaining`, `Function.getCallingFunctions`
for callers, `ReferenceManager.getReferencesTo` for data xrefs, and
`DecompInterface` for reading a branch condition as C instead of inferring it
from `testb`/`je`. A virtual method shows no callers: find its address in
`.rdata`, subtract the class vftable to get the slot, then search for
`call [reg+<slot>]`.

### objdump, for bytes

```
x86_64-w64-mingw32-objdump -d --no-show-raw-insn \
  --start-address=0x140250dc0 --stop-address=0x140250f00 "<exe>"
```

Disassembling a **range** is reliable; a single linear `-d` of the whole
`.text` desyncs wherever data sits between functions, and silently omits real
instructions. Drop `--no-show-raw-insn` when you need the encoding, and note
that objdump wraps a long instruction's bytes onto a second line with no
mnemonic — a regex that reads one line per instruction will truncate it.

### Live memory, from Linux

The injector answers request files dropped into the install root and writes
its reply to a log beside them. See `docs/DS2_LIVE_MEMORY_ACCESS.md` and
`docs/DS2_INVESTIGATION_TOOLS.md`.

`DS2_MemProbe.req` → `DS2_MemProbe.log`, one command per line:

```
abs   <label> <hex address> <length>
mod   <label> <hex offset from the module base> <length>
chain <label> <hex offset from the module base> <off,off,...> <length>
scan  <label> <hex value> <width 1|2|4|8> [max hits]
```

Writes take the same three shapes with a `poke` prefix and hex bytes in place
of the length — `pokemod <label> 250e5b b001c3` — and may carry a fourth
field with the bytes they expect to find, in which case a mismatch is
refused.

`DS2_Trace.req` → `DS2_Trace.log` sets breakpoints that report registers and
sweep the stack window for return addresses, because `[rsp]` is only a return
address at a function's entry.

Anything written this way is live memory and is lost when the client
restarts. Write to **both** installations.

## Ground rules that were learned the expensive way

**Run the control first.** A negative result in Majula means nothing until
the same test passes in Heide, which works without any patch. Several days
went into explaining a refusal that turned out to be the test being broken in
both places.

**Success is a positive signal, never the absence of an error.** For a
summon, that means `RequestNotifyJoinGuestPlayer` followed by
`RequestNotifyJoinSession` reaching the server. A missing error message
proves nothing.

**`LogFirstMessageOfEachType` is a census, not a trace.** It logs the first
of each message type per client, which once hid a sign being created and
removed repeatedly and cost a wrong diagnosis. The DS2 server now also logs
every `RequestNotify*` as `Notify RequestNotify<type>: ...`, so joins, leaves
and deaths are visible on any connection. Every other message type is still
only in the census.

**Patch the source of a value, not the value.** Poking the multiplay block
counters cleared one consumer and moved the failure to the next, and the
object holding them is rebuilt when a client loads into another world.
Patching the predicate let the game's own edge logic drain them.

**A hook that writes into `.text` must verify the bytes it expects and refuse
on mismatch**, so a game update declines instead of corrupting. See
`DS2_UnblockMultiPlayHook`.

**When inventorying "every place that touches X", seed from a scan of every
function, not from a pattern.** A search for the explicit form
(`param_1 + 0x1a8`) misses every site where the compiler folded the base into
a displacement (`[obj + idx + 0x1e8]`), and the miss is silent. Then check
completeness against something that does not share the inventory's
assumptions: re-running your own classifier over your own result agrees with
itself by construction and proves nothing. This mistake shipped a patch that
crashed the game on save load.

**A test that fails and a character that died look identical.** Both leave a
log full of nothing, and telling them apart by screenshot afterwards has cost
several afternoons. DS2 does not implement the API death counter: `players`
reports `null`, not zero. `watch` and `timeline` read deaths from
`DS2_Death.log` and warp reasons from `DS2_Seamless.log`; a death with the
death hook absent is not observable there.
`goto` stops on large discontinuities, falling, stale telemetry or a changed
process/boot. A jump is a discontinuity, not by itself proof of death. Run a
watch beside scripted actions and keep unknown evidence inconclusive.

The web UI's login is off until `WebUIServerUsername` and `WebUIServerPassword`
are set in the server config, and the harness reads the credentials from that
same config - so whatever the server was started with is what works. The
config lives under `Saved/` and is gitignored.

**A killed client leaves its sign behind, and it looks real.** The server
does clean up — `DS2_SignManager::OnLostPlayer` removes the player's signs and
pushes `PushRequestRemoveSign` to everyone aware of them — but only once it
notices the client is gone, and `game stop` is not a graceful disconnect. Until
the connection times out the sign is still in the cache: the other player polls
it, sees it on the ground, and can touch it. The summon then fails because
nobody is waiting behind it, which reads as a summoning bug and is not one.
After stopping an instance, wait for the server to say `0 signs cached` before
believing anything about signs: `ds2os-dev server wait --signs 0`.

**This is what a stale party sign looks like from the outside**, and it cost an
hour on 19/09. After a `game stop` and a fresh `up`, the party never formed:
the guest's log said `placa no chao`, the host's said
`invocando a placa 80000001`, and the host's screen said

> Summoning failed. Player was unable to join multiplayer session.

The server was offering a sign it had cached before the restart — its recorded
position was where the guest had stood an hour earlier — and the guest's client
logged nothing at all, because as far as it knew nobody had touched its sign.
Two crashes earlier in the same session had made this look like the illegal
disconnect penalty, which it was not. The cure is to restart the server
(`reload` does it in the right order) and then `server wait --signs 0` before
believing the next summon.

**Two Steam accounts on one machine cannot make a third player.** Anything
about three or more players in a session is untestable here, and the honest
report says so rather than implying it works.

`docs/DS2_TO_VALIDATE.md` is the standing list of what is untested. Keep it
true.
