#!/usr/bin/env python3
"""Frozen paired native-role/file pilot; paid execution requires explicit flags."""
import argparse
import ast
from decimal import Decimal
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SPIKE = ROOT.parent / 'TextFoundryEngine/experiments/code-fragments-spike'
ORIGINAL = SPIKE / 'artifacts/role-tree-2026-10-24/probe-criticnode/roles/original.py'
ARMS = ('files', 'native')
TASKS = {
    1: 'In CriticNode.post_async add question_chars to update_current_span metadata: the length in Unicode '
       'characters of the original question (shared.dialog.messages[0].content), or 0 when messages is empty. '
       'Preserve the event payload schema and all other behavior. Change only the existing update_current_span statement.',
    2: 'In CriticNode.post_async allow one additional research iteration: force needs_more_work to final only '
       'when shared.iteration >= shared.max_iterations + 1. Before that boundary keep needs_more_work. '
       'Preserve report.verdict and keep logs, span and event route consistent with the returned route. '
       'Change only the existing max-iterations if statement; preserve every other byte and behavior.',
}
COMMON = '''Work in this workspace on out/critic_node.py. The original external packages are absent;
verify.py uses stubs and checks behavior plus exact preservation outside the requested statement.
Use focused reads and one batch edit when possible. Preserve unrelated bytes and behavior.
Do not edit task.json, baseline/, verify.py or the supplied tools. No Git repository is present.
Report briefly after the behavioral check passes.
'''
SOURCE = '''Source files are authoritative. Use ordinary shell tools to locate/read/edit the code.
After your edit run: python3 -B verify.py
'''
NATIVE = '''The native role CLI is ./tfe-code and the source is already imported.
The supplied behavioral check is: python3 -B verify.py
'''
PROTOCOL = '''# Native paired pilot protocol

Two arms (`native`, `files`), two existing CriticNode tasks, three paired repetitions.
Model deepseek/deepseek-v4.1-flash via https://api.vsellm.ru/v1, effort none, max output 4096, max 20 steps, request
timeout 90 seconds, wall timeout 240 seconds. Order is shuffled independently within
each repeat/task with seed 20261029 + repeat*10 + task. Tasks, binary, verifier and
arm-specific skills are frozen before the first scored request. No tuning during a series.
The native prompt contains only the imported-workspace skill; the file prompt contains
the source-code-editing skill. User task text is byte-identical between arms.

A live attempt passes only with turn.completed, independent behavior/non-target checks,
and native source/index consistency where relevant. Incomplete and infrastructure attempts
remain in the report. Unknown usage is unknown cost, never zero. All provider charges count.
The series stops at the selected spending guard or after two consecutive incomplete turns.
The guard is not a provider-enforced hard cap; the response crossing it is still charged.

Decision: native saves on this workload only if all six native attempts pass and its summed
cost for both tasks is <= 0.8 x files in at least two of three complete repetitions, with
no worse pass rate. Otherwise report not_confirmed (or incomplete). This bounded pilot does
not establish repository-wide or language-wide savings.
'''


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def prompt(arm, task, harness):
    environment = NATIVE if arm == 'native' else SOURCE
    skill = (harness / f'{arm}-skill.md').read_text()
    return COMMON + environment + '\n' + skill + '\nTask: ' + TASKS[task] + '\n'


def prepare(root, arm, task, harness):
    source = (harness / 'original.py').read_text()
    root.mkdir(parents=True)
    (root / 'out').mkdir()
    (root / 'baseline').mkdir()
    (root / 'out/critic_node.py').write_text(source)
    (root / 'baseline/original.py').write_text(source)
    allowed = {}
    lines = source.splitlines(keepends=True)
    for node in ast.walk(ast.parse(source)):
        target = None
        if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call) and ast.unparse(node.value.func) == 'self._lf_client.update_current_span':
            target = 1
        if isinstance(node, ast.If) and 'shared.iteration >= shared.max_iterations' in ast.unparse(node.test):
            target = 2
        if target:
            allowed[str(target)] = [sum(map(len, lines[:node.lineno - 1])), sum(map(len, lines[:node.end_lineno]))]
    assert set(allowed) == {'1', '2'}
    (root / 'baseline/allowed.json').write_text(json.dumps(allowed))
    shutil.copy2(harness / 'verify.py', root / 'verify.py')
    (root / 'task.json').write_text(json.dumps({'task': 0}))
    baseline = subprocess.run([sys.executable, '-B', 'verify.py'], cwd=root, capture_output=True, text=True)
    assert baseline.returncode == 0, baseline.stdout + baseline.stderr
    if arm == 'native':
        shutil.copy2(harness / 'tfe-code', root / 'tfe-code')
        imported = subprocess.run([str(root / 'tfe-code'), 'import', 'out/critic_node.py'], cwd=root,
                                  capture_output=True, text=True)
        assert imported.returncode == 0, imported.stdout + imported.stderr
        rendered = subprocess.run([str(root / 'tfe-code'), 'render'], cwd=root,
                                  capture_output=True, text=True, check=True)
        assert json.loads(rendered.stdout)['out/critic_node.py'] == source
    (root / 'task.json').write_text(json.dumps({'task': task}))
    return root


def apply_native(root, old, new, expected=None):
    shown = subprocess.run([str(root / 'tfe-code'), 'outline', 'out/critic_node.py', '--depth', '3'],
                           cwd=root, capture_output=True, text=True, check=True)
    role = next(line.strip().split(' ', 1)[0] for line in shown.stdout.splitlines()
                if line.strip().endswith('(MISSING)') is False and line.strip().split(' ', 1)[0].endswith('.post_async'))
    if expected is None:
        record = subprocess.run([str(root / 'tfe-code'), 'show', role, '--json'], cwd=root,
                                capture_output=True, text=True, check=True)
        expected = json.loads(record.stdout)[role]['version']
    payload = json.dumps({'edits': [{'role': role, 'expected_version': expected, 'old': old, 'new': new}]})
    return subprocess.run([str(root / 'tfe-code'), 'apply', '--check', 'python3 -B verify.py'],
                          cwd=root, input=payload, capture_output=True, text=True), role, expected


def local_checks(harness):
    checked = []
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix='tfe-native-paired-') as tmp:
        root = Path(tmp)
        native = prepare(root / 'native', 'native', 1, harness)
        files = prepare(root / 'files', 'files', 1, harness)
        source_old = '"route": route,\n                "critic_verdict": report.verdict,'
        source_new = '"route": route,\n                "question_chars": len(shared.dialog.messages[0].content) if shared.dialog.messages else 0,\n                "critic_verdict": report.verdict,'
        native_old = source_old.replace('\n                ', '\n            ')
        native_new = source_new.replace('\n                ', '\n            ')
        result, role, version = apply_native(native, native_old, native_new)
        assert result.returncode == 0, result.stdout + result.stderr
        checked.append('native task1 real apply/check')
        before = {name: (native / name).read_bytes() for name in
                  ('out/critic_node.py', '.tfe-code/project.json', '.tfe-code/state.json')}
        stale, _, _ = apply_native(native, native_old, native_new, expected=version)
        assert stale.returncode != 0
        assert all((native / name).read_bytes() == value for name, value in before.items())
        checked.append('stale has no writes')
        bad, _, _ = apply_native(native, native_old, native_new.replace('"route": route', '"route": "WRONG"'))
        assert bad.returncode != 0
        assert all((native / name).read_bytes() == value for name, value in before.items())
        checked.append('failed behavior rolls back source/index/receipt')
        source = (files / 'out/critic_node.py').read_text()
        (files / 'out/critic_node.py').write_text(source.replace(source_old, source_new, 1))
        assert subprocess.run([sys.executable, '-B', 'verify.py'], cwd=files).returncode == 0
        checked.append('file control task1 behavior')
        second = prepare(root / 'second', 'native', 2, harness)
        boundary, _, _ = apply_native(second, 'shared.iteration >= shared.max_iterations',
                                      'shared.iteration >= shared.max_iterations + 1')
        assert boundary.returncode == 0, boundary.stdout + boundary.stderr
        checked.append('native task2 boundary behavior')
        original = (native / 'out/critic_node.py').read_bytes()
        (native / 'out/critic_node.py').write_bytes(original + b'\n# external\n')
        assert subprocess.run([str(native / 'tfe-code'), 'show', role, '--json'], cwd=native,
                              capture_output=True).returncode != 0
        checked.append('external edit rejected before sync')
    result = {'passed': True, 'checks': checked, 'seconds': round(time.monotonic() - started, 3)}
    print(json.dumps(result, indent=2))
    return result


def freeze(out, binary):
    if out.exists():
        raise SystemExit(f'Refusing to overwrite {out}')
    harness = out / 'harness'
    harness.mkdir(parents=True)
    sources = {
        'trial.py': Path(__file__).resolve(),
        'api_agent.py': SPIKE / 'api_agent.py',
        'budget_agent.py': SPIKE / 'role_contracts/budget_agent.py',
        'tariff_cost.py': SPIKE / 'role_contracts/tariff_cost.py',
        'original.py': ORIGINAL,
        'verify.py': SPIKE / 'role_contracts/verify.py',
        'files-skill.md': ROOT / 'skills/source-code-editing/SKILL.md',
        'tfe-code': binary.resolve(strict=True),
    }
    for name, source in sources.items():
        shutil.copy2(source, harness / name)
    native_skill = (ROOT / 'skills/tfengine-code/SKILL.md').read_text()
    end = native_skill.index('## Explicit') if '## Explicit' in native_skill else len(native_skill)
    native_skill = native_skill[native_skill.index('## Imported'):end]
    assert 'dedented' in native_skill and 'OBSERVED_ID' not in native_skill
    (harness / 'native-skill.md').write_text(native_skill)
    (harness / 'tfe-code').chmod(0o755)
    (harness / 'PROTOCOL.md').write_text(PROTOCOL)
    for arm in ARMS:
        for task in TASKS:
            (harness / f'{arm}-task{task}.txt').write_text(prompt(arm, task, harness))
    local = local_checks(harness)
    (out / 'local-checks.json').write_text(json.dumps(local, indent=2))
    manifest = {path.name: sha(path) for path in sorted(harness.iterdir()) if path.is_file()}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    print(json.dumps({'frozen': str(out), 'files': len(manifest), 'paid_requests': 0}))


def parse_attempt(run):
    events = []
    for line in (run / 'events.jsonl').read_text().splitlines():
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    responses = [event for event in events if event.get('type') == 'api.response']
    costs = [(event.get('usage') or {}).get('cost_rub') for event in responses]
    commands = [event['item'] for event in events if event.get('type') == 'item.completed'
                and event.get('item', {}).get('type') == 'command_execution']
    completed = any(event.get('type') == 'turn.completed' for event in events)
    grade = json.loads((run / 'grade.json').read_text()) if (run / 'grade.json').exists() else {'passed': False}
    return {
        'run': run.name,
        'completed': completed,
        'passed': completed and grade['passed'],
        'cost_rub': float(sum((Decimal(str(value)) for value in costs if value is not None), Decimal(0))),
        'unknown_cost': any(value is None for value in costs) or not responses,
        'steps': len(responses),
        'command_calls': len(commands),
        'read_chars': sum(len(command.get('aggregated_output', '')) for command in commands),
        'uncached_input': sum((event.get('usage') or {}).get('prompt_tokens', 0)
                              - ((event.get('usage') or {}).get('prompt_tokens_details') or {}).get('cached_tokens', 0)
                              for event in responses),
        'cached_input': sum(((event.get('usage') or {}).get('prompt_tokens_details') or {}).get('cached_tokens', 0)
                            for event in responses),
        'api_seconds': round(sum(event.get('request_seconds', 0) for event in responses), 3),
        'tool_seconds': round(sum(command.get('tool_seconds') or 0 for command in commands), 3),
        'errors': [event.get('message') or event.get('error', {}).get('message')
                   for event in events if event.get('type') in ('error', 'turn.failed')],
        'grade': grade,
    }


def report(out):
    rows = [parse_attempt(path) for path in sorted((out / 'runs').glob('*')) if (path / 'grade.json').exists()]
    summaries = {}
    for arm in ARMS:
        selected = [row for row in rows if row['run'].startswith(arm + '-')]
        summaries[arm] = {
            'attempts': len(selected),
            'completed': sum(row['completed'] for row in selected),
            'passed': sum(row['passed'] for row in selected),
            'cost_rub': round(sum(row['cost_rub'] for row in selected), 6),
            'steps': sum(row['steps'] for row in selected),
            'command_calls': sum(row['command_calls'] for row in selected),
            'read_chars': sum(row['read_chars'] for row in selected),
            'uncached_input': sum(row['uncached_input'] for row in selected),
            'cached_input': sum(row['cached_input'] for row in selected),
        }
    repetitions = []
    for rep in (1, 2, 3):
        pairs = {arm: [row for row in rows if row['run'].startswith(arm + '-') and row['run'].endswith(f'-r{rep}')]
                 for arm in ARMS}
        full = all(len(items) == 2 and all(row['completed'] and not row['unknown_cost'] for row in items)
                   for items in pairs.values())
        costs = {arm: round(sum(row['cost_rub'] for row in pairs[arm]), 6) for arm in ARMS}
        repetitions.append({'repeat': rep, 'evaluable': full, 'costs': costs,
                            'saves': full and costs['native'] <= 0.8 * costs['files']})
    evaluable = len(rows) == 12 and all(row['evaluable'] for row in repetitions)
    success = evaluable and summaries['native']['passed'] == 6 and sum(row['saves'] for row in repetitions) >= 2
    result = {'rows': rows, 'summaries': summaries, 'repetitions': repetitions,
              'verdict': 'saves_on_this_workload' if success else 'not_confirmed' if evaluable else 'incomplete',
              'total_cost_rub': round(sum(row['cost_rub'] for row in rows), 6)}
    (out / 'report.json').write_text(json.dumps(result, indent=2))
    print(json.dumps({key: value for key, value in result.items() if key != 'rows'}, indent=2))
    return result


def run_series(out, budget, go_ahead):
    if not go_ahead:
        raise SystemExit('Paid run requires --go-ahead and an explicit --budget')
    harness = out / 'harness'
    manifest = json.loads((out / 'manifest.json').read_text())
    assert all(sha(harness / name) == value for name, value in manifest.items()), 'frozen harness changed'
    key = (SPIKE.parent.parent / '.dt.key').read_text().strip()
    env = dict(os.environ, TFE_LLM_API_KEY=key, TFE_ATTEMPT_RUB='1')
    runs = out / 'runs'
    runs.mkdir(exist_ok=True)
    spent = 0.0
    consecutive_incomplete = 0
    for rep in (1, 2, 3):
        for task in TASKS:
            order = list(ARMS)
            random.Random(20261029 + rep * 10 + task).shuffle(order)
            for arm in order:
                target = runs / f'{arm}-t{task}-r{rep}'
                if target.exists():
                    raise SystemExit(f'Refusing to overwrite {target}')
                if spent >= budget or consecutive_incomplete >= 2:
                    print(json.dumps({'stopped': 'budget/incomplete guard', 'spent': spent}), flush=True)
                    return report(out)
                target.mkdir()
                started = time.monotonic()
                workspace = prepare(target / 'ws', arm, task, harness)
                (target / 'setup.json').write_text(json.dumps({'seconds': time.monotonic() - started, 'model_cost_rub': 0}))
                protected = ['verify.py', 'baseline', 'task.json']
                if arm == 'native':
                    protected.append('tfe-code')
                args = [sys.executable, '-B', str(harness / 'budget_agent.py'),
                        '--base-url', 'https://api.vsellm.ru/v1', '--model', 'deepseek/deepseek-v4.1-flash',
                        '--effort', 'none', '--api-key-env', 'TFE_LLM_API_KEY', '--max-output-tokens', '4096',
                        '--workspace', str(workspace), '--max-steps', '20', '--command-timeout', '30',
                        '--request-timeout', '90', '--max-nudges', '1', '--done-check', 'python3 -B verify.py']
                for name in protected:
                    args.extend(['--read-only', name])
                with (target / 'events.jsonl').open('w') as log:
                    try:
                        result = subprocess.run(args, input=(harness / f'{arm}-task{task}.txt').read_text(), text=True,
                                                stdout=log, stderr=subprocess.PIPE, env=env, timeout=240)
                        if result.stderr:
                            (target / 'stderr.txt').write_text(result.stderr.replace(key, '[REDACTED]'))
                    except subprocess.TimeoutExpired:
                        log.write(json.dumps({'type': 'turn.failed', 'error': {'message': 'wall timeout'}}) + '\n')
                verifier = load_module('independent_verify', harness / 'verify.py')
                grade = verifier.verify(workspace, task)
                if arm == 'native':
                    consistent = subprocess.run([str(workspace / 'tfe-code'), 'render'], cwd=workspace,
                                                capture_output=True, text=True)
                    if consistent.returncode != 0:
                        grade['passed'] = False
                        grade['failures'].append('native source/index divergence')
                (target / 'grade.json').write_text(json.dumps(grade))
                leaks = [str(path.relative_to(target)) for path in target.rglob('*')
                         if path.is_file() and key.encode() in path.read_bytes()]
                if leaks:
                    raise SystemExit('Secret scan failed; series stopped')
                (target / 'secret-scan.json').write_text(json.dumps({'matches': 0}))
                row = parse_attempt(target)
                row['seconds'] = round(time.monotonic() - started, 3)
                (target / 'summary.json').write_text(json.dumps(row, indent=2))
                spent += row['cost_rub']
                consecutive_incomplete = 0 if row['completed'] else consecutive_incomplete + 1
                print(json.dumps({key: row[key] for key in ('run', 'completed', 'passed', 'cost_rub', 'steps', 'seconds')}), flush=True)
    return report(out)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['check', 'freeze', 'run', 'report'])
    parser.add_argument('--out', type=Path, default=Path('/tmp/tfe-native-paired-skilled-2026-10-29'))
    parser.add_argument('--binary', type=Path, default=ROOT / 'build/tfe-code')
    parser.add_argument('--budget', type=float, default=4.0)
    parser.add_argument('--go-ahead', action='store_true')
    args = parser.parse_args()
    if args.command == 'check':
        class Harness:
            pass
        raise SystemExit('Use freeze to run local checks with the complete frozen harness')
    if args.command == 'freeze':
        freeze(args.out.resolve(), args.binary)
    elif args.command == 'run':
        run_series(args.out.resolve(), args.budget, args.go_ahead)
    else:
        report(args.out.resolve())
