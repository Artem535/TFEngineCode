# TFEngine Code

Edit code as a tree of named roles, then render ordinary source files with
[TextFoundryEngine](https://github.com/Artem535/TextFoundryEngine).

TFEngine Code gives coding agents a compact interface for reading the relevant
parts of a program and changing several roles in one operation. A role can be a
whole file, a method, or a smaller unit of code. Parent roles reference children
with `{{role.id}}`; indentation belongs to the parent.

**Status: experimental extraction.** The CLI uses the real C++ TF renderer.
Role definitions and file mappings live in a local JSON store. This is not an
Engine/ObjectBox publication or version-history implementation.

## Why

- Read a complete role tree as one document.
- Find roles containing specific names without reading every generated file.
- Apply multiple edits in a single document and render the outputs together.
- Keep agent workflow instructions in a reusable skill, separate from the task.
- Generate normal source files that existing build tools can consume.

## Requirements

Python 3.11+, a C++23 compiler, CMake 3.20+, Ninja, OpenMP, and the TFEngine core
dependencies (ctre, fmt, reflect-cpp, spdlog). ObjectBox is not required.
The native renderer must be built separately from the Python package.

## Build and install

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

## Quick start

From this repository root after installation:

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
agent's skill mechanism, then provide the task as a separate instruction. Copy
the skill folder into the location used by your agent if necessary. This project
does not install skills or change agent configuration automatically.

## Verification

```bash
python3 -m unittest discover -s tests -v
```

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
See [docs/evaluation.md](docs/evaluation.md) for the evaluation contract.

Roles currently use a mutable JSON store; version fields are not publication
history. Failed checks can leave saved edits and generated files for repair.
There is no transaction across the store and generated files, no automatic
source-to-role import, no runtime sandbox, and no concurrency control. Use
trusted role documents in an isolated workspace. File outputs are constrained
to `out/`; C++ rendering follows TFEngine semantics.

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
