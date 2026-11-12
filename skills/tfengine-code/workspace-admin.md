---
name: tfengine-code-workspace-admin
description: Setup, ignore rules, real-time file watch, external sync, structural role operations, token telemetry, JSON apply schema, and crash recovery for imported .tfe-code workspaces. Read only when needed.
---

# TFEngine Code — workspace setup, sync, JSON schema, and recovery

Read this file only when importing a workspace for the first time, configuring
`.tfe-codeignore`, running background real-time synchronization (`watch`),
reconciling edits made outside `tfe-code`, using structural role operations,
measuring token economy via `stats`, using raw JSON batches, or recovering
from an interrupted command.

## Supported Languages

`tfe-code` parses ASTs, tracks role versions, and verifies byte-identical render across:
- **Python:** `.py`
- **C / C++:** `.c`, `.cc`, `.cpp`, `.cxx`, `.h`, `.hpp`
- **TypeScript / JavaScript:** `.ts`, `.tsx`, `.js`, `.jsx`
- **Rust:** `.rs`
- **Go:** `.go`

## Importing and `.tfe-codeignore`

Import specific files with `tfe-code import FILE...` or recursively with
`tfe-code import .`. Recursive discovery always skips dot-directories — `.git/`,
`.tfe-code/`, `.worktrees/`, `.venv/`, `.cache/` — because they hold checkouts,
caches or state rather than managed sources, and no `.tfe-codeignore` rule can
re-include them. On top of that it excludes `venv/`, `env/`, `__pycache__/`,
`build/`, `dist/`, `target/`, and `node_modules/` by default. `tfe-code watch`
applies the same dot-directory rule.

Run `tfe-code ignore` to write those defaults to `.tfe-codeignore` without
overwriting an existing file. When `.tfe-codeignore` exists, its rules replace
the defaults while `.tfe-code` always stays protected. Supported patterns:
comments (`#`), `*`, `**`, relative paths, leading `/`, and trailing `/`.
Negation (`!`) is rejected; consumer `.gitignore` files are never read or
modified. Explicit file paths passed to `import` bypass `.tfe-codeignore`.

A C++ or TypeScript/JavaScript file whose tree-sitter parse contains ERROR
nodes does not fail the import: Qt macros (`Q_OBJECT`, `emit`, `Q_ARG(int, 2)`)
parse as errors without a preprocessor even though the compiler accepts the
file, and `.ts` collides with Qt Linguist XML translation files. Such a file
degrades to a single whole-file role (`.file`, holding the exact bytes) and its
path is listed in the `degraded` array of the `import`/`sync` JSON result.
Degraded files stay editable through `apply` on the file role; `apply` still
rejects edits that would break the syntax of a structurally imported file.
Within a structurally imported file, a role whose lines sit at a smaller
indentation than the role itself (verbatim raw string or template literal
content, column-0 preprocessor guards) stays inside its parent verbatim so the
byte round trip stays exact.

## Real-Time Synchronization Daemon (`tfe-code watch`)

Instead of manually running `tfe-code sync` after external edits:
```bash
tfe-code watch [--timeout-ms N] [--once]
```
`tfe-code watch` uses a native cross-platform watcher (`efsw` on Linux `inotify`, macOS `kqueue`, and fallback) with debounced file events to automatically synchronize changed, created, moved, and deleted source files in the background without manual intervention.

## External Source Edits and `sync`

In default JSON mode, managed source files are authoritative:

- After editing, deleting, or byte-identically renaming a managed file outside
  `tfe-code`, run `tfe-code sync` (or run `watch`) and read fresh role versions before `apply`.
- `sync` removes deleted file roles and reconciles 1:1 content-preserving
  renames. Ambiguous duplicate-content matches fail without writing; resolve
  them to 1:1 first.
- Unrelated new files and edited renames require an explicit
  `tfe-code import PATH...`.
- Simultaneous source and index changes fail as a conflict for user resolution.

In `TFE_CODE_INDEX=objectbox` mode, the role tree lives in `.tfe-code/db` and
source files are rendered output (`tfe-code render --write` materialises any
unwritten files on disk; `apply` materialises changed files automatically).

## Structural Role Operations

To manipulate code structure without manual string surgery or copy-pasting:
- `tfe-code add-role PARENT_ROLE NEW_ROLE_NAME [TARGET_CHILD_ROLE] [--before|--after]`
- `tfe-code move-role SOURCE_ROLE DEST_PARENT_ROLE [TARGET_CHILD_ROLE] [--before|--after]`
- `tfe-code delete-role ROLE_ID`

Structural operations update AST role relations, adjust indentation and container templates, and re-render the affected source files atomically.

## Token Economy Telemetry (`tfe-code stats`)

To inspect token savings achieved by role-based operations vs raw whole-file baselines:
```bash
tfe-code stats [--json] [--reset]
```
Outputs total calls, baseline bytes/tokens, emitted bytes/tokens, and overall savings percentage broken down by command (`apply`, `context`).

## JSON `apply` Alternative and `prepare-batch`

Besides the compact `#@ role [v:HASH]` patch format shown in `SKILL.md`,
`tfe-code apply` accepts JSON on stdin (`tfe-code apply --help` prints the full
schema):

```json
{
  "edits": [
    {
      "role": "service.OrderService.create",
      "expected_version": "a1b2c3d4e5f6",
      "replace": [
        {"old": "timeout: int = 10", "new": "timeout: int = 30"}
      ]
    }
  ]
}
```

Each entry uses either `"old"`/`"new"` or `"replace": [{"old": ..., "new": ...}]`
(mutually exclusive), or `"drop": true` with `apply --amend`. `expected_version`
accepts the full 64-hex SHA-256 or a unique `>=8`-hex prefix; `role` accepts the
full role ID or a unique dotted suffix. `tfe-code prepare-batch [QUERY...] [--file FILE]...`
emits a pre-populated JSON batch template when programmatic JSON editing is needed.

## Interruption Recovery

If a process is interrupted mid-commit, `.tfe-code/transaction.json` restores
the pre-batch managed files automatically on the next valid `tfe-code` command
under `.tfe-code/lock`. Check commands must not invoke `tfe-code` (they would
contend for the workspace lock). Recovery covers process interruption, not OS
crash/power loss or external side effects of check commands; in ObjectBox mode,
interruption recovery rebuilds the store from the `project.json` mirror.
