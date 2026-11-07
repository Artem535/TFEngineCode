# Paired native-role / source-file pilot, 2026-11-07

**Verdict: `not_confirmed`.** Both arms passed all six behavioral checks, but the
native role workflow cost **50.3% more** than ordinary file editing and was 29%
slower in model API time. The predeclared saving condition (native at or below
0.8x files in at least two of three complete repetitions) was met **zero** times.

This is a reproduction, not a new claim: it agrees with the engine's
`tfengine-code-holdout-20261026` finding that additional role reads outweigh
compact partial edits once the baseline source is already short.

## Protocol

Two existing CriticNode tasks, two arms, three paired repetitions, 12 runs.
Order shuffles independently per repeat/task with seed `20261029 + repeat*10 + task`.
Tasks, `tfe-code`, verifier and both arm skills were frozen with SHA-256 before the
first scored request (`freeze` copied 14 files; 0 paid requests).
Task text is byte-identical between arms; the native prompt carries only the
imported-workspace skill, the file prompt only the source-code-editing skill.

Model `deepseek/deepseek-v4.1-flash` via `https://api.vsellm.ru/v1`, effort none,
max output 4096, max 20 steps, request timeout 90 s, wall timeout 240 s.
Provider cache was observed, not forced; every attempt starts from a clean context.

## Cost accounting

The provider returns token usage but **no** `cost_rub` field, so every charge is
`cost_source: "computed_from_tariff"` under the published tariff
**33 RUB / 1M input, 133 RUB / 1M output** (`role_contracts/tariff_cost.py`).
Cached input is not billed separately because it was zero on every request.
Reasoning tokens are reported inside `completion_tokens` and are not added twice.

End-to-end recount from raw events, independent of the report: 50 requests,
95,310 input tokens, 5,488 output tokens, **3.875134 RUB** — identical to the sum
of per-request charges.

## Results

| Arm | Passed | Cost RUB | Steps | Command calls | Read chars | Uncached input | Cached input | API seconds |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| files | 6/6 | 1.548154 | 25 | 19 | 20,645 | 8,698 | 28,672 | 31.5 |
| native | 6/6 | 2.326980 | 25 | 22 | 25,999 | 12,884 | 45,056 | 40.4 |

Native is **+50.3%** in cost (1.50x) and **+28.2%** in model API time.

| Repeat | files RUB | native RUB | native / files | Saves |
| --- | ---: | ---: | ---: | --- |
| 1 | 0.409139 | 0.998899 | 2.44x | no |
| 2 | 0.401789 | 0.654210 | 1.63x | no |
| 3 | 0.737226 | 0.673871 | 0.91x | no |

Per pair: 1.94x, 1.68x, 1.67x, 2.94x, 1.58x, **0.61x**. Native was cheaper in one
of six pairs, and that pair is a files-arm outlier (8 steps against a normal 3–4),
not a native improvement.

## Why the roles lost

The task fixture is `critic_node.py`, 111 lines. The whole first request — system
prompt, task text and a full `cat` — is 816 prompt tokens. The role route cannot
match that in one step:

| Step | files arm | native arm |
| --- | --- | --- |
| 0 | `cat file` (816 in) | `outline` then `show` (1,407 in) |
| 1 | edit (1,822 in) | `show` (2,138 in) |
| 2 | done (2,087 in) | `apply` (3,008 in) |
| 3 | — | done (3,273 in) |

Three effects compound:

1. **One extra step.** Navigation needs `outline` before `show`; `cat` needs nothing.
   Because each step re-sends the whole accumulated context, the fourth step also
   re-bills the first three.
2. **Outline overhead is real output.** `outline` prints `(N lines; source X-Y)`
   plus the first non-reference line for every role. On a file this small that
   metadata costs more than the code it describes.
3. **Mixed tool use.** Three of six native runs issued `grep`/`sed` against the raw
   source alongside `tfe-code` (4 commands where 3 sufficed), paying for both
   interfaces. The two most expensive native results are mixed runs (0.394 and
   0.605); the third mixed run was cheap at 0.324, and the three pure-role runs
   span 0.321–0.350. Mixed tool use is therefore an aggravating factor, not the cause.

Pure-role attempts were still 1.58–1.68x the file arm, so the cause is step count
on a short file, not contamination by shell commands.

## What this does not show

The fixture is a single 117-line class. The engine's own large-file series
(`v7`, `v9f/v9h`) reported gains on 4-language large files and 8–16-site template
edits; nothing here contradicts those, and nothing here confirms them either.
Two tasks cannot establish a repository-wide claim.

Charges are computed from a published tariff, not provider-reported, and
preparation/development-agent cost is excluded and unknown. Timing is model API
request time, not end-to-end wall time.

## Reproduce

```bash
python3 scripts/native_paired_trial.py freeze --out /tmp/tfe-pair-2026-11-07
python3 scripts/native_paired_trial.py run --out /tmp/tfe-pair-2026-11-07 --go-ahead --budget 4.0
python3 scripts/native_paired_trial.py report --out /tmp/tfe-pair-2026-11-07
```

`freeze` performs six local checks with no paid requests. `run` requires
`--go-ahead` and a `--budget`, and reads the API key from
`../TextFoundryEngine/.dt.key`.