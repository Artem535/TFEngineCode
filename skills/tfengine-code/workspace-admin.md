---
name: tfengine-code-workspace-admin
description: Setup, ignore rules, external sync, JSON apply schema, and crash recovery for imported .tfe-code workspaces. Read only when needed.
---

# TFEngine Code — workspace setup, sync, JSON schema, and recovery

Read this file only when importing a workspace for the first time, configuring
`.tfe-codeignore`, reconciling edits made outside `tfe-code`, using raw JSON
batches, or recovering from an interrupted command.

## Importing and `.tfe-codeignore`

Import specific files with `tfe-code import FILE.py...` or recursively with
`tfe-code import .`. Recursive discovery excludes `.git/`, `.tfe-code/`,
`.venv/`, `venv/`, `env/`, `__pycache__/`, `build/`, `dist/`, and `node_modules/`
by default.

Run `tfe-code ignore` to write those defaults to `.tfe-codeignore` without
overwriting an existing file. When `.tfe-codeignore` exists, its rules replace
the defaults while `.tfe-code` always stays protected. Supported patterns:
comments (`#`), `*`, `**`, relative paths, leading `/`, and trailing `/`.
Negation (`!`) is rejected; consumer `.gitignore` files are never read or
modified. Explicit `.py` paths passed to `import` bypass `.tfe-codeignore`.

## External source edits and `sync`

In default JSON mode, managed `.py` files are authoritative:

- After editing, deleting, or byte-identically renaming a managed file outside
  `tfe-code`, run `tfe-code sync` and read fresh role versions before `apply`.
- `sync` removes deleted file roles and reconciles 1:1 content-preserving
  renames. Ambiguous duplicate-content matches fail without writing; resolve
  them to 1:1 first.
- Unrelated new `.py` files and edited renames require an explicit
  `tfe-code import PATH...`.
- Simultaneous source and index changes fail as a conflict for user resolution.

In `TFE_CODE_INDEX=objectbox` mode, the role tree lives in `.tfe-code/db` and
source files are rendered output (`tfe-code render --write` materialises any
unwritten files on disk; `apply` materialises changed files automatically).

## JSON `apply` alternative and `prepare-batch`

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

## Interruption recovery

If a process is interrupted mid-commit, `.tfe-code/transaction.json` restores
the pre-batch managed files automatically on the next valid `tfe-code` command
under `.tfe-code/lock`. Check commands must not invoke `tfe-code` (they would
contend for the workspace lock). Recovery covers process interruption, not OS
crash/power loss or external side effects of check commands; in ObjectBox mode,
interruption recovery rebuilds the store from the `project.json` mirror.
