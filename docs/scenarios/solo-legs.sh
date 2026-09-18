#!/bin/bash
# solo-legs.sh <destination>... : the same transport as travel-legs.sh, one game,
# no session. The control: whatever fails here is the transport's own, not the
# session's. Instance 1 only; it must be in the world with the seamless hooks.
D=/home/suel/projects/ds2-jmj/Source/LoaderLinux/target/debug/ds2os-dev
I1="/mnt/ssd/SteamLibrary/steamapps/common/Dark Souls II Scholar of the First Sin"
declare -A MAPS=( [majula]="0a040000 122a" [heide]="0a1f0000 7ba2" [ironkeep]="0a130000 4cc2" [brume]="140b0000 2d82" )
count() { grep -ac "$2" "$1" 2>/dev/null || echo 0; }
n=0; clean=0
for dest in "$@"; do
  set -- ${MAPS[$dest]}
  e=$(count "$I1/DS2_Crash.log" excecao); f=$(count "$I1/DS2_Backread.log" "FALHA APARADA")
  b=$(wc -l < "$I1/DS2_Bonfire.log")
  printf 'ir %s %s\n' "$1" "$2" > "$I1/DS2_Bonfire.req"
  sleep 60
  g=$(count "$I1/DS2_Crash.log" excecao); h=$(count "$I1/DS2_Backread.log" "FALHA APARADA")
  alive=$($D status 2>&1 | grep "processos do jogo" | grep -o "[0-9]\+" | wc -l)
  arrived=$(tail -n +$((b+1)) "$I1/DS2_Bonfire.log" | grep -acE "o mundo assentou|cheguei na fogueira")
  n=$((n+1))
  echo "$n $dest: alive=$alive arrived=$arrived exceptions +$((g-e)) trapped +$((h-f))"
  [ "$alive" -ge 1 ] && [ "$arrived" -ge 1 ] && [ $((g-e)) = 0 ] && [ $((h-f)) = 0 ] || { echo "  FAILED at $dest"; break; }
  clean=$((clean+1))
done
echo "=== $clean of $n solo legs clean ==="
