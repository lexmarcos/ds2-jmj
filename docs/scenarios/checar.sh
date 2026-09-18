#!/bin/bash
# checar.sh <expected bonfire>: the real acceptance rule.
# Surviving is not enough - both have stood up with no co-op at all before.
source /tmp/claude-1000/-home-suel-projects-ds2-jmj/9d261fb2-280e-47b8-ba23-4b87cded241b/scratchpad/viagem/lib.sh
esperado=$1; ok=1
l1=$($D character --instance 1 2>&1); l2=$($D character --instance 2 2>&1)
p1=$(echo "$l1" | grep -oE "papel [0-9]+" | awk '{print $2}')
p2=$(echo "$l2" | grep -oE "papel [0-9]+" | awk '{print $2}')
f1=$(echo "$l1" | grep -oE "fogueira [0-9a-f]+/0*[0-9a-f]+" | sed -E 's|.*/0*||')
m1=$(echo "$l1" | grep -oE "pos \([^)]*\)")
m2=$(echo "$l2" | grep -oE "pos \([^)]*\)")
ses=$($D session 2>&1 | grep -o "p2pSessionVerified: Some(true)")
echo "  host: role $p1 $m1 bonfire $f1"
echo "  guest: role $p2 $m2"
[ -n "$ses" ] && echo "  session: verified" || { echo "  session: DROPPED"; ok=0; }
[ "$p2" = "1" ] || { echo "  *** the guest is no longer a phantom (role $p2)"; ok=0; }
[ "$f1" = "$esperado" ] || { echo "  *** the host is not at bonfire $esperado"; ok=0; }
[ "$ok" = 1 ] && echo "  => JOINT TRAVEL OK" || echo "  => FAILED"
exit $((1-ok))
