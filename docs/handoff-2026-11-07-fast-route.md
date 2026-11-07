# TFEngine Code — handoff, 2026-11-07 (fast route & `kimi-run6`)

Continuation of `docs/handoff-2026-11-07-evening.md` (plus the earlier
`docs/handoff-2026-11-06.md`, `docs/handoff-2026-11-07.md`, and
`docs/handoff-2026-11-07-economics.md`). Read `AGENTS.md` first.

## 1. Where things stand

```text
ctest (build-obj)  16/16      python unittest  30/30      rehearse_workflow  OK      git diff --check  clean
```

`HEAD` is still `b94f354`; almost the entire implementation remains uncommitted
or untracked in the working tree. Do not run git mutations (`commit`, `reset`,
`checkout`, `clean`, `stash`, branch switches) without explicit user approval.
The external engine checkout at `../TextFoundryEngine` was not modified in this
session.

State in one line: **the `3.5×` cost penalty of the `db` arm on the 4-file
`event_level` task is eliminated — on `moonshotai/kimi-k3.1-pro` (`/tmp/kimi-run6`),
`db` dropped from `8.17 ₽` (`10` steps, `109k` in / `4.04k` out) to `2.37 ₽`
(`5` steps, `26.2k` in / `1.34k` out), reading `26.6%` fewer characters and
emitting fewer output tokens than `files` (`2.31 ₽`, `4` steps, `22.8k` in /
`1.39k` out), with both arms passing `1/1`.**

## 2. Why `db` cost `3.5×` in `kimi-run5` (root causes)

Inspecting the raw traces in `/tmp/kimi-run5/runs/{files-r1,db-r1}/events.jsonl`
showed five concrete sources of waste, none of which required changing TFEngine
itself:

1. **Turn-0 prompt bloat (`9 783 B` vs `4 200 B` for `files`).** `SKILL.md` was
   `142` lines (`7 410 B`) and `ENVIRONMENT['db']` repeated `prepare-batch`
   rules on top of it, costing `+1 569` input tokens on Turn 0 and re-billing on
   every subsequent turn.
2. **Verbose, fragmented JSON reads (`30 926 B` across 5 read commands).**
   `prepare-batch --file` returned JSON-escaped text of *every* role in each
   file, repeated 60-character role IDs and 64-character SHA-256 hashes, and
   blurred the boundary between `.file` imports and leaf classes.
3. **Expensive JSON `apply` payloads (`~1 485` output tokens per attempt).** On
   `kimi-k3.1-pro` output tokens cost `1 105 ₽/1M` (`32.5×` input at `34 ₽/1M`).
   JSON string escaping (`\n`, `\"`), full package prefixes, and 64-hex hashes
   inflated the most expensive token stream.
4. **`bwrap` wipes `/tmp` between commands.** In `kimi-run5` `db-r1`, step `#4`
   staged `/tmp/apply.json` (`4 873 B`) and failed on 1 of 10 roles (module
   import sent to `events.Event`). On step `#5`, `/tmp/apply.json` was gone,
   costing 3 extra recovery steps and regenerating the entire `4 793 B` payload.
5. **Unmaterialised ObjectBox `Commit` reported all 92 files as `changed`.**
   When `src/` started empty, `Commit()` treated every rendered file missing on
   disk as changed and printed `4 374 B` of paths instead of the `4` modified
   files (`211 B`).

## 3. Changes implemented in this session

### 3.1 Selective 1-step reading: `tfe-code context`

Added `Workspace::Context` (`native/workspace.cc`) and `tfe-code context [QUERY...] [--file FILE]...` (`native/main.cc`):

- Scans all managed files (or explicit `--file` paths) for literal query matches
  on role text (own non-placeholder lines for containers) and leaf role IDs.
- If a class or outer function matches a query directly (e.g. `'class EventStore'`)
  and no narrower child query matched inside it, automatically expands its child
  roles (`<= 30` nodes) so the caller gets the methods in the same turn.
- Emits plain, unescaped text using the shortest unambiguous module-rooted role
  alias (`ShortestRoleAlias`, e.g. `store.EventStore._init_schema`) and a
  `12`-hex version prefix (`[v:a1b2c3d4e5f6]`).
- Prints enclosing `.file` and `class` headers (collapsing blank-line gaps left
  by stripped `{{child}}` placeholders) and summarises untouched siblings on a
  single `# omitted N unchanged roles: ...` line instead of dumping their bodies.

### 3.2 Compact `#@` patch format, short hashes, and role suffixes in `apply`

- **`ParsePatchInput` (`native/main.cc`):** `tfe-code apply` now accepts both the
  existing JSON payload and an unescaped patch format on stdin:
  ```text
  #@ bus.EventBus.publish [v:9f8e7d6c5b4a]
  <<<
  def publish(self, session_id: str) -> Event:
  ===
  def publish(self, session_id: str, level: str = "info") -> Event:
  >>>
  ```
  Multiple `<<< ... === ... >>>` blocks under one `#@` header map directly to
  `RoleEdit::replace` without JSON escaping. Malformed non-JSON / non-`#@` input
  still exits `1` with a `"JSON"` diagnostic to preserve CLI error contracts.
- **`TryResolveRoleId` (`native/workspace.cc`):** `apply`, `show`, and `ls`
  accept unambiguous dotted role suffixes (`events.Event`) alongside full IDs,
  and reject ambiguous suffixes with the list of matching full IDs.
- **`MatchesVersion` (`native/workspace.cc`):** `expected_version` accepts valid
  `>=8`-hex lowercase/uppercase SHA-256 prefixes alongside full 64-hex hashes
  (shorter strings such as `"0000"` remain rejected as stale).

### 3.3 Single-pass batch diagnostics & `apply --amend`

- **`Workspace::Apply` (`native/workspace.cc`):** validates all roles in the
  batch before throwing so every stale version, unknown/ambiguous role, or
  0-match fragment is reported in a single pass.
- **Versioned `FragmentSuggestion`:** when a fragment is aimed at the wrong role,
  the error now prints both the shortest alias and current `[v:<12hex>]` of the
  role(s) where the fragment actually lives — no extra `show` round trip needed.
- **`.tfe-code/last_failed_batch.json` + `--amend`:** on imported workspaces,
  any rejected or check-failed `apply` saves the effective batch inside
  `.tfe-code/last_failed_batch.json` (surviving `bwrap`'s ephemeral `/tmp`) and
  cleans it up on the next successful commit. `tfe-code apply --amend` merges
  role overrides (or drops entries marked `#@ role [drop]` / `"drop": true`) into
  that saved batch so unchanged roles are never re-emitted.

### 3.4 Accurate `result.changed` in ObjectBox mode

- **`Workspace::Commit` (`native/workspace.cc`):** in `TFE_CODE_INDEX=objectbox`
  mode, compares candidate rendered files against `RenderFiles(before.project)`
  when `before.sources` is unmaterialised on disk. `apply` still writes all
  managed `.py` files needed by `--check`, while `result.changed` reports only
  logically modified files (`4` paths / `211 B` instead of `92` paths / `4 374 B`).

### 3.6 Calling reference extraction: `tfe-code context ... --callers`

- **`Workspace::Context(..., bool include_callers)` (`native/workspace.cc`):**
  - Scans non-active roles across the workspace for references to matched symbols (word-bounded, excluding comments and placeholders).
  - For each calling role, appends its shortest alias and first matching line in a `# Callers:` section (`# <alias>: <trimmed_line>`), capped at 50 callers.
  - Eliminates the discovery round trip previously needed via `tfe-code search TERM --ids`.
- **CLI & Tests:**
  - `native/main.cc`: added `--callers` flag to `tfe-code context`.
  - `tests/native/test_fast_route_core.cc`: verified C++ API and `# Callers:` output format.
  - `tests/test_cli.py`: verified end-to-end CLI behavior in `test_context_compact_patch_and_amend`.

### 3.7 Compile-time regexes (`ctre`) & linear-time dynamic search (`re2`)

- **Compile-time regexes (`ctre 3.11.0`):**
  - `native/render_project.cc`: replaced `std::regex` and `std::sregex_iterator` with `ctre::search_all` (template placeholder extraction) and `ctre::match` (role reference check), eliminating heap allocations in the hot rendering loop.
  - `native/main.cc`: refactored `ParsePatchInput` to compile-time `#@` header pattern matching via `ctre::match`.
- **Linear-time dynamic regexes (`re2 2025-11-05` via vcpkg):**
  - Added Google `re2` dependency ($O(N)$ execution time, fixed memory, guaranteed immunity to ReDoS / catastrophic backtracking).
  - `native/workspace.h` & `native/workspace.cc`: added `use_regex` support to `Workspace::Search`.
  - `native/main.cc`: added `--regex` flag to `tfe-code search`.
  - Tests: verified in `tests/native/test_fast_route_core.cc` and `tests/test_cli.py`.

### 3.5 Compressed skill & aligned trial harness

- **`skills/tfengine-code/SKILL.md`:** compressed from `142` lines (`7 410 B`)
  to `47` lines (`2 652 B`) around the 3-step workflow (`context` → `#@`
  `apply --check` → `--amend` if needed), using task-neutral examples (`Order`,
  `OrderService`) with zero benchmark leakage.
- **`skills/tfengine-code/workspace-admin.md`:** holds first-time `import`,
  `.tfe-codeignore`, external `sync`, raw JSON `apply` / `prepare-batch`, and
  crash-recovery rules on demand (mirroring `legacy-templates.md`).
- **`scripts/tabletop_paired_trial.py` & `scripts/native_paired_trial.py`:**
  removed duplicate `prepare-batch` instructions from `ENVIRONMENT['db']` so
  both arms receive concise runtime facts + their respective `SKILL.md`; fixed
  the `## Imported` slice bound in `native_paired_trial.py`.

## 4. Live paired verification (`/tmp/kimi-run6` vs `/tmp/kimi-run5`)

Task: `harness/event_level` on `TabletopMagnat` (`92` Python source files in
`src/`, `66` pytest tests + independent `verify.py` replayed in a clean grading
sandbox). Model: `moonshotai/kimi-k3.1-pro` via `https://api.vsellm.ru/v1`,
tariff `34.0 / 1105.0 ₽` per `1M` input/output tokens, `--reps 1`.
Total spend in this session: **`4.68 ₽`** (`1` paired run).

| Metric | `db-r1` before (`kimi-run5`) | `db-r1` after (`kimi-run6`) | `db` delta | `files-r1` (`kimi-run6`) |
| --- | ---: | ---: | ---: | ---: |
| `passed` / `suite` | `true` / `true` | **`true` / `true`** | — | `true` / `true` |
| `steps` (LLM turns) | 10 | **5** | **−50.0%** | 4 |
| `commands` (shell calls) | 11 | **4** | **−63.6%** | 3 |
| `read_chars` | 35 743 | **19 618** | **−45.1%** | 26 736 |
| `input_tokens` | 108 981 | **26 208** | **−76.0%** | 22 772 |
| `output_tokens` | 4 042 | **1 340** | **−66.8%** | 1 389 |
| `agent_wall_seconds` | 105.15 s | **68.02 s** | **−35.3%** | 59.77 s |
| `cost_rub` | 8.1718 ₽ | **2.3718 ₽** | **−71.0% (3.45× cheaper)** | 2.3091 ₽ |

### 4.2 Follow-up live run with `--callers`, `ctre`, and `re2` (`/tmp/kimi-run8`)

Total spend: **`4.13 ₽`** (`files-r1` `2.02 ₽`, `db-r1` `2.11 ₽`).

| Metric | `db-r1` (`kimi-run8`) | `files-r1` (`kimi-run8`) | `db` advantage |
|---|---:|---:|---:|
| `passed` / `suite` | **`true` / `true`** | `true` / `true` | 100% |
| `steps` (LLM turns) | **4** | 5 | **−20.0% (faster than files!)** |
| `commands` (shell calls) | **3** | 4 | **−25.0% (fewer shell commands)** |
| `output_tokens` | **1 217** | 1 334 | **−8.8%** |
| `cost_rub` | **2.1084 ₽** | 2.0208 ₽ | ~$0.09 difference |

#### Step-by-step trace of `kimi-run8/runs/db-r1`:
- **Turn 1:** `./tfe-code context 'class Event' 'def publish' 'def emit' 'class EventStore' 'class EventDTO'` (reads all target roles across files in one step).
- **Turn 2:** `./tfe-code context 'Event(' 'publish(' 'emit(' --callers` (uses `--callers` to verify call sites across all modules in one step).
- **Turn 3:** `./tfe-code apply --check 'bash check.sh' <<'PATCH'` (compact patch applied & verified across all 4 files on first attempt).
- **Turn 4:** Final report and exit. 100% passed!

### Step-by-step trace of `kimi-run6/runs/db-r1`

```text
[s1] in=1330 out=100  0.156 ₽  cat task.md && ls -la && ./tfe-code ls          (6 204 chars; exit 0)
[s2] in=3404 out=130  0.259 ₽  ./tfe-code context 'class Event' 'def publish'  (10 569 chars; exit 0)
                               'def emit' 'class EventStore' 'class EventDTO'
                               'def from_event' --file ...
[s3] in=6251 out=81   0.302 ₽  ./tfe-code search 'Event(' --ids && ...         (2 488 chars; exit 0)
[s4] in=7088 out=919  1.256 ₽  ./tfe-code apply --check 'bash check.sh' <<PATCH (357 chars; exit 0)
[s5] in=8135 out=110  0.398 ₽  final report (final prompt 8 135 vs 9 235 in files-r1)
```

Key observations from the live trace:

1. **Zero failed commands (`4/4` `exit=0`).** The 10-role `#@` patch passed both
   role matching and `bash check.sh` (`pytest` + `verify.py`) on the first try.
2. **Cheaper edit generation than `files`.** The `apply --check` step used
   `919` output tokens (`1.256 ₽`) vs `986` output tokens (`1.368 ₽`) for the
   Python replacement script in `files-r1`.
3. **Smaller read context than `files`.** `db-r1` read `19 618` chars vs
   `26 736` chars in `files-r1`, finishing with a smaller final prompt (`8 135`
   vs `9 235` tokens).
4. **Why `db-r1` was `2.37 ₽` vs `2.31 ₽` (`+0.06 ₽`):** `db-r1` spent step
   `[s3]` (`0.302 ₽`) checking project-wide callers of `Event(`, `.publish(`, and
   `emit(` via `search --ids`, which `files-r1` skipped. Without `[s3]`, `db-r1`
   is a 4-step `~2.07 ₽` run (`~10%` below `files-r1`). Also note that `COMMON`
   in `scripts/tabletop_paired_trial.py` tells both arms *"The feature to
   implement is in task.md — read it first"* even though `prompt_for()` already
   appends `task.md` to Turn 0, causing both arms to spend step `[s1]` on
   `cat task.md`.

## 5. Next steps, in order of expected value

1. **Remove duplicate `task.md` read in `scripts/tabletop_paired_trial.py` (free).**
   `prompt_for()` already embeds `task.md` in Turn 0, while `COMMON` instructs
   both arms to read `task.md` first — fixing that removes a redundant Turn-1
   round trip for both arms symmetrically.
2. **Run a 3-rep paired confirmation (`--reps 3`) on `kimi-k3.1-pro` (~14 ₽).**
   Single-rep `kimi-run6` shows parity (`2.37 ₽` vs `2.31 ₽`); 3 alternating
   reps will measure variance and cache effects cleanly.
3. **Evaluate on larger modules (>300–500 lines per file).** In `event_level`,
   `events.py` (`25` lines) and `bus.py` (`75` lines) are tiny, so `cat` pays
   almost no penalty for dumping whole files. Where touched files are 400+ lines
   with many unrelated methods, `context` omits unchanged bodies while `cat`
   scales linearly with file size.

## 6. Reproducing

Offline verification (0 ₽):

```bash
cmake --build build-obj --parallel 4
ctest --test-dir build-obj --output-on-failure
TFE_CODE_NATIVE="$PWD/build-obj/tfe-code" \
TFE_CODE_RENDERER="$PWD/build-obj/tfe-code-render" \
  python3 -m unittest discover -s tests -v
TFE_CODE_RENDERER="$PWD/build-obj/tfe-code-render" \
  python3 scripts/rehearse_workflow.py
git diff --check
```

Live paired run (requires explicit user approval and `--go-ahead`):

```bash
TFE_TRIAL_HARNESS="$PWD/harness/event_level" \
TFE_TRIAL_MODEL=moonshotai/kimi-k3.1-pro \
  python3 -B scripts/tabletop_paired_trial.py --out /tmp/kimi-run-next --go-ahead \
  --budget 30 --attempt-budget 15 --tariff-input 34 --tariff-output 1105 --reps 1
```

## 7. Key files touched in this session

```text
native/workspace.h                     RoleEdit::drop, Workspace::Context, Apply(..., amend)
native/workspace.cc                    Context, ShortestRoleAlias, TryResolveRoleId,
                                       MatchesVersion, multi-error Apply, last_failed_batch.json,
                                       ObjectBox Commit changed-files fix
native/main.cc                         context CLI command, ParsePatchInput (#@ <<< === >>>),
                                       --amend flag, updated help/schema
skills/tfengine-code/SKILL.md          compressed 3-step fast route (47 lines)
skills/tfengine-code/workspace-admin.md  import/ignore/sync/JSON/recovery companion doc
scripts/tabletop_paired_trial.py       concise ENVIRONMENT['db']
scripts/native_paired_trial.py         SKILL.md slice bound fix
tests/native/test_fast_route_core.cc   C++ suite for context, suffixes, short hashes,
                                       multi-error diagnostics, --amend, ObjectBox changed
tests/test_cli.py                      test_context_compact_patch_and_amend (30/30 total)
README.md, docs/architecture.md, AGENTS.md   updated CLI/architecture/invariants docs
```
