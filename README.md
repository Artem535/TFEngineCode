# TFEngine Code

Edit code as a tree of named roles, then render ordinary source files with
[TextFoundryEngine](https://github.com/Artem535/TextFoundryEngine).

TFEngine Code gives coding agents a compact interface for reading the relevant
parts of a program and changing several roles in one operation. A role can be a
whole file, a method, or a smaller unit of code. Parent roles reference children
with `{{role.id}}`; indentation belongs to the parent.

**Status: experimental.** The native `tfe-code` CLI imports existing Python and C++
files into file/class/function roles and edits them through the real C++ TF
renderer. The legacy Python CLI supports explicit role templates. Neither mode
is an Engine/ObjectBox publication or version-history implementation.

## Why

- Read a complete role tree as one document.
- Find roles containing specific names without reading every generated file.
- Apply multiple edits in a single document and render the outputs together.
- Keep agent workflow instructions in a reusable skill, separate from the task.
- Generate normal source files that existing build tools can consume.

## Installation

### 1. Install CLI binary (Linux x86_64)

```bash
curl -fsSL https://raw.githubusercontent.com/Artem535/TFEngineCode/main/install.sh | sh
```
This downloads the pre-built portable binary release and installs `tfe-code` into `$HOME/.local/bin`.

### 2. Install AI Agent Skill

Install the skill for Claude Code, Cursor, Windsurf, or Codex via [skills.sh](https://skills.sh):

```bash
npx skills@latest add Artem535/TFEngineCode --skill tfengine-code
```

## Requirements (for building from source)

Python 3.11+, a C++23 compiler, CMake 3.20+, Ninja, OpenMP, tree-sitter, and the
TFEngine core dependencies (ctre, fmt, reflect-cpp, spdlog). ObjectBox is not
required. CMake fetches the Python and C++ grammars at pinned commits. The native
executables are built separately from the legacy Python package.

## Build and install from source

Clone TFEngine alongside this repository. The initially tested upstream revision
is `43d0dca2be2cb60164233ca113da77e695123ff6`.

```bash
git clone https://github.com/Artem535/TextFoundryEngine.git ../TextFoundryEngine
git -C ../TextFoundryEngine checkout 43d0dca2be2cb60164233ca113da77e695123ff6
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -e .
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DTEXTFOUNDRY_SOURCE_DIR="$PWD/../TextFoundryEngine" \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --parallel 2
export TFE_CODE_RENDERER="$PWD/build/tfe-code-render"
```

If dependencies are already installed, use `CMAKE_PREFIX_PATH` instead of the
vcpkg toolchain. Alternatively install the native executable into a directory
on `PATH`: `cmake --install build --prefix "$HOME/.local"`.

## Quick start: import an existing Python or C++ project

Run the native binary from the consumer root (replace the example path with
this checkout):

```bash
TFE=/absolute/path/to/TFEngineCode/build/tfe-code
"$TFE" ignore  # optional: create editable recursive-import exclusions
"$TFE" import .
"$TFE" context my_function
"$TFE" apply --check 'python3 -m unittest discover -s tests -v' <<'PATCH'
#@ module.my_function [v:VERSION_PREFIX_FROM_CONTEXT]
<<<
return 1
===
return 2
>>>
PATCH
"$TFE" sync
```

Imported source files stay authoritative. The derived index and consistency
hashes live in `.tfe-code/`. Recursive import excludes common dependency/build
directories by default. `tfe-code ignore` creates `.tfe-codeignore` with those
rules; custom rules replace the defaults, while `.tfe-code` always stays excluded.
The supported subset includes comments, `*`, `**`, relative paths, leading `/`,
and trailing `/`. Negation (`!`) is rejected: this is not full `.gitignore` syntax.
Explicit imports bypass the file, and the consumer `.gitignore` is never
interpreted or modified. After editing, deleting, or exactly renaming a managed file
outside the CLI, run `sync`; it reports deletions and both sides of content-preserving
renames. Unrelated new files still require explicit `import`, edited renames are not
inferred, and ambiguous duplicate-content matches require manual resolution. Concurrent
source/index changes are rejected rather than merged.

`outline` includes `source START-END`, plain `show` adds `[lines: START-END]`,
and `show --json` exposes `source_start_line`/`source_end_line`. These 1-based,
inclusive source ranges refresh on import, apply, and sync. Zero means unavailable
for a legacy snapshot, empty role, or role whose ID no longer matches the parsed
source. `tree` and `search` output is unchanged. Ranges are navigation metadata,
not role identity, versions, or publication history.

## Quick start: explicit templates (legacy Python CLI)

From this repository root after `pip install -e .`:

```bash
mkdir /tmp/tfe-code-example
cd /tmp/tfe-code-example
tfe-code init
tfe-code apply < /absolute/path/to/TFEngineCode/examples/hello/roles.txt
python3 out/hello.py
tfe-code tree hello.py
tfe-code who hello. greet
```

The program prints `Hello, world!`. Replace the example path with your checkout.
A role document looks like this:

```text
#@ hello.file [file: hello.py]
{{hello.greet}}

#@ hello.greet
def greet(name):
    return f"Hello, {name}!"
```

Change only `hello.greet` by sending that section to `tfe-code apply`. Unchanged
roles stay in the store. Generated files are written below `out/`.

## CLI

Native imported workspace:

| Command | Purpose |
| --- | --- |
| `tfe-code ignore` | Create `.tfe-codeignore` without overwriting existing rules |
| `tfe-code import PATH...` | Import or re-import files/directories with a byte-exact render check |
| `tfe-code ls [FILE_OR_ROLE_OR_PREFIX]` | List managed files or direct children by local name |
| `tfe-code context [QUERY...] [--file FILE]... [--callers]` | Read compact selective context (module headers + matched roles with short aliases and 12-hex versions, optionally with project callers) |
| `tfe-code outline [FILE_OR_ROLE] [--depth N]` | Inspect the role hierarchy |
| `tfe-code tree FILE_OR_ROLE` | Read a complete subtree with versions |
| `tfe-code show ROLE... [--json]` | Read roles and current versions |
| `tfe-code search QUERY... [--prefix PREFIX] [--file FILE] [--ids] [--regex]` | Find roles by literal or RE2 regex ID/text matches, optionally restricted to a file or one-line IDs |
| `tfe-code prepare-batch [QUERY...] [--file FILE]...` | Emit a pre-populated JSON edit batch template |
| `tfe-code role FILE NAME` | Return one role by exact name or dotted ID suffix; report ambiguity |
| `tfe-code who [PREFIX.] NAME...` | Search with legacy-compatible prefix syntax |
| `tfe-code apply [--amend] [--check COMMAND]` | Atomically apply a compact `#@` patch or JSON edit batch (or amend the last failed batch) and optionally run real checks |
| `tfe-code render [--write]` | Verify and return managed source (or materialise missing/changed files with `--write`) |
| `tfe-code sync` | Re-import edits, reconcile deletions/exact renames, and reject conflicts |
| `tfe-code check COMMAND` | Run a real check and store its receipt |

Legacy explicit-template CLI:

| Command | Purpose |
| --- | --- |
| `tfe-code init` | Create an empty workspace |
| `tfe-code tree FILE_OR_ROLE` | Read a complete subtree |
| `tfe-code show ROLE` | Read one role |
| `tfe-code outline [FILE_OR_ROLE]` | Inspect the tree structure |
| `tfe-code who [PREFIX.] NAME...` | Find roles mentioning names |
| `tfe-code apply` | Read a multi-role document from stdin |
| `tfe-code put ROLE` | Read a role body or role document from stdin |
| `tfe-code replace PREFIX OLD NEW` | Replace text in matching roles |
| `tfe-code check` | Render and check Python syntax |

Run commands from the workspace directory. Commit `.roles/store.json` in a
consumer project if it is the authoritative source; the repository's ignore
rule only keeps development workspaces out of this tool's own source tree.

## Agent skill

Load [skills/tfengine-code/SKILL.md](skills/tfengine-code/SKILL.md) through your
agent's skill mechanism, or install it via:
```bash
npx skills@latest add Artem535/TFEngineCode --skill tfengine-code
```
For a balanced comparison, the [source-file skill](skills/source-code-editing/SKILL.md)
uses the same batching and verification rules.

## Verification

```bash
ctest --test-dir build --output-on-failure
TFE_CODE_RENDERER="$PWD/build/tfe-code-render" \
TFE_CODE_NATIVE="$PWD/build/tfe-code" \
  python3 -m unittest discover -s tests -v
```

The [offline route rehearsal](docs/evaluation.md#short-route-revision) checks
creation and modification through both roles and ordinary files without model
requests.

Set `TFE_CODE_RENDERER` to the built executable to enable native integration
tests. Without it those tests are explicitly skipped. `check` checks Python
syntax; it does not prove runtime correctness. Run the consumer's real tests.

## Evidence and limitations

An earlier synthetic prototype completed six tasks in three two-task repeats
for 0.160832 RUB versus 0.578837 RUB for a historical file-editing baseline.
Those measurements combined a new interface with optimized instructions; one
navigation command also supplied a task-specific hint. That hint has been
removed from this extraction. The earlier numbers are **not a benchmark of
this release**, an isolated storage advantage, or a general cost guarantee.
A [single follow-up with a separate skill](docs/skill-trial-2026-10-25.md) passed
both tasks for 0.145866 RUB; it did not reproduce the earlier v5 cost.
A later [short-route pilot](docs/paired-short-route-2026-10-25.md) stopped after
an incomplete turn with a passing artifact; no comparable pair was obtained.
See [docs/evaluation.md](docs/evaluation.md) for the evaluation contract.

Roles use mutable JSON snapshots; version fields are content hashes, not
publication history. Native apply stages a complete candidate and rolls managed
source/index/receipts back after handled write or check failures. A write-ahead
journal restores the pre-batch state on the next valid native command after process
interruption. Recovery does not cover OS/power-loss durability, external side effects
of check commands, automatic conflict merges, or runtime sandboxing; run only trusted
check commands. The legacy Python CLI has no rollback: failed checks can leave saved
edits and generated files for repair. C++ rendering follows TFEngine semantics.

## Project layout

```text
src/tfengine_code/     Python CLI
native/               C++ adapter using the TFEngine renderer
skills/tfengine-code/ Agent workflow
examples/hello/       Small standalone example
tests/                CLI and native integration tests
docs/                 Architecture and evaluation notes
```

## License

MIT. Extracted from the TextFoundryEngine code-fragments experiments.
