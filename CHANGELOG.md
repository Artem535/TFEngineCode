# Changelog

## 0.6.0

### XML import
- `.xml` is a first-class imported language (`native/xml_import.cc`, grammar
  `tree-sitter-grammars/tree-sitter-xml` pinned at `5000ae8`). A document imports
  as `<module>.file` plus one role per element, named by its `id`/`name`
  attribute or by its tag with a numeric suffix for duplicates; nested elements
  are child roles, while text, comments, CDATA and processing instructions stay
  in the owning element and self-closing elements are leaves.
- A malformed `.xml` parse degrades to one whole-file role and is reported in
  `degraded`; `apply` rejects an edit that breaks the XML and leaves the file
  untouched.

### C++ headers
- A header wrapped in an include guard now exposes roles for its classes,
  structs, inline functions and `extern "C"` definitions. Preprocessor
  conditionals, `declaration_list` and linkage blocks are not scopes, so their
  declarations belong to the enclosing container; previously such headers
  imported as a single whole-file role even when the parse was clean, which made
  every header edit rewrite the whole file.

### Import receipts
- `import` prints a one-line summary (files written, counts, degraded paths,
  index size and mode) instead of one large JSON object. `--json` keeps the
  previous machine receipt and `--verbose` lists every changed path; a no-op
  re-import reports `No changes`.
- When the workspace is a git repository whose rules do not cover `.tfe-code/`,
  `import` warns on stderr instead of editing `.gitignore`.

### Role index
- The index mode is sticky per workspace: the first successful write records it
  in `.tfe-code/index-mode`, and commands without `TFE_CODE_INDEX` follow it.
  An explicit value overrides the record and becomes sticky; an unknown value is
  a usage error; a pre-existing `db/` without a record is adopted with a
  one-off stderr notice. `stats` and the TUI header show the active mode.

### Docs
- The skill (and its installed copies) points agents at `tfe-code help COMMAND`
  instead of private option lists, documents the import summary, the
  `.tfe-code/` git warning and the sticky index mode, and lists XML.

## 0.5.0

### QML import
- `.qml` is a first-class imported language (`native/qml_import.cc`, grammar
  `yuja/tree-sitter-qmljs` pinned at `de96ed6`). A file imports as
  `<module>.file` plus roles for every object (named by its `id:` binding or by
  its type), property, signal, binding/handler and function, recursing into
  objects inside `states`/`transitions` arrays and `component Name: Type {}`
  inline components. `id:` bindings stay inside the object text instead of
  becoming roles of their own.
- A `.qml` parse with ERROR or MISSING nodes degrades to one whole-file role and
  is reported through the existing `degraded` list, like C++ and TypeScript.
  `apply` rejects an edit that breaks the rendered QML and leaves the file
  untouched, and the TUI watcher treats `.qml` as a managed file.

### CLI
- Help is Typer-style: `tfe-code help` prints a grouped command index
  (Workspace / Read / Edit / Run / Help), while `tfe-code help COMMAND` and
  `tfe-code COMMAND --help` (or `-h`) print usage, arguments, options with
  defaults, notes, examples and exit codes. Output wraps to 88 columns and uses
  ANSI styling only on a TTY with `TERM` set and `NO_COLOR` unset.
- Help resolves before the workspace opens, so it also works outside a managed
  project. Usage errors keep exit `2` and a `Usage:` line, now followed by
  `Try 'tfe-code help' for help`. `check` still forwards its whole tail
  (`check bash -c true`) and `apply --help` still prints the JSON input schema.

### Build
- CI carries an upstream `libunwind` vcpkg overlay: GitHub re-archived the
  `v1.8.3` tarball, so the SHA512 pinned by `builtin-baseline 06da45aa` no
  longer matched and every install step failed. The overlay moves only that
  port; the baseline and every other dependency stay unchanged.

## 0.4.1

- Fix the release build for macOS arm64 (#72): the UTF-8 BOM literal in
  `tests/native/test_ts_import_core.cc` was written as `"\xEF\xBB\xBFexport
  ..."`, and a C++ hex escape consumes every following hex-digit character, so
  `\xBF` swallowed the `e` of `export` to form `\xBFe` (0xBFe, out of range
  for `char`). GCC only warns — silently truncating the literal to
  `EF BB FE` + `xport ...` while the test still passed by comparing the
  literal with itself — but AppleClang on the `macos-15` release runner
  rejects it. That error failed the `Build aarch64-apple-darwin` job of the
  v0.4.0 release run and skipped the Homebrew formula job, so v0.4.0 shipped
  Linux-only. The literal is now split into two adjacent string literals and
  the test asserts the true `EF BB BF` bytes; v0.4.1 restores the macOS arm64
  tarball and the formula.
- The release workflow generated release notes in the per-matrix upload step,
  so every successful build job re-wrote the release body and appended another
  copy of the same notes (v0.3.0 shipped three, v0.4.0 shipped two). Notes are
  now generated exactly once by a dedicated `publish-release-notes` job that
  runs after all build jobs; the per-matrix uploads only attach assets.

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
