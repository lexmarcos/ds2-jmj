#!/usr/bin/env bash
#
# Launches a second Dark Souls II next to the one Steam started, so a single
# copy of the game can test PvP against itself.
#
# Two things make this work:
#
#   * A separate Proton prefix. Named kernel objects live per prefix, so the
#     game's single-instance mutex in the first prefix is invisible here and
#     there is nothing to kill.
#   * Steam refuses to launch one game twice, so this bypasses Steam and calls
#     Proton directly.
#
# The server must have AllowDuplicateSteamIds enabled, otherwise it refuses the
# second session instead of giving it its own profile.
#
# Usage:  second-instance.sh [--prefix DIR] [--game DIR] [--proton DIR]
#                            [--reset] [--dry-run]

set -euo pipefail

APPID=335300
GAME_EXE=DarkSoulsII.exe
prefix_dir="${XDG_DATA_HOME:-$HOME/.local/share}/ds2os/second-instance"
game_dir=""
proton_dir=""
reset=0
dry_run=0

die() { echo "erro: $*" >&2; exit 1; }
note() { echo "  $*"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) prefix_dir="$2"; shift 2;;
    --game)   game_dir="$2";   shift 2;;
    --proton) proton_dir="$2"; shift 2;;
    --reset)  reset=1;         shift;;
    --dry-run) dry_run=1;      shift;;
    -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    *) die "opção desconhecida: $1";;
  esac
done

# ---- Steam ------------------------------------------------------------------

steam_root=""
for candidate in "$HOME/.steam/root" "$HOME/.steam/steam" "$HOME/.local/share/Steam" \
                 "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
  resolved=$(readlink -f "$candidate" 2>/dev/null || true)
  if [ -n "$resolved" ] && [ -d "$resolved/steamapps" ]; then steam_root="$resolved"; break; fi
done
[ -n "$steam_root" ] || die "não encontrei uma instalação da Steam"

# Every library, from the install root plus libraryfolders.vdf.
libraries=("$steam_root")
vdf="$steam_root/steamapps/libraryfolders.vdf"
if [ -f "$vdf" ]; then
  while read -r path; do
    [ -d "$path/steamapps" ] && libraries+=("$path")
  done < <(grep -oP '"path"\s*"\K[^"]+' "$vdf")
fi

# ---- The game ---------------------------------------------------------------

if [ -z "$game_dir" ]; then
  for library in "${libraries[@]}"; do
    manifest="$library/steamapps/appmanifest_$APPID.acf"
    [ -f "$manifest" ] || continue
    name=$(grep -oP '"installdir"\s*"\K[^"]+' "$manifest" | head -1)
    [ -n "$name" ] || continue
    if [ -d "$library/steamapps/common/$name" ]; then
      game_dir="$library/steamapps/common/$name"; break
    fi
  done
fi
[ -n "$game_dir" ] || die "não encontrei o Dark Souls II; passe --game DIR"

# Scholar of the First Sin keeps the executable under Game/ rather than at the
# root of the install, so look for it instead of assuming.
game_exe_path=$(find "$game_dir" -maxdepth 2 -name "$GAME_EXE" -print -quit 2>/dev/null || true)
[ -n "$game_exe_path" ] || die "$GAME_EXE não está em $game_dir"

for needed in Injector.exe Injector.dll Injector.config; do
  [ -f "$game_dir/$needed" ] || die "$needed não está em $game_dir; use Preparar lançamento no loader primeiro"
done

# Proton launched outside Steam does not set this, and the game needs it to
# talk to the running Steam client.
echo -n "$APPID" > "$game_dir/steam_appid.txt"

# ---- Proton -----------------------------------------------------------------

if [ -z "$proton_dir" ]; then
  # Newest first, so "Proton - Experimental" wins over an old numbered build.
  for library in "${libraries[@]}"; do
    while read -r candidate; do
      [ -f "$candidate/proton" ] && proton_dir="$candidate"
    done < <(find "$library/steamapps/common" -maxdepth 1 -name 'Proton*' -type d 2>/dev/null | sort)
  done
  for candidate in "$steam_root"/compatibilitytools.d/*; do
    [ -f "$candidate/proton" ] && proton_dir="$candidate"
  done
fi
[ -n "$proton_dir" ] || die "não encontrei um Proton; passe --proton DIR"

# ---- Prefix -----------------------------------------------------------------

if [ "$reset" = 1 ] && [ -d "$prefix_dir" ]; then
  note "apagando o prefixo antigo"
  rm -rf "$prefix_dir"
fi
mkdir -p "$prefix_dir"

note "steam    $steam_root"
note "jogo     $game_exe_path"
note "proton   $proton_dir"
note "prefixo  $prefix_dir"
note ""
note "a primeira execução leva um tempo enquanto o Proton monta o prefixo"

export STEAM_COMPAT_CLIENT_INSTALL_PATH="$steam_root"
export STEAM_COMPAT_DATA_PATH="$prefix_dir"
export SteamAppId="$APPID"
export SteamGameId="$APPID"
# The injector relaxes a memory protection check when it sees this, which it
# needs under Wine.
export WINEPREFIX="$prefix_dir/pfx"

if [ "$dry_run" = 1 ]; then
  note ""
  note "dry run, não vou lançar nada. O comando seria:"
  note "  $proton_dir/proton run $game_dir/Injector.exe $game_exe_path"
  exit 0
fi

cd "$game_dir"
exec "$proton_dir/proton" run "$game_dir/Injector.exe" "$game_exe_path"
