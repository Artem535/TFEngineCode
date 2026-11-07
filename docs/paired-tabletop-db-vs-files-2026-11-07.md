# Paired database / source-file pilot on a real project, 2026-11-07

**Result: the database arm loses on this task.** Both arms solved the identical
task on a real project, and the ObjectBox-backed role arm cost **2.26x** as much:
6.472 RUB against 2.860 RUB over three paired repetitions. Correctness was equal
(3/3 passed in both arms), so this is a cost difference, not a quality one.

## Task

A real feature in `TabletopMagnat`: `src/tabletopmagnat/observability/llm_usage.py`
must account for provider-reported `cached_tokens` and `reasoning_tokens` when
pricing a completion. The file is 53 lines. The task text
(`harness/tabletop/task.md`) is byte-identical in both arms.

Acceptance is independent of the attempt: at grading time the produced artefact is
replayed into a pristine template, then `harness/tabletop/verify.py` (nine
behavioural checks written for this evaluation) and the project's own suite both
run there. An attempt cannot pass by weakening a test.

## Arms

| | `files` | `db` |
| --- | --- | --- |
| Role tree | ordinary files under `src/` | only in `.tfe-code/db` (ObjectBox) |
| Editing | shell tools | `tfe-code outline/show/apply` |
| Sources | authoritative | rendered output, `tfe-code render --write` |
| Initial state | 135 Python files | 44 Python files, `src/` empty, store 652K |

The `db` arm starts with no source at all and must materialise the tree from the
database, which is the workflow the engine's storage is meant to enable.

## Protocol

Three paired repetitions, arms alternating by seed. Model
`deepseek/deepseek-v4.1-flash` via `https://api.vsellm.ru/v1`, effort none, max
output 8192, up to 30 steps, 5 RUB per attempt and 20 RUB for the series. The
agent runs in a bwrap sandbox with no network; the project interpreter is
re-exposed read-only and `TFE_CODE_INDEX=objectbox` is passed explicitly, because
the sandbox runs with `--clearenv` and masks `$HOME` with a tmpfs.

The provider reports token usage but no charge, so every attempt is marked
`cost_source: computed_from_tariff` at 33 RUB per 1M input and 133 RUB per 1M
output. An independent recount from the raw events reproduces the totals exactly.

## Results

| Repetition | files RUB | db RUB | db / files |
| --- | ---: | ---: | ---: |
| 1 | 0.923634 | 1.692383 | 1.83x |
| 2 | 0.978204 | 2.155528 | 2.20x |
| 3 | 0.958471 | 2.624185 | 2.74x |
| **total** | **2.860309** | **6.472096** | **2.26x** |

| Arm | Passed | Requests | Steps | Commands | Input tokens | Output tokens |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| files | 3/3 | 16 | 16 | 15 | 69,914 | 4,159 |
| db | 3/3 | 25 | 25 | 28 | 171,902 | 6,010 |

No limit was reached: the longest attempt used 10 of 30 steps and 2.62 of 5 RUB.
All commands exited 0; there were no wall timeouts and no failed turns.

## Why the database arm costs more

Two costs are structural, and both grow with context length:

1. **A mandatory materialisation step.** The `db` arm must run
   `tfe-code render --write` before it can read anything, and the presence of
   generated sources is what lets it check its work at all.
2. **A version lookup per edited role.** `apply` requires `expected_version`, so
   the agent must `show ROLE --json` before every edit. In the best attempt it
   batched those lookups into one call; in the worst it paid for them separately.

On a 53-line file the file arm simply `cat`s the module once. The role arm pays
navigation on top of the same edit, and that overhead cannot be amortised over a
single short file. This matches the engine's `holdout` finding that extra role
reads outweigh compact partial edits once the baseline source is already short,
and the presentation's own limit that a small single file reaches parity at best.

`db` input also drifts upward across repetitions (44k, 56k, 71k tokens) while
`files` stays flat (21.7k, 23.4k, 24.7k), so the ratio would worsen with more
repetitions rather than converge.

## What this does not show

One task, one short file, three repetitions, one model. Nothing here says the
database approach cannot win: the engine's own `v9h` run found `storage_justified`
on a template shared by eight files, where one edit replaces eight edits — the
exact case this fixture does not contain. A fair test of the stored-role idea
needs a task whose edit must land in several files.

Both arms also spend most of their budget on the same discovery work (reading the
verifier, the task, the surrounding code), which dilutes any interface difference.

## Reproduce

```bash
./harness/tabletop/setup.sh /tmp/arms
python3 scripts/tabletop_paired_trial.py \
  --out /tmp/run --go-ahead --budget 20.0 --attempt-budget 5.0
```

`setup.sh` needs no model. `tabletop_paired_trial.py` requires `--go-ahead` and
reads the API key from `../TextFoundryEngine/.dt.key`; it never spends without
both.