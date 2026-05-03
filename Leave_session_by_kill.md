# LeaveSession por kill no DS2 PvP

Este documento descreve somente o comportamento observado com o server novo, usando a
instrumentacao `DS2PvpDebugLogging` no server e `DS2TraceLeaveSession` no injector.

## Sequencia esperada

Em uma morte por kill durante uma sessao PvP DS2, a sequencia confirmada e:

1. O cliente entra na sessao.
2. O cliente morto envia `RequestNotifyDeath`.
3. O outro cliente envia `RequestNotifyKillPlayer`.
4. O cliente morto envia `RequestNotifyLeaveSession`.
5. O outro cliente envia `RequestNotifyLeaveGuestPlayer`.

No teste validado, os tempos foram:

- `JoinSession` no injector: `time=6483.406`.
- `RequestNotifyDeath` no injector: `time=6501.671`.
- `RequestNotifyLeaveSession` no injector: `time=6514.062`.
- Delta entre `Death` e `LeaveSession`: `12.391s`.

No server, a mesma sessao apareceu com `session_id=1`:

- `JoinGuestPlayer`: `profile_id=1`, `remote_profile_id=3`, `session_role=guest_player`.
- `JoinSession`: `profile_id=3`, `remote_profile_id=1`, `session_role=session`.
- `Death`: `profile_id=3`, `matched_session_count=1`.
- `KillPlayer`: `profile_id=1`, `remote_profile_id=3`, `matched_session_count=1`.
- `LeaveSession`: `profile_id=3`, `leave_reason_hint=killed_or_death_related`.
- `LeaveGuestPlayer`: `profile_id=1`, `leave_reason_hint=killed_or_death_related`.

## Logs do injector

O injector registra eventos de sessao com:

```text
event=DS2ClientSessionTrace
trace_scope=session_event
class=RequestNotifyJoinSession|RequestNotifyDeath|RequestNotifyKillPlayer|...
thread_id=<id>
sequence_id=<ordem local>
stack_signature=<assinatura>
recent_events_before=<eventos recentes>
recent_*_delta=<segundos desde evento anterior>
```

Para morte por kill, o ponto principal e o `RequestNotifyDeath` antes do
`RequestNotifyLeaveSession`:

```text
class=RequestNotifyDeath
recent_join_session_delta=18.265

class=RequestNotifyLeaveSession
recent_death_delta=12.391
recent_events_before=2:RequestNotifyDeath:12.391:...
```

Interpretacao:

- `recent_death_delta` positivo e proximo ao leave e a evidencia local de morte.
- `recent_kill_delta=-1.000` no cliente morto e esperado se esse cliente apenas morreu.
- `sequence_id` ajuda a verificar a ordem real vista por aquele processo.
- `stack_signature` ajuda a comparar se eventos iguais vieram do mesmo caminho do jogo.

## Logs do server

O server novo adiciona campos de diagnostico nos leaves:

```text
leave_reason_hint=<classificacao>
evidence_flags=<evidencias usadas>
session_id=<id interno da sessao>
session_duration=<segundos>
recent_kill_delta=<segundos ou -1>
recent_death_delta=<segundos ou -1>
recent_disconnect_delta=<segundos ou -1>
leave_order=<ordem global do leave>
paired_remote_leave_delta=<delta do leave pareado ou -1>
paired_session_id=<session_id pareado>
paired_leave_order=<leave_order pareado>
remote_profile_id=<perfil remoto>
field_1..field_4=<campos crus do protobuf>
session_role=session|guest_player
trace_state=known|unknown
recent_events=<eventos rastreados da sessao>
```

No caso validado, o lado morto ficou assim:

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
recent_events=JoinSession:30.656|Death:12.391
```

O outro lado ficou assim:

```text
event_type=LeaveGuestPlayer
profile_id=1
remote_profile_id=3
leave_reason_hint=killed_or_death_related
evidence_flags=trace_found|recent_kill|paired_remote_leave
session_id=1
session_duration=33.219
recent_kill_delta=12.782
paired_remote_leave_delta=0.438
recent_events=JoinGuestPlayer:33.219|KillPlayer:12.782
```

## Como reconhecer morte por kill

Uma sessao deve ser tratada como morte por kill quando:

- O leave tem `leave_reason_hint=killed_or_death_related`.
- O lado morto tem `evidence_flags` contendo `recent_death`.
- O outro lado tem `evidence_flags` contendo `recent_kill`.
- Ambos compartilham o mesmo `session_id`.
- `remote_profile_id` cruza os dois jogadores.
- O segundo leave normalmente tem `paired_remote_leave`.

Exemplo de cruzamento:

```text
LeaveSession profile_id=3 remote_profile_id=1 session_id=1
LeaveGuestPlayer profile_id=1 remote_profile_id=3 session_id=1
```

## Campos importantes para futuras alteracoes

- `session_id`: chave principal para agrupar os dois lados da sessao.
- `leave_reason_hint`: classificacao heuristica do motivo do leave.
- `evidence_flags`: lista compacta do que sustentou a classificacao.
- `recent_death_delta`: evidencia de morte recente no lado que morreu.
- `recent_kill_delta`: evidencia de kill recente no lado que matou.
- `paired_remote_leave_delta`: confirma que o outro lado saiu logo depois/antes.
- `matched_session_count`: confirma que `Death` ou `KillPlayer` foi associado a uma sessao ativa.
- `recent_events`: resumo humano para auditar a ordem sem abrir todo o log.

## Regra pratica

Para novas alteracoes, use `session_id` como agrupador e `evidence_flags` como fonte de
decisao. `leave_reason_hint` e util para leitura humana, mas continua sendo uma inferencia:
o protobuf `RequestNotifyLeaveSession` nao traz motivo explicito.
