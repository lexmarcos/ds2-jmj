#!/bin/bash
# campanha.sh <fogueira> [minutos]: uma viagem em conjunto, do zero, com tudo
# conferido antes, na hora e depois. Aprendido a duras penas:
#  - os dois jogos tem que estar vivos ANTES, senao o teste nao mede nada
#    (rodei um ciclo inteiro com o convidado ja morto);
#  - sobreviver nao e passar: ja aconteceu de os dois ficarem de pe sem co-op;
#  - a queda pode vir minutos depois, entao a vigilancia e longa e olha
#    processo, sessao e papel a cada passada.
source /tmp/claude-1000/-home-suel-projects-ds2-jmj/9d261fb2-280e-47b8-ba23-4b87cded241b/scratchpad/viagem/lib.sh
FOG=$1; MIN=${2:-4}
vivos() { $D status 2>&1 | grep "processos do jogo" | grep -o "[0-9]\+" | wc -l; }
sessao() { $D session 2>&1 | grep -c "p2pSessionVerified: Some(true)"; }

echo "== preparo =="
$D session end >/dev/null 2>&1
# --force porque o save e restaurado logo abaixo, o que apaga qualquer ponto de
# desconexao ilegal. Sem isto o stop falha calado quando ha sessao viva, os
# jogos seguem com o estado velho e o teste mede outra coisa - aconteceu.
$D game stop --instance both --force >/dev/null 2>&1
[ "$(vivos)" = 0 ] || { echo "nao consegui parar os jogos; abortado"; exit 2; }
$D save restore --instance both base-majula >/dev/null 2>&1 || { echo "restore falhou"; exit 2; }
$D up --seamless --party >/dev/null 2>&1 || { echo "up falhou"; exit 2; }
$D human --instance both >/dev/null 2>&1
# `session end` pausa o DS2_Party e o `up` NAO retoma: sem isto nenhuma sessao
# se forma de novo, e o sintoma le como "o party parou de funcionar".
printf 'retoma\n' > "$I1/DS2_Party.req"; printf 'retoma\n' > "$I2/DS2_Party.req"
for i in $(seq 1 30); do [ "$(sessao)" = 1 ] && break; sleep 5; done
[ "$(vivos)" = 2 ] || { echo "nao ha dois jogos vivos; abortado"; exit 2; }
[ "$(sessao)" = 1 ] || { echo "sem sessao verificada; abortado"; exit 2; }
roles >/dev/null || exit 2
echo "  dois jogos vivos, sessao verificada, host=$HOST"

IH="$(inst $HOST)"; IG="$(inst $GUEST)"
cH=$(grep -ac excecao "$IH/DS2_Crash.log"); cG=$(grep -ac excecao "$IG/DS2_Crash.log")
nH=$(grep -c . "$IH/DS2_Bonfire.log")
echo "== viagem para $FOG =="
req $HOST "presenca retira"; sleep 5
req $HOST "nativo 0 $FOG"; sleep 3; req $GUEST "fantasma 0 $FOG"
for w in $(seq 1 90); do tail -n +$((nH+1)) "$IH/DS2_Bonfire.log" | grep -q "o mundo voltou\|pelo teto" && break; sleep 1; done
sleep 10; req $HOST "presenca recria"; sleep 6
echo "== logo apos =="; ./checar.sh $FOG || { echo "FALHOU na chegada"; exit 1; }

# O watchdog de 300 s: o warp arma o bit 0x10 de ctrl+0x1b8 e nao renova a
# marca em +0x1b4, que fica em 0.0 - entao ele dispara assim que o relogio da
# sessao passa de 300. Renovar a marca aqui e o teste e o conserto ao mesmo
# tempo. Se a sessao passar dos 180 s que caiu sem isto, esta diagnosticado.
if [ "$RENOVA" = 1 ]; then
  CTRL=$($D probe --instance $HOST "scan hc 1410d7998 8 8" 2>&1 | head -1 | python3 -c "
import json,sys
h=json.load(sys.stdin)['hits']
print([x for x in h if int(x,16)>0x7f0000000000][0][2:].lstrip('0'))" 2>/dev/null)
  if [ -n "$CTRL" ]; then
    REL=$($D probe --instance $HOST "abs c $CTRL 16" 2>&1 | head -1 | python3 -c "
import json,sys; b=bytes.fromhex(json.load(sys.stdin)['bytes']); print(b[8:12].hex())")
    $D probe --instance $HOST "pokeabs m $(printf '%x' $((0x$CTRL + 0x1b4))) $REL" >/dev/null 2>&1
    echo "  watchdog: marca renovada em $CTRL+0x1b4 com $REL"
  else
    echo "  watchdog: nao achei o controlador"
  fi
fi

echo "== vigiando $MIN min =="
for w in $(seq 1 $((MIN*6))); do
  sleep 10
  [ "$(vivos)" = 2 ] || { echo "  um jogo morreu apos $((w*10)) s"; 
    echo "  host: $(grep -a excecao "$IH/DS2_Crash.log" | tail -1 | awk '{print $1,$4}')";
    echo "  convidado: $(grep -a excecao "$IG/DS2_Crash.log" | tail -1 | awk '{print $1,$4}')"; exit 1; }
  [ "$(sessao)" = 1 ] || { echo "  a sessao caiu apos $((w*10)) s"; exit 1; }
done
echo "== depois de $MIN min =="; ./checar.sh $FOG
