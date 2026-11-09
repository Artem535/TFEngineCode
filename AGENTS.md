@/home/a.durynin/.codex/RTK.md

# TFEngineCode

These are the shared repository instructions for Codex and Claude Code.

## Scope and architecture

TFEngine Code edits Python and C/C++ code as a tree of named roles and re-renders ordinary
source files with the external C++ TextFoundryEngine renderer. Read `README.md`
for usage and `docs/architecture.md` for the two workflow contracts.

- `native/`: the C++20/23 adapter — `tfe-code` (CLI) and `tfe-code-render`.
  `native/workspace.cc` owns persistence, locking, hashes, optimistic versions,
  checks, import/apply/sync, `context`, and crash recovery; `native/python_import.cc`
  and `native/cpp_import.cc` build roles from tree-sitter byte ranges;
  `native/render_project.cc` is the shared resolver used by both executables;
  `native/main.cc` only translates arguments, compact `#@` patch syntax, and JSON.
  Do not put business logic in `main.cc`.
- `src/tfengine_code/`: the legacy Python CLI for explicit `#@` templates.
- `skills/tfengine-code/`: the agent workflow skill (`SKILL.md` for the fast
  3-step route, `workspace-admin.md` for setup/sync/recovery/JSON schema, and
  `legacy-templates.md` for `.roles`); `skills/source-code-editing/`: the
  file-editing control skill used for comparisons.
- `tests/native/`: C++ suites; `tests/test_cli.py`: Python integration tests
  covering both the native and the legacy CLI.
- `docs/`: architecture, evaluation contract, handoff and pilot notes.
- `scripts/`: `rehearse_workflow.py` (deterministic offline rehearsal),
  `native_paired_trial.py`, and `tabletop_paired_trial.py`.
- `examples/hello/`: a small standalone example.

Role model: a file is split into file/class/struct/namespace/function/method/nested-function
roles. A parent references a direct child with `{{role.id}}`; indentation belongs
to the parent; role text is stored dedented.

Persistence has two modes, selected by `TFE_CODE_INDEX`:

- **Default (JSON).** `.tfe-code/project.json` is a derived index and the source
  files are authoritative. Never treat the index as the source of truth.
- **`TFE_CODE_INDEX=objectbox`.** The role tree lives in the engine's ObjectBox
  store at `.tfe-code/db`; the source files are rendered output that
  `tfe-code render --write` materialises. `project.json` remains only the
  crash-recovery mirror. A workspace holding nothing but `.tfe-code/db` is valid
  and must render the whole tree.

The engine at `/home/a.durynin/Projects/C++/TextFoundryEngine` is an external,
unvendored dependency (`feat/structural-fragments` branch).

## tfengine-code skill (agent rule)

This repository is itself a TFEngine Code-managed workspace. Whenever a task
touches a managed workspace (a directory with `.tfe-code/`, or a legacy
`.roles/store.json`) — including this repo — agents must read managed files only
through `tfe-code` (`context`, `show`, `outline`, `tree`, `search`,
`prepare-batch`) and edit them only through `tfe-code` (`apply` compact `#@`
patches or JSON batches, `add-role` / `move-role` / `delete-role`, `import` for
new files, `sync` after external changes). Do not open managed files in a file
tool and do not hand-edit them: load the `tfengine-code` skill first
(`$tfengine-code` / `/tfengine-code`; ZCode installs it at
`~/.zcode/skills/tfengine-code/SKILL.md`, Codex at
`~/.agents/skills/tfengine-code/SKILL.md`; both mirror this repo's
`skills/tfengine-code/` docs) and follow its route, via the CLI or the MCP tools
served by `tfe-code serve --mcp`.

Files outside the supported languages (`.py`, `.c`/`.cc`/`.cpp`/`.cxx`,
`.h`/`.hpp`, `.ts`/`.tsx`/`.js`/`.jsx`, `.rs`, `.go`) cannot be role-edited:
CMakeLists, manifests, markdown, and the `.tfe-codeignore` rules are edited
directly. `.tfe-code/` state is never touched directly.

Agent sessions standardise on the ObjectBox index: export
`TFE_CODE_INDEX=objectbox` before interactive `tfe-code` work, and materialise
the rendered sources with `tfe-code render --write` after a batch lands.

## Two deliberately separate workflows

Neither workflow is an Engine/ObjectBox publication or version-history
implementation, and they do not share state.

- **Native imported workspace** (`.tfe-code/project.json`, `state.json`, `lock`,
  `last_failed_batch.json`, and transient `transaction.json`) imports existing
  Python and C/C++ sources in place.
- **Legacy explicit templates** (`.roles/store.json`, generated `out/`) is the
  Python CLI for explicit `#@` documents, including `put`, `replace`, file
  mappings and legacy apply.

Add new functionality to `native/` only. Do not port native behavior into the
legacy CLI, and do not reintroduce native `put`/`replace`/`file` commands: the
native edit path is `tfe-code apply` (compact `#@ role [v:HASH]` `<<< === >>>`
patches or JSON batches).

## Task & Issue Pipeline (Mandatory Workflow)

Every feature, stage, or fix must strictly follow this 5-step development pipeline:

1. **Create Issue, Task & Feature Branch:**
   - Create an epic/parent issue (if needed) and a concrete task issue in GitHub (`gh issue create`) with clear scope, Acceptance Criteria (AC), and Definition of Done (DoD).
   - Create and switch to a dedicated feature branch for the task (e.g., `git checkout -b feat/issue-<N>-<short-slug>`).
2. **Label as In Progress:**
   - Attach the `in-progress` label to the active issue (`gh issue edit <N> --add-label "in-progress"`).
3. **Start Work & Post Initial Status:**
   - Begin work on the task and post a comment on the task issue (`gh issue comment <N> --body "..."`) indicating that work has started, the branch name, and the execution plan.
4. **Log Progress Continuously to the Task:**
   - As implementation milestones, test runs, or architectural decisions are completed, post progress updates and verification results directly as comments to the task issue (`gh issue comment <N>`).
5. **Close Task & Create Merge Request (PR):**
   - Once all Acceptance Criteria and tests pass (`ctest` and `pytest`), push the feature branch (`git push -u origin <branch>`), open a Pull Request / Merge Request (`gh pr create`) linking the issue (`Closes #<N>`), post the final summary to the task, and close the task issue (`gh issue close <N>` / merge PR).

Do not run destructive git commands (`git reset --hard`, `git checkout --`, `git clean`) that could discard uncommitted work.

## Invariants

- Import, apply and sync accept a candidate only when the real TF renderer
  reproduces every imported byte — CRLF, blank lines, Unicode, and a missing
  final newline included.
- A role version is the SHA-256 of its current text. Version fields are content
  hashes, not publication history. `apply` accepts either the full 64-hex hash
  or a valid `>=8`-hex prefix, and resolves either exact role IDs or unique
  dotted suffixes.
- `apply` requires a nonempty `old`, exactly one match of `old` in the role
  text, and a current `expected_version`. Byte-identical duplicate entries for
  one role collapse silently, because merging several `prepare-batch` outputs
  produces them; two *different* edits to one role are still rejected as an
  ambiguous batch. One role may carry several edits through `replace`
  (`[{"old","new"}, …]` in JSON or multiple `<<< ... === ... >>>` blocks in a
  compact `#@` patch), each matching exactly once in the evolving text; that
  form and `old`/`new` are mutually exclusive for a single role. Failed batches
  are saved to `.tfe-code/last_failed_batch.json` (on imported workspaces) so
  `apply --amend` can override or drop individual roles without re-sending
  unchanged ones. Agents must send fragments, not whole role bodies: re-emitting
  unchanged lines is the main cost of a batch.
- A check command must not invoke `tfe-code` itself. `apply --check` holds the
  workspace lock while the check runs, so a nested CLI call would wait on that
  lock. The lock now waits at most 30 s and then fails with an explicit message
  instead of hanging forever.
- A malformed or invalid transaction journal must stay in place and must not
  modify managed files; recovery removes the journal only after complete,
  idempotent restoration.
- Source line ranges are 1-based inclusive navigation metadata refreshed from
  rendered bytes and copied only onto matching role IDs; unmatched, empty and
  legacy roles use `0`/`0`. They are not identity or version.
- ObjectBox is used only through the optional `textfoundry_objectbox_store`
  target, which `CMakeLists.txt` now enables. The role adapter lives in
  `native/objectbox_store.{h,cc}`; keep ObjectBox types out of `engine.h` and out
  of `native/workspace.h`. A batch publish runs inside one `obx::Transaction`, so
  a failed batch must leave the database unchanged.
- `.tfe-code` stays excluded from recursive import unconditionally; the consumer
  `.gitignore` is neither interpreted nor modified.

## Build and verification

Use the RTK instructions imported above for shell commands. `CMakeLists.txt`
requires `TEXTFOUNDRY_SOURCE_DIR` pointing at a TextFoundryEngine checkout that
contains `tf/renderer.h`; configuration fails with `FATAL_ERROR` otherwise.

```bash
rtk cmake -S /home/a.durynin/Projects/C++/TFEngineCode \
  -B /home/a.durynin/Projects/C++/TFEngineCode/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DTEXTFOUNDRY_SOURCE_DIR=/home/a.durynin/Projects/C++/TextFoundryEngine \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
rtk cmake --build /home/a.durynin/Projects/C++/TFEngineCode/build --parallel
rtk ctest --test-dir /home/a.durynin/Projects/C++/TFEngineCode/build --output-on-failure
rtk git diff --check
```

CTest currently registers 22 tests in `build` (including `test_cpp_import_core`,
`test_fast_route_core`, `test_apply_replace_core`, `test_list_core`,
`test_prepare_batch_core`, and the core/crash/ObjectBox/manifest suites). Every
`tests/native/test_<module>_*.cc` becomes its own executable, and the module name
is the part between `test_` and the *last* underscore, so `test_crash_matrix_long_tail.cc`
belongs to module `crash_matrix_long`. The `TFE_CODE_ADD_TESTS` cache option takes a
semicolon-separated module list and narrows the registered subset (e.g.
`-DTFE_CODE_ADD_TESTS=apply;crash` so a worker builds only its own module); an
empty value builds every test.

Python integration tests need **both** environment variables, otherwise they are
skipped:

```bash
TFE_CODE_NATIVE=/home/a.durynin/Projects/C++/TFEngineCode/build/tfe-code \
TFE_CODE_RENDERER=/home/a.durynin/Projects/C++/TFEngineCode/build/tfe-code-render \
  rtk python3 -m unittest discover -s tests -v
```

**Warning:** without `TFE_CODE_NATIVE` and `TFE_CODE_RENDERER`, 29 of the 36
Python tests are skipped by the `skipUnless` guards in `tests/test_cli.py`, and
a green run proves almost nothing. With both variables set the suite must report
36/36. The C++ suites are plain `int main()` programs using a local `Expect()`
helper — there is no doctest, so a suite passes only when its process exits 0.

Report the checks actually run and any remaining failures or skipped
verification. For documentation-only changes, verify referenced paths and
commands and check whitespace; compilation is unnecessary.

## Exit-code contract

Both binaries print plain-text diagnostics on stderr — there is no
`{"error":{"code":…}}` JSON envelope here (that is the engine's contract, not
this repository's).

- `0`: success.
- `1`: domain/engine error, including a failed `check` command.
- `2`: usage error — unknown command, missing or extra arguments (`tfe-code`
  with no arguments, `tfe-code ignore extra`), and a malformed `apply`
  invocation.

`ignore` is idempotent only in effect: the second run exits `1` with stderr
`Ignore file already exists: .tfe-codeignore` and leaves the file unmodified.

## Making changes

Write the failing test first (RED), then the implementation, then run the full
native and Python suites. A compile or import check is not completion. Read
`docs/architecture.md` before changing a documented workflow, and update `docs/`
or `skills/` only for behavior actually introduced by the change — keep proposed
features clearly separate from implemented ones. When touching a skill, keep one
source of truth per rule, do not add task-specific hints, and do not embed
benchmark answers.

## Deferred and known limitations

- No automatic conflict merge; simultaneous source/index divergence stops until
  the operator resolves it.
- No runtime sandbox: `apply --check` and `check` execute trusted consumer
  commands in a child shell and do not roll back their external side effects.
- No publication history or version store; roles are mutable JSON snapshots.
  The ObjectBox mode does keep every published revision as a `major.minor`
  version, but that history is not exposed through the CLI yet.
- In `TFE_CODE_INDEX=objectbox` mode a batch publish is atomic through an
  ObjectBox transaction, but the crash journal cannot snapshot the binary store.
  After an interruption the database is rebuilt from the recovery mirror on the
  next write, so this is not a byte-level restore of the index.
- The legacy Python CLI has no rollback: a failed check can leave saved edits and
  generated files behind.
- Recovery covers process interruption, not OS crash or power-loss durability.
- Ambiguous duplicate-content renames are rejected rather than guessed;
  unrelated new files and edited renames are not imported automatically.
- Case-insensitive filesystems and cross-platform rename behavior are not
  specified.

## Shared memory (MCP `memory`)

This memory is shared with other agents (Claude Code and Codex). Always use `group_id` `TFEngine` — both when writing and when searching (`group_ids: ["TFEngine"]`).

**At the start of a task**, call `search_memory_facts` (and `search_nodes` if useful) about the task: decisions, conventions, known issues, how to build/test.

**Save with `add_memory`** when:
- an architectural or technical decision is made (what was chosen and why);
- a non-obvious bug cause or workaround is found;
- a project convention, build/deploy command, or environment quirk is discovered;
- the user states a preference or rule.

Format: `name` — short title; `episode_body` — 2–6 factual sentences, no filler, with file/service names; `source: "text"`; `source_description: "claude-code"` or `"codex"`.

**Never save** secrets, keys, passwords, personal data, `.env` contents, or anything already recorded in the code or git history.

If a memory fact contradicts the code, trust the code and save the corrected fact.
Do not call `clear_graph`, `delete_episode`, or `delete_entity_edge` unless the user explicitly asks.
