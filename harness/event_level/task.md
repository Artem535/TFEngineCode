# Task: persist and expose an event severity level

The observability event journal has no severity field. Add `level` to events
through the Python observability and web API layers so clients can distinguish
normal, warning and error events. Preserve existing behavior and callers.

Requirements:

1. `Event.level` is one of `info`, `warning`, `error`; omitted level defaults
   to `info`, and invalid levels are rejected by the event model.
2. `EventBus.publish` and the `emit` helper accept an optional `level` argument
   defaulting to `info`. The chosen level reaches the published event and
   subscribers; a call without a session context remains a no-op.
3. Persist and read `level` in `EventStore`. Opening an existing SQLite database
   created with the current schema migrates it safely and idempotently; existing
   events read as `info`, and a fresh database works too. Preserve ordering,
   payload and other existing fields.
4. `EventDTO.from_event` exposes `level`, so REST and websocket event DTOs carry
   it. Existing callers that do not provide a level keep working.

Verify with `bash check.sh` from the workspace root. Do not modify tests,
`verify.py` or `check.sh`.
