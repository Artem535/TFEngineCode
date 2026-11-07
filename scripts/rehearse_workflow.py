#!/usr/bin/env python3
"""Offline route rehearsal; no model calls, credentials, or cost estimates."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", default=os.environ.get("TFE_CODE_RENDERER"))
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    if not args.renderer:
        parser.error("provide --renderer or TFE_CODE_RENDERER")
    renderer = Path(args.renderer).resolve(strict=True)
    env = dict(os.environ, PYTHONPATH=str(ROOT / "src"), TFE_CODE_RENDERER=str(renderer))
    records = []
    with tempfile.TemporaryDirectory(prefix="tfe-code-route-") as directory:
        directory = Path(directory)
        roles, files = directory / "roles", directory / "files"
        roles.mkdir()
        (files / "out").mkdir(parents=True)

        def cli(*argv, text=None):
            result = subprocess.run([sys.executable, "-m", "tfengine_code", *argv],
                                    input=text, capture_output=True, text=True, cwd=roles, env=env)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            return result.stdout

        cli("init")
        cli("apply", text=(ROOT / "examples/hello/roles.txt").read_text())
        original = (roles / "out/hello.py").read_bytes()
        (files / "out/hello.py").write_bytes(original)

        def verify(workspace, stage):
            # Isolated consumer behavior check, separate from the generated code.
            check = ("from out.hello import greet; from out.welcome import welcome; "
                     "assert greet('Ada') == 'Hello, Ada!'; "
                     "assert welcome('Ada') == 'Welcome, Ada!'; "
                     + ("assert welcome('') == 'Welcome, friend!'; " if stage == 2 else "")
                     + "print('PASS')")
            result = subprocess.run([sys.executable, "-c", check], cwd=workspace,
                                    capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stderr)
            if (workspace / "out/hello.py").read_bytes() != original:
                raise RuntimeError("unrelated example changed")

        for stage in (1, 2):
            if stage == 1:
                context = cli("tree", "hello.py")
                assert "hello.greet" in context
                (files / "out/hello.py").read_text()
                body = 'def welcome(name):\n    return f"Welcome, {name}!"'
                document = '#@ welcome.file [file: welcome.py]\n{{welcome.body}}\n\n#@ welcome.body\n' + body + '\n'
            else:
                context = cli("who", "welcome.", "welcome")
                assert "#@ welcome.body" in context
                (files / "out/welcome.py").read_text()
                body = "def welcome(name):\n    recipient = name or 'friend'\n    return f'Welcome, {recipient}!'"
                document = '#@ welcome.body\n' + body + '\n'
            cli("apply", text=document)
            (files / "out/welcome.py").write_text(body + "\n")
            for workspace in (roles, files):
                verify(workspace, stage)
            role_text = (roles / "out/welcome.py").read_bytes()
            file_text = (files / "out/welcome.py").read_bytes()
            # Formatting can include a harmless blank line from the parent.
            if role_text.rstrip() != file_text.rstrip():
                raise RuntimeError("rendered and ordinary sources differ")
            records.append(dict(stage=stage, role_reads=1, role_applies=1,
                                file_reads=1, file_edits=1, behavior_checks_per_arm=1,
                                behavior_passed=True, original_preserved=True,
                                equivalent_outputs=True))
    report = dict(kind="deterministic_offline_rehearsal", model_requests=0,
                  financial_or_model_speed_claim=False, stages=records)
    output = json.dumps(report, indent=2) + "\n"
    if args.report:
        args.report.write_text(output)
    print(output, end="")


if __name__ == "__main__":
    main()
