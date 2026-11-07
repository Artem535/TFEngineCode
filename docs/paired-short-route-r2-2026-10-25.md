# Short-route paired pilot, revision 2, 2026-10-25

**The corrected skill did not restore the short workflow. No comparable pair
was obtained.** This was a separately user-authorized repeat, not an automatic
retry of the previous pilot.

| Attempt | Completion | Artifact | API responses | Shell commands | Wall seconds | Reported RUB |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| Roles, task 1 | Incomplete: step limit | Not created | 12 | 13 | 84.171 | 0.071424 |

The first two commands tried a nonexistent `/workspace` directory. After finding
the actual workspace, the model searched for unavailable consumer-package role
definitions, despite the runtime skill explicitly saying the packages and
additional source files were absent. No apply operation was attempted and no
target file was created. This is incomplete execution without an artifact;
it is not evidence of the quality of a generated implementation.

The protocol stopped the entire pilot after the incomplete first stage. Files
stages1/2 and roles stage2 were not started; their costs are unknown/not measured,
not zero-cost successful results. There is no comparative cost or speed estimate.

All 12 responses contain provider usage. Reported cost is 0.071424 RUB, computed
with Decimal from raw events. Together with the previous incomplete short-route
pilot, reported cost is 0.174468 RUB. These are provider-reported sums, not a
certified bill. The conservative possible_unreported_charge flag remains true
for incomplete turns. Current Codex and local preparation costs are unavailable.

## Controls

Only role SKILL.md and the protocol revision note changed from the preceding
pilot. Runner, model parameters, native executable, tool implementation, grader,
TASK bytes, file-control skill and runtime context remained unchanged. The exact
stdin heredoc and output mapping from the corrected skill passed in an isolated
preflight workspace. No target implementation was prepared in live workspaces.
Frozen protected hashes and the unchanged critic example were verified after
execution. No provider key appeared in the archive. API time83.640s, tool
time0.467s; input43260, output834, cached input36096 tokens. Native binary omitted
from the evidence archive; its fingerprint remains in the manifest.

## Interpretation

Explicit stdin/path guidance fixes a real ambiguity but does not force this
model to follow a bounded read/edit/verify workflow. Do not infer that TF storage
is slower or more expensive: the file-control arm was not run. The experiment
shows that another prose-only skill revision is not yet evidence of reliable
call reduction. The skill was not changed again after this result.

A possible next experiment would replace arbitrary shell access with typed
read/apply/verify operations and use equivalent tools in the file-control arm.
That is a proposal, not an implemented feature or an approved additional paid
series. An alternative is a controlled test of a different model. No extra paid
requests, commits or pushes were made after this pilot.

Raw transcripts and consumer snapshots remain outside the public repository.
