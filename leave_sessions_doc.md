# DS2 PvP Leave Sessions

Este documento centraliza o comportamento observado com o server novo, usando:

- `DS2PvpDebugLogging` no server.
- `DS2TraceLeaveSession` no injector.

O foco aqui e comparar `LeaveSession` por kill e por tempo. O pacote
`RequestNotifyLeaveSession` nao traz motivo explicito; a classificacao no server e
heuristica, baseada em eventos proximos, duracao da sessao e estado rastreado.

## Conceitos

Cada sessao DS2 PvP tem dois lados:

- `session`: normalmente o lado do phantom/invocado que envia `RequestNotifyJoinSession`
  e depois `RequestNotifyLeaveSession`.
- `guest_player`: normalmente o outro lado que envia `RequestNotifyJoinGuestPlayer`
  e depois `RequestNotifyLeaveGuestPlayer`.

O server novo cria um `session_id` interno para amarrar os dois lados. Esse e o principal
campo para agrupar logs do mesmo encontro.

Campos importantes nos leaves do server:

```text
leave_reason_hint=<classificacao heuristica>
evidence_flags=<evidencias usadas>
session_id=<id interno da sessao>
session_duration=<segundos desde o join rastreado>
recent_kill_delta=<segundos desde KillPlayer, ou -1>
recent_death_delta=<segundos desde Death, ou -1>
recent_disconnect_delta=<segundos desde DisconnectSession, ou -1>
leave_order=<ordem global de leaves>
paired_remote_leave_delta=<delta do leave pareado, ou -1>
paired_session_id=<session_id do leave pareado>
paired_leave_order=<leave_order do leave pareado>
remote_profile_id=<perfil remoto>
field_1..field_4=<campos crus do protobuf>
session_role=session|guest_player
trace_state=known|unknown
recent_events=<resumo dos eventos rastreados>
```

## Logs do injector

O injector registra mensagens de sessao em `DS2_LeaveSessionTrace.log` com:

```text
event=DS2ClientSessionTrace
trace_scope=session_event|leave
class=RequestNotifyJoinSession|RequestNotifyDeath|RequestNotifyKillPlayer|RequestNotifyLeaveSession|...
thread_id=<id da thread>
sequence_id=<ordem local no processo>
stack_signature=<assinatura da callstack>
recent_events_before=<eventos recentes vistos antes desse pacote>
recent_*_delta=<segundos desde evento anterior, ou -1>
raw_hex=<protobuf bruto>
decoded_protobuf=<campos decodificados>
callstack=<frames>
```

Com a instrumentacao client-side nova, cada evento tambem inclui o estado persistente de
sessao visto pelo injector:

```text
client_trace_state=known|unknown
client_session_id=<id local do injector>
client_session_role=session|guest_player
client_remote_profile_id=<perfil remoto>
client_leave_reason_hint=<classificacao local>
client_evidence_flags=<evidencias locais>
client_session_duration=<segundos desde o join local>
client_recent_kill_delta=<segundos ou -1>
client_recent_death_delta=<segundos ou -1>
client_recent_disconnect_delta=<segundos ou -1>
client_prevent_timer_configured=0|1
client_prevent_timer_would_apply=0|1
client_recent_events=<eventos persistentes da sessao>
```

Esse estado persistente corrige a limitacao antiga em que `recent_events_before` ficava
`none` no timer porque o join tinha passado da janela curta de eventos recentes.

Observacao importante: nos testes validados, `RequestNotifyLeaveSession` por kill e por
tempo chegaram com o mesmo protobuf bruto e a mesma assinatura de stack final dentro da
mesma build do injector:

```text
raw_hex=08 01 10 05 18 00 20 00
field_1=1 field_2=5 field_3=0 field_4=0
stack_signature=0x1c71e1b222d64cf3
```

A assinatura pode mudar quando o injector muda, mas a regra importante e comparar kill e
timer dentro da mesma build. A conclusao continua a mesma: o ponto final de envio do leave
e compartilhado. A diferenca entre kill e timer nao esta no protobuf do leave nem nessa
callstack final; ela esta no estado anterior do jogo e nos eventos que ocorreram antes do
leave.

## Leave por kill

### Sequencia observada

Em uma morte por kill durante a sessao PvP, a sequencia confirmada foi:

1. O cliente entra na sessao.
2. O cliente morto envia `RequestNotifyDeath`.
3. O outro cliente envia `RequestNotifyKillPlayer`.
4. O cliente morto envia `RequestNotifyLeaveSession`.
5. O outro cliente envia `RequestNotifyLeaveGuestPlayer`.

### Injector no lado morto

No teste validado:

```text
JoinSession time=6483.406
RequestNotifyDeath time=6501.671
RequestNotifyLeaveSession time=6514.062
```

Deltas:

```text
Death depois do JoinSession: 18.265s
LeaveSession depois do Death: 12.391s
```

O leave por kill no lado morto apareceu assim:

```text
class=RequestNotifyLeaveSession
trace_scope=leave
recent_death_delta=12.391
recent_kill_delta=-1.000
recent_disconnect_delta=-1.000
recent_events_before=2:RequestNotifyDeath:12.391:...
```

No teste mais recente, com estado persistente do injector:

```text
class=RequestNotifyDeath
client_trace_state=known
client_session_id=2
client_session_role=session
client_leave_reason_hint=not_applicable
client_evidence_flags=trace_found|recent_death
client_session_duration=39.203
client_recent_death_delta=0.000
client_recent_events=JoinSession:39.203|Death:0.000

class=RequestNotifyLeaveSession
client_leave_reason_hint=killed_or_death_related
client_evidence_flags=trace_found|recent_death
client_session_duration=51.609
client_recent_death_delta=12.406
client_recent_kill_delta=-1.000
client_recent_disconnect_delta=-1.000
```

Interpretacao:

- `recent_death_delta` positivo e proximo ao leave e a evidencia local de morte.
- `recent_kill_delta=-1.000` no cliente morto e esperado, porque esse cliente morreu,
  nao matou.
- O pacote de leave em si continua igual ao pacote de timer.

### Server

No server, a mesma sessao apareceu com `session_id=1`:

```text
JoinGuestPlayer profile_id=1 remote_profile_id=3 session_role=guest_player
JoinSession profile_id=3 remote_profile_id=1 session_role=session
Death profile_id=3 matched_session_count=1
KillPlayer profile_id=1 remote_profile_id=3 matched_session_count=1
```

O lado morto:

```text
event_type=LeaveSession
profile_id=3
remote_profile_id=1
leave_reason_hint=killed_or_death_related
evidence_flags=trace_found|recent_death
session_id=1
session_duration=30.656
recent_death_delta=12.391
recent_kill_delta=-1.000
recent_disconnect_delta=-1.000
recent_events=JoinSession:30.656|Death:12.391
```

O outro lado:

```text
event_type=LeaveGuestPlayer
profile_id=1
remote_profile_id=3
leave_reason_hint=killed_or_death_related
evidence_flags=trace_found|recent_kill|paired_remote_leave
session_id=1
session_duration=33.219
recent_kill_delta=12.782
recent_death_delta=-1.000
recent_disconnect_delta=-1.000
paired_remote_leave_delta=0.438
recent_events=JoinGuestPlayer:33.219|KillPlayer:12.782
```

### Como reconhecer kill

Trate como leave por kill quando:

- `leave_reason_hint=killed_or_death_related`.
- O lado morto contem `recent_death` em `evidence_flags`.
- O outro lado contem `recent_kill` em `evidence_flags`.
- Ambos usam o mesmo `session_id`.
- `remote_profile_id` cruza os dois jogadores.
- O segundo leave normalmente contem `paired_remote_leave`.

Exemplo de cruzamento:

```text
LeaveSession profile_id=3 remote_profile_id=1 session_id=1
LeaveGuestPlayer profile_id=1 remote_profile_id=3 session_id=1
```

## Leave por tempo

### Sequencia observada

Na expiracao natural do phantom, a sequencia confirmada foi:

1. O cliente entra na sessao.
2. Nenhum `RequestNotifyDeath` aparece antes do leave.
3. Nenhum `RequestNotifyKillPlayer` aparece antes do leave.
4. Nenhum `RequestNotifyDisconnectSession` aparece antes do leave.
5. O cliente envia `RequestNotifyLeaveSession` depois de aproximadamente 12m38s.
6. O outro lado envia `RequestNotifyLeaveGuestPlayer` logo depois como leave pareado.

### Injector no lado que expirou

No teste validado:

```text
JoinSession time=6707.078
RequestNotifyLeaveSession time=7465.625
```

Delta:

```text
LeaveSession depois do JoinSession: 758.547s
```

O leave por tempo apareceu assim no injector:

```text
class=RequestNotifyLeaveSession
trace_scope=leave
recent_events_before=none
recent_join_session_delta=-1.000
recent_kill_delta=-1.000
recent_death_delta=-1.000
recent_disconnect_delta=-1.000
```

No teste mais recente, com estado persistente do injector:

```text
class=RequestNotifyLeaveSession
client_trace_state=known
client_session_id=1
client_session_role=session
client_leave_reason_hint=timer_candidate
client_evidence_flags=trace_found|duration_in_timer_window
client_session_duration=763.734
client_recent_kill_delta=-1.000
client_recent_death_delta=-1.000
client_recent_disconnect_delta=-1.000
client_prevent_timer_configured=0
client_prevent_timer_would_apply=0
client_recent_events=1:RequestNotifyJoinSession:763.734:...
```

Detalhe: `recent_events_before=none` no injector nao significa que nao houve join. Esse
campo ainda usa uma janela curta. Para duracao real no cliente, use `client_session_duration`
e `client_recent_events`; para cruzar com o server, use `session_duration` do server.

### Server

No server, a mesma sessao apareceu com `session_id=2`.

O lado que iniciou o leave por tempo:

```text
event_type=LeaveSession
profile_id=3
remote_profile_id=1
leave_reason_hint=timer_candidate
evidence_flags=trace_found|duration_in_timer_window
session_id=2
session_duration=758.516
recent_kill_delta=-1.000
recent_death_delta=-1.000
recent_disconnect_delta=-1.000
paired_remote_leave_delta=-1.000
recent_events=JoinSession:758.516
```

O outro lado:

```text
event_type=LeaveGuestPlayer
profile_id=1
remote_profile_id=3
leave_reason_hint=paired_remote_leave
evidence_flags=trace_found|duration_in_timer_window|paired_remote_leave
session_id=2
session_duration=761.171
recent_kill_delta=-1.000
recent_death_delta=-1.000
recent_disconnect_delta=-1.000
paired_remote_leave_delta=0.546
recent_events=JoinGuestPlayer:761.171
```

### Como reconhecer timer

Trate como leave por tempo quando:

- O primeiro leave tem `leave_reason_hint=timer_candidate`.
- `evidence_flags` contem `duration_in_timer_window`.
- `session_duration` fica perto da janela observada, aproximadamente `758s` neste teste.
- `recent_kill_delta=-1.000`.
- `recent_death_delta=-1.000`.
- `recent_disconnect_delta=-1.000`.
- O leave do outro lado normalmente fica como `paired_remote_leave`.

Exemplo de cruzamento:

```text
LeaveSession profile_id=3 remote_profile_id=1 session_id=2 session_duration=758.516
LeaveGuestPlayer profile_id=1 remote_profile_id=3 session_id=2 session_duration=761.171
```

## Probe do timer candidate

Quando o injector classifica um leave como `timer_candidate`, ele adiciona um bloco
`timer_candidate_probe` no log. Esse bloco nao aplica patch ainda; ele coleta bytes ao redor
dos frames `DarkSoulsII` para ajudar a encontrar um hook anterior ao envio final.

No teste mais recente:

```text
timer_candidate_probe prevent_configured=0 prevent_would_apply=0 prevent_action=none timer_min=700.000 timer_max=820.000
```

Com a flag experimental ligada, antes de existir um hook anterior real, a expectativa e:

```text
prevent_configured=1
prevent_would_apply=1
prevent_action=not_applied_no_timer_patch_hook
```

Frames relevantes observados no timer:

```text
game_rel=0x6ab737
game_rel=0x2c3c19
game_rel=0x2c36d7
game_rel=0x2c9844
```

O frame `0x6ab737` contem o valor `ba eb 03 00 00`, que carrega `0x03EB`, o tipo de
mensagem `RequestNotifyLeaveSession`. Isso indica que esse frame e parte do envio/montagem
final do pacote, nao necessariamente o timer.

Os frames mais interessantes para investigar o estado anterior sao:

```text
0x2c3c19
0x2c36d7
0x2c9844
```

No timer, perto de `0x2c3c19`, apareceu uma escrita de estado:

```text
c7 87 f8 00 00 00 09 00 00 00
```

Isso parece escrever `9` em um campo de objeto em `this+0xF8`. Ainda nao esta provado que
esse campo seja o motivo do timer, porque kill e timer passam pelo mesmo finalizador de
leave. Mas esta regiao e um candidato melhor para instrumentacao do que o hook final de
protobuf.

## Comparacao kill vs timer

Resumo dos testes validados com a instrumentacao atual:

```text
Kill:
client_leave_reason_hint=killed_or_death_related
client_evidence_flags=trace_found|recent_death
client_session_duration=51.609
client_recent_death_delta=12.406
client_recent_kill_delta=-1.000
stack_signature=0x1c71e1b222d64cf3

Timer:
client_leave_reason_hint=timer_candidate
client_evidence_flags=trace_found|duration_in_timer_window
client_session_duration=763.734
client_recent_death_delta=-1.000
client_recent_kill_delta=-1.000
client_recent_disconnect_delta=-1.000
stack_signature=0x1c71e1b222d64cf3
```

Conclusoes:

- O leave final usa o mesmo protobuf em kill e timer.
- A stack final de serializacao do `RequestNotifyLeaveSession` tambem e a mesma.
- O injector agora consegue classificar timer e kill localmente antes de qualquer patch.
- `client_session_duration` bate com o `session_duration` do server com margem pequena
  (`763.734s` no cliente contra `763.750s` no server no teste mais recente).
- O server consegue classificar usando eventos anteriores e duracao.
- O server nao decide a expiracao do timer; ele recebe o leave ja decidido pelo cliente.
- Bloquear ou alterar apenas o handler do server tende a ser insuficiente para deixar a
  sessao infinita, porque o cliente ja iniciou a desmontagem local.

## Uso em futuras alteracoes

Para novas alteracoes:

- Use `session_id` para agrupar os dois lados.
- Use `evidence_flags` para decisao automatica.
- Use `leave_reason_hint` como leitura humana da classificacao.
- Use `session_duration` para separar timer de leaves curtos.
- Use `recent_death_delta` e `recent_kill_delta` para preservar o comportamento de morte.
- Use `paired_remote_leave_delta` para identificar o segundo leave causado pelo outro lado.

Para investigar PvP infinito:

- Nao use o protobuf do `RequestNotifyLeaveSession` como discriminador; ele e igual nos
  dois casos observados.
- Nao use apenas a `stack_signature` final; ela tambem foi igual nos dois casos observados.
- Procure o ponto anterior no cliente onde a duracao da sessao e avaliada ou onde o estado
  decide chamar o envio de `RequestNotifyLeaveSession`.
- Priorize instrumentar os candidatos `0x2c3c19`, `0x2c36d7` e `0x2c9844` antes de tentar
  bloquear qualquer coisa.
- Qualquer patch deve preservar leaves com `recent_death` ou `recent_kill`, para nao
  quebrar a finalizacao normal por morte.
