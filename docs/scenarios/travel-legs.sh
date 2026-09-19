#!/bin/bash
# travel-legs.sh <destination>... : group travel by vote, one leg per argument.
#
# Destinations: majula heide ironkeep brume, plus ironhearth and amana.
# The host opens the vote, the guest presses A on the box, and the leg is
# scored 75 s later. A leg is CLEAN only if nothing grew on either side: no
# `excecao` in DS2_Crash.log, no `FALHA APARADA` in DS2_Backread.log, the host
# logged its arrival, and the session is still verified. The trap turns a crash
# into silent corruption, so the first trapped fault already is the failure,
# and a leg where nobody moved is not a clean leg.
#
# Set KEEP_GOING=1 to carry on past a leg with faults and stop only when a game
# dies or the session drops. Set REBUILD_PRESENCE=1 to take the other player's
# copy out and recreate it on both sides twelve seconds after each vote.
D=/home/suel/projects/ds2-jmj/Source/LoaderLinux/target/debug/ds2os-dev
I1="/mnt/ssd/SteamLibrary/steamapps/common/Dark Souls II Scholar of the First Sin"
I2="/home/suel/steam2/.steam/debian-installation/steamapps/common/Dark Souls II Scholar of the First Sin"
inst() { if [ "$1" = 1 ]; then echo "$I1"; else echo "$I2"; fi; }
count() { grep -ac "$2" "$1" 2>/dev/null || echo 0; }
# Which instance owns the world is decided by the party flags at `up`; ask.
roles() {
  local s; s=$($D session 2>&1)
  if echo "$s" | grep -q "^conta 1: Host"; then HOST=1; GUEST=2
  elif echo "$s" | grep -q "^conta 2: Host"; then HOST=2; GUEST=1
  else return 1; fi
}
# The first bonfire of each area the brief asks for: The Far Fire (Majula), Tower
# of Flame (Heide's Tower of Flame), Threshold Bridge (Iron Keep) and Foyer
# (Brume Tower, Crown of the Old Iron King). Until 18/09 this table had
# "ironkeep" on Ironhearth Hall, Iron Keep's second bonfire, and "brume" on
# 140b0000/2d82, which is Tower of Prayer in Shrine of Amana - a label inherited
# from an old shortcut and never checked against the game's own name. Both are
# kept under their real names.
declare -A MAPS=( [majula]="0a040000 122a" [heide]="0a1f0000 7ba2" [ironkeep]="0a130000 4cc7" [brume]="32240000 8f2f" [ironhearth]="0a130000 4cc2" [amana]="140b0000 2d82" )
legs=()
for n in "$@"; do [ -n "${MAPS[$n]}" ] || { echo "unknown destination: $n"; exit 1; }; legs+=("${MAPS[$n]} $n"); done
[ ${#legs[@]} -gt 0 ] || { echo "usage: travel-legs.sh <majula|heide|ironkeep|brume|ironhearth|amana>..."; exit 1; }
ok=0; clean=0
for leg in "${legs[@]}"; do
  set -- $leg; MAP=$1; FIRE=$2; NAME=$3
  roles || { echo "$NAME: no session; stopping"; break; }
  HI=$(inst $HOST); GI=$(inst $GUEST)
  e1=$(count "$I1/DS2_Crash.log" excecao); e2=$(count "$I2/DS2_Crash.log" excecao)
  f1=$(count "$I1/DS2_Backread.log" "FALHA APARADA"); f2=$(count "$I2/DS2_Backread.log" "FALHA APARADA")
  b1=$(wc -l < "$HI/DS2_Bonfire.log")
  # A travel costs nothing: a death billed during the leg means the landing
  # was taken for a real death, even when the leg then reports an arrival.
  c1=$(count "$I1/DS2_Death.log" "custos da morte"); c2=$(count "$I2/DS2_Death.log" "custos da morte")
  echo "=== $NAME ($MAP/$FIRE) host=$HOST ==="
  printf 'votar %s %s\n' "$MAP" "$FIRE" > "$HI/DS2_Bonfire.req"
  sleep 3
  $D pad seq "press a" --focus $GUEST >/dev/null 2>&1
  if [ "${REBUILD_PRESENCE:-0}" = 1 ]; then
    # Take the other player's copy out and bring it back on both sides, so it
    # is rebuilt in the map the group is in now and not left holding objects
    # of the map it was created in.
    sleep 12
    printf 'presenca retira\n' > "$HI/DS2_Bonfire.req"; printf 'presenca retira\n' > "$GI/DS2_Bonfire.req"
    sleep 6
    printf 'presenca recria\n' > "$HI/DS2_Bonfire.req"; printf 'presenca recria\n' > "$GI/DS2_Bonfire.req"
    sleep 57
  else
    sleep 75
  fi
  n1=$(count "$I1/DS2_Crash.log" excecao); n2=$(count "$I2/DS2_Crash.log" excecao)
  g1=$(count "$I1/DS2_Backread.log" "FALHA APARADA"); g2=$(count "$I2/DS2_Backread.log" "FALHA APARADA")
  games=$($D status 2>&1 | grep "processos do jogo" | grep -o "[0-9]\+" | wc -l)
  sess=$($D session 2>&1 | grep -c "p2pSessionVerified: Some(true)")
  arrived=$(tail -n +$((b1+1)) "$HI/DS2_Bonfire.log" | grep -ac "host: cheguei na fogueira")
  d1=$(count "$I1/DS2_Death.log" "custos da morte"); d2=$(count "$I2/DS2_Death.log" "custos da morte")
  echo "  games=$games session=$sess arrived=$arrived | exceptions host +$((n1-e1)) guest +$((n2-e2)) | trapped host +$((g1-f1)) guest +$((g2-f2)) | billed deaths 1:+$((d1-c1)) 2:+$((d2-c2))"
  if [ "$games" = 2 ] && [ "$sess" = 1 ] && [ "$arrived" -ge 1 ]; then
    ok=$((ok+1))
    if [ $((n1-e1)) = 0 ] && [ $((n2-e2)) = 0 ] && [ $((g1-f1)) = 0 ] && [ $((g2-f2)) = 0 ] && [ $((d1-c1)) = 0 ] && [ $((d2-c2)) = 0 ]; then
      clean=$((clean+1)); echo "  CLEAN"
    else
      echo "  ARRIVED WITH FAULTS"; [ "${KEEP_GOING:-0}" = 1 ] || break
    fi
  else
    echo "  FAILED at $NAME"; break
  fi
done
echo "=== $clean clean, $ok with the session up, of ${#legs[@]} legs ==="
