---
name: tfengine-code
description: Read and edit code stored as named roles in a TFEngine Code workspace, and render source files with tfe-code. Use when the workspace has .roles/store.json or the user requests this workflow.
---

# TFEngine Code

Work from the workspace directory. Use `tfe-code` when installed, or
`./tfe-code` when the workspace provides that launcher. The user task describes the behavior to
implement; keep workflow instructions here rather than adding them to the task.

For an unfamiliar file, use `tfe-code tree FILE` to read its complete role tree
in one document. For a localized edit, use `tfe-code who PREFIX. NAME...` to read
matching roles. Fetch additional context when required; search output is not a
proof that all dependencies have been found.

Send all related changes together through `tfe-code apply` on stdin:

```text
#@ component.file [file: component.py]
{{component.method}}

#@ component.method
def method():
    return 1
```

Each role has zero outer indentation. A parent indents its child reference at
the required position. Include the file mapping when creating a new file.
For existing files send only changed roles. Keep unrelated roles unchanged.
Names defined in one function are unavailable in another unless passed or
recomputed; a role reference does not create a Python scope.

A successful apply already renders outputs and checks Python syntax; do not
repeat identical reads or checks without a reason. Run the consumer's relevant
behavioral tests, because successful compilation does not prove the task.
If apply fails, inspect the diagnostic and repair the saved roles; changes are
not automatically rolled back. Report the edits and the checks actually run.
Do not modify the tool implementation to make a task pass.
