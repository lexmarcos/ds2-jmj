#!/bin/bash
# checar.sh <mapa esperado>: a regra de aceitacao de verdade.
# Sobreviver nao basta - ja aconteceu de os dois ficarem de pe sem co-op nenhum.
source /tmp/claude-1000/-home-suel-projects-ds2-jmj/9d261fb2-280e-47b8-ba23-4b87cded241b/scratchpad/viagem/lib.sh
esperado=$1; ok=1
l1=$($D character --instance 1 2>&1); l2=$($D character --instance 2 2>&1)
p1=$(echo "$l1" | grep -oE "papel [0-9]+" | awk '{print $2}')
p2=$(echo "$l2" | grep -oE "papel [0-9]+" | awk '{print $2}')
f1=$(echo "$l1" | grep -oE "fogueira [0-9a-f]+/0*[0-9a-f]+" | sed -E 's|.*/0*||')
m1=$(echo "$l1" | grep -oE "pos \([^)]*\)")
m2=$(echo "$l2" | grep -oE "pos \([^)]*\)")
ses=$($D session 2>&1 | grep -o "p2pSessionVerified: Some(true)")
echo "  host: papel $p1 $m1 fogueira $f1"
echo "  convidado: papel $p2 $m2"
[ -n "$ses" ] && echo "  sessao: verificada" || { echo "  sessao: CAIU"; ok=0; }
[ "$p2" = "1" ] || { echo "  *** o convidado nao e mais fantasma (papel $p2)"; ok=0; }
[ "$f1" = "$esperado" ] || { echo "  *** o host nao esta na fogueira $esperado"; ok=0; }
[ "$ok" = 1 ] && echo "  => VIAGEM EM CONJUNTO OK" || echo "  => FALHOU"
exit $((1-ok))
