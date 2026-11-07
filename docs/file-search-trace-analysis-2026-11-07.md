# File-scoped search: trace analysis, 2026-11-07

This examines the six ObjectBox attempts in `paired-file-search-2026-11-07.md`.
No model calls were made for this analysis. Raw traces are temporarily stored
under `/tmp/tfe-short-search-{tabletop,span}-20261107`.

## Observations

* All six attempts called file-scoped search. Four also called `show`; one
  called `outline`. In one usage attempt the agent omitted `src/` from the
  file argument on its first search and received `Unknown file`.
* Search matches every role whose ID or stored text contains the query.
  Parent text includes child references, and other role bodies can call the
  named function. Search therefore returns related roles as well as the target.
* On the pristine usage fixture, `search usage_from_response --file ...`
  returned three roles (file, function and nested function), 1,471 UTF-8 bytes.
  Showing the already-known function ID returned 641 bytes; `outline --depth 2`
  returned 789 bytes.
* On the pristine span fixture, `search _build_span_name --file ...` returned
  three roles (the class, the method, and a caller), 4,199 UTF-8 bytes. Showing
  the already-known method returned 403 bytes; `outline --depth 2` returned
  1,367 bytes. These are output sizes, not model token counts. `show` requires
  an exact ID, and its one-role output is not always sufficient context.
* In span repetition 1, the agent used `re` without importing it; the project
  suite caught the `NameError`. Its next batch used a stale method version and
  failed. It fetched the current version and succeeded on the next batch.
  This is a combination of an agent error and correct optimistic locking, not
  evidence that either originated in search.
* In span repetition 3, an initial edit was rejected as invalid Python; the
  agent then submitted a working alternative. The initial shell command's exit
  status was zero despite the diagnostic, so shell-level success alone is not
  a reliable indication that every operation in a compound command succeeded.
* First materialisation returned a long list of paths. In usage repetition 1,
  an `apply` command followed by materialisation emitted 4,378 characters;
  its attribution cannot be separated from that combined command alone.
  The list is useful as a record of written paths, but its value to the agent
  in this workflow is unproven.

## Candidate, not a measured saving

Test a distinct exact-role selector restricted to a file. A unique match would
return its body, ID and current version; ambiguous matches must be explicit,
and an absent match must not silently select another role. Keep literal text
search available for callers, nested roles and discovery. A targeted local
comparison should measure bytes and tokenizer counts, lookup time, ambiguity,
and whether the returned role provides enough context for each task. A paired
model experiment remains necessary to establish any cost or time saving.

The saved source and db tasks use the same selected project gate, but exclude
two failing tests and one collection-error file; passing it is not a full-suite
quality claim. These six runs do not establish a general ranking of interfaces.
