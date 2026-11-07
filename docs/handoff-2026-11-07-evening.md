# TFEngine Code — handoff, 2026-11-07 (evening)

Continuation of `docs/handoff-2026-11-07.md` and
`docs/handoff-2026-11-07-economics.md`. Read `AGENTS.md` first. This document
covers the work after those two: navigation commands, the batch-edit format, the
harness defects that were burning money, and every model measured.

## 1. Where things stand

```text
ctest  15/15      python unittest  29/29      git diff --check  clean
```

`HEAD` is still `b94f354`; almost everything is uncommitted or untracked. Do not
run git mutations. The engine checkout is modified (see the earlier handoff §2,
§3, and this document's §6).

State in one line: **the role interface works and passes the multi-file task
reliably on `moonshotai/kimi-k3.1-pro`, but it costs 3.5× the file arm. No model
tested produced a saving.**

## 2. Commands added in this session

| Command | Unix analogue | Why |
| --- | --- | --- |
| `tfe-code ls` | `find` | managed file paths, 4 004 B for 92 files |
| `tfe-code ls FILE\|ROLE\|PREFIX` | `ls` | direct children by local name, 21 B for a file; a package prefix behaves like a directory |
| `tfe-code search Q --ids` | `grep -rn` | one `id: matching line` per role, 1 581 B where bodies cost 6 883 B |
| `tfe-code outline --relative` | `ls -R` | drops the repeated 40-char id prefix |
| `tfe-code prepare-batch --file f1 --file f2` | `cat` | **all** roles of those files in one call when no query is given |
| `apply` with `replace[]` | `patch` | several fragments edited in one role |
| `apply --help` | — | prints the payload schema; exit 0 |
| `tfe-code help` | — | exit 0 (was 2, which made agents think the tool was broken) |

Measured sizes on the consumer project, before → after:

```text
map of the whole project   55 776 (outline)      →  4 004 (ls)
one file's children           305 (outline)      →     21 (ls FILE)
search with bodies          6 883                →  1 581 (--ids)
outline --relative         55 776                → 37 143
render --write output     165 296                →  4 187 (paths only)
```

Errors now name their own next step:

- unknown `--file` → `did you mean: src/tabletopmagnat/observability/events.py`
- no `prepare-batch` match → `try a literal identifier from: <ids>`
- fragment aimed at the wrong role → `this fragment appears in: <ids>`

## 3. The batch format, and why it changed

`apply` used to take one `old`/`new` pair per role. A model changing a signature
**and** the call site inside one function had no way to express it: it duplicated
the role entry and the batch was rejected as ambiguous. In the traces this single
failure class cost three steps and about 2.5 RUB per occurrence.

`replace` fixes it, and it also cuts output: 40% of the text in `new` was
unchanged code being echoed back.

```json
{"edits":[{"role":"…publish","expected_version":"…",
  "replace":[{"old":"def publish(self, event_type: str,","new":"…, level: str = \"info\","},
             {"old":"            event_type=event_type,","new":"            event_type=event_type,\n            level=level,"}]}]}
```

Rules, all covered by `test_apply_replace_core`:

- `old`/`new` and `replace` are mutually exclusive per role;
- each fragment must be nonempty and match exactly once in the evolving text;
- byte-identical duplicate entries for one role collapse; different ones are
  rejected as ambiguous;
- payload parsing uses `rfl::DefaultIfMissing` and then checks the fields, so a
  typo produces `apply edit is missing 'expected_version'` instead of an rfl cast
  error.

Agents must send **fragments**, never whole role bodies. That is stated in
`AGENTS.md` and in the skill.

## 4. Harness defects that were burning money (fixed)

Each of these silently converted a working run into a paid failure.

1. **`$HOME` masked by bwrap** — the project `.venv` was invisible, so agents
   spent entire budgets on `find / -name python3`. Fixed with `--expose` plus
   `--dir` for ancestors in `api_agent.py`. The interpreter is a symlink chain
   into `~/.local/share/uv/python`, so expose the whole `uv/python` directory,
   not the resolved version.
2. **`--clearenv` drops everything** — `TFE_CODE_INDEX=objectbox` never reached
   the sandbox; the db arm ran in JSON mode against a workspace with no
   `project.json` and burned 14 steps reverse-engineering the binary with
   `strings`/`objdump`. Fixed with `--env KEY=VALUE`; bwrap's `--setenv` takes
   **two** arguments.
3. **The package is installed editable against the original project** — a `.pth`
   in the venv points at the real `src/`, so `pytest` and the verifier tested the
   untouched original unless `PYTHONPATH=<arm>/src` is set. This affected grading
   too: correct edits were reported as failures. Both calls set it now.
4. **`apply --check` deadlocked.** The workspace lock is held while the check
   runs, and the check script ran `tfe-code render --write`, which waits on the
   same lock. Observed as a 120 s command timeout; verified by inspecting the
   hung process (`apply` held the lock, `render` blocked). Fixed twice over: the
   gate renders only when `src` is empty, and the lock waits at most 30 s then
   fails with an explicit message.
5. **`Commit` published to ObjectBox before running `--check`**, so a rejected
   batch stayed in the store while files rolled back. Publication now happens
   after the check; see `test_objectbox_check_rollback_core`. This was flagged as
   an unproven risk in `handoff-2026-11-07-economics.md` and is now reproduced
   and closed.
6. **`apply` mutated `edits` instead of the deduplicated list**, so an identical
   duplicate carrying a real change would have applied twice.

A `report()` crash (`NameError: args`) also destroyed one run's metadata after
the money was spent; the report now records model and tariff.

## 5. Experiments and every number

All runs use `harness/event_level` (a feature spanning the event model, bus,
SQLite migration and web DTO — four files). Grading replays the artefact into a
pristine template, so an attempt cannot pass by weakening a test.

| Model | Tariff ₽/1M in/out | files | db | db/files | Result |
| --- | --- | ---: | ---: | ---: | --- |
| deepseek-v4.1-flash | 33 / 133 | 0.74–1.16 | 5.0–5.4 | — | db **failed** 0/4 |
| deepseek-v4-pro-0813 | 146 / 438 | 12.59 | 62.30 | 4.9× | db **failed**, cap |
| moonshot kimi-k3.1-pro | 34 / 1105 | **2.19–2.33** | **8.17–9.90** | **3.5–4.5×** | db **passed** 3/3 |
| claude-opus-5.5 | 442 / 2210 | 13.81 | >98.98 | >7× | db **killed at budget** |

Simpler earlier tasks (a 53-line file and a 457-line file) also lost: 2.26× and
1.56×, both arms passing. See `docs/paired-tabletop-db-vs-files-2026-11-07.md`.

**Kimi is the only model on which the db arm passes the multi-file task, and it
is the cheapest in absolute terms.** Tariff explains the spread: opus spent
181 k input tokens against kimi's 86 k and cost ten times more.

## 6. What the traces say about the remaining gap

Best db run so far: 10 steps, 8.17 RUB, passed.

```text
[s0] cat task.md; tfe-code ls
[s1] prepare-batch --file events.py      (one call per file, three steps total)
[s2] prepare-batch --file store.py; prepare-batch EventDTO
[s3] prepare-batch --file models.py
[s4] apply … 10 roles, replace[]  → FAILED: "import datetime" sent to Event
[s5] own helper script, broken temporary file
[s6] ls, retry
[s7] apply again                  → passed
```

Three things account for the gap:

1. **Two apply rounds.** The first failed because a module-level fragment was
   aimed at a leaf role. The error now names the roles that do contain it, which
   should remove this round; not yet re-measured.
2. **`prepare-batch` called per file** instead of once with several `--file`
   flags, which the command supports (11 roles from two files in one call).
3. **Late steps are expensive**: at 21 k accumulated input a request costs
   ~0.7 RUB on kimi and ~10 RUB on opus. Step count dominates everything.

The files arm needs 3–4 steps: read the task, `cat` the sources, one edit script.
A 3-step db route exists on paper — `prepare-batch --file …` → `apply --check` →
report — but has not been observed yet.

## 7. Costs spent

About **258 RUB** in total:

- 30 RUB on harness debugging that produced no result;
- 93 RUB on the four kimi runs;
- 124 RUB on pro and opus, both of which turned out worse than kimi.

The pro/opus spend was a mistake in judgement: after pro made both arms fail to
complete, testing a more expensive model added no information. Stop at the first
model that is clearly worse, not the third.

## 8. Honest interpretation

**The product does not save money on handwritten, well-factored code.** Four
independent measurements agree, and the engine's own `holdout` and `v9h` runs say
the same: the only `storage_justified` result in the project's history came from a
**planted** template across eight files, which does not occur in real code
(`v9h` limits: "The template is planted, not imported").

The interface is now good enough that a model uses it correctly. What remains is
intrinsic: navigation and version bookkeeping cost more than reading one file, and
on a small change that overhead cannot be amortised.

## 9. Next steps, in order of expected value

1. **Re-measure once** on kimi after the fragment-suggestion fix, to see whether
   the first apply round disappears. About 10 RUB.
2. **Make multi-file `prepare-batch` obvious** — the skill mentions it in one
   clause; give it an example like the others. Free.
3. **Do not chase cost further without a new hypothesis.** The spread across four
   models is explained entirely by tariff and step count.
4. If cost is the goal, the only untested shape is **generated or cross-language
   artefacts**, where `import` cannot deduplicate. The competitor there is a code
   generator, not `cat`, and no fixture exists yet.
5. Alternatively sell what the tool demonstrably gives: byte-exact deterministic
   rendering, verifiable check receipts, transactional rollback, and no drift
   between artefacts. Several of those the file arm does not offer at all.

## 10. Reproducing

```bash
./harness/tabletop/setup.sh /tmp/arms $PWD/harness/event_level
TFE_TRIAL_HARNESS=$PWD/harness/event_level \
TFE_TRIAL_MODEL=moonshotai/kimi-k3.1-pro \
  python3 scripts/tabletop_paired_trial.py --out /tmp/run --go-ahead \
  --budget 60 --attempt-budget 15 --tariff-input 34 --tariff-output 1105 --reps 1
```

`setup.sh` needs no model. The driver requires `--go-ahead` and a budget, reads
the key from `../TextFoundryEngine/.dt.key`, refuses an existing `--out`, and
records model and tariff in `report.json`. Before any paid series, run a reference
implementation through the harness verifier and the project suite — that check
caught two wrong assertions this session.

The provider reports tokens but no price; every attempt is tagged
`cost_source: computed_from_tariff`. Recount from the raw events whenever you
change anything in the accounting path.

## 11. Files that matter

```text
native/workspace.cc          ls, search --ids, --relative, prepare-batch,
                             apply with replace[], lock timeout, ObjectBox publish
native/objectbox_store.cc    roles <-> Block, one transaction per batch
native/main.cc               CLI surface, apply schema, log level
harness/tabletop/setup.sh    builds both arms for any harness directory
harness/{tabletop,span,event_level}/   task + independent verifier
scripts/tabletop_paired_trial.py       driver: arms, grading, cost, report
skills/tfengine-code/SKILL.md          route with examples (142 lines)
skills/tfengine-code/legacy-templates.md  the .roles branch, loaded on demand
tests/native/test_apply_replace_core.cc   replace[], duplicates, fragment hints
tests/native/test_list_core.cc            ls, --ids, --relative, did-you-mean
tests/native/test_prepare_batch_core.cc   skeleton, deepest match, suggestions
tests/native/test_objectbox_check_rollback_core.cc   rejected batch leaves no trace
```