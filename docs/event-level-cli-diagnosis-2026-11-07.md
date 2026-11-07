# Event-level pilot: trace diagnosis and CLI reference, 2026-11-07

The two db attempts from `paired-event-level-pilot-2026-11-07.md` reached the
estimated 5 RUB budget without completing. No additional model calls were made
for this diagnosis.

## Observed attempt failures

* Attempt 1 read the internal project/state mirror despite the instruction to
  use the CLI, then used outline and show to locate roles. Its first batch was
  rejected because the same role occurred twice. A later batch was accepted;
  a subsequent batch failed on a stale role version. The independently replayed
  result failed with `NameError: EventLevel is not defined`.
* Attempt 2 used an unscoped outline, tried an incorrect role ID and attempted to
  read source files before they were materialised. Its first batch failed on a
  stale version. A later batch and a repair passed the selected project suite,
  but the independent verifier still found missing level propagation in
  subscribers, persistence, DTO and emit. The last shell command returned zero
  even though the verifier reported failure; a pipeline masked the check status.
* Both attempts ended because of the trial request budget. These observations
  do not prove which individual error caused the failure or whether a larger
  budget or different model would have solved the task.

## Public CLI reference

An isolated copy of the pristine db arm was compared with a separately imported
copy of the known-good reference. Public `outline` and `show --json` identified
41 roles across the four task files; 10 roles differed. One `apply --check
'bash check.sh'` batch replaced those role bodies using the current versions. It
passed the behavioral verifier and the selected project suite. The four rendered
source files matched the reference bytes exactly.

The checked apply took 5.711 seconds, including the project and behavioral
checks, and returned 4,228 bytes. Individual `outline` and `show` invocations
took 17–19 ms in this single local run. This diagnostic used the already-known
solution and read both indexed versions (22,617 and 23,210 bytes for two bulk
`show` responses); it is an oracle-assisted feasibility check, not an agent
benchmark or evidence of token savings. Temporary workspace:
`/tmp/tfe-event-cli-reference-pg3p1r_u`.

The CLI can therefore express this particular reference solution in one batch.
The traces suggest useful follow-up questions around obtaining fresh versions,
building one edit per role, interpreting command and verifier failures, and
retrieving only relevant context. They do not establish which change would save
money without a controlled comparison.
