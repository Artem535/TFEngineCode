# Exact role lookup: local measurement, 2026-11-07

`tfe-code role FILE NAME` selects one reachable role by exact full ID or dotted
suffix. An ambiguous name fails with candidate IDs; a missing name fails. It
returns the role body and current optimistic version in the existing show format.
Literal text search remains available for callers and discovery.

On the unchanged database arm templates from the paired trials, 12 independent
CLI processes per command gave these results (UTF-8 output bytes and median wall
time, including process startup and workspace loading):

| Fixture | Query | `search --file` bytes | `role` bytes | Search median | Role median |
| --- | --- | ---: | ---: | ---: | ---: |
| usage | `usage_from_response` | 1471 | 641 | 18.40 ms | 16.89 ms |
| span | `_build_span_name` | 4199 | 403 | 17.50 ms | 17.80 ms |

The response reductions are 56% and 90%, respectively. The local Python runtime
had no tokenizer installed, so token counts were not measured. The timing
differences are too small and noisy to claim a speedup. The role body is not
necessarily sufficient context: for example a function can reference nested
roles or depend on imports and callers. This deterministic comparison does not
measure model success, total tokens, cost or end-to-end agent time; a new paired
model trial is required for those claims.
