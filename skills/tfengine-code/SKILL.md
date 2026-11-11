---
name: tfengine-code
description: Read and edit named roles with tfe-code. Use for multi-language workspaces (Python, C/C++, TypeScript, JavaScript, Rust, Go) with .tfe-code, explicit-template workspaces with .roles/store.json, or when the user requests this workflow.
---

# TFEngine Code

- `.tfe-code/` present → follow **Imported workspace** below (use native MCP tools `tfe_context`, `tfe_apply`, `tfe_search`, `tfe_outline`, `tfe_show`, `tfe_ls`, `tfe_prepare_batch`, `tfe_import`, `tfe_sync`, and the structural `tfe_add_role` / `tfe_move_role` / `tfe_delete_role` / `tfe_stats` if connected via `tfe-code serve --mcp`, or the `tfe-code` CLI).
- Supported languages: **Python** (`.py`), **C/C++** (`.cc`, `.cpp`, `.cxx`, `.c`, `.h`, `.hpp`), **TypeScript/JavaScript** (`.ts`, `.tsx`, `.js`, `.jsx`), **Rust** (`.rs`), and **Go** (`.go`).
- `.roles/store.json` present → read `legacy-templates.md` in this folder instead.
- First-time `import`/`.tfe-codeignore`, real-time `watch`, external `sync`, JSON `apply` schema, telemetry `stats`, or crash recovery → read `workspace-admin.md` in this folder.

## Imported workspace (`.tfe-code`)

Use the `tfe-code` launcher from the workspace root. If `tfe-code` is not installed:
```bash
curl -fsSL https://raw.githubusercontent.com/Artem535/TFEngineCode/main/install.sh | sh
export PATH="$HOME/.local/bin:$PATH"
```
Never store state in `/tmp` across commands; pipe via heredocs or keep files inside the workspace.

1. **Read selective context in one call.** Queries are case-sensitive literal substrings of role IDs or code. Fetch all relevant roles at once with `context` (or MCP tool `tfe_context`):
   ```bash
   tfe-code context 'class Order' 'def create_order' [--file src/pkg/orders.py] [--references]
   ```
   If paths or symbol names are unknown, run `tfe-code ls [PATH_OR_PREFIX]` or `tfe-code search TERM --ids` (or MCP tool `tfe_search` with `ids_only: true`) first, then `context`. Use `--references` (or `--callers`) to append one-line calling references across the project for matched symbols without an extra search step. Do not read one file per turn or run `outline` across the whole project.

2. **Apply all edits and verify in one compact batch.** `context` prints `#@ role [v:HASH]` headers and **dedented** role text. Copy `old` lines verbatim from `context` — send short unique fragments, never whole role bodies. Edit leaf roles for functions/methods, class roles only for class headers/fields, and `.file` roles only for module imports/constants/includes. Repeat `<<< ... === ... >>>` under one `#@` header when one role needs multiple replacements (never repeat the same `#@` role twice), via `tfe-code apply` or MCP tool `tfe_apply`:
   ```bash
   tfe-code apply --check 'bash check.sh' <<'PATCH'
   #@ orders.file [v:a1b2c3d4e5f6]
   <<<
   from dataclasses import dataclass
   ===
   from dataclasses import dataclass
   from typing import Literal
   >>>

   #@ service.OrderService.create [v:9f8e7d6c5b4a]
   <<<
   def create(self, item: str) -> Order:
   ===
   def create(self, item: str, priority: str = "normal") -> Order:
   >>>
   <<<
       return Order(item=item)
   ===
       return Order(item=item, priority=priority)
   >>>
   PATCH
   ```
   To perform structural modifications without manual file editing, use native structural operations (MCP equivalents take the same anchors as `target_child_role` plus `before` / `after_role`):
   - `tfe-code add-role PARENT_ROLE NEW_ROLE_NAME [TARGET_CHILD_ROLE] [--before|--after]`
   - `tfe-code move-role SOURCE_ROLE DEST_PARENT_ROLE [TARGET_CHILD_ROLE] [--before|--after]`
   - `tfe-code delete-role ROLE_ID`
   To add a brand-new source file, write it in the workspace and run `tfe-code import PATH` (or MCP tool `tfe_import`).

3. **Amend on failure; stop once verified.** A failed `apply` rolls back managed files, reports all role errors with target aliases and `[v:...]` hashes, and saves the draft in `.tfe-code/last_failed_batch.json`. Fix only the broken role(s) with `tfe-code apply --amend --check 'bash check.sh' <<'PATCH'` (use `#@ role [drop]` to drop a staged role) instead of re-sending unchanged roles. A passing `apply --check` has already run the check — report immediately without re-running it. Inspect token savings with `tfe-code stats`. Do not modify tools or verification scripts.
