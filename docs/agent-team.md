# Agent team test targets

Several agents can work on this repository at the same time. Linking the full
native test set for every short command is too slow for that workflow, so the
C++ suites are split into per-module targets and the build can be narrowed to
the module an agent is touching. Combining or rebuilding the complete set stays
possible; it is just no longer the only option.

The split changes build and registration only. Every suite still links
`tfe_code_core` and still talks to the real renderer, so a narrowed build gives
the same assertions as the full one, restricted to fewer modules.

## Selecting test modules

`TFE_CODE_ADD_TESTS` is a `CACHE STRING` holding a semicolon-separated list of
module names. Empty (the default) builds and registers every suite.

```bash
# Only the two modules an agent is working on.
# Quote the value and separate modules with semicolons: a comma is not a CMake
# list separator, so `-DTFE_CODE_ADD_TESTS=a,b` silently selects nothing.
rtk cmake -S . -B build '-DTFE_CODE_ADD_TESTS=read_models;apply_transaction'

# Back to everything.
rtk cmake -S . -B build -DTFE_CODE_ADD_TESTS=
```

CMake reports the configured selection on every configure run:

```text
-- tfe-code test modules: apply_transaction;autotests;crash_recovery;import_sync;python;read_models;workspace
-- tfe-code registered tests: test_apply_transaction_core;test_crash_recovery_journal;test_import_sync_core;test_read_models_core;python_import_tests
```

The first line always lists every module the checkout contains, independent of
the selection; the second line lists what will actually be built and registered
for this configuration. A full (empty) selection adds
`test_autotests_cmake_wiring_manifest` to that list.

Module names come from the file names. Every `tests/native/test_<module>_*.cc`
is compiled into its own executable `test_<module>_*`, and the module name is
the part between `test_` and the last underscore group. `test_read_models_core.cc`
therefore belongs to module `read_models`. The glob uses `CONFIGURE_DEPENDS`, so
adding a file is picked up by the next build without a manual reconfigure. A file
that does not match the pattern is skipped with a CMake warning.

The regex is greedy, so everything after the *last* underscore is the discarded
suffix. Name files with a single trailing group: `test_crash_matrix.cc` gives
module `crash_matrix`, while `test_crash_matrix_long_tail.cc` gives
`crash_matrix_long` and will not be selected by `-DTFE_CODE_ADD_TESTS=crash_matrix`.

## Modules

The table reflects the current tree and the reported module list.

| Module | Source file(s) | Coverage | ctest target |
| --- | --- | --- | --- |
| `apply_transaction` | `tests/native/test_apply_transaction_core.cc` | Multi-role atomic apply, stale expected-version rejection with no project write, check-command rollback of source and project, empty `old` rejection, receipt preserved by a clean sync, external source edit detected as stale, sync afterwards importing the new role | `test_apply_transaction_core` |
| `autotests` | `tests/native/test_autotests_cmake_wiring_manifest.cc` | CMake wiring self test: `tfe_code_core` and the public headers are reachable, a fresh `Workspace` rejects `LoadProject`, the generated manifest has one `CMSUITE:` row per module, and the compiled-in registered-target list matches | `test_autotests_cmake_wiring_manifest` |
| `crash_recovery` | `tests/native/test_crash_recovery_journal.cc` | Interrupted commit at three write boundaries leaves `transaction.json`; the next command restores the pre-batch snapshot and removes the journal, then the batch can be retried; malformed journals are rejected without touching managed files or recovery evidence | `test_crash_recovery_journal` |
| `import_sync` | `tests/native/test_import_sync_core.cc` | Byte-exact import, recursive discovery with sorted/deduplicated expansion and no directory-symlink traversal, ignore-file defaults and custom rules, path traversal rejection, literal `{{role}}` text preserved, deletion and exact-rename reconciliation, ambiguous rename rejection, search filters | `test_import_sync_core` |
| `python` | `tests/native/test_python_import.cc` | Legacy parser suite: SHA-256 vector, tree-sitter import of an annotated CRLF/Unicode file into file/function/nested roles, source ranges, render round trip | `python_import_tests` |
| `read_models` | `tests/native/test_read_models_core.cc` | Outline/tree/show/show-roles/search formatting, source ranges exposed in outline and `show` but not in tree/search, ranges refreshed after apply, ranges cleared for a renamed (unmatched) role, search result shape | `test_read_models_core` |
| `workspace` | `tests/native/test_workspace.cc` | Legacy end-to-end workspace suite (import, load, render, apply, check, sync in one script) | `workspace_tests` |

`workspace_tests` is opt-in, not part of a full build. The legacy Python and
workspace suites are excluded from the glob because they keep their historical
target names. `python_import_tests` is registered whenever `python` is selected,
which includes the empty (all) selection. `workspace_tests` is different: the
module `workspace` appears in the reported module list, but the target is only
added when `workspace` is requested explicitly. An empty `TFE_CODE_ADD_TESTS`
does **not** build it, so a default configure registers six targets while the
reported module list has seven entries. The read-model, import/sync, apply and
crash-recovery suites above are the maintained coverage; `workspace_tests`
duplicates part of it and exists as a fallback, so select it explicitly when you
need it:

```bash
rtk cmake -S . -B build -DTFE_CODE_ADD_TESTS=workspace
```

## Split of the former `test_workspace.cc`

The single legacy suite was split into four maintained suites. Coverage was
moved, not dropped, and `test_workspace.cc` is kept only as the opt-in fallback
described above.

| Former `test_workspace.cc` area | New module | ctest target |
| --- | --- | --- |
| Byte-exact import, recursive discovery, ignore-file defaults and custom rules, path traversal rejection, literal `{{role}}` text | `import_sync` | `test_import_sync_core` |
| Deletion, exact-rename, one-to-many and many-to-one ambiguous renames, search filters | `import_sync` | `test_import_sync_core` |
| Outline/tree/show/`show_roles`/search formatting | `read_models` | `test_read_models_core` |
| Source ranges on import, after apply, and after a rename | `read_models` | `test_read_models_core` |
| Multi-role apply, stale version, rollback on failed check, empty `old` | `apply_transaction` | `test_apply_transaction_core` |
| Receipt bookkeeping and staleness after an external edit | `apply_transaction` | `test_apply_transaction_core` |
| `TFE_CODE_TEST_CRASH_AFTER` interruption, journal rollback, retry, malformed journals | `crash_recovery` | `test_crash_recovery_journal` |

## Running the suites

Everything, from the repository root:

```bash
rtk cmake --build build --parallel && rtk ctest --test-dir build --output-on-failure
```

One module only — build just its target, then run just its test:

```bash
rtk cmake --build build --target test_read_models_core
rtk ctest --test-dir build -R '^test_read_models_core$' --output-on-failure
```

The generated manifest `build/tests/native/tfe_code_test_manifest.txt` carries
the same command for every suite as a `CMSUITE:` row, so agents do not have to
guess target names:

```text
CMSUITE:read_models:tests/native/test_read_models_core.cc:cmake --build build --target test_read_models_core && ctest --test-dir build -R ^test_read_models_core$ --output-on-failure
```

Selection narrows compilation and registration only. A module that was not
selected is not reported as skipped — it is simply absent from the ctest list.
Selecting a name that does not exist is not an error either: the result is fewer
tests, so read the `tfe-code registered tests:` line after every reconfigure.

## Core-only builds and the Python test suite

A narrowed native configuration still builds `tfe_code_core`, `tfe-code` and
`tfe-code-render`: the library and both executables are unconditional. What a
narrowed configuration saves is the compile and link of the test executables,
which is the expensive part of short repeated commands.

The native suites have no dependency on the Python package. The Python CLI tests
in `tests/test_cli.py` are a separate suite and need environment variables:

```bash
TFE_CODE_RENDERER="$PWD/build/tfe-code-render" \
TFE_CODE_NATIVE="$PWD/build/tfe-code" \
  python3 -m unittest discover -s tests -v
```

Without `TFE_CODE_NATIVE` and `TFE_CODE_RENDERER` the run reports 21 skips out of
27 tests and proves nothing about the native binary; with both set, all 27 run.
`TFE_CODE_RENDERER` alone enables the renderer tests, and `TFE_CODE_NATIVE`
alone leaves the whole native-CLI class skipped. Neither variable affects
`ctest`, which is why `ctest` and the unittest run are separate verification
steps.

## Limits

- Module names are derived from file names. Renaming a test file renames its
  module and invalidates any `-DTFE_CODE_ADD_TESTS=...` value that used the old
  name.
- The empty selection means "every module of a full build", and that set excludes
  `workspace`. A full build is therefore not a superset of every selectable
  module.
- `autotests` is skipped in a subset build, so the manifest and
  compile-definition wiring are only verified when the selection is empty or
  includes `autotests`.
- The split is a build-time split of test executables. It does not subdivide a
  single suite: a module with one big `main()` still links and runs as one
  target.
- A subset proves exactly the assertions of the selected modules. Treat a green
  subset as evidence about those modules only and run the full set before
  handing work off.