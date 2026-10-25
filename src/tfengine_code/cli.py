#!/usr/bin/env python3
"""TFEngine Code: named code roles rendered through TextFoundryEngine.

Commands:
  tfe-code init                               initialize a workspace
  tfe-code outline [PATH|ID] [--depth N]     role tree hierarchy (line count and first line)
  tfe-code tree [PATH|ID]                    read an entire branch/file as one #@ role document
  tfe-code show ID [ID2 ...] [--tree]        read one or more roles (or subtree with --tree)
  tfe-code apply [PATH=ROOT ...] <<'EOF'     create/update any number of #@ roles in one call
  tfe-code put ID <<'EOF'                    create/update one role (or #@ doc), render & check
  tfe-code replace PREFIX OLD NEW            replace OLD -> NEW across roles matching PREFIX
  tfe-code who [PREFIX.] NAME [NAME2 ...]    find roles mentioning NAME(s) and show their bodies
  tfe-code file PATH ROOT                    declare out/PATH rendered from role ROOT
  tfe-code check                             check missing roles, render out/, and compile-check
"""
import ast
import json
from pathlib import Path
import re
import subprocess
import sys
import textwrap
import os
import shutil

REF = re.compile(r'\{\{([A-Za-z0-9_-]+(?:\.[A-Za-z0-9_-]+)+)\}\}')
HEADER = re.compile(r'^#@\s+([A-Za-z0-9_.-]+)(?:\s+\[file:\s*([^\]]+)\])?\s*$')
STORE_DIR = Path('.roles')
STORE = STORE_DIR / 'store.json'
RENDERER = Path(os.environ.get('TFE_CODE_RENDERER') or shutil.which('tfe-code-render') or str(STORE_DIR / 'tfe_render')).resolve()


def validate_path(name: str) -> None:
    path = Path(name)
    if not name or path.is_absolute() or '..' in path.parts or '\\' in name:
        raise ValueError('file paths must stay inside out/')


def print_checked(project: dict, extra_msg: str = '') -> None:
    message = check(project, extra_msg)
    print(message)
    if 'rendered, compiles' not in message:
        raise SystemExit(1)


def load() -> dict:
    return json.loads(STORE.read_text())


def save(project: dict) -> None:
    temporary = STORE.with_suffix('.tmp')
    temporary.write_text(json.dumps(project, indent=1, ensure_ascii=False))
    temporary.replace(STORE)


def normalize_role_text(text: str, is_root: bool = False) -> str:
    text = text.strip('\n')
    text = textwrap.dedent(text)
    if is_root:
        text += '\n'
    return text


def outline(project: dict, block: str, depth: int, indent: int = 0, out: list[str] | None = None) -> list[str]:
    out = [] if out is None else out
    if block not in project['blocks']:
        out.append(f'{"  " * indent}{block}  (MISSING)')
        return out
    text = project['blocks'][block]['text']
    own = [l for l in text.split('\n') if l.strip() and not REF.fullmatch(l.strip())]
    first = own[0].strip() if own else ''
    out.append(f'{"  " * indent}{block}  ({len(own)} lines)  {first[:60]}')
    if depth > 0:
        for child in REF.findall(text):
            outline(project, child, depth - 1, indent + 1, out)
    return out


def collect_subtree(project: dict, root: str) -> list[str]:
    order = []
    seen = set()

    def visit(block: str) -> None:
        if block in seen or block not in project['blocks']:
            return
        seen.add(block)
        order.append(block)
        for child in REF.findall(project['blocks'][block]['text']):
            visit(child)

    visit(root)
    return order


def format_doc(project: dict, role_ids: list[str]) -> str:
    root_to_file = {entry['root']: name for name, entry in project['files'].items()}
    chunks = []
    for role_id in role_ids:
        if role_id not in project['blocks']:
            chunks.append(f'#@ {role_id}  # MISSING')
            continue
        file_tag = f' [file: {root_to_file[role_id]}]' if role_id in root_to_file else ''
        body = project['blocks'][role_id]['text'].rstrip('\n')
        chunks.append(f'#@ {role_id}{file_tag}\n{body}')
    return '\n\n'.join(chunks)


def parse_doc(raw: str) -> tuple[dict[str, str], dict[str, str]]:
    roles: dict[str, list[str]] = {}
    files: dict[str, str] = {}
    current_id: str | None = None
    for line in raw.splitlines():
        m = HEADER.match(line)
        if m:
            current_id = m.group(1)
            if '.' not in current_id:
                raise SystemExit(f'role IDs must contain a dot: {current_id}')
            roles[current_id] = []
            if m.group(2):
                files[m.group(2).strip()] = current_id
        elif current_id is not None:
            roles[current_id].append(line)
    return {k: '\n'.join(v) for k, v in roles.items()}, files


def missing_for_root(project: dict, root: str) -> list[str]:
    found = []
    pending, seen = [root], set()
    while pending:
        block = pending.pop()
        if block in seen:
            continue
        seen.add(block)
        if block not in project['blocks']:
            found.append(block)
        else:
            pending += REF.findall(project['blocks'][block]['text'])
    return sorted(set(found))


def missing(project: dict) -> list[str]:
    found = []
    for entry in project['files'].values():
        found.extend(missing_for_root(project, entry['root']))
    return sorted(set(found))


def check(project: dict, extra_msg: str = '') -> str:
    prefix = f'{extra_msg}; ' if extra_msg else ''
    gaps = missing(project)
    if gaps:
        return prefix + 'saved; not rendered yet, missing roles: ' + ', '.join(gaps)
    result = subprocess.run([str(RENDERER), str(STORE), 'render-all', '0'], capture_output=True, text=True)
    if result.returncode:
        return prefix + 'saved; RENDER FAILED: ' + result.stderr[:500]
    files = json.loads(result.stdout)
    for name, text in files.items():
        cleaned = re.sub(r'^[ \t]+$', '', text, flags=re.M)
        if not cleaned.endswith('\n'):
            cleaned += '\n'
        validate_path(name)
        path = Path('out') / name
        if not path.resolve().is_relative_to(Path('out').resolve()):
            raise ValueError('output escapes out directory')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(cleaned)
    python_files = [f'out/{n}' for n in files if n.endswith('.py')]
    if not python_files:
        return prefix + 'rendered, compiles (no Python files)'
    compiled = subprocess.run(
        [sys.executable, '-m', 'py_compile', *python_files],
        capture_output=True,
        text=True,
    )
    if compiled.returncode != 0:
        return prefix + 'rendered, DOES NOT COMPILE:\n' + compiled.stderr[-800:]
    return prefix + 'rendered, compiles'


def store_roles(project: dict, role_map: dict[str, str], file_map: dict[str, str]) -> None:
    for file_path, root_id in file_map.items():
        validate_path(file_path)
        project['files'][file_path] = {'root': root_id, 'params': {}}
    roots = {e['root'] for e in project['files'].values()}
    for role_id, raw_body in role_map.items():
        body = normalize_role_text(raw_body, is_root=(role_id in roots))
        project['blocks'][role_id] = {
            'version': '1.0',
            'text': body,
            'literal': not bool(REF.search(body)),
        }
    for root_id in roots:
        if root_id in project['blocks'] and not project['blocks'][root_id]['text'].endswith('\n'):
            project['blocks'][root_id]['text'] += '\n'


def resolve_target(project: dict, target: str) -> str:
    t = target.strip().rstrip('.')
    if not t:
        return target
    if t in project['blocks']:
        return t
    if t in project['files']:
        return project['files'][t]['root']
    if t.startswith('out/') and t[4:] in project['files']:
        return project['files'][t[4:]]['root']
    if f'{t}.file' in project['blocks']:
        return f'{t}.file'
    low = t.lower().removesuffix('node').removesuffix('_node.py').removesuffix('.py')
    if not low:
        return target
    if f'{low}.file' in project['blocks']:
        return f'{low}.file'
    for name, entry in project['files'].items():
        if name.lower().startswith(low):
            return entry['root']
    return target


def normalize_prefix(project: dict, raw_prefix: str) -> str:
    p = raw_prefix.strip().rstrip('.')
    if not p or p in ('*', '.'):
        return ''
    resolved = resolve_target(project, p)
    if resolved.endswith('.file'):
        return resolved[:-5]
    return p


def main() -> None:
    args = sys.argv[1:]
    if args == ['--version']:
        print('TFEngine Code 0.1.0')
        return
    if args == ['init']:
        if STORE.exists():
            raise SystemExit('workspace already initialized')
        STORE_DIR.mkdir(exist_ok=True)
        save({'blocks': {}, 'files': {}})
        print('initialized .roles/store.json')
        return
    if not args:
        print(__doc__)
        return

    project = load()
    cmd = args[0]
    if cmd == 'outline':
        depth = int(args[args.index('--depth') + 1]) if '--depth' in args else 99
        rest = [a for i, a in enumerate(args[1:], 1) if a != '--depth' and args[i - 1] != '--depth']
        if not rest or rest[0] in ('.', '*', 'all', 'out', 'out/'):
            for name, entry in project['files'].items():
                print(f'{name}:\n' + '\n'.join(outline(project, entry['root'], depth, 1)))
        else:
            root = resolve_target(project, rest[0])
            print('\n'.join(outline(project, root, depth)))

    elif cmd in ('tree', 'show'):
        want_tree = (cmd == 'tree') or ('--tree' in args)
        targets = [a for a in args[1:] if a != '--tree']
        if not targets or targets[0] in ('.', '*', 'all', 'out', 'out/'):
            all_ids = []
            for name, entry in project['files'].items():
                for rid in collect_subtree(project, entry['root']):
                    if rid not in all_ids:
                        all_ids.append(rid)
            print(format_doc(project, all_ids))
            return
        resolved = [resolve_target(project, t) for t in targets]
        roots = {e['root'] for e in project['files'].values()}
        if want_tree or (len(resolved) == 1 and (resolved[0] in roots or targets[0] in project['files'])):
            role_ids = []
            for r in resolved:
                for rid in collect_subtree(project, r):
                    if rid not in role_ids:
                        role_ids.append(rid)
            print(format_doc(project, role_ids))
        elif len(resolved) == 1:
            rid = resolved[0]
            print(project['blocks'][rid]['text'].rstrip('\n') if rid in project['blocks'] else f'no role {rid}')
        else:
            print(format_doc(project, resolved))

    elif cmd == 'apply':
        file_map = {}
        for arg in args[1:]:
            if '=' in arg:
                fpath, rroot = arg.split('=', 1)
                file_map[fpath.removeprefix('out/')] = rroot
        raw = sys.stdin.read()
        role_map, parsed_files = parse_doc(raw)
        file_map.update(parsed_files)
        if not role_map:
            raise SystemExit('apply expects one or more "#@ <role.id>" sections on stdin')
        store_roles(project, role_map, file_map)
        save(project)
        print_checked(project, f'{len(role_map)} role(s) updated')

    elif cmd == 'put' and len(args) == 2:
        raw = sys.stdin.read()
        if any(HEADER.fullmatch(line) for line in raw.splitlines()):
            role_map, parsed_files = parse_doc(raw)
            store_roles(project, role_map, parsed_files)
            save(project)
            print_checked(project, f'{len(role_map)} role(s) updated')
            return
        role_id = args[1]
        if '.' not in role_id:
            raise SystemExit('role IDs contain a dot, e.g. verifier.post.route')
        store_roles(project, {role_id: raw}, {})
        save(project)
        print_checked(project, f'1 role updated ({role_id})')

    elif cmd == 'replace' and len(args) == 4:
        prefix, old, new = args[1], args[2], args[3]
        norm_p = normalize_prefix(project, prefix)
        prefix_dot = f'{norm_p}.' if norm_p else ''
        changed = []
        for role_id, block in project['blocks'].items():
            if not prefix_dot or role_id.startswith(prefix_dot):
                if old in block['text']:
                    block['text'] = block['text'].replace(old, new)
                    block['literal'] = not bool(REF.search(block['text']))
                    changed.append(role_id)
        save(project)
        print_checked(project, f'replaced in {len(changed)} role(s): {", ".join(changed) or "none"}')

    elif cmd == 'file' and len(args) == 3:
        fpath = args[1].removeprefix('out/')
        store_roles(project, {}, {fpath: args[2]})
        save(project)
        print_checked(project)

    elif cmd == 'who' and len(args) >= 2:
        prefix = ''
        raw_args = [t for t in args[1:] if t.strip()]
        if len(raw_args) >= 2:
            cand = normalize_prefix(project, raw_args[0])
            if raw_args[0].endswith('.') or f'{cand}.file' in project['blocks']:
                prefix = f'{cand}.'
                terms = raw_args[1:]
            else:
                terms = raw_args
        else:
            cand = normalize_prefix(project, raw_args[0])
            if f'{cand}.file' in project['blocks']:
                role_ids = collect_subtree(project, f'{cand}.file')
                print(format_doc(project, role_ids))
                return
            terms = raw_args
        expanded_terms = list(terms)
        for t in terms:
            for part in t.split('_'):
                if len(part) >= 5 and part not in expanded_terms:
                    expanded_terms.append(part)
        patterns = [re.compile(r'\b' + re.escape(term) + r'\b') for term in expanded_terms]
        matched = []
        for block, value in project['blocks'].items():
            if prefix and not block.startswith(prefix):
                continue
            lines = [l.strip() for l in value['text'].split('\n') if not REF.fullmatch(l.strip())]
            if any(p.search(line) for p in patterns for line in lines) or any(term in block for term in expanded_terms):
                matched.append(block)
        if not matched:
            print(f'no roles mention {terms!r}')
        else:
            out_doc = format_doc(project, matched)
            print(out_doc)

    elif cmd == 'check':
        print_checked(project)

    else:
        raise SystemExit('unknown command or invalid arguments; run tfe-code for usage')


def entrypoint() -> None:
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        print(f'error: {exc}', file=sys.stderr)
        raise SystemExit(1)


if __name__ == '__main__':
    entrypoint()
