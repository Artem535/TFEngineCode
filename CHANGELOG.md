# Changelog

## Unreleased

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
