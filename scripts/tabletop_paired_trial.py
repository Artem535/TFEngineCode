#!/usr/bin/env python3
"""Paired files/database pilot on a real feature of a real project.

Two arms solve the identical task:

  files : ordinary sources on disk, edited with shell tools.
  db    : the role tree lives only in the engine's ObjectBox store; sources are
          rendered output and every edit goes through `tfe-code`.

Paid execution requires --go-ahead. Grading never trusts the attempt's own
copies: the produced artefact is replayed into a pristine template and the
independent verifier plus the project suite run there, so weakening tests or
verify.py inside the attempt cannot help.
"""
import argparse
from decimal import Decimal
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
SPIKE = ROOT.parent / 'TextFoundryEngine/experiments/code-fragments-spike'
HARNESS = Path(os.environ.get('TFE_TRIAL_HARNESS', ROOT / 'harness/tabletop'))
TFE = ROOT / 'build-obj/tfe-code'
PROJECT = Path('/home/a.durynin/Projects/Python/TabletopMagnatAll/TabletopMagnat')
VENV = PROJECT / '.venv/bin/python'
KEY_FILE = ROOT.parent / 'TextFoundryEngine/.dt.key'

ARMS = ('files', 'db')
MODEL = os.environ.get('TFE_TRIAL_MODEL', 'deepseek/deepseek-v4.1-flash')
BASE_URL = 'https://api.vsellm.ru/v1'
ARTEFACT = 'src/tabletopmagnat/observability/llm_usage.py'

COMMON = '''You are already inside the project workspace; the current directory is its root.
Never cd outside it and never search the filesystem: every path you need is relative
to the current directory. The task text is appended below; do not read task.md again.

When you are done, run `bash check.sh` from the workspace root; it must pass. It
already knows which Python interpreter to use, so do not look for one.
Do not modify task.md, verify.py, check.sh or anything under tests/; the grader
restores them before grading.
Report briefly after the checks pass.
'''

ENVIRONMENT = {
    'files': '''The sources are ordinary files under src/. Read and edit them with shell tools.
''',
    'db': '''The source tree starts in .tfe-code/db; src/ is initially empty.
The launcher is ./tfe-code and TFE_CODE_INDEX=objectbox is already exported.
Use the imported Python workspace branch of the following skill.
''',
}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def prompt_for(arm, skill):
    task = (HARNESS / 'task.md').read_text()
    return COMMON + ENVIRONMENT[arm] + '\n' + skill + '\n' + task


def build_arms(out):
    """Index the real project once and materialise both arm templates."""
    result = subprocess.run(['bash', str(ROOT / 'harness/tabletop/setup.sh'),
                            str(out / 'arms'), str(HARNESS)],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit('setup failed: ' + result.stdout + result.stderr)
    return result.stdout.strip()


def grade(run, arm, out):
    """Replay the produced artefact into a pristine template and grade there."""
    pristine = out / 'arms' / arm
    replay = out / 'grades' / run.name
    shutil.copytree(pristine, replay)
    notes = []
    if arm == 'files':
        produced = run / 'ws/src'
        if produced.is_dir():
            shutil.rmtree(replay / 'src', ignore_errors=True)
            shutil.copytree(produced, replay / 'src')
        else:
            notes.append('attempt produced no src/')
    else:
        store = run / 'ws/.tfe-code/db'
        if store.is_dir():
            shutil.rmtree(replay / '.tfe-code', ignore_errors=True)
            (replay / '.tfe-code').mkdir(parents=True)
            shutil.copytree(store, replay / '.tfe-code/db')
            rendered = subprocess.run([str(TFE), 'render', '--write'], cwd=replay,
                                      capture_output=True, text=True,
                                      env=dict(os.environ, TFE_CODE_INDEX='objectbox'))
            if rendered.returncode != 0:
                notes.append('render failed: ' + rendered.stderr.strip()[:200])
        else:
            notes.append('attempt left no ObjectBox store')

    verifier = subprocess.run([str(VENV), 'verify.py', '.'], cwd=replay,
                              capture_output=True, text=True,
                              # The package is installed editable against the
                              # original project, so without this the verifier
                              # would import the untouched sources instead of
                              # the attempt's.
                              env=dict(os.environ, PYTHONPATH=str(replay / 'src')))
    try:
        verified = json.loads(verifier.stdout or '{}')
    except json.JSONDecodeError:
        verified = {'passed': False, 'failures': ['verifier output unreadable: ' + verifier.stdout[:200]]}
    suite = subprocess.run([str(VENV), '-m', 'pytest', 'tests/', '-q',
                            '--ignore=tests/test_sessions_controller.py',
                            '--deselect=tests/test_service_session_scope.py::ServiceSessionScopeTest::test_run_with_request_id_sets_session_context_during_flow_execution',
                            '--deselect=tests/test_session_manager.py::SessionManagerTest::test_cancel_marks_session_cancelled'],
                           cwd=replay, capture_output=True, text=True,
                           env=dict(os.environ, PYTHONPATH=str(replay / 'src')))
    suite_ok = suite.returncode == 0
    if not suite_ok:
        notes.append('project suite failed: ' + suite.stdout.strip().splitlines()[-1][:160])
    return {
        'passed': bool(verified.get('passed')) and suite_ok,
        'feature': verified,
        'project_suite_passed': suite_ok,
        'notes': notes,
    }


def run(args):
    if not args.go_ahead:
        raise SystemExit('Paid run requires --go-ahead and an explicit --budget')
    out = Path(args.out)
    if out.exists():
        raise SystemExit(f'Refusing to overwrite {out}')
    out.mkdir(parents=True)
    (out / 'runs').mkdir()
    (out / 'grades').mkdir()
    print(build_arms(out), flush=True)

    harness = out / 'harness'
    harness.mkdir(exist_ok=True)
    for source, name in ((SPIKE / 'api_agent.py', 'api_agent.py'),
                         (SPIKE / 'role_contracts/budget_agent.py', 'budget_agent.py'),
                         (SPIKE / 'role_contracts/tariff_cost.py', 'tariff_cost.py')):
        shutil.copy2(source, harness / name)
    skill_paths = {'db': ROOT / 'skills/tfengine-code/SKILL.md',
                   'files': ROOT / 'skills/source-code-editing/SKILL.md'}
    skills = {arm: path.read_text() for arm, path in skill_paths.items()}
    for arm, skill in skills.items():
        (harness / f'{arm}-SKILL.md').write_text(skill)
    prompts = {arm: prompt_for(arm, skills[arm]) for arm in ARMS}
    for arm, prompt in prompts.items():
        (harness / f'{arm}-prompt.txt').write_text(prompt)
    (harness / 'prompt-hashes.json').write_text(json.dumps({
        arm: hashlib.sha256(prompt.encode()).hexdigest() for arm, prompt in prompts.items()
    }, indent=2) + '\n')

    key = KEY_FILE.read_text().strip()
    establish = {}
    # The venv's interpreter is a symlink chain into the uv-managed tree, so the
    # whole uv/python directory must be re-exposed, not just the resolved version.
    interpreter = os.path.realpath(VENV)
    for candidate in (str(VENV.parent.parent),
                      os.path.dirname(os.path.dirname(os.path.dirname(interpreter)))):
        establish[candidate] = None
    base_env = dict(os.environ, TFE_LLM_API_KEY=key,
                    TFE_ATTEMPT_RUB=str(args.attempt_budget),
                    TFE_TARIFF_INPUT=str(args.tariff_input),
                    TFE_TARIFF_OUTPUT=str(args.tariff_output))

    spent = 0.0
    rows = []
    order = [a for a in ARMS if a in args.arms]
    for rep in range(1, args.reps + 1):
        arms_this_round = list(order)
        # Alternate which arm starts, so a warm provider cache cannot favour one.
        if rep % 2 == 0:
            arms_this_round.reverse()
        for arm in arms_this_round:
            if spent >= args.budget:
                print(json.dumps({'stopped': 'series budget', 'spent': round(spent, 6)}),
                      flush=True)
                return report(out, rows, {'tariff_input': args.tariff_input, 'tariff_output': args.tariff_output, 'reps': args.reps, 'attempt_budget': args.attempt_budget, 'budget': args.budget})
            target = out / 'runs' / f'{arm}-r{rep}'
            target.mkdir()
            ws = target / 'ws'
            shutil.copytree(out / 'arms' / arm, ws)

            env = dict(base_env)
            env.pop('TFE_CODE_INDEX', None)
            read_only = ['verify.py', 'task.md', 'check.sh']
            if not (out / 'arms' / arm).is_dir():
                raise SystemExit(f'missing arm template: {arm}')
            if arm == 'db':
                shutil.copy2(TFE, ws / 'tfe-code')
                (ws / 'tfe-code').chmod(0o755)
                read_only.append('tfe-code')
                # The sandbox uses --clearenv, so the index mode must be passed
                # explicitly; setting it on the parent process is not enough.
                command_env = ['--env', 'TFE_CODE_INDEX=objectbox']
            else:
                command_env = []

            command = [sys.executable, '-B', str(harness / 'budget_agent.py'),
                       '--base-url', BASE_URL, '--model', MODEL, '--effort', 'none',
                       '--api-key-env', 'TFE_LLM_API_KEY',
                       '--max-output-tokens', str(args.max_output),
                       '--workspace', str(ws), '--max-steps', str(args.max_steps),
                       '--command-timeout', str(args.command_timeout),
                       '--request-timeout', str(args.request_timeout),
                       '--max-nudges', '1', '--done-check', 'bash check.sh'] + command_env
            for candidate in establish:
                command.extend(['--expose', candidate])
            for name in read_only:
                command.extend(['--read-only', name])

            started = time.monotonic()
            with (target / 'events.jsonl').open('w') as log:
                try:
                    finished = subprocess.run(command, input=prompts[arm], text=True,
                                              stdout=log, stderr=subprocess.PIPE,
                                              env=env, timeout=args.wall_timeout)
                    if finished.stderr:
                        (target / 'stderr.txt').write_text(
                            finished.stderr.replace(key, '[REDACTED]'))
                except subprocess.TimeoutExpired:
                    log.write(json.dumps({'type': 'turn.failed',
                                          'error': {'message': 'wall timeout'}}) + '\n')
            elapsed = time.monotonic() - started
            rows.append(collect(target, arm, out))
            rows[-1]['agent_wall_seconds'] = round(elapsed, 3)
            spent += rows[-1]['cost_rub']
            print(json.dumps({k: rows[-1][k] for k in ('run', 'completed', 'passed',
                                                       'cost_rub', 'steps', 'commands',
                                                       'suite')}), flush=True)

    return report(out, rows, {'tariff_input': args.tariff_input, 'tariff_output': args.tariff_output, 'reps': args.reps, 'attempt_budget': args.attempt_budget, 'budget': args.budget})


def collect(target, arm, out):
    """Parse one attempt's events and grade the artefact it produced."""
    events = []
    for line in (target / 'events.jsonl').read_text().splitlines():
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    responses = [e for e in events if e.get('type') == 'api.response']
    costs = [(e.get('usage') or {}).get('cost_rub') for e in responses]
    turns = [e for e in events if e.get('type') == 'item.completed'
             and (e.get('item') or {}).get('type') == 'command_execution']
    completed = any(e.get('type') == 'turn.completed' for e in events)
    row = {
        'run': target.name,
        'arm': arm,
        'completed': completed,
        'cost_rub': float(sum((Decimal(str(v)) for v in costs if v is not None), Decimal(0))),
        'unknown_cost': any(v is None for v in costs) or not responses,
        'cost_sources': sorted({(e.get('usage') or {}).get('cost_source', 'none')
                                for e in responses}),
        'steps': len(responses),
        'commands': len(turns),
        'read_chars': sum(len((e.get('item') or {}).get('aggregated_output', ''))
                          for e in turns),
        'input_tokens': sum((e.get('usage') or {}).get('prompt_tokens', 0)
                            for e in responses),
        'output_tokens': sum((e.get('usage') or {}).get('completion_tokens', 0)
                             for e in responses),
        'api_seconds': round(sum(e.get('request_seconds', 0) for e in responses), 3),
    }
    graded = grade(target, arm, out)\
        if (target / 'ws').exists() else {'passed': False, 'feature': {}, 'project_suite_passed': False, 'notes': []}
    (target / 'grade.json').write_text(json.dumps(graded, indent=2))
    row['passed'] = bool(graded['passed']) if completed else False
    row['feature_failures'] = graded['feature'].get('failures', [])
    row['suite'] = graded['project_suite_passed']
    return row


def report(out, rows, meta=None):
    meta = meta or {}
    summaries = {}
    for arm in ARMS:
        selected = [r for r in rows if r['run'].startswith(arm + '-')]
        summaries[arm] = {
            'attempts': len(selected),
            'completed': sum(r['completed'] for r in selected),
            'passed': sum(r['passed'] for r in selected),
            'cost_rub': round(sum(r['cost_rub'] for r in selected), 6),
            'steps': sum(r['steps'] for r in selected),
            'commands': sum(r['commands'] for r in selected),
            'read_chars': sum(r['read_chars'] for r in selected),
            'input_tokens': sum(r['input_tokens'] for r in selected),
            'output_tokens': sum(r['output_tokens'] for r in selected),
        }
    result = {'rows': rows, 'summaries': summaries,
              'model': MODEL, 'base_url': BASE_URL,
              'tariff_rub_per_million': {'input': meta.get('tariff_input'),
                                         'output': meta.get('tariff_output')},
              'reps_requested': meta.get('reps'),
              'attempt_budget_rub': meta.get('attempt_budget'),
              'series_budget_rub': meta.get('budget'),
              'total_cost_rub': round(sum(r['cost_rub'] for r in rows), 6)}
    (out / 'report.json').write_text(json.dumps(result, indent=2))
    print(json.dumps({'summaries': summaries,
                      'total_cost_rub': result['total_cost_rub']}, indent=2))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', default='/tmp/tfe-tabletop-2026-11-07-run')
    parser.add_argument('--budget', type=float, default=3.0)
    parser.add_argument('--attempt-budget', type=float, default=1.5)
    parser.add_argument('--max-steps', type=int, default=30)
    parser.add_argument('--max-output', type=int, default=8192)
    parser.add_argument('--command-timeout', type=int, default=120)
    parser.add_argument('--request-timeout', type=int, default=120)
    parser.add_argument('--wall-timeout', type=int, default=900)
    parser.add_argument('--seed', type=int, default=20261107)
    parser.add_argument('--tariff-input', type=float, default=33.0,
                        help='RUB per 1M input tokens used to price the attempt')
    parser.add_argument('--tariff-output', type=float, default=133.0,
                        help='RUB per 1M output tokens used to price the attempt')
    parser.add_argument('--reps', type=int, default=1)
    parser.add_argument('--arms', default=','.join(ARMS),
                        help='comma-separated arms to run, default both')
    parser.add_argument('--go-ahead', action='store_true')
    run(parser.parse_args())


if __name__ == '__main__':
    main()
