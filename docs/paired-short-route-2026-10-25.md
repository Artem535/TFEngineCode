# Short-route paired pilot, 2026-10-25

**No comparable pair was obtained.** The user authorized one two-task repeat
per arm. The frozen protocol stopped the entire pilot after the first incomplete
stage; no whole-attempt retries or extra paid runs were started.

| Attempt | Completion | Independent behavior check | API responses | Wall seconds | Reported RUB |
| --- | --- | --- | ---: | ---: | ---: |
| Roles, task 1 | Incomplete: step limit | PASS | 12 | 149.566 | 0.103044 |

The final generated artifact is correct according to the existing stub-based
grader, its source matches the role store, and the critic example is byte-exact.
There is no turn.completed. Classify this as incomplete execution with a passing
artifact, not a task-quality failure. All 12 responses include provider usage;
0.103044 RUB is the sum of those charges, not a certified bill. The conservative
possible_unreported_charge flag remains true for an incomplete turn.

Files task 1, files task 2 and roles task 2 were not started. Their costs are not
measured; absent attempts must not be presented as zero-cost successful controls.
There is no evidence from this pilot of relative savings or speed.

## Observed friction

The model initially invoked `apply /tmp/apply.yaml`; the command reads stdin,
so it received no document. It later mapped `[file: out/verifier_node.py]`,
which produced `out/out/verifier_node.py`. Further reads and repairs consumed
steps. The first behavioral verification failed because the intended output
path was absent; after repair the independent parent grader passed. The event
summary counts two commands with nonzero exit codes; a compound command also
contained diagnostics despite returning zero.

The frozen skill had a role-document example but no explicit shell stdin
invocation and no explicit rule distinguishing file mappings from generated
paths. These are actionable guidance omissions. They do not justify changing
this pilot's frozen inputs or assigning a storage advantage.

After the pilot, the local skill was clarified with a stdin heredoc example and
relative output mapping. No task-specific target expression was added. The
correction is a new, unmeasured revision, not a reclassification of this attempt.
The deterministic local rehearsal is the available verification for that revision;
it does not prove model compliance or financial savings.

Model: Beget deepseek-v4-flash, effort none, output4096, max12steps. API time
148.896 s, tool time 0.600 s; wall149.566 s. Input55323, output2979,
cached input47360 tokens. Frozen skills, runtime guidance, tasks, runner and
protected tool hashes matched. No provider key appeared in the evidence archive.
The consumer checker uses stubs and has known incomplete TOC/log coverage.
Current Codex and local preparation costs are unavailable and excluded.

Raw logs and consumer snapshots remain outside the public repository. No
production implementation changes, commits or pushes were made for this pilot.
