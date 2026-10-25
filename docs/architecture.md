# Architecture

The Python CLI owns a workspace-local `.roles/store.json` with `blocks` and
`files` mappings. It normalizes role documents and invokes `tfe-code-render`.
The C++ adapter traverses the JSON role graph and handles indentation. For
non-literal roles it turns child references into template parameters and uses
TFEngine RenderBlock for substitution; literal leaves pass through unchanged.
It returns rendered file text as JSON. Python writes outputs and
checks syntax. Rendering itself does not write source files or invoke models.

The engine remains an external dependency; this repository does not vendor it.
The native adapter is extracted from the experimental adapter and still has
internal commands beyond the Python-facing `render-all` operation. Its command
surface is not yet a stable public API.

The prototype's task-specific navigation hint and unsound custom AST scope
checker were removed. Python compilation is the declared check, with runtime
validation delegated to consumer tests. The multiline `put` document bug was
corrected. Output paths are validated before saving new mappings.

No model provider client, credential handling, evaluation logs, private consumer
sources, or paid-run automation is included in this repository.
