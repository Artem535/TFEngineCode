# Comparative Benchmark: `tfe-code` vs `rtk` vs `aider`

Reproducible benchmark comparing **`tfe-code`** against **`rtk`** (`rtk 0.50.0`), **`aider`** (`aider 0.86.2`), and **Raw File Editing** across a 5-language polyglot workspace (**Python**, **C++**, **TypeScript/TSX**, **Rust**, and **Go**).

Produce or verify this report at any time with:
```bash
python3 scripts/benchmark_vs_rtk_aider.py --output docs/benchmark-rtk-aider.md
```

## 1. Architectural Comparison

| Capability | Raw Baseline (`grep` + `cat`) | `rtk` (`0.50.0`) | `aider` (`0.86.2`) | `tfe-code` (`0.2.2`) |
| :--- | :--- | :--- | :--- | :--- |
| **Primary Model** | Unfiltered shell reads & diffs | Read-only CLI output filter proxy | RepoMap + Full-file chat + `SEARCH/REPLACE` | Native C++23 AST Role Tree (`tree-sitter` + `ObjectBox`) |
| **Read Granularity** | Whole file (`cat`) | Signature preview (`--level aggressive`) or full file | Whole file added to chat + RepoMap | Matched AST leaf role + container header + `--references` |
| **Write Granularity** | Full function/region `SEARCH/REPLACE` | None (requires external editor/tool + full read) | Indented `SEARCH/REPLACE` or unified diff | Dedented `[v:HASH]` micro-patch (`<<< ... === ... >>>`) |
| **Structural Ops** | Manual copy-paste | None | Prompt-driven whole-block rewrite | Native `add-role`, `move-role`, `delete-role` |
| **Atomic Verification** | Manual test run + `git checkout` | `rtk test` (read-only output filter) | Post-edit lint/test turn | Atomic `apply --check` with automatic rollback & journal |
| **MCP Server** | None | CLI hook | None | Native `tfe-code serve --mcp` (`PhoenixMcp`) |
| **Real-Time Watcher** | None | None | None | Native `tfe-code watch` (`efsw`) |

## 2. Aggregate Token Economy (5-Language Suite: Python, C++, TS/TSX, Rust, Go)

| Workflow | Read Context Tokens | Write Patch Tokens | Total Tokens (5 Tasks) | `tfe-code` Token Savings |
| :--- | ---: | ---: | ---: | ---: |
| **Raw Baseline** (`grep` + full file + `SEARCH/REPLACE`) | 4028 | 655 | **4683** | **-60.2%** |
| **`rtk` (Edit-Ready)** (`rtk grep` + `rtk read` + `SEARCH/REPLACE`) | 3833 | 655 | **4488** | **-58.4%** |
| **`rtk` (Aggressive Preview)** (`rtk grep` + `rtk read -l aggressive` + `SEARCH/REPLACE`) | 1262 | 655 | **1917** | **-2.7%** |
| **`aider`** (`--show-repo-map` + full chat file + `SEARCH/REPLACE`) | 20635 | 655 | **21290** | **-91.2%** |
| **`tfe-code`** (`context --references` + compact `#@` `apply`) | **1644** | **222** | **1866** | **Baseline (Best)** |

## 3. Per-Language Breakdown (Read + Write Tokens & Tool Latency)

| Language | Target Symbol | Raw Tokens | `rtk` (Edit-Ready) | `rtk` (Aggressive) | `aider` Tokens | `tfe-code` Tokens | Savings vs `aider` | Savings vs `rtk` | `tfe-code` Latency (`context` + `apply`) |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **Python** | `create_order` | 1497 | 1452 | 650 | 4813 | **401** (369R + 32W) | **-91.7%** | **-72.4%** | **14.00 ms** (6.18 + 7.82) |
| **C++** | `ExecuteJob` | 764 | 724 | 142 | 4084 | **311** (260R + 51W) | **-92.4%** | **-57.0%** | **14.26 ms** (6.19 + 8.07) |
| **TypeScript/TSX** | `renderStatusBadge` | 848 | 812 | 301 | 4172 | **474** (425R + 49W) | **-88.6%** | **-41.6%** | **15.08 ms** (6.10 + 8.99) |
| **Rust** | `build_entry` | 971 | 936 | 467 | 4297 | **429** (386R + 43W) | **-90.0%** | **-54.2%** | **13.77 ms** (5.93 + 7.85) |
| **Go** | `FormatEndpoint` | 603 | 564 | 357 | 3924 | **251** (204R + 47W) | **-93.6%** | **-55.5%** | **14.21 ms** (5.91 + 8.30) |

## 4. Execution Speed & Latency (`tfe-code` vs `rtk` vs `aider`)

| Operation | Tool | Measured Wall-Clock Latency | Notes |
| :--- | :--- | ---: | :--- |
| **Workspace Import (6 files, 5 languages)** | `tfe-code import src` (JSON) | **10.09 ms** | Full `tree-sitter` parse + lossless render verification |
| **Workspace Import (ObjectBox mode)** | `TFE_CODE_INDEX=objectbox tfe-code import src` | **13.28 ms** | Embedded ObjectBox ACID role store |
| **Selective Context + Callers** | `tfe-code context <sym> --references` | **6.18 ms** | Role slice + cross-file caller discovery |
| **Selective Context (ObjectBox mode)** | `TFE_CODE_INDEX=objectbox tfe-code context` | **7.27 ms** | Direct ObjectBox read + render verification |
| **Atomic Patch Apply** | `tfe-code apply` | **7.82 ms** | Version check + journal + lossless file render |
| **Grep + Aggressive Read** | `rtk grep` + `rtk read -l aggressive` | **28.76 ms** | Rust CLI proxy (read-only; no write engine) |
| **RepoMap Generation (Cold)** | `aider --show-repo-map` | **3224.86 ms** | Python startup + tree-sitter tag scan |
| **RepoMap Generation (Warm Cache)** | `aider --show-repo-map` | **3156.42 ms** | Cached SQLite tags + PageRank formatting |

## 5. Built-in `tfe-code stats` Telemetry Verification

After running the 5 `context` + 5 `apply` operations in the benchmark workspace, `tfe-code stats --json` reported:

```json
{
  "total_calls": 10,
  "baseline_bytes": 93825,
  "emitted_bytes": 7265,
  "saved_bytes": 86560,
  "baseline_tokens": 23460,
  "emitted_tokens": 1821,
  "saved_tokens": 21639,
  "savings_percent": 92.25686117772449,
  "by_command": {
    "apply": {
      "calls": 5,
      "baseline_bytes": 15322,
      "emitted_bytes": 705,
      "saved_bytes": 14617,
      "baseline_tokens": 3833,
      "emitted_tokens": 179,
      "saved_tokens": 3654
    },
    "context": {
      "calls": 5,
      "baseline_bytes": 78503,
      "emitted_bytes": 6560,
      "saved_bytes": 71943,
      "baseline_tokens": 19627,
      "emitted_tokens": 1642,
      "saved_tokens": 17985
    }
  }
}
```

### Why `tfe-code` Achieves Higher Token Savings than Both `rtk` and `aider`

1. **True AST Role Slicing (`context` vs `rtk read` / `aider` full-file chat):**
   - `aider` sends the global RepoMap **plus** the entire source file whenever a file is added to the chat for editing.
   - `rtk read --level aggressive` strips function bodies for previewing, which means an agent **cannot** construct a valid edit patch from the aggressive preview alone and must still run `rtk read <file>` on the full file (or line range) before editing.
   - `tfe-code context <symbol> --references` returns **only** the target role body (dedented), its enclosing class/file header (with `# omitted N unchanged roles`), and 1-line cross-project callers/references.
2. **Compact Version-Guarded Micro-Patches (`#@ role [v:HASH]` vs `SEARCH/REPLACE`):**
   - Standard `SEARCH/REPLACE` blocks in `aider` require repeating enough surrounding indented lines to uniquely match within the whole file.
   - `tfe-code apply` scopes the replacement directly to the named role ID (`#@ OrderProcessor.create_order [v:...]`), so a 1-line change needs only a 1-line `old` anchor, cutting write tokens by **55%–70%** while guaranteeing zero accidental matches in sibling functions.
