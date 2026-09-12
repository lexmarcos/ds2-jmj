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

**The standing goal is seamless co-op**, and it is written down rather than
carried in anyone's head. `docs/DS2_SEAMLESS_COOP_DESIGN.md` is the brief —
the host owns the world, each player keeps their own save, and everything that
follows from that. `docs/DS2_SEAMLESS_COOP_TASKS.md` is the work left, in
dependency order; keep it true as pieces land, and read both before starting
anything co-op. Nothing below M1 there is reachable until a death stops
tearing the session down.

## The harness: `ds2os-dev`

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
| `doctor` | reports everything missing from the environment |
| `status` | one screen: server, ports, player count, game processes, log paths |
| `up` / `down` | server, both instances, and both characters standing in the world |
| `reload` | restarts the server and puts everyone back in the world, without closing the game |
| `server up\|down\|restart\|status` | the local server on its own |
| `game prepare` | writes `Injector.config`, the wrapper, and copies the injector binaries into **both** installations |
| `game launch\|stop --instance 1\|2\|both` | starts or stops an instance, through Proton, without Steam |
| `game enter\|leave --instance <1\|2>` | walks the menus from the title into the world, and back out |
| `game focus <1\|2>` / `game shot` | window focus and per-window PNG capture |
| `players` | what the server knows: name, soul level, souls, soul memory, deaths, covenant, area |
| `watch` | prints only when something changes; a death above all |
| `where` | where each character is standing, from the game's own memory |
| `goto --instance N --to x,z` / `--to-instance M` | walks a character there, unattended |
| `game options` | the line to paste into Steam's launch options |
| `game watch` | asks the injector to watch the area address for a few seconds |
| `pad start\|press\|dpad\|trigger\|stick\|seq\|status` | a virtual gamepad over `/dev/uinput` |
| `steam2 init\|run\|show` | the second Steam client, which gives instance 2 its own account |
| `logs <server\|instance2\|injector\|timer\|cli>` | with `-g <pattern>`, `-n <lines>`, `-f` |

`game prepare` takes the flags that end up in `Injector.config`:
`--force-zone` (multiplayer in the closed areas), `--no-timer`,
`--timer-seconds`, `--probe-area`, `--watch-reads`, `--area-address`,
`--probe-zone`. The config is read **when the injector is injected**, so
changing it means closing and reopening the game.

### Getting into the world

`up` ends with both characters standing in Majula, and `reload` puts them back
there after a server restart. Both are menu walks, and the walk is short:

```
title  --START-->  main menu  --A-->  save list  --A-->  world
```

What makes it reliable is that nothing waits for a duration. Two oracles say
what the game is doing:

- **The game itself.** `DS2_MemProbe` reads `mod 1614804 1`: the byte is 1
  while the title screen's state machine is alive and 0 from the moment
  loading starts. It is the only answer that works when the client is not
  talking to the server at all, which is exactly when things go wrong.
- **The server's log**, for the half the game will not admit to:
  `has logged in as player` means the title screen is behind us, and
  `Renaming connection to '<n>:<name>'` means that character is in the world.
  The name is the only statement tying an instance to a save, so it is also
  the check for "did the right character load".

The log is written with box-drawing bytes that are not UTF-8, so `grep` calls
it binary and prints its match to **stderr**. Use `grep -a`, or the harness.

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

**A hollow character cannot use the Cracked Red Eye Orb.** X does nothing at
all — no animation, no message, nothing reaching the server — and it reads
exactly like a dead button. Burn a Human Effigy first (menu → Inventory → the
second item, A, A). Standing on a bonfire does **not** block the item; that was
the first guess and it was wrong. Dying **in your own world** hollows you;
dying as an invader in someone else's world does not, so a duel loss costs no
effigy.

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
buttons until it times out. End the session first: a death, the timer, or
`game stop`.

`game focus <n>`, `pad --focus <n>` and `game shot` all mean the **instance**,
resolved by the owning process. They used to index the window list, which put
`shot-1.png` on either account depending on boot order, and sent presses to the
wrong game.

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
server now refuses the duplicate outright, and `game enter` explains it.

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
`Source/Injector/**`; fetch the result with

```
gh run download <run-id> -n injector -D ~/Downloads/injector
```

and `game prepare` installs it from there.

`.github/workflows/ci.yml` is broken and unrelated to our code: `Build Linux`
pins the retired `ubuntu-20.04` and queues forever, and the two nix jobs use
the shut-down `magic-nix-cache-action`. Cancel its runs (`gh run cancel`)
rather than waiting for them.

## Reverse engineering

Every offset in this project is hardcoded against **version 1.03,
Calibrations 2.02** (`DarkSoulsII.exe`, 28,200,992 bytes). Module base
`0x140000000`, no ASLR, so a file offset and a runtime address differ by a
constant. A patch moves everything.

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
removed repeatedly and cost a wrong diagnosis.

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
several afternoons. `ds2os-dev players` shows the death count, and
`ds2os-dev watch` prints a line the moment it goes up; `goto` stops on its own
when the character is moved by something other than the walk, because one burst
cannot cover eight metres and a jump that large is a respawn. Run a watch beside
anything scripted.

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
believing anything about signs.

**Two Steam accounts on one machine cannot make a third player.** Anything
about three or more players in a session is untestable here, and the honest
report says so rather than implying it works.

`docs/DS2_TO_VALIDATE.md` is the standing list of what is untested. Keep it
true.
