#!/bin/bash
# campanha.sh <bonfire> [minutes]: a joint travel, from scratch, with
# everything checked before, during and after. Learned the hard way:
#  - both games have to be alive BEFORE, or the test measures nothing
#    (I ran a whole cycle with the guest already dead);
#  - surviving is not passing: both have stood up with no co-op before;
#  - the fall can come minutes later, so the watch is long and looks at
#    process, session and role on every pass.
source /tmp/claude-1000/-home-suel-projects-ds2-jmj/9d261fb2-280e-47b8-ba23-4b87cded241b/scratchpad/viagem/lib.sh
FOG=$1; MIN=${2:-4}
vivos() { $D status 2>&1 | grep "processos do jogo" | grep -o "[0-9]\+" | wc -l; }
sessao() { $D session 2>&1 | grep -c "p2pSessionVerified: Some(true)"; }

echo "== preparation =="
$D session end >/dev/null 2>&1
# --force because the save is restored right below, which wipes any illegal
# disconnect point. Without this the stop fails quietly when a session is live,
# the games carry on with the old state and the test measures something else -
# it happened.
$D game stop --instance both --force >/dev/null 2>&1
[ "$(vivos)" = 0 ] || { echo "could not stop the games; aborted"; exit 2; }
$D save restore --instance both base-majula >/dev/null 2>&1 || { echo "restore failed"; exit 2; }
$D up --seamless --party >/dev/null 2>&1 || { echo "up failed"; exit 2; }
$D human --instance both >/dev/null 2>&1
# `session end` pauses DS2_Party and `up` does NOT resume it: without this no
# session forms again, and the symptom reads as "the party stopped working".
printf 'retoma\n' > "$I1/DS2_Party.req"; printf 'retoma\n' > "$I2/DS2_Party.req"
for i in $(seq 1 30); do [ "$(sessao)" = 1 ] && break; sleep 5; done
[ "$(vivos)" = 2 ] || { echo "there are not two live games; aborted"; exit 2; }
[ "$(sessao)" = 1 ] || { echo "no verified session; aborted"; exit 2; }
roles >/dev/null || exit 2
echo "  two live games, verified session, host=$HOST"

IH="$(inst $HOST)"; IG="$(inst $GUEST)"
cH=$(grep -ac excecao "$IH/DS2_Crash.log"); cG=$(grep -ac excecao "$IG/DS2_Crash.log")
nH=$(grep -c . "$IH/DS2_Bonfire.log")
echo "== travel to $FOG =="
req $HOST "presenca retira"; sleep 5
req $HOST "nativo 0 $FOG"; sleep 3; req $GUEST "fantasma 0 $FOG"
for w in $(seq 1 90); do tail -n +$((nH+1)) "$IH/DS2_Bonfire.log" | grep -q "o mundo voltou\|pelo teto" && break; sleep 1; done
sleep 10; req $HOST "presenca recria"; sleep 6
echo "== right after =="; ./checar.sh $FOG || { echo "FAILED on arrival"; exit 1; }

# The 300 s watchdog: the warp arms bit 0x10 of ctrl+0x1b8 and does not renew
# the mark at +0x1b4, which stays at 0.0 - so it fires as soon as the session
# clock passes 300. Renewing the mark here is the test and the fix at the same
# time. If the session gets past the 180 s it dropped at without this, it is
# diagnosed.
if [ "$RENOVA" = 1 ]; then
  CTRL=$($D probe --instance $HOST "scan hc 1410d7998 8 8" 2>&1 | head -1 | python3 -c "
import json,sys
h=json.load(sys.stdin)['hits']
print([x for x in h if int(x,16)>0x7f0000000000][0][2:].lstrip('0'))" 2>/dev/null)
  if [ -n "$CTRL" ]; then
    REL=$($D probe --instance $HOST "abs c $CTRL 16" 2>&1 | head -1 | python3 -c "
import json,sys; b=bytes.fromhex(json.load(sys.stdin)['bytes']); print(b[8:12].hex())")
    $D probe --instance $HOST "pokeabs m $(printf '%x' $((0x$CTRL + 0x1b4))) $REL" >/dev/null 2>&1
    echo "  watchdog: mark renewed at $CTRL+0x1b4 with $REL"
  else
    echo "  watchdog: could not find the controller"
  fi
fi

echo "== watching $MIN min =="
for w in $(seq 1 $((MIN*6))); do
  sleep 10
  [ "$(vivos)" = 2 ] || { echo "  a game died after $((w*10)) s"; 
    echo "  host: $(grep -a excecao "$IH/DS2_Crash.log" | tail -1 | awk '{print $1,$4}')";
    echo "  guest: $(grep -a excecao "$IG/DS2_Crash.log" | tail -1 | awk '{print $1,$4}')"; exit 1; }
  [ "$(sessao)" = 1 ] || { echo "  the session dropped after $((w*10)) s"; exit 1; }
done
echo "== after $MIN min =="; ./checar.sh $FOG
