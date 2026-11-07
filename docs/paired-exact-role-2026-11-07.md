# Paired exact-role trial, 2026-11-07

Three alternating-order pairs per task compared ordinary files with ObjectBox
and `tfe-code role FILE NAME`. The db prompt also allowed search, outline and
show for discovery or context. The two task series ran sequentially. Every
artefact passed the independent verifier and selected project suite; one db
attempt was marked failed because it exhausted its request budget before the
agent completed. The gate excludes two tests and one test file with known
pre-existing failures.

| Task | Arm | Rep 1 RUB | Rep 2 RUB | Rep 3 RUB | Total RUB | Passed | Input tokens | API seconds |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| usage | files | 1.048131 | 1.207929 | 0.924125 | 3.180185 | 3/3 | 77,850 | 37.051 |
| usage | db | 2.772113 | 5.377473 | 4.257327 | 12.406913 | 2/3 | 330,606 | 90.174 |
| span | files | 0.371745 | 0.372941 | 0.366566 | 1.111252 | 3/3 | 29,378 | 15.256 |
| span | db | 0.304788 | 1.125274 | 0.239536 | 1.669598 | 3/3 | 43,694 | 26.049 |

Estimated cost for both series: 18.367948 RUB; raw event sums reproduce the
individual rows. The provider did not report charges. Estimates use 33 RUB/M
input and 133 RUB/M output without a separate discount for cached input, even
though cached tokens were reported. API time excludes tool execution, grading,
setup and other elapsed time.

The per-attempt threshold was 5 RUB; usage db repetition 2 reached 5.377473 RUB
because the final request is accounted for after it completes. Its agent turn
failed with the budget error, despite the produced artefact passing the gate.
Series budgets were 20 RUB each, checked between attempts.

The agent used `role` in five of the six db attempts. Usage db repetition 1
used outline/show instead. The usage db attempts also issued multiple `show`
commands; in span, db won two pairs but the second was substantially more
expensive. Compared with files, the db arm cost 3.90x on usage and 1.50x on
span. This is a small stochastic sample and changes both interface and prompt;
it does not isolate the command or establish a general causal effect.

Raw events and reports are in `/tmp/tfe-exact-role-tabletop-20261107` and
`/tmp/tfe-exact-role-span-20261107`, both temporary. The driver and prompt are
`scripts/tabletop_paired_trial.py`; the local deterministic output-size
comparison is `docs/exact-role-local-measurement-2026-11-07.md`.
