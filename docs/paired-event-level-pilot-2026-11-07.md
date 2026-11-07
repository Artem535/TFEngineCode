# Multi-file event-level pilot, 2026-11-07

The real TabletopMagnat Python project was given an event-level contract spanning
the event model, event bus, SQLite persistence and migration, and web DTO. The
task and behavioral verifier are in `harness/event_level/`. The verifier checks
defaults and validation, subscriber delivery, old-schema migration (including a
second open), persisted ordering and payload, DTOs, `emit` and a fresh database.
The unmodified project failed the verifier (`Event` had no `level`). A reference
implementation in a separate temporary copy passed both the verifier and the
selected project suite (66 passed, 2 deselected; one file ignored).

Two alternating-order pairs used the same provider/model as previous trials,
full arm-specific skills and the same frozen task, verifier and project copy.
Each attempt had a 5 RUB estimated-cost threshold; the series had a 20 RUB
threshold checked between attempts. Outputs were replayed into a pristine arm
before independent grading.

| Attempt | Passed | Estimated RUB | Input tokens | Agent wall seconds | API seconds |
| --- | --- | ---: | ---: | ---: | ---: |
| files 1 | yes | 1.245665 | 31,972 | 25.773 | 14.427 |
| db 1 | no | 5.338700 | 145,847 | 30.958 | 30.375 |
| files 2 | yes | 1.254262 | 31,233 | 25.215 | 14.124 |
| db 2 | no | 5.092684 | 143,450 | 42.783 | 31.160 |

Both db attempts stopped at the budget threshold and did not complete. The first
failed the verifier with `NameError: EventLevel` and the project suite during
collection; the second passed the selected project suite but failed the level
delivery, persistence, DTO and emit checks. Each exceeded the threshold because
the last response is accounted for after receipt. Both source attempts passed.
The total estimate is 12.931311 RUB, reproduced from raw events. The provider
did not supply actual charges; the estimate uses 33 RUB/M input and 133 RUB/M
output with no separate cached-input discount. Agent wall time includes the
model run and its commands/checks but excludes workspace setup and replay grading.

This is a two-pair pilot on one task and model, not a general quality or cost
claim. Both approaches were allowed up to the same budget; the role arm might
have completed with a larger budget, at additional cost. The prompts differ by
workflow, skill and environment. The unchanged project suite is not a full-suite
pass because two tests and one test file were excluded.

Raw events, frozen prompts, skills, workspaces, grades and report are temporarily
in `/tmp/tfe-event-level-pilot-20261107`. The reference copy is temporarily in
`/tmp/tfe-event-level-reference-20261107`; neither path is durable.
