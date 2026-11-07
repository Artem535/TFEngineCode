# Handoff: TFEngine Code economics and multi-file pilot — 2026-11-07

Read this after `AGENTS.md`. This handoff extends `docs/handoff-2026-11-07.md`
with later work in the same chat. The user wants TFEngine Code to reduce agent
tokens, money and/or elapsed time, with no loss of quality. The proposed gate
is at least 10% savings in cost or full time without a quality regression;
no experiment here establishes that gate as passed.

## Workspace and safety

- Repository: `/home/a.durynin/Projects/C++/TFEngineCode`, branch `main`.
  `HEAD` does not represent most of the implementation: many `native/` files,
  tests, harnesses, reports and scripts are untracked, and tracked files are
  modified. Check `git status` before working; preserve all existing work.
- Engine: `/home/a.durynin/Projects/C++/TextFoundryEngine`, branch
  `feat/structural-fragments`, is also modified and contains substantial
  untracked work. The consumer project TabletopMagnat has its own unrelated
  dirty worktree. No consumer source was intentionally edited during the pilot;
  experiments used copies.
- Do not commit, push, switch branches, clean, reset, stash or remove untracked
  files without the user's explicit permission; see repository `AGENTS.md`.
- Raw experiment events, reports, prepared workspaces and reference copies are
  under `/tmp` and can disappear. They were present when this was written;
  paths are in the linked reports. Copy only the minimal needed evidence to a
  durable location after checking for private source, prompts and secrets.
- Shared Memory MCP tools requested by `AGENTS.md` were unavailable in this
  chat. Retry discovery if available in the next environment.

## Implemented since the earlier handoff

1. `tfe-code search QUERY --file FILE` restricts literal ID/text search to roles
   reachable from a file; its output includes role text and current versions.
2. `tfe-code role FILE NAME` resolves a unique exact full ID or dotted suffix in
   the file, returns its text/ID/version, and reports missing or ambiguous names.
   Literal `search` remains for discovery and callers.
3. `skills/tfengine-code/SKILL.md` recommends a focused lookup, reuses observed
   versions, batches edits and differentiates JSON and ObjectBox workflows.
   It has not been split by mode; the full skill still includes legacy templates.
4. The paired trial driver now freezes full skill text and prompts for **both**
   arms and records `agent_wall_seconds`. Previous tabletop trials before the
   full-skill series used a hand-written db prompt, not this skill. The driver
   currently uses the full skills, the fixed model/provider, and the selected
   Tabletop project suite; inspect it before reproducing older experiments.

Tests run after the CLI changes: Release `build-obj` build, CTest 11/11,
Python `unittest discover` 29/29 with both native binary environment variables,
the exact-role test in ObjectBox mode, and `git diff --check`. These are prior
observations, not a claim about the next checkout; rerun relevant checks after
further edits. Exact-role local response sizes were 641 vs 1,471 bytes for one
task and 403 vs 4,199 bytes for another; token counts were not measured. See
`docs/exact-role-local-measurement-2026-11-07.md`.

## Economic evidence

- Full-skill two-task experiment: 3 paired attempts per task, all passed the
  selected gate. DB cost 4.48x and 2.35x source files, respectively, with no
  pair won. First-request db prompts had 1,207 more input tokens, attributable
  to skill **and** environment differences. See
  `docs/paired-full-skills-2026-11-07.md`.
- Multi-file event-level pilot: 2 pairs on a real Python project; the task spans
  event model, bus, SQLite migration and web DTO. Files passed 2/2, db stopped
  at its per-attempt budget 0/2 and failed the independent verifier. Files cost
  2.499927 RUB combined, db 10.431384 RUB. Agent wall times were 25.8/25.2 s
  vs 31.0/42.8 s. The unmodified project failed the verifier; a reference in an
  isolated copy passed it and the selected suite. The suite excluded two tests
  and one file with pre-existing failures. Task and verifier:
  `harness/event_level/task.md` and `harness/event_level/verify.py`. See
  `docs/paired-event-level-pilot-2026-11-07.md`.
- One db failure had an undefined `EventLevel`; the other did not propagate
  levels through subscribers, persistence, DTO and emit. Both were stopped by
  the budget; a larger budget might allow completion at additional cost. This is
  a two-pair, one-model pilot, not proof about all tasks or models.
- All ruble amounts are estimates at 33 RUB/M input and 133 RUB/M output, not
  provider charges. Cached tokens were reported but not separately discounted.
  The 5 RUB per-attempt threshold is checked after a response; attempts exceeded
  it (up to 5.52 RUB). The 20 RUB series threshold is checked between attempts,
  so neither is a hard pre-request spending cap.

## Feasibility diagnosis

`docs/event-level-cli-diagnosis-2026-11-07.md` records the two agent traces and a
local public-CLI check. In a pristine db workspace, a known-good reference was
applied as **one batch of 10 roles in 4 files**; `apply --check 'bash check.sh'`
passed the independent verifier and selected suite, and all four output files
matched the reference byte-for-byte. This used a known solution and bulk reads
of both indexed versions: it proves feasibility for this solution, not agent
quality or savings.

Observed difficulties: duplicate role in a batch; stale versions; a wrong role
ID; reads before materialisation; intermediate code with missing imports or
incomplete data propagation; check failure masked by a shell pipeline's exit
status. The traces do not establish a single root cause. The first agent also
read internal index files despite its instruction.

## Important open correctness risks

- The earlier handoff says ObjectBox is authoritative and `LoadForImport`
  prefers it. Current `native/workspace.cc` loads `project.json` first when
  present; on mismatch the mirror may be used to repopulate the store. Verify
  the intended authority contract before relying on that documentation.
- By code inspection, a db-only workspace may retain a published db change if
  an `apply --check` fails after publication and no previous mirror existed.
  This was **not reproduced**. Add a targeted process-level test before
  asserting a bug or claiming transactional rollback; preserve user data.
- ObjectBox's per-block publication versions do not by themselves prove
  project-wide history/rollback or history across deletion/recovery.

**Resolution (added after the above was written).** The first two items are
fixed. `Commit` now publishes to ObjectBox only after a `--check` command passes,
and `tests/native/test_objectbox_check_rollback_core.cc` reproduces the old bug
and guards the fix. The authority wording was corrected: `LoadForImport` reads
`project.json` first, falls back to the store only when the mirror is absent, and
on a hash mismatch the mirror wins and is republished into the store — so the
database is the working index, not the final arbiter. See §12 of
`docs/handoff-2026-11-07.md`. The third item stands as a real limit.

## Next work agreed in chat

The user accepted investigating how to simplify multi-role batch edits and make
verification failures visible. Start by inspecting the actual CLI/agent traces,
then construct a narrow local reproduction and compare options such as fetching
several roles and versions together, handling one edit per role, and propagating
failed check exit codes. Do not assume the local 10-role oracle is representative
of an agent finding the solution. Keep existing optimistic version checks,
dedented role text and validation semantics intact.

Before a further paid experiment: fix or explicitly account for pipeline exit
status and budget overshoot, measure full elapsed time and cache-aware cost, freeze
inputs/skills, validate the verifier against both baseline and reference, and
set a concrete budget. Compare both arms with identical tasks and quality gates;
isolate prompt, model, storage and initial materialisation where possible. A
stronger model is a plausible follow-up, but was **not** tested in this chat.
Avoid claiming that the provider tariff estimate is an actual invoice.
