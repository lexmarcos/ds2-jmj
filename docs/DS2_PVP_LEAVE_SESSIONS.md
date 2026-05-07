# DS2 PvP Leave Sessions

This document centralizes the findings about Dark Souls II PvP session endings, especially the difference between a normal kill-based leave and the client-side phantom timer leave.

## Instrumentation used

Observed with:

- Server-side `DS2PvpDebugLogging`
- Injector-side `DS2TraceLeaveSession`

## Concepts

DS2 PvP sessions have two visible roles in the server flow:

- `session`: the main host/session side
- `guest_player`: the joined guest side

The server assigns an internal `session_id`. Use this id to group related PvP events.

## Important server leave fields

Important fields emitted by the server diagnostics:

- `leave_reason_hint`
- `evidence_flags`
- `session_id`
- `session_duration`
- death / kill / disconnect deltas
- paired remote leave fields
- raw protobuf fields
- session role
- trace state
- recent events

## Injector trace fields

The injector records:

- message class
- raw protobuf bytes
- decoded protobuf fields
- stack signature
- callstack when tracing is enabled
- persistent session state

## Important observation

In validated builds, kill leave and timer leave produced the same final `RequestNotifyLeaveSession` payload and stack signature.

Observed raw protobuf:

```text
raw_hex=08 01 10 05 18 00 20 00
field_1=1 field_2=5 field_3=0 field_4=0
```

Observed stack signature:

```text
stack_signature=0x1c71e1b222d64cf3
```

Conclusion: the final leave message is not enough to distinguish kill from timer. The decision must use recent session evidence.

## Kill leave

Typical kill sequence:

1. Client enters a PvP session.
2. Dead client sends `RequestNotifyDeath`.
3. Other client sends `RequestNotifyKillPlayer`.
4. Dead client sends `RequestNotifyLeaveSession`.
5. Other client sends `RequestNotifyLeaveGuestPlayer`.

Validated injector timestamps:

- `JoinSession`: `time=6483.406`
- `RequestNotifyDeath`: `time=6501.671`
- `RequestNotifyLeaveSession`: `time=6514.062`

Delta:

- `Death` to `LeaveSession`: `12.391s`

How to recognize:

- recent death evidence exists
- recent kill evidence exists
- same `session_id`
- remote profile ids cross correctly
- paired remote leave is present or expected

## Timer leave

The phantom timer leave has a different pattern:

- no recent death
- no recent kill
- no disconnect evidence before leave
- session duration around 12 minutes and 38 seconds
- final leave protobuf same as kill leave
- final stack signature same as kill leave in validated builds

Validated timer timestamps:

- `JoinSession`: `time=6707.078`
- `RequestNotifyLeaveSession`: `time=7465.625`

Delta:

- `758.547s`

Persistent state example:

```text
client_leave_reason_hint=timer_candidate
```

Server-side example:

```text
session_id=2
```

How to recognize:

- session duration is near the timer window
- no recent death or kill evidence
- leave comes from the normal client path
- same raw leave protobuf as kill

## Timer candidate probe

Candidate frames observed during timer investigation:

- `0x6ab737`
- `0x2c3c19`
- `0x2c36d7`
- `0x2c9844`

Notes:

- `0x6ab737` contains `ba eb 03 00 00`, matching message id `0x03EB`.
- `0x2c3c19` is near `c7 87 f8 00 00 00 09 00 00 00`.

These were useful for discovery but should not be treated as stable public APIs.

## Comparison

| Signal | Kill leave | Timer leave |
| --- | --- | --- |
| Recent death | Yes | No |
| Recent kill | Yes | No |
| Session duration | Short after death | Around 758s |
| Final protobuf | Same observed payload | Same observed payload |
| Final stack signature | Same observed signature | Same observed signature |
| Best classifier | Recent evidence | Duration plus missing evidence |

## Conclusions

The final `RequestNotifyLeaveSession` cannot be blocked blindly because it is used for legitimate kill cleanup.

The phantom timer is client-side. The safer fix is to patch or refresh the client-side timer before it emits the normal leave message.

Server-side blocking should remain a last resort because it risks leaving stale sessions or breaking normal PvP cleanup.

## Future usage

Use this document when changing:

- leave-session diagnostics
- phantom timer patches
- PvP cleanup logic
- server-side session classification

Do not classify leave reason from the final leave protobuf alone.
