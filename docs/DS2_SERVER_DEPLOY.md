# Running the DS2 server on a shared Linux host

How the Jamuja Edition server is deployed, written for the case this
project actually has: a small VPS that already runs something else which
must not go down. The address is deliberately not here; it lives in the
loader's gitignored `.env` and nowhere in the repository.

## Build here, not there

Compile on a development machine and copy the result. A 2 GB VPS cannot
build this server: the C++ build with protobuf and OpenSSL exhausts the
memory, and on a shared host the kernel's OOM killer chooses what dies —
not necessarily the build.

A binary built on Ubuntu 24.04 runs on Debian 13. It needs at most
`GLIBC_2.38` and `GLIBCXX_3.4.30`; check the target provides both before
assuming the same holds for another distribution:

```
objdump -T bin/x64_release/server/Server | grep -oE "GLIBCXX_[0-9.]+" | sort -Vu | tail -1
```

Ship these four together:

| file | why |
| --- | --- |
| `Server` | the server |
| `libsteam_api.so` | Steam API, loaded beside the binary |
| `steamclient.so` | the 64-bit one from a Steam install's `linux64/`. The server calls `SteamGameServer_Init` to validate players' tickets, and a host with no Steam client has nothing to load it from |
| `steam_appid.txt` | `335300` |

**Not** `Saved/`. A development machine's `Saved/default` holds its own
keys and database; the server on the host must generate its own.

## Seed the config before the first start

On a host with no `config.json` the server writes defaults on first boot,
and two of those defaults are wrong for this project:

- `Advertise` is `true`, which lists the server on upstream's public
  master server. With the default config it would do that in the seconds
  before anyone edits it.
- `GameType` is `"DarkSouls3"`.

The server accepts a partial `config.json`, keeps the defaults for every
key missing from it, and writes the completed file back on first boot. So
write only the overrides first:

```json
{
  "ServerName": "Dark Souls 2 Jamuja Edition",
  "ServerHostname": "<public address>",
  "GameType": "DarkSouls2",
  "Advertise": false,
  "AllowDuplicateSteamIds": false,
  "DS2IncludeCurrentAreaInRightMatchingArea": true,
  "DS2_StickySigns": true,
  "DS2_InvadeAnywhere": true
}
```

Key names are matched exactly and an unknown one is ignored in silence, so
a typo in `Advertise` means the server advertises anyway. Every name above
has exactly one `SERIALIZE_VAR` in `Source/Server/Config/RuntimeConfig.cpp`.

`ServerHostname` must be set. Left empty, the server guesses its public
address (`Server.cpp:233`); on a host with more than one interface it
guesses wrong, and a player logs in successfully and is then redirected
to an address they cannot reach.

`DS2_InvadeAnywhere` is what makes invasion across areas work. It is half
server and half client, and the loader can only turn on the client half.

## The service

```ini
[Service]
User=ds2os
WorkingDirectory=/opt/ds2os
Environment=LD_LIBRARY_PATH=/opt/ds2os
StateDirectory=ds2os
Environment=HOME=/var/lib/ds2os
ExecStart=/usr/bin/stdbuf -oL -eL /opt/ds2os/Server
Restart=on-failure
UMask=0077
MemoryHigh=384M
MemoryMax=512M
CPUWeight=50
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/opt/ds2os/Saved /opt/ds2os/steam_appid.txt
```

Three lines in there are the result of the deployment going wrong first:

**The log has to be line buffered.** Under systemd the server's stdout is
not a terminal, so glibc holds it in 4 KB blocks and writes nothing until
a block fills. The server looked silent for minutes while running
normally: the last line in the journal was the Steam library's own, and
every line the server logged after it sat in the buffer until the process
exited. That is worse than no log, because a player connecting would not
show up either, and the natural conclusion would be that nobody reached
the server. `stdbuf -oL -eL` fixes it; the tell that it worked is a
server line appearing while the process is still alive.

**`HOME` must not be under `Saved/`.** The Steam API writes under `HOME`,
and the obvious place for it is beside the server's state. But
`ServerManager.cpp:44-56` starts `default` and then every other
directory in `Saved/` as a further server instance. A `Saved/home` became
a server named "home" with no config, and its failure takes the whole
process down after `default` has come up cleanly — which makes the log
look like success followed by a mystery.

**`steam_appid.txt` must be writable.** The server rewrites it on every
start. Under `ProtectSystem=strict` a read-only one is logged as an error
and otherwise tolerated, but it is noise on every boot.

The server uses about 56 MB. The memory limits are not a budget; they are
there so that a leak here can never starve whatever else shares the host.
`UMask=0077` matters because the server creates `private.key` itself, and
with the default mask it came out readable by every user on the machine.

## Firewall

Open exactly these:

| port | protocol | service |
| --- | --- | --- |
| 50050 | TCP | login |
| 50000 | TCP | auth |
| 50010 | UDP | game |

**Not 50005.** The WebUI listens on every interface, and its `/sharding`
endpoint is the one mutating endpoint that does not check authentication.
Leave it behind the firewall and reach it through an SSH tunnel if ever
needed.

On a host that already runs a firewall, add rules; never reset, disable or
re-enable it. On a host running Docker, its published ports are handled in
the `nat` table ahead of `ufw`'s chains, so adding `ufw` rules does not
touch the existing containers — but check them before and after anyway.

## The key

The server generates its RSA pair on first boot if either file is
missing, and loads it on every later boot. The **public** key is what the
loader embeds; copy it down and point the loader's `.env` at it with
`DS2OS_SERVER_KEY_FILE`.

**Back up `private.key`.** If it is lost the server makes a new pair, and
every loader already handed out stops working — the key it carries no
longer matches. It is the one file whose loss forces a new loader for
everyone.

## Updating the server

Build here, copy `Server` over the old one, restart:

```
make -j$(nproc) Server                   # in intermediate/make
scp bin/x64_release/server/Server root@<host>:/opt/ds2os/Server
ssh root@<host> systemctl restart ds2os
```

**A restart ends every session**, and the symptom looks like something
else. The game service accepts a client only if its session token is in
`AuthenticationStates` (`GameService.cpp`), a map held in memory and
filled when the player authenticates. A restart empties it. A player who
was already in the game keeps retrying with the old token, and the log
fills with `Clients authentication token (0x...) does not appear to be
valid` — which reads like a Steam or login failure and is neither. The
player has to go back to the title screen, or restart the game, to log in
again. So restart with nobody connected: the periodic status line says
`0 players`.

`config.json` and the keys survive, because they live in `Saved/default`
and the build does not ship `Saved/`. That also means a change to a
config *default* in `RuntimeConfig.h` — the welcome announcement, say —
reaches a new server only. An existing one keeps what its `config.json`
already says, and has to be edited there.
