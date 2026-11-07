# Full-skill paired trial, 2026-11-07

Three alternating-order pairs per task compared the full `tfengine-code` skill
in ObjectBox mode with the full `source-code-editing` skill for source files.
The harness saved both skill texts, complete prompts and prompt SHA-256 hashes
before inference. The two series ran sequentially. All 12 attempts completed
and passed the independent verifier and selected project suite. The suite
excludes two tests and one file with pre-existing failures.

| Task | Arm | Rep 1 RUB | Rep 2 RUB | Rep 3 RUB | Total RUB | Input tokens | Steps | API seconds |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| usage | files | 0.598717 | 0.857856 | 0.884945 | 2.341518 | 55,781 | 14 | 28.693 |
| usage | db | 1.806681 | 3.157560 | 5.517692 | 10.481933 | 280,616 | 32 | 73.633 |
| span | files | 0.353278 | 0.219062 | 0.358493 | 0.930833 | 22,081 | 11 | 16.629 |
| span | db | 0.736622 | 0.382945 | 1.067722 | 2.187289 | 59,035 | 15 | 21.795 |

Total estimated cost: 15.941573 RUB; independent sums of raw response events
match every attempt. The provider did not report charges. The estimate uses
33 RUB/M input and 133 RUB/M output without separate cached-input discounts,
although cached input was reported. API seconds exclude commands, setup and
grading; they are not end-to-end times. The 5 RUB per-attempt threshold can be
exceeded by the final response: usage db rep 3 reached 5.517692 RUB and finished.

First-request inputs were 2,442 db vs 1,235 files tokens for usage and 2,276 vs
1,069 for span. The 1,207-token difference includes the different skill and
environment instructions; it cannot all be attributed to skill text alone.
The agent used `role` in all six db attempts. The db arm cost 4.48x and 2.35x
the files arm across the respective series and won no pair. Different skill
and environment prompts and stochastic attempts mean this does not isolate a
single instruction or establish general performance. Earlier trials did not
load these skill files, so comparisons across series are observational.

Raw events, saved prompts, skills and reports are in temporary directories
`/tmp/tfe-skill-tabletop-20261107` and `/tmp/tfe-skill-span-20261107`.
The driver is `scripts/tabletop_paired_trial.py`.
