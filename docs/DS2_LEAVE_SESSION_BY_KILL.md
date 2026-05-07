# DS2 LeaveSession by Kill

This document records how Dark Souls II reports a PvP session ending after a player kill.

## Context

The behavior was observed with:

- Server-side `DS2PvpDebugLogging`
- Injector-side `DS2TraceLeaveSession`

The goal was to distinguish a legitimate kill-based session exit from the client-side phantom timer exit.

## Expected sequence

In a PvP kill flow, the expected sequence is:

1. The client enters the session.
2. The dead client sends `RequestNotifyDeath`.
3. The other client sends `RequestNotifyKillPlayer`.
4. The dead client sends `RequestNotifyLeaveSession`.
5. The other client sends `RequestNotifyLeaveGuestPlayer`.

## Validated timing

Validated injector timestamps:

- `JoinSession`: `time=6483.406`
- `RequestNotifyDeath`: `time=6501.671`
- `RequestNotifyLeaveSession`: `time=6514.062`

Delta:

- `Death` to `LeaveSession`: `12.391s`

This is different from the long-running phantom timer leave, which occurs after roughly 12 minutes and does not have recent death or kill evidence.

## Server-side session evidence

The server observed the same `session_id=1` across:

- `JoinGuestPlayer`
- `JoinSession`
- `Death`
- `KillPlayer`
- `LeaveSession`
- `LeaveGuestPlayer`

This gives a reliable grouping key for correlating both players' events.

## Injector fields

The injector records fields in `DS2ClientSessionTrace`.

Relevant fields include:

- message class
- sequence id
- thread id
- client timestamp
- raw protobuf bytes
- decoded protobuf fields
- stack signature
- recent session state

## Server diagnostic fields

The server logs diagnostic fields such as:

- `session_id`
- `session_duration`
- `leave_reason_hint`
- `evidence_flags`
- recent death / kill timestamps
- paired remote leave state
- local and remote profile ids

## How to recognize kill-based leave

A kill-based leave generally has:

- `leave_reason_hint=killed_or_death_related`
- recent `RequestNotifyDeath`
- recent `RequestNotifyKillPlayer`
- same `session_id`
- crossed `remote_profile_id` values
- paired remote leave or guest leave

## Rule of thumb

Use `session_id` as the grouping key.

Use `evidence_flags` for decisions.

Treat `leave_reason_hint` as a diagnostic hint only. It is useful for logs, but it should not be the only control-flow input.
