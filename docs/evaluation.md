# Evaluation contract

Compare the complete role workflow with ordinary source-file editing using
identical behavioral tasks. Keep tool-specific instructions in separate skills.
Give both variants equivalent examples, verification commands, and contextual
hints; never embed a target answer in a search tool. Freeze tool and skill
versions before running the trial. A historical baseline is a historical
comparison, not a controlled contemporaneous experiment.

Record completion independently from functional correctness. Missing usage is
unknown cost, not zero. Report incomplete and infrastructure attempts separately.
Count all provider-reported charges, including failed attempts. Record wall time
separately from model API request time and tool time. Validate outputs through
consumer behavior checks as well as syntax. Report dataset size, model, limits,
cache usage and instruction changes. Do not generalize small synthetic results
to arbitrary repositories or attribute an interface win to storage alone.

Paid runs require an explicit user go-ahead; they are not part of install,
examples, tests, or CI. This extraction contains no paid-run command.

## Short-route revision

The role skill now specifies a default route of one relevant read and one batch
apply, with additional context only for a concrete unresolved dependency. It
also makes the stop condition explicit: successful syntax validation plus the
supplied behavioral verifier, followed by a concise report. This is a target
workflow, not a mandatory tool-call cap.

The [source-file skill](../skills/source-code-editing/SKILL.md) gives the control
arm the same context boundaries, batching preference and stop rule. The next
comparison must use this skill rather than the historical unoptimized prompt.
The role implementation and renderer are unchanged by this revision.

Supply environment facts alongside each activated skill, separately from the
byte-identical user task:

- Workspace and exact launcher, so discovery is unnecessary.
- Initial source or role paths and the same example content.
- Package availability; unavailable imports do not justify filesystem searches.
- Behavioral verification command, including syntax checks, and its limits.

Do not supply target expressions, implementation snippets, route-specific
answers or a fixed sequence tailored to the hidden expected solution. Ordinary
file context and role context should expose equivalent source information.
Search output is not a guarantee that every dependency has been included.

An offline rehearsal exercises both routes:

```bash
TFE_CODE_RENDERER="$PWD/build/tfe-code-render" \
  python3 scripts/rehearse_workflow.py --report /tmp/tfe-code-rehearsal.json
```

It creates and modifies a small welcome function in isolated temporary
workspaces. Each stage performs one read, one edit and one independent consumer
behavior check per arm. It verifies equal outputs and preservation of the
original example. It makes no model requests, uses no credentials, and provides
no evidence of model compliance, cost or speed. The patches are supplied by the
script; this is route verification, not a model benchmark.

For the next paid comparison, first freeze the tool revision, both skills,
separate environment contexts, byte-identical tasks, verifier, model parameters
and attempt limits. Use paired repetitions and alternate which arm starts;
include a task not used to tune the skills. Keep all attempts in the report.
Report whether extra calls were needed for unresolved dependencies, errors or
redundant confirmation. A low call count only counts as a useful result when
independent behavioral checks pass. Paid execution remains a separate explicitly
authorized action; this repository has no automatic provider requests.
