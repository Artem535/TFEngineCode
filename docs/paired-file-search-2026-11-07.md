# Paired file-scoped search trial, 2026-11-07

The ObjectBox workflow used `search NAME --file FILE` before `apply`; sources
were materialised for checks. The ordinary-files control and both tasks were
the same as the previous Tabletop and span pilots. Both series ran concurrently,
with three alternating-order pairs per task. All 12 attempts passed the
independent verifier and the selected project suite; the suite excludes two tests
and one test file with pre-existing failures. The two series had a 20 RUB
estimated-cost limit each and a 5 RUB per-attempt limit.

| Task | Arm | Rep 1 RUB | Rep 2 RUB | Rep 3 RUB | Total RUB | Input tokens | API seconds |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| usage | files | 1.136287 | 0.947533 | 0.694931 | 2.778751 | 67,503 | 35.306 |
| usage | db | 1.394351 | 1.382922 | 1.870497 | 4.647770 | 118,562 | 47.361 |
| span | files | 0.382075 | 0.732885 | 0.577453 | 1.692413 | 44,337 | 20.109 |
| span | db | 1.670695 | 0.373581 | 1.142645 | 3.186921 | 84,821 | 35.531 |

Estimated total 12.305855 RUB. Per-request raw-event sums agree with the report.
The provider did not report charges; the calculation uses 33 RUB/M input and
133 RUB/M output, without a separate cache discount despite reported cached
tokens. API seconds are not end-to-end elapsed time; concurrent execution may
also affect timing. One task, three pairs and one model are insufficient for a
general conclusion. The prompt and interface changed together, so this does not
isolate the effect of the command.

The db arm used search in all six attempts and was cheaper in one of six pairs.
Its cost ratios against files were 1.67x on usage and 1.88x on span. Previous
series ratios were 2.26x and 1.56x respectively, but different model attempts
and prompt mean these changes do not establish a causal improvement.

Raw reports and events are in `/tmp/tfe-short-search-tabletop-20261107` and
`/tmp/tfe-short-search-span-20261107`; `/tmp` is temporary. The trial driver is
`scripts/tabletop_paired_trial.py`, and its db prompt was updated to use the new
search workflow before these runs.
