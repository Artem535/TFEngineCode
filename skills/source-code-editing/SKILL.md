---
name: source-code-editing
description: Edit ordinary source files in a controlled comparison with the TFEngine Code role workflow. Use for the source-file arm of that comparison.
---

# Source-file editing

Use the supplied workspace, source paths, example and verification command.
If setup is already specified, start with the relevant read, without `pwd`,
`ls` or tool inspection. Resolve missing setup with one focused lookup.
Do not search for packages explicitly declared unavailable. Keep workflow and
environment guidance in this skill or its supplied runtime context; the user
task contains the requested behavior only.

For a new file or class, read the supplied example once, then write the complete
new file in one edit. For a localized change, search the supplied source scope
for the relevant names, read enough matching context, then make one focused
edit. Preserve unrelated behavior and formatting.

Aim for one sufficient read and one edit. Fetch additional definitions, callers
or imports when a concrete missing contract or scope dependency requires them.
Do not reread unchanged files just to confirm an edit. Names local to one
function are unavailable in another unless passed or recomputed.

Run the supplied behavioral verifier once after the final edit. It should
include syntax validation where applicable. Once it passes and all requirements
are resolved, return a short final report immediately. A new edit or concrete
failure justifies another relevant check. If only syntax verification is
available, state that limit; do not claim runtime correctness.

Do not change tools or verification code to make the task pass.
