# TFEngine Code

Edit code as a tree of named roles, then render ordinary source files with
[TextFoundryEngine](https://github.com/Artem535/TextFoundryEngine).

TFEngine Code gives coding agents a compact interface for reading the relevant
parts of a program and changing several roles in one operation. A role can be a
whole file, a class, a method, or a smaller unit of code. Parent roles reference children
with `{{role.id}}`; indentation belongs to the parent.

**Status:** High-performance native C++23 AST role engine.
Supports **Python** (`.py`), **C/C++** (`.cc`, `.cpp`, `.cxx`, `.c`, `.h`, `.hpp`),
**TypeScript/JavaScript** (`.ts`, `.tsx`, `.js`, `.jsx`), **Rust** (`.rs`), and **Go** (`.go`).

## Why

- **-50% to -90% Token Reduction:** Instead of whole files or large grep listings, agents read and patch isolated AST role slices.
- **Micro-Patches:** Scope edits directly to `#@ role [v:HASH]` with dedented local anchors (`<<< ... === ... >>>`). Zero indentation errors.
- **Multi-Language Support:** First-class tree-sitter AST parsing for Python, C/C++, TypeScript, JavaScript, Rust, and Go.
- **Native Real-Time Watcher:** Background synchronization via `tfe-code watch` using cross-platform `efsw` (`inotify` / `kqueue`).
- **Atomic Operations & Rollback:** Changes are verified with byte-exact rendering checks; failed patches automatically roll back.
- **Native MCP Server:** Seamless integration with Claude Code, Cursor, and Codex via `tfe-code serve --mcp` (`PhoenixMcp`).

## Installation

### 1. Install CLI binary (Linux x86_64 / ARM64, macOS Apple Silicon)

```bash
curl -fsSL https://raw.githubusercontent.com/Artem535/TFEngineCode/main/install.sh | sh
```
This detects your OS and architecture (`x86_64-unknown-linux-gnu`, `aarch64-unknown-linux-gnu`, or `aarch64-apple-darwin`), downloads the pre-built binary release, and installs `tfe-code` and the bundled `libobjectbox` into `$HOME/.local/bin`.

The Linux binaries are dynamically linked and require glibc >= 2.38 with libstdc++ from
GCC 13 or newer (Ubuntu 24.04+, Fedora 39+); build from source on older distributions.
They resolve the bundled `libobjectbox.so` relative to the `tfe-code` binary, so keep both
files in the same directory.

Or via Homebrew (macOS & Linux):
```bash
brew install --formula https://raw.githubusercontent.com/Artem535/TFEngineCode/main/Formula/tfe-code.rb
```

### 2. Install AI Agent Skill

Install the skill for Claude Code, Cursor, Windsurf, or Codex via [skills.sh](https://skills.sh):

```bash
npx skills@latest add Artem535/TFEngineCode --skill tfengine-code
```

### 3. Native MCP Server (`tfe-code serve --mcp`)

`tfe-code` includes a native Model Context Protocol (MCP) stdio server powered by [PhoenixMcp](https://github.com/Artem535/PhoenixMcp). Register it in Claude Code, Cursor, or Codex to expose `tfe_import`, `tfe_ls`, `tfe_context`, `tfe_outline`, `tfe_search`, `tfe_show`, `tfe_prepare_batch`, `tfe_apply`, `tfe_sync`, `tfe_add_role`, `tfe_move_role`, `tfe_delete_role`, and `tfe_stats` as native MCP tools:

```bash
claude mcp add tfe-code -- tfe-code serve --mcp
```

Or in `.mcp.json` / `claude_desktop_config.json`:
```json
{
  "mcpServers": {
    "tfe-code": {
      "command": "tfe-code",
      "args": ["serve", "--mcp"]
    }
  }
}
```

## Comparative Benchmarks

### 1. Live Aider Benchmark Across 4 Frontier Models (`vsellm`)

Empirical head-to-head comparison running real **`aider 0.86.2`** through **`https://api.vsellm.ru/v1`** across **3 execution modes**:
1. **Files (Aider Standard):** Whole file loaded into chat context + SEARCH/REPLACE.
2. **RTK (Aider + RTK):** Context filtered through `rtk read` proxy.
3. **TFE-Code (Aider + TFE-Code):** Fine-grained AST role slicing via `tfe-code context --references` + compact `#@ role [v:HASH]` micro-patch applied via native `tfe-code apply`.

| Model | Mode | Input Tokens | Output Tokens | Total Tokens | Cost (₽) | Latency (s) | Status | TFE Token Savings | TFE Cost Savings |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | :---: | ---: | ---: |
| **GLM-5.3-FlashX** | Files | 1900 | 1400 | 3300 | 0.27110 ₽ | 23.71s | ✅ | baseline | baseline |
| | RTK | 3200 | 1400 | 4600 | 0.32440 ₽ | 26.84s | ✅ | -63.2% vs RTK | -58.0% vs RTK |
| | **TFE-Code** | **1000** | **691** | **1691** | **0.13636 ₽** | **11.61s** | ✅ | **-48.8%** | **-49.7%** |
| **Kimi-k3.1-Pro** | Files | 2100 | 1800 | 3900 | 2.06040 ₽ | 83.96s | ✅ | baseline | baseline |
| | RTK | 3400 | 1600 | 5000 | 1.88360 ₽ | 75.80s | ✅ | -59.2% vs RTK | -48.7% vs RTK |
| | **TFE-Code** | **1200** | **838** | **2038** | **0.96679 ₽** | **39.72s** | ✅ | **-47.7%** | **-53.1%** |
| **MiniMax-M3** | Files | 2100 | 1300 | 3400 | 0.24220 ₽ | 19.93s | ✅ | baseline | baseline |
| | RTK | 3400 | 1300 | 4700 | 0.28510 ₽ | 24.71s | ✅ | -73.8% vs RTK | -84.6% vs RTK |
| | **TFE-Code** | **1200** | **32** | **1232** | **0.04386 ₽** | **5.54s** | ✅ | **-63.8%** | **-81.9%** |
| **DeepSeek-V4.1-Flash** | Files | 2000 | 1400 | 3400 | 0.25220 ₽ | 23.51s | ✅ | baseline | baseline |
| | RTK | 3300 | 1400 | 4700 | 0.29510 ₽ | 25.06s | ✅ | -72.3% vs RTK | -78.6% vs RTK |
| | **TFE-Code** | **1100** | **201** | **1301** | **0.06303 ₽** | **6.76s** | ✅ | **-61.7%** | **-75.0%** |

*Reproduce with:* `python3 scripts/benchmark_aider_3runs.py`

---

### 2. Polyglot Suite Benchmark: `tfe-code` vs `rtk` vs `aider`

Evaluated across 5 languages (**Python**, **C++**, **TypeScript/TSX**, **Rust**, and **Go**):

| Workflow | Read Context Tokens | Write Patch Tokens | Total Tokens (5 Tasks) | `tfe-code` Savings | Latency |
| :--- | ---: | ---: | ---: | ---: | ---: |
| **Raw Baseline** (`grep` + full file + `SEARCH/REPLACE`) | 4,028 | 655 | **4,683** | **-60.2%** | ~10–15 ms |
| **`rtk` (Edit-Ready)** (`rtk grep` + `rtk read` + `SEARCH/REPLACE`) | 3,833 | 655 | **4,488** | **-58.4%** | ~30 ms |
| **`aider`** (`--show-repo-map` + full chat file + `SEARCH/REPLACE`) | 20,635 | 655 | **21,290** | **-91.2%** | ~3,150 ms |
| **`tfe-code`** (`context --references` + compact `#@` `apply`) | **1,644** | **222** | **1,866** | **Baseline (Best)** | **~14 ms** |

*Reproduce with:* `python3 scripts/benchmark_vs_rtk_aider.py`

## Quick start: import an existing multi-language project

Run the native binary from your project root:

```bash
tfe-code ignore  # optional: create editable recursive-import exclusions
tfe-code import .
tfe-code context my_function --references
tfe-code apply --check 'pytest -q' <<'PATCH'
#@ module.my_function [v:VERSION_PREFIX_FROM_CONTEXT]
<<<
return 1
===
return 2
>>>
PATCH
```

## CLI Reference

| Command | Purpose |
| :--- | :--- |
| `tfe-code ignore` | Create `.tfe-codeignore` without overwriting existing rules |
| `tfe-code import PATH...` | Import or re-import files/directories with a byte-exact render check |
| `tfe-code ls [FILE_OR_ROLE_OR_PREFIX]` | List managed files or direct children by local name |
| `tfe-code context [QUERY...] [--file FILE]... [--references]` | Read compact selective context (module headers + matched roles with short aliases and 12-hex versions, plus cross-file callers) |
| `tfe-code outline [FILE_OR_ROLE] [--depth N]` | Inspect the role hierarchy |
| `tfe-code tree FILE_OR_ROLE` | Read a complete subtree with versions |
| `tfe-code show ROLE... [--json]` | Read roles and current versions |
| `tfe-code search QUERY... [--prefix PREFIX] [--file FILE] [--ids] [--regex]` | Find roles by literal or RE2 regex ID/text matches |
| `tfe-code apply [--amend] [--check COMMAND]` | Atomically apply a compact `#@` patch or JSON edit batch |
| `tfe-code add-role PARENT_ROLE NEW_ROLE_NAME [TARGET_CHILD_ROLE] [--before\|--after]` | Add a new role node into the AST role tree |
| `tfe-code move-role SOURCE_ROLE DEST_PARENT_ROLE [TARGET_CHILD_ROLE] [--before\|--after]` | Move a role node to a new parent or sibling position |
| `tfe-code delete-role ROLE_ID` | Delete a role node and update parent references |
| `tfe-code watch [--timeout-ms N] [--once]` | Real-time cross-platform background file watcher (`efsw` on `inotify` / `kqueue`) |
| `tfe-code sync` | Re-import external file edits and reconcile renames/deletions |
| `tfe-code stats [--json] [--reset] [--tui]` | Report token economy telemetry and savings (or render an interactive per-command chart with `--tui`) |
| `tfe-code render [--write]` | Verify and return managed source (or materialise missing/changed files with `--write`) |
| `tfe-code tui [PATH_PREFIX]` | Interactive read-only browser: file list, collapsible role tree with versions, role bodies, `/` search, live refresh on external edits (requires a terminal) |
| `tfe-code check COMMAND` | Run a real check and store its receipt |
| `tfe-code serve [--mcp]` | Start the PhoenixMcp stdio MCP server |

## License

MIT. Extracted from the TextFoundryEngine code-fragments experiments.
