# Architecture

TFEngine Code has two deliberately separate workflows.

## Native imported workspace

The native `tfe-code` executable manages existing Python source in place. The
source files are authoritative; `.tfe-code/project.json` is a derived TF role
snapshot and `.tfe-code/state.json` records source/render/project SHA-256 hashes
and the latest check receipt. The legacy `.roles/store.json` is not shared with
this mode.

`native/python_import.cc` parses Python with tree-sitter and creates roles for
files, classes, functions, methods, and nested functions. It uses byte ranges,
not regular expressions, and stores each role relative to its own indentation.
A parent references direct children with `{{role.id}}`. Import and sync accept a
candidate only when the real TF renderer reproduces every imported byte,
including CRLF, blank lines, Unicode, and a missing final newline. Syntax errors,
unsafe paths, and role collisions reject the candidate.

Each node stores 1-based inclusive `source_start_line`/`source_end_line` metadata.
Every commit reparses rendered candidates and copies ranges only onto matching role
IDs, so import, apply, and sync cannot retain coordinates for an unmatched role.
Legacy, empty, and unmatched roles use `0`/`0`. `outline` and plain `show` expose
available ranges, JSON exposes both fields, and tree/search formatting is unchanged.
Ranges describe current source locations, not role identity, versions, or history.

Import accepts individual `.py` files, directories, or `.` for the workspace.
Directory expansion is sorted, deduplicated, and does not follow directory symlinks.
`Workspace::CreateIgnoreFile()` and default traversal share one rules constant;
`tfe-code ignore` writes it to `.tfe-codeignore` without overwriting an existing file.
When present, custom rules replace the defaults. They support comments, `*`, `**`,
relative paths, leading `/`, and trailing `/`; negation is rejected rather than
claiming full `.gitignore` compatibility. Matching occurs only during recursive
discovery, so explicit `.py` imports bypass the file. `.tfe-code` remains unconditionally
protected, and the consumer `.gitignore` is neither interpreted nor modified. Search
is a read-only, case-sensitive literal selector over role IDs and text in the same
consistent snapshot; `--file FILE` restricts it to roles reachable from that
imported file. It returns roles with versions and changes no receipts.
`context` selects matching roles across the workspace (or named `--file` paths),
emits enclosing `.file` and class headers alongside matched leaf bodies using the
shortest unambiguous dotted role suffix and 12-hex version prefixes, and summarises
omitted unchanged siblings without dumping their bodies (and appends one-line `# Callers:` references across the project when `--callers` is passed). `role FILE NAME` selects a
unique role in that file by exact role ID or dotted suffix and returns its text and
version; missing or ambiguous names fail.

`native/render_project.cc` owns the shared resolver used by both `tfe-code` and
`tfe-code-render`. It performs TF substitution and indentation without writing
files or invoking models. `native/workspace.cc` owns persistence, read models,
optimistic versions, checks, and sync; `native/main.cc` only translates CLI
arguments, compact `#@` patch documents, and JSON.

Before a read or apply, the workspace compares source and index hashes with the
last consistent state. An external source edit or deletion makes the view stale until
`tfe-code sync`; simultaneous source/index divergence is a conflict, not an automatic
merge. Sync removes deleted file roles and discovers exact renames by matching one
deleted path's old source SHA-256 to one unmanaged path's current hash. Ambiguous
matches reject the candidate without index writes. Unrelated new files and edited
renames are not imported automatically. Each role version is the SHA-256 of its current
text; `apply` accepts either the full 64-hex hash or a valid `>=8`-hex prefix, and
resolves either full role IDs or unique dotted suffixes. `apply` accepts both JSON
batches and compact `#@ role [v:HASH]` `<<< ... === ... >>>` patch documents,
validates every role in one pass (reporting all mismatches together with target role
aliases and current short versions), and persists a rejected or check-failed batch in
`.tfe-code/last_failed_batch.json` so `apply --amend` can update or `[drop]` individual
roles without re-sending unchanged entries.

A batch renders and parses all candidate files before writing. Changed sources,
project, and state are written through same-directory temporary files and rename.
Before the first managed write, `.tfe-code/transaction.json` records every touched
path's previous bytes or absence. Entries are workspace-relative names (for
example `.tfe-code/project.json` or `pkg/a.py`), so recovery resolves them
against the workspace root unchanged even when that root is itself a symlink. A
failing check or handled write error restores those bytes and removes the
journal. After process interruption, the next valid
native command restores the pre-batch snapshot under the exclusive workspace lock;
journal removal happens only after complete, idempotent recovery. A rollback or
recovery failure retains the journal and is reported explicitly. This covers process
crashes, not OS/power-loss durability or external check-command side effects. The
lock serializes native writers and source hashes are checked again before commit.

`apply --check COMMAND` and `check COMMAND` execute the trusted command in a
child shell rooted at the consumer workspace. The receipt key covers the
project, all managed sources, and command. Any accepted source/index change
invalidates an unrelated receipt. Checks are real consumer tests, not a sandbox
or a proof of runtime correctness.

## ObjectBox index (`TFE_CODE_INDEX=objectbox`)

Setting `TFE_CODE_INDEX=objectbox` moves the role tree into the engine's ObjectBox
store at `.tfe-code/db`. `native/objectbox_store.cc` maps one role to one `Block`:
the role id becomes the block id, the role text the template, and a JSON
`description` carries the SHA-256 the `apply` contract depends on plus `literal`
and the source ranges. The engine `Version` stays `major.minor`, so a new role
publishes `1.0` and every edit publishes the next minor — history the JSON mode
does not keep. A per-file `Meta` block holds the `{root, params}` mapping, so the
whole `Project` is reconstructable from the database alone.

In this mode the source files are rendered output and the store holds the role
tree, but the mirror decides when the two disagree. A workspace that contains
nothing but `.tfe-code/db` is valid: `tfe-code render` produces every file from
the store and `tfe-code render --write` materialises them, returning only the
written paths. During `Commit`, `result.changed` compares candidate renders against
the pre-batch rendered tree even when `src/` was initially unmaterialised, so an
`apply` still materialises all needed source files for `--check` while reporting only
logically modified paths. When `project.json` is present it is loaded first, and the
stored tree is served only if its serialised hash equals `state.project_hash`; on
disagreement the mirror wins and is republished into the store, so a stale index
is never presented as the current tree.

`ObjectBoxRoleStore::Save` publishes an entire batch inside one
`obx::Transaction`, so a failure part-way through leaves the store unchanged and
the file rollback still restores sources and the mirror. `Commit` publishes only
after any `--check` command has passed: in a database-only workspace there is no
mirror to rebuild from, so publishing earlier would leave rejected roles in the
store. Because a binary store
cannot be byte-snapshotted, recovery after an interruption rebuilds the index from
the mirror instead of restoring it; that is a documented limit, not a guarantee.

The adapter owns the `obx::Store` and passes `EngineConfig::defer_init` so the
engine does not open a second store on the same path, which ObjectBox forbids.
`native/main.cc` sets the engine log level to `Off` by default: the engine reports
"Block not found" as an error on every first publish, and an agent merges stderr
into command output, which would inflate measured cost. `TFE_CODE_LOG` restores
detail.

## Legacy explicit templates

The Python CLI owns `.roles/store.json` with `blocks` and `files` mappings. It
normalizes explicit `#@` role documents and invokes `tfe-code-render`, then
writes generated files below `out/`. This mode can save edits before a failed
check and does not provide native import, sync, or rollback guarantees.

The engine remains an external dependency; this repository does not vendor it.
No model provider client, credential handling, evaluation logs, private consumer
sources, or paid-run automation is included. The prototype's task-specific
navigation hint and unsound custom AST scope checker are not part of either
workflow.
