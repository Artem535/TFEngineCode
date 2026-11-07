# Task: make Langfuse span names single-line

`Service._build_span_name` in `src/tabletopmagnat/services/llm_service.py` builds
the name of the tracing span for a request. It currently returns the first
message verbatim, so a multi-line question produces a span name that contains
newlines and runs of spaces. Langfuse renders that as a broken, hard-to-scan
trace list, and a whitespace-only question produces a visually empty name.

Change only `_build_span_name`. Keep every other byte and behaviour.

Required behaviour:

1. Collapse every run of whitespace in the first message — spaces, tabs,
   newlines, carriage returns — into a single space, then strip leading and
   trailing whitespace. Collapsing happens **before** measuring for truncation,
   so the 50-character limit applies to the collapsed text.
2. If the dialog has no messages, or the collapsed text is empty, return
   `Request: (empty)`.
3. Otherwise return `Request: ` followed by the collapsed text truncated to 50
   characters. Append `...` only when text was actually dropped; a message whose
   collapsed form is exactly 50 characters must not get the ellipsis.

The method stays a `@staticmethod` and its signature does not change. Verify with
`bash check.sh` from the workspace root.