#!/usr/bin/env bash
# Builds the two arms for the TabletopMagnat cache-aware-cost experiment.
#
# The arms are plain copies rather than `git worktree` on purpose: a worktree
# carries a .git pointer to the real repository, so an agent with a writable
# sandbox could mutate the user's checkout. Copies give the same isolation with
# no write path back to the source repository.
#
#   files : the project as it exists on disk; the agent edits normal sources.
#   db    : the role tree lives only in .tfe-code/db; src/*.py is produced by
#           `tfe-code render --write`, and the agent edits through the CLI.
set -euo pipefail

PROJECT=/home/a.durynin/Projects/Python/TabletopMagnatAll/TabletopMagnat
TFE=/home/a.durynin/Projects/C++/TFEngineCode/build-obj/tfe-code
HARNESS=${2:-/home/a.durynin/Projects/C++/TFEngineCode/harness/tabletop}
OUT=${1:-/tmp/tfe-tabletop-2026-11-07}
VENV=$PROJECT/.venv/bin/python

rm -rf "$OUT"
mkdir -p "$OUT"

# Shared, non-role content that both arms need to run the behavioural checks.
SUPPORT=(tests prompts data mcp_servers graph_rag scripts pyproject.toml)

for arm in files db; do
  mkdir -p "$OUT/$arm"
  for item in "${SUPPORT[@]}"; do
    [ -e "$PROJECT/$item" ] && cp -r "$PROJECT/$item" "$OUT/$arm/"
  done
  find "$OUT/$arm" -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true
done

# files arm: the real sources.
cp -r "$PROJECT/src" "$OUT/files/"

# db arm: index the real sources into ObjectBox, then drop the sources.
mkdir -p "$OUT/db/src"
cp -r "$PROJECT/src/." "$OUT/db/src/"
( cd "$OUT/db" && TFE_CODE_INDEX=objectbox "$TFE" import src > /tmp/tfe-tabletop-import.json )
rm -rf "$OUT/db/src"
mkdir -p "$OUT/db/src"

# The verifier is copied into both arms read-only for the agent.
cp "$HARNESS/verify.py" "$OUT/files/verify.py"
cp "$HARNESS/verify.py" "$OUT/db/verify.py"
cp "$HARNESS/task.md" "$OUT/files/task.md"
cp "$HARNESS/task.md" "$OUT/db/task.md"

cat > "$OUT/check.sh" <<EOF
#!/usr/bin/env bash
# Behavioural gate: the project's own suite must stay green and the feature
# verifier must pass. Runs from the arm root. The package is installed editable
# against the original project, so PYTHONPATH must point at this arm's src first.
#
# Materialising the tree belongs to the gate, not to the agent: making the arm do
# it costs a whole discovery step that the file arm does not pay.
set -euo pipefail
# Materialise only when nothing is on disk yet. This must NOT run on every check:
# `apply --check` holds the workspace lock while the check runs, so a nested
# `tfe-code render` blocks on that lock forever. After an apply the sources are
# already written, so the guard is false and no second process is started.
if [ -n "\${TFE_CODE_INDEX:-}" ] && [ -x ./tfe-code ] && [ -z "\$(ls -A src 2>/dev/null)" ]; then
  ./tfe-code render --write > /dev/null
fi
export PYTHONPATH="\$PWD/src"
$VENV -m pytest tests/ -q \\
  --ignore=tests/test_sessions_controller.py \\
  --deselect=tests/test_service_session_scope.py::ServiceSessionScopeTest::test_run_with_request_id_sets_session_context_during_flow_execution \\
  --deselect=tests/test_session_manager.py::SessionManagerTest::test_cancel_marks_session_cancelled
$VENV verify.py .
# Report the verifier's status explicitly so a wrapper cannot mask it.
EOF
chmod +x "$OUT/check.sh"
# Each arm runs the gate from its own root, so it needs its own copy.
for arm in files db; do
  cp "$OUT/check.sh" "$OUT/$arm/check.sh"
done

echo "arms built in $OUT"
echo "files: $(find "$OUT/files" -name '*.py' | wc -l) python files"
echo "db   : $(find "$OUT/db" -name '*.py' | wc -l) python files, store $(du -sh "$OUT/db/.tfe-code/db" 2>/dev/null | cut -f1)"