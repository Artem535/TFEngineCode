import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from tfengine_code import cli


class Documents(unittest.TestCase):
    def test_multiple_sections_and_indentation(self):
        roles, files = cli.parse_doc("#@ sample.file [file: sample.py]\n    {{sample.body}}\n#@ sample.body\nreturn 1\n")
        self.assertEqual(files, {"sample.py": "sample.file"})
        self.assertIn("    {{sample.body}}", roles["sample.file"])

    def test_no_task_specific_search_hint(self):
        self.assertNotIn("# Hint", (ROOT / "src/tfengine_code/cli.py").read_text())

    def test_output_paths(self):
        for name in ["../escape.py", "/tmp/escape.py", "a/../../x.py"]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                cli.validate_path(name)
        cli.validate_path("package/module.py")


class Workspace(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.cwd = Path(self.temp.name)
        self.env = dict(os.environ, PYTHONPATH=str(ROOT / "src"))
        self.call("init")

    def tearDown(self):
        self.temp.cleanup()

    def call(self, *args, text=None):
        return subprocess.run([sys.executable, "-m", "tfengine_code", *args], input=text,
                              text=True, capture_output=True, cwd=self.cwd, env=self.env)

    @unittest.skipUnless(os.environ.get("TFE_CODE_RENDERER"), "native renderer not configured")
    def test_double_brace_literal(self):
        result = self.call("apply", text='#@ literal.file [file: literal.py]\ns = "{{1, 2}}"\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('{{1, 2}}', (self.cwd / "out/literal.py").read_text())

    @unittest.skipUnless(os.environ.get("TFE_CODE_RENDERER"), "native renderer not configured")
    def test_parent_preserves_non_role_braces(self):
        result = self.call("apply", text='#@ a.file [file: a.py]\ns = "{{value}}"\n{{a.body}}\n#@ a.body\nx = 1\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('{{value}}', (self.cwd / "out/a.py").read_text())

    @unittest.skipUnless(os.environ.get("TFE_CODE_RENDERER"), "native renderer not configured")
    def test_nested_scope_and_indentation(self):
        result = self.call("apply", text='#@ nested.file [file: nested.py]\ndef outer():\n    {{nested.body}}\n#@ nested.body\nx = 7\ndef inner():\n    return x\nreturn inner()\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        run = subprocess.check_output([sys.executable, '-c', 'exec(open("out/nested.py").read()); print(outer())'], cwd=self.cwd, text=True)
        self.assertEqual(run.strip(), '7')

    @unittest.skipUnless(os.environ.get("TFE_CODE_RENDERER"), "native renderer not configured")
    def test_missing_role_returns_error(self):
        result = self.call("apply", text='#@ missing.file [file: missing.py]\n{{missing.body}}\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('missing roles', result.stdout)

    def test_init_does_not_overwrite(self):
        before = (self.cwd / ".roles/store.json").read_bytes()
        self.assertNotEqual(self.call("init").returncode, 0)
        self.assertEqual(before, (self.cwd / ".roles/store.json").read_bytes())

    def test_invalid_mapping_is_not_saved(self):
        before = (self.cwd / ".roles/store.json").read_bytes()
        result = self.call("apply", text="#@ a.file [file: ../escape.py]\nx = 1\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(before, (self.cwd / ".roles/store.json").read_bytes())

    def test_put_parses_multiline_document(self):
        result = self.call("put", "demo.file", text="#@ demo.file [file: demo.py]\n{{demo.body}}\n#@ demo.body\nx = 1\n")
        store = json.loads((self.cwd / ".roles/store.json").read_text())
        self.assertEqual(set(store["blocks"]), {"demo.file", "demo.body"})
        self.assertNotIn("#@", store["blocks"]["demo.file"]["text"])

    @unittest.skipUnless(os.environ.get("TFE_CODE_RENDERER"), "native renderer not configured")
    def test_native_example_and_patch(self):
        result = self.call("apply", text=(ROOT / "examples/hello/roles.txt").read_text())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        out = self.cwd / "out/hello.py"
        self.assertEqual(subprocess.check_output([sys.executable, str(out)], text=True).strip(), "Hello, world!")
        result = self.call("apply", text='#@ hello.greet\ndef greet(name):\n    return f"Welcome, {name}!"\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(subprocess.check_output([sys.executable, str(out)], text=True).strip(), "Welcome, world!")
        self.assertNotIn("# Hint", self.call("who", "hello.", "greet").stdout)

    @unittest.skipUnless(os.environ.get("TFE_CODE_RENDERER"), "native renderer not configured")
    def test_syntax_failure_returns_error(self):
        result = self.call("apply", text="#@ bad.file [file: bad.py]\ndef broken(\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("DOES NOT COMPILE", result.stdout)
