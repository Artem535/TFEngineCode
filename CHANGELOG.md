# Changelog

## 0.4.0

### Structural roles
- `add-role` / `move-role` accept a positional `TARGET_CHILD_ROLE` and
  `--before`, matching the shape the skill and README already documented;
  combining `--before` with `--after` is a usage error (exit `2`).
- Container targets resolve a file path, then a bare module (`dialect` ->
  `dialect.file`), then a unique dotted suffix, so structural operations accept
  the same targets `apply` does.
- A created class marker inherits the container `;` from its anchor sibling (or
  from the all-siblings convention when appended), which removes the
  `expected ';' after class definition` repair patch.
- `ApplyResult` surfaces `created` and `moved`, so a re-parent no longer answers
  with an empty `changed` list.

### MCP server
- The handshake accepts protocol `2025-11-25` (PhoenixMcp pin `f2fd8579`) and an
  unknown client version negotiates down to the latest supported revision
  instead of failing initialization.
- `serverInfo.version` tracks the project version instead of a hardcoded value.
- `tfe_add_role` / `tfe_move_role` take `target_child_role` plus `before`, and
  the new `tfe_ls` / `tfe_prepare_batch` tools expose the cheapest read routes.
  The server instructions, README and skill name all 13 registered tools, and the
  stdio suite covers every one of them.

### Import, sync and navigation
- Recursive import and `watch` skip dot-directories (`.worktrees/`, `.cache/`,
  ...) instead of importing checkouts and generated trees.
- `ls` accepts directory prefixes: `ls src/pkg` lists what `ls src.pkg` lists.
- Removing a file's roles walks the subtree leniently, so a literal marker inside
  a source string no longer wedges `sync` with a role id collision.
- Macro-heavy C++ files no longer abort the import (#68): `Q_OBJECT`, `emit`,
  `Q_ARG(int, 2)` parse as tree-sitter ERROR nodes without a preprocessor (233
  of 672 C++ files on a real Qt project), and Qt Linguist writes XML with a
  `.ts` extension. Such files degrade to a single whole-file role holding the
  exact bytes, and `import` / `sync` report the paths in the new `degraded`
  array. Degraded files stay editable through `apply` on the file role; `apply`
  still rejects edits that break the syntax of a structurally imported file.
- Fix three byte-loss bugs in the structural import of files that do parse
  (#68): a one-line template header (`template <typename T> QByteArray f() {`)
  no longer strips body indentation; a role holding verbatim column-0 lines
  (`#if` guards, raw string or template literal content) stays inside its
  parent verbatim instead of shifting at render time; a UTF-8 BOM survives in
  the file role.

### Known limitations
- A role whose text keeps a literal marker (a `{{...}}` sequence in a string
  literal) still cannot be edited through `apply` (#64).

## 0.3.0

- Fix the 0.3.0 release build: link libstdc++ statically no longer succeeds once
  folly (pulled in by the PhoenixMcp stdio server) is part of the binary, because
  folly's exception tracer defines the same `__cxa_*` symbols as `libstdc++.a`.
  Release binaries link libstdc++ dynamically and `TFE_CODE_STATIC_LIBSTDCXX`
  stays off.
- Fix the test-only crash-injection hook (`TFE_CODE_TEST_CRASH_AFTER`) to compare
  canonical paths, so a crash point under a symlinked workspace root is reached.
  Previously `test_crash_recovery_journal` and `test_crash_matrix_long_tail`
  failed on macOS, where `TMPDIR` is `/var/folders/...` and `/var` links to
  `/private/var`.
- Record workspace-relative names in the recovery journal instead of deriving
  them with `lexically_relative()`. When the workspace root is a symlink (any
  macOS `/var/folders/...` workspace), the derived entry was `../<real-root>/file`
  and the next command rejected the journal as escaping the workspace, so an
  interrupted commit became unrecoverable.

- Add release packaging and CI/CD automation (`.github/workflows/release.yml` and `.github/workflows/tests.yml`):
  - Portable Linux and macOS binary builds with `-static-libgcc` and `$ORIGIN` runtime path resolution for bundled `libobjectbox`.
  - Automatic packaging into `tfe-code-<target>.tar.gz` for `x86_64-unknown-linux-gnu`, `aarch64-unknown-linux-gnu` and `aarch64-apple-darwin`, with GitHub Release asset publishing on tags.
- Add standalone curl-pipe-sh installer `install.sh`:
  - Automatically resolves latest GitHub release tag via redirect inspection, downloads the portable tarball, installs to `$HOME/.local/bin`, and verifies binary execution.
- Support agent skill installation via `npx skills@latest add Artem535/TFEngineCode --skill tfengine-code`.
- Add C++ language support (`.cc`, `.cpp`, `.cxx`, `.c`, `.h`, `.hpp`, `.hxx`, `.hh`)
  via Tree-sitter C++ grammar:
  - Role decomposition for standalone functions, inline methods, classes, structs,
    and templates (`template_declaration` unwrapping).
  - Namespace flattening preserving file-level includes and wrappers.
  - Byte-exact lossless round-trip render `RenderFiles(imported) == source`.
  - Full workspace support: `import`, `outline`, `tree`, `context`, `search`,
    `prepare-batch`, and `apply` patching with automatic syntax validation rollback.
  - Verified with 100% test coverage including self-hosting smoke test parsing and
    losslessly re-rendering all 17 C++ source and header files of `native/`.
- Add `tfe-code context [QUERY...] [--file FILE]... [--callers]` for selective,
  compact one-call context extraction: emits module headers, matched leaf role
  bodies with shortest unambiguous aliases and 12-char version prefixes, and
  one-line summaries of omitted siblings without dumping their bodies.
- Add `--callers` flag to `tfe-code context` to append one-line calling references
  across the project for matched symbols in a single step, removing the need for
  a separate `search --ids` round trip.
- Support compact `#@ ROLE [VERSION]` patch format with `<<< ... === ... >>>`
  blocks in `tfe-code apply`, accepting short role aliases and >=8-hex version
  prefixes without JSON escaping.
- Add multi-error batch validation in `apply` with versioned `FragmentSuggestion`
  diagnostics and persistence to `.tfe-code/last_failed_batch.json`, enabling
  zero-regeneration recovery via `tfe-code apply --amend`.
- Fix ObjectBox `Workspace::Commit` when workspace starts unmaterialised so
  `result.changed` reports only logically modified files.
- Refactor hot-path template placeholder rendering and `#@` patch header parsing
  to compile-time regular expressions via `ctre` 3.11.0, eliminating heap allocations
  and yielding a 37x-51x speedup.
- Integrate Google `re2` for linear-time $O(N)$, ReDoS-safe dynamic regular
  expression searching via `tfe-code search QUERY... --regex`.
- Compress agent skill `SKILL.md` from 142 lines (7.4 KB) to 47 lines (2.6 KB)
  around the fast 3-step route (`context` -> `apply --check` -> `--amend`), moving
  administrative/recovery rules to `workspace-admin.md`.
- Validate on real `TabletopMagnat` task (`event_level`) on `moonshotai/kimi-k3.1-pro`:
  `db` solved the task in 4 steps and 3 commands (faster than direct file editing
  at 5 steps / 4 commands) with 100% pass rate at 2.11 ₽.

- Add native recursive project import with deterministic path expansion.
- Add `tfe-code ignore` and editable `.tfe-codeignore` filtering for recursive
  imports, with documented wildcard/path rules and explicit negation rejection.
- Add native literal `search` and compatible `who` role navigation with versions.
- Reconcile deleted and exactly renamed managed files in `sync`, rejecting
  ambiguous duplicate-content rename matches without changing the index.
- Add 1-based inclusive source ranges to native outline/show and role JSON,
  refreshed on import/apply/sync with explicit unavailable zero values.
- Recover interrupted native multi-file commits from a write-ahead journal on
  the next command, with real process-crash tests and documented retry limits.
- Clarify native role IDs, dedented apply text, and mismatch diagnostics in the
  imported-workspace skill and CLI errors.
- In a bounded paired pilot of two CriticNode tasks and three repetitions, the
  skilled native and file arms both passed 6/6 attempts; native cost was 15.3%
  lower in aggregate and met the economic gate in 2/3 repetitions. This is not
  a repository-wide or language-wide savings claim.

- Clarify stdin invocation and output-relative file mappings after an incomplete
  pilot; the correction has local verification, no new cost measurement.

- Specify short read/edit/verify routes and context boundaries in the role skill.
- Add a balanced source-file skill for future comparisons.
- Add a deterministic offline rehearsal with independent behavioral checks;
  no new model cost or speed claim.

## 0.1.0

- Extract the role-document interface into a standalone Python package.
- Keep native rendering on the external TextFoundryEngine core.
- Add workspace initialization, agent skill, example, tests, and documentation.
- Remove task-specific hints and the unsound experimental scope checker.
- Fix multiline role documents in `put` and reject escaping output paths.
