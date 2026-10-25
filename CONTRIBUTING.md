# Contributing

Keep the TFEngine dependency separate from the code-editing interface. Preserve
unrelated role bodies, keep task hints out of tool implementations, and place
agent workflow guidance in the skill. Add regression coverage for parsing,
rendering, and CLI failures. Run unit tests and native integration tests before
submitting a behavior change. Document any skipped checks.

Do not commit workspace stores, binaries, credentials, provider transcripts or
consumer source code. Use the small hello example for reproducible reports.
