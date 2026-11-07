---
name: tfengine-code-legacy-templates
description: Explicit-template (.roles/store.json) branch of the tfengine-code skill. Read only when the workspace has no .tfe-code directory.
---

# TFEngine Code — explicit templates (`.roles`)

Use this only when the workspace contains `.roles/store.json`. An imported
workspace with `.tfe-code/` uses the JSON `apply` described in `SKILL.md` and
must ignore everything here: the two `apply` payloads are different documents.

## Establish the workspace once

Use the supplied workspace, launcher, example file and verification command.
An installed launcher is `tfe-code`; a supplied local launcher is `./tfe-code`.
If these are already specified, start with the relevant read, without `pwd`,
`ls`, tool-source inspection or store dumps. Missing setup information warrants
one focused lookup. Do not search for packages explicitly declared unavailable.
Workflow and environment guidance belongs in this skill or its supplied runtime
context; the user task contains the requested behavior only.

## Choose the shortest sufficient route

**New file or class:** read the supplied example with `tfe-code tree FILE`, then
send all new roles and the file mapping in one `tfe-code apply` document.
Do not search for a target role that the task asks you to create.

**Localized change:** read matching roles with `tfe-code who PREFIX. NAME...`,
then send only changed roles together in one `tfe-code apply` document.

These routes normally need one read and one batch edit. They are not a fixed
call limit: fetch an additional role or subtree when a concrete missing
contract, import, caller, definition or scope dependency affects the edit.
Do not chain outline, tree, show and full generated-file reads for the same
already-visible context. Do not inspect `.roles/store.json` or tool sources as
a substitute for the public interface.

## Apply a complete edit

`apply` reads **stdin**, not a positional document filename. Use
`tfe-code apply < edits.roles` or a quoted heredoc:

```bash
tfe-code apply <<'ROLES'
#@ component.file [file: component.py]
{{component.method}}

#@ component.method
def method():
    return 1
ROLES
```

Each role has zero outer indentation; the parent indents its child reference.
File mappings are relative to the generated `out/` directory:
`[file: component.py]` creates `out/component.py`. Do not put `out/` in the
mapping itself. Include a mapping for each new file. Preserve unrelated roles. A
reference does not create a Python scope: inspect the actual definition when a
value is needed in another method. Never assume locals cross function boundaries.

## Verify and stop

A successful apply already renders and checks Python syntax. Do not repeat
`check`, compilation, `tree` or generated-file reads solely for confirmation.
Run the supplied behavioral verifier once after the final edit. When it passes
and no requirement remains unresolved, return a short final report immediately.
A new edit or a concrete failure justifies another relevant check.

If no behavioral verifier is available, state that only syntax was checked;
do not invent an unavailable integration check or claim runtime correctness.
If apply fails, repair the saved roles from the diagnostic: there is no automatic
rollback. Do not change tools or verification code to make the task pass.