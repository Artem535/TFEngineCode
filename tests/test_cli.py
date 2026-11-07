import json
import os
from pathlib import Path
import shutil
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
        result = self.call("put", "demo.file", text="#@ demo.file [file: demo.py]\n{{demo.body}}\n#@ demo.body\\nx = 1\\n".replace("\\n", "\n"))
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


@unittest.skipUnless(os.environ.get("TFE_CODE_NATIVE"), "native workspace CLI not configured")
class NativeImportedWorkspace(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.cwd = Path(self.temp.name)
        self.binary = Path(os.environ["TFE_CODE_NATIVE"]).resolve()

    def tearDown(self):
        self.temp.cleanup()

    def call(self, *args, text=None, cwd=None):
        return subprocess.run([self.binary, *args], input=text, text=True,
                              capture_output=True, cwd=cwd or self.cwd)

    def show(self, role, cwd=None):
        result = self.call("show", role, "--json", cwd=cwd)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(result.stdout)[role]

    def apply(self, role, old, new, *extra, cwd=None):
        current = self.show(role, cwd=cwd)
        payload = json.dumps({"edits": [{"role": role, "expected_version": current["version"],
                                        "old": old, "new": new}]})
        return self.call("apply", *extra, text=payload, cwd=cwd)

    def test_real_package_fresh_process_apply_check_and_sync(self):
        shutil.copytree(ROOT / "src", self.cwd / "src")
        shutil.copytree(ROOT / "tests", self.cwd / "tests")
        files = [str(path.relative_to(self.cwd)) for path in (self.cwd / "src").rglob("*.py")]
        result = self.call("import", *files)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        outline = self.call("outline", "src/tfengine_code/cli.py", "--depth", "1")
        self.assertIn("src.tfengine_code.cli.main", outline.stdout)
        command = ("env -u TFE_CODE_NATIVE -u TFE_CODE_RENDERER PYTHONPATH=src "
                   f"{sys.executable} -m unittest discover -s tests -v")
        first = self.apply("src.tfengine_code.cli.validate_path", "file paths must stay inside out/",
                           "managed paths stay inside workspace", "--check", command)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        second = self.apply("src.tfengine_code.cli.normalize_prefix", "p = raw_prefix.strip().rstrip('.')",
                            "p = raw_prefix.strip()\n    p = p.rstrip('.')", "--check", command)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        source = self.cwd / "src/tfengine_code/cli.py"
        source.write_text(source.read_text() + "\n\ndef external_sync():\n    return 42\n")
        stale = self.call("show", "src.tfengine_code.cli.external_sync", "--json")
        self.assertNotEqual(stale.returncode, 0)
        self.assertIn("sync", stale.stderr)
        self.assertEqual(self.call("sync").returncode, 0)
        self.assertIn("return 42", self.show("src.tfengine_code.cli.external_sync")["text"])
        changed = self.apply("src.tfengine_code.cli.external_sync", "return 42", "return 43",
                             "--check", command)
        self.assertEqual(changed.returncode, 0, changed.stdout + changed.stderr)

    def test_failed_unittest_rolls_back_every_file(self):
        (self.cwd / "sample.py").write_text("def value():\n    return 1\n")
        (self.cwd / "test_sample.py").write_text(
            "import unittest\nfrom sample import value\n"
            "class Sample(unittest.TestCase):\n    def test_value(self):\n        self.assertEqual(value(), 1)\n")
        self.assertEqual(self.call("import", "sample.py", "test_sample.py").returncode, 0)
        managed = [self.cwd / "sample.py", self.cwd / "test_sample.py",
                   self.cwd / ".tfe-code/project.json", self.cwd / ".tfe-code/state.json"]
        before = {path: path.read_bytes() for path in managed}
        result = self.apply("sample.value", "return 1", "return 2", "--check",
                            f"{sys.executable} -m unittest -v")
        self.assertNotEqual(result.returncode, 0)
        after = {path: path.read_bytes() for path in managed}
        self.assertEqual(after, before)
        checked = self.apply("sample.value", "return 1", "return 1", "--check", "printf check-noise")
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertTrue(json.loads(checked.stdout)["checked"])
        self.assertIn("check-noise", checked.stderr)

    def test_sync_deleted_managed_file(self):
        (self.cwd / "keep.py").write_text("def kept():\n    return 1\n")
        (self.cwd / "deleted.py").write_text("def gone():\n    return 2\n")
        imported = self.call("import", "keep.py", "deleted.py")
        self.assertEqual(imported.returncode, 0, imported.stdout + imported.stderr)
        (self.cwd / "deleted.py").unlink()
        stale = self.call("show", "deleted.gone", "--json")
        self.assertNotEqual(stale.returncode, 0)
        self.assertIn("run sync", stale.stderr)
        synced = self.call("sync")
        self.assertEqual(synced.returncode, 0, synced.stdout + synced.stderr)
        self.assertEqual(json.loads(synced.stdout)["changed"], ["deleted.py"])
        rendered = json.loads(self.call("render").stdout)
        self.assertEqual(set(rendered), {"keep.py"})
        self.assertNotIn("deleted.gone", self.call("outline").stdout)
        repeated = self.call("sync")
        self.assertEqual(repeated.returncode, 0, repeated.stdout + repeated.stderr)
        self.assertEqual(json.loads(repeated.stdout)["changed"], [])

    def test_sync_deleted_source_index_conflict(self):
        (self.cwd / "deleted.py").write_text("def gone():\n    return 2\n")
        self.assertEqual(self.call("import", "deleted.py").returncode, 0)
        project = self.cwd / ".tfe-code/project.json"
        project.write_bytes(project.read_bytes() + b"\n")
        before = project.read_bytes()
        (self.cwd / "deleted.py").unlink()
        conflict = self.call("sync")
        self.assertNotEqual(conflict.returncode, 0)
        self.assertIn("Both project and source changed", conflict.stderr)
        self.assertEqual(project.read_bytes(), before)

    def test_sync_exact_rename(self):
        source = "def moved():\n    return 'same bytes'\n"
        (self.cwd / "old.py").write_text(source)
        self.assertEqual(self.call("import", "old.py").returncode, 0)
        (self.cwd / "old.py").rename(self.cwd / "new.py")
        synced = self.call("sync")
        self.assertEqual(synced.returncode, 0, synced.stdout + synced.stderr)
        self.assertEqual(json.loads(synced.stdout)["changed"], ["new.py", "old.py"])
        self.assertEqual(self.show("new.moved")["text"], "def moved():\n    return 'same bytes'")
        old = self.call("show", "old.moved", "--json")
        self.assertNotEqual(old.returncode, 0)
        self.assertIn("new.moved", self.call("outline").stdout)
        self.assertNotIn("old.moved", self.call("outline").stdout)
        rendered = self.call("render")
        self.assertEqual(rendered.returncode, 0, rendered.stdout + rendered.stderr)
        self.assertEqual(json.loads(rendered.stdout), {"new.py": source})
        self.assertEqual((self.cwd / "new.py").read_text(), source)

    def test_sync_ambiguous_rename_recovery(self):
        source = "def moved():\n    return 'same bytes'\n"
        (self.cwd / "old.py").write_text(source)
        self.assertEqual(self.call("import", "old.py").returncode, 0)
        (self.cwd / "old.py").rename(self.cwd / "new.py")
        shutil.copy2(self.cwd / "new.py", self.cwd / "duplicate.py")
        ambiguous = self.call("sync")
        self.assertNotEqual(ambiguous.returncode, 0)
        self.assertIn("Ambiguous rename", ambiguous.stderr)
        (self.cwd / "duplicate.py").rename(self.cwd / "duplicate.txt")
        recovered = self.call("sync")
        self.assertEqual(recovered.returncode, 0, recovered.stdout + recovered.stderr)
        self.assertEqual(json.loads(recovered.stdout)["changed"], ["new.py", "old.py"])
        self.assertEqual(json.loads(self.call("render").stdout), {"new.py": source})

    def test_interrupted_apply_recovers_on_next_command(self):
        source_a = "def one():\n    return 1\n"
        source_b = "def two():\n    return 2\n"
        (self.cwd / "a.py").write_text(source_a)
        (self.cwd / "b.py").write_text(source_b)
        imported = self.call("import", "a.py", "b.py")
        self.assertEqual(imported.returncode, 0, imported.stdout + imported.stderr)
        first = self.show("a.one")
        second = self.show("b.two")
        payload = json.dumps({"edits": [
            {"role": "a.one", "expected_version": first["version"],
             "old": "return 1", "new": "return 10"},
            {"role": "b.two", "expected_version": second["version"],
             "old": "return 2", "new": "return 20"}]})
        env = dict(os.environ, TFE_CODE_TEST_CRASH_AFTER=str(self.cwd / "a.py"))
        crashed = subprocess.run([self.binary, "apply"], input=payload, text=True,
                                 capture_output=True, cwd=self.cwd, env=env)
        self.assertEqual(crashed.returncode, 90, crashed.stdout + crashed.stderr)
        journal = self.cwd / ".tfe-code/transaction.json"
        self.assertTrue(journal.exists())
        self.assertIn("return 10", (self.cwd / "a.py").read_text())
        recovered = self.show("a.one")
        self.assertEqual((recovered["version"], recovered["text"]),
                         (first["version"], first["text"]))
        self.assertEqual((self.cwd / "a.py").read_text(), source_a)
        self.assertEqual((self.cwd / "b.py").read_text(), source_b)
        self.assertFalse(journal.exists())
        retried = self.call("apply", text=payload)
        self.assertEqual(retried.returncode, 0, retried.stdout + retried.stderr)
        self.assertEqual(json.loads(retried.stdout)["changed"], ["a.py", "b.py"])

    def test_source_line_ranges_follow_apply_and_sync(self):
        (self.cwd / "sample.py").write_text(
            "def first():\n    return 1\n\ndef second():\n    return 2\n")
        imported = self.call("import", "sample.py")
        self.assertEqual(imported.returncode, 0, imported.stdout + imported.stderr)
        outline = self.call("outline", "sample.py", "--depth", "1")
        self.assertIn("sample.first (2 lines; source 1-2)", outline.stdout)
        self.assertIn("sample.second (2 lines; source 4-5)", outline.stdout)
        first = self.show("sample.first")
        self.assertEqual(first["source_start_line"], 1)
        self.assertEqual(first["source_end_line"], 2)
        self.assertIn("[lines: 1-2]", self.call("show", "sample.first").stdout)
        applied = self.apply("sample.first", "return 1", "if True:\n        return 2",
                             "--check", f"{sys.executable} -m py_compile sample.py")
        self.assertEqual(applied.returncode, 0, applied.stdout + applied.stderr)
        self.assertEqual(self.show("sample.first")["source_end_line"], 3)
        second = self.show("sample.second")
        self.assertEqual((second["source_start_line"], second["source_end_line"]), (5, 6))
        source = self.cwd / "sample.py"
        source.write_text(source.read_text().replace("def second():", "\n\ndef second():"))
        synced = self.call("sync")
        self.assertEqual(synced.returncode, 0, synced.stdout + synced.stderr)
        self.assertEqual(self.show("sample.first")["source_end_line"], 3)
        second = self.show("sample.second")
        self.assertEqual((second["source_start_line"], second["source_end_line"]), (7, 8))

    def test_recursive_directory_import(self):
        (self.cwd / "pkg").mkdir()
        (self.cwd / "pkg/a.py").write_text("def first():\n    return 'first needle'\n")
        (self.cwd / "pkg/b.py").write_text("def second():\n    return 'second needle'\n")
        (self.cwd / "pkg/notes.txt").write_text("not Python\n")
        for directory in (".venv", "build"):
            (self.cwd / directory).mkdir()
            (self.cwd / directory / "skipped.py").write_text("def skipped():\n    pass\n")
        (self.cwd / "empty").mkdir()
        result = self.call("import", ".")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["changed"], ["pkg/a.py", "pkg/b.py"])
        outline = self.call("outline")
        self.assertIn("pkg.a.first", outline.stdout)
        self.assertIn("pkg.b.second", outline.stdout)
        self.assertNotIn("skipped", outline.stdout)
        duplicate = self.call("import", "pkg/a.py", "pkg/a.py")
        self.assertEqual(duplicate.returncode, 0, duplicate.stdout + duplicate.stderr)
        self.assertEqual(json.loads(duplicate.stdout)["changed"], [])
        self.assertNotEqual(self.call("import", "empty").returncode, 0)
        self.assertNotEqual(self.call("import", "../outside.py").returncode, 0)

    def test_ignore_generates_defaults_without_overwriting(self):
        created = self.call("ignore")
        self.assertEqual(created.returncode, 0, created.stdout + created.stderr)
        ignore_file = self.cwd / ".tfe-codeignore"
        self.assertIn(".venv/", ignore_file.read_text())
        self.assertIn("build/", ignore_file.read_text())
        before = ignore_file.read_bytes()
        repeated = self.call("ignore")
        self.assertEqual(repeated.returncode, 1, repeated.stdout + repeated.stderr)
        self.assertEqual(ignore_file.read_bytes(), before)
        self.assertEqual(self.call("ignore", "extra").returncode, 2)
        (self.cwd / "pkg").mkdir()
        (self.cwd / "pkg/a.py").write_text("def included():\n    return 1\n")
        for directory in (".venv", "build"):
            (self.cwd / directory).mkdir()
            (self.cwd / directory / "skipped.py").write_text("def skipped():\n    pass\n")
        imported = self.call("import", ".")
        self.assertEqual(imported.returncode, 0, imported.stdout + imported.stderr)
        self.assertEqual(json.loads(imported.stdout)["changed"], ["pkg/a.py"])

    def test_custom_ignore_rejects_negation_and_bypasses_for_explicit_files(self):
        (self.cwd / "pkg").mkdir()
        (self.cwd / "pkg/a.py").write_text("def included():\n    return 1\n")
        (self.cwd / "pkg/b.py").write_text("def excluded():\n    return 2\n")
        ignore_file = self.cwd / ".tfe-codeignore"
        ignore_file.write_text("pkg/b.py\n")
        imported = self.call("import", ".")
        self.assertEqual(imported.returncode, 0, imported.stdout + imported.stderr)
        self.assertEqual(json.loads(imported.stdout)["changed"], ["pkg/a.py"])
        explicit = self.call("import", "pkg/b.py")
        self.assertEqual(explicit.returncode, 0, explicit.stdout + explicit.stderr)
        ignore_file.write_text("!important.py\n")
        negated = self.call("import", ".")
        self.assertNotEqual(negated.returncode, 0)
        self.assertIn("ignore negation is unsupported", negated.stderr)

    def test_search_who_and_checked_apply(self):
        (self.cwd / "pkg").mkdir()
        (self.cwd / "pkg/a.py").write_text("def first():\n    return 'first needle'\n")
        (self.cwd / "pkg/b.py").write_text("def second():\n    return 'second needle'\n")
        self.assertEqual(self.call("import", ".").returncode, 0)
        found = self.call("search", "needle")
        self.assertEqual(found.returncode, 0, found.stdout + found.stderr)
        self.assertIn("#@ pkg.a.first [version: ", found.stdout)
        self.assertIn("#@ pkg.b.second [version: ", found.stdout)
        self.assertIn("return 'first needle'", found.stdout)
        prefixed = self.call("search", "first", "second", "--prefix", "pkg.a")
        self.assertIn("pkg.a.first", prefixed.stdout)
        self.assertNotIn("pkg.b.second", prefixed.stdout)
        regex_found = self.call("search", r"def\s+first", "--regex")
        self.assertEqual(regex_found.returncode, 0, regex_found.stderr)
        self.assertIn("pkg.a.first", regex_found.stdout)
        self.assertNotIn("pkg.b.second", regex_found.stdout)

        regex_ids = self.call("search", r"return\s+'second", "--ids", "--regex")
        self.assertEqual(regex_ids.returncode, 0, regex_ids.stderr)
        self.assertIn("pkg.b.second:     return 'second needle'", regex_ids.stdout)

        bad_regex = self.call("search", r"[invalid(", "--regex")
        self.assertNotEqual(bad_regex.returncode, 0)
        self.assertIn("Invalid regular expression", bad_regex.stderr)

        compatible = self.call("who", "pkg.", "second")
        self.assertEqual(compatible.returncode, 0, compatible.stdout + compatible.stderr)
        self.assertIn("pkg.b.second", compatible.stdout)
        self.assertNotIn("pkg.a.first", compatible.stdout)
        self.assertEqual(self.call("search", "absent").stdout.strip(), "no matching roles")
        self.assertNotEqual(self.call("search").returncode, 0)
        self.assertNotEqual(self.call("search", "needle", "--prefix").returncode, 0)
        checked = self.apply("pkg.a.first", "'first needle'", "'changed'", "--check",
                             f"{sys.executable} -c \"from pkg.a import first; assert first() == 'changed'\"")
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_search_file_returns_editable_role_without_other_files(self):
        (self.cwd / "pkg").mkdir()
        (self.cwd / "pkg/a.py").write_text("def target():\n    return 'needle'\n")
        (self.cwd / "pkg/b.py").write_text("def target():\n    return 'needle'\n")
        self.assertEqual(self.call("import", ".").returncode, 0)
        found = self.call("search", "target", "--file", "pkg/a.py")
        self.assertEqual(found.returncode, 0, found.stderr)
        self.assertIn("#@ pkg.a.target [version: ", found.stdout)
        self.assertIn("return 'needle'", found.stdout)
        self.assertNotIn("pkg.b", found.stdout)
        version = found.stdout.split("#@ pkg.a.target [version: ", 1)[1].split("]", 1)[0]
        payload = json.dumps({"edits": [{"role": "pkg.a.target", "expected_version": version,
                                         "old": "'needle'", "new": "'changed'"}]})
        applied = self.call("apply", text=payload)
        self.assertEqual(applied.returncode, 0, applied.stderr)
        self.assertIn("'changed'", (self.cwd / "pkg/a.py").read_text())
        self.assertIn("'needle'", (self.cwd / "pkg/b.py").read_text())
        self.assertNotEqual(self.call("search", "target", "--file", "missing.py").returncode, 0)
        self.assertNotEqual(self.call("search", "target", "--file").returncode, 0)

    def test_role_lookup_exact_name_and_ambiguity(self):
        (self.cwd / "sample.py").write_text(
            "class A:\n    def target(self):\n        return 1\n\n"
            "class B:\n    def target(self):\n        return 2\n\n"
            "def target_extra():\n    return 3\n")
        self.assertEqual(self.call("import", "sample.py").returncode, 0)
        result = self.call("role", "sample.py", "A.target")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("#@ sample.A.target [version: ", result.stdout)
        self.assertIn("return 1", result.stdout)
        self.assertNotIn("sample.B.target", result.stdout)
        ambiguous = self.call("role", "sample.py", "target")
        self.assertNotEqual(ambiguous.returncode, 0)
        self.assertIn("sample.A.target", ambiguous.stderr)
        self.assertIn("sample.B.target", ambiguous.stderr)
        self.assertNotIn("target_extra", ambiguous.stderr)
        self.assertNotEqual(self.call("role", "sample.py", "missing").returncode, 0)
        self.assertNotEqual(self.call("role", "missing.py", "target").returncode, 0)
        self.assertNotEqual(self.call("role", "sample.py", "").returncode, 0)
        version = result.stdout.split("#@ sample.A.target [version: ", 1)[1].split("]", 1)[0]
        payload = json.dumps({"edits": [{"role": "sample.A.target", "expected_version": version,
                                         "old": "return 1", "new": "return 4"}]})
        applied = self.call("apply", text=payload)
        self.assertEqual(applied.returncode, 0, applied.stderr)
        self.assertIn("return 4", (self.cwd / "sample.py").read_text())

    def test_unknown_role_points_to_outline(self):
        (self.cwd / "sample.py").write_text("def value():\n    return 1\n")
        self.assertEqual(self.call("import", "sample.py").returncode, 0)
        result = self.call("show", "sample/value", "--json")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("use outline to list exact role IDs", result.stderr)

    def test_apply_mismatch_reports_count_and_show_contract(self):
        (self.cwd / "sample.py").write_text("def value():\n    return 1\n    return 2\n")
        self.assertEqual(self.call("import", "sample.py").returncode, 0)
        missing = self.apply("sample.value", "        return 1", "        return 3")
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn("old matched 0 times", missing.stderr)
        self.assertIn("show sample.value --json", missing.stderr)
        self.assertIn("dedented", missing.stderr)
        ambiguous = self.apply("sample.value", "return", "result")
        self.assertNotEqual(ambiguous.returncode, 0)
        self.assertIn("old matched 2 times", ambiguous.stderr)

    def test_context_compact_patch_and_amend(self):
        (self.cwd / "src/app").mkdir(parents=True)
        (self.cwd / "src/app/events.py").write_text(
            "from dataclasses import dataclass\n\n\n"
            "@dataclass\nclass Event:\n    kind: str = 'click'\n\n"
            "    def label(self) -> str:\n        return self.kind\n")
        (self.cwd / "src/app/bus.py").write_text(
            "from app.events import Event\n\n\n"
            "class Bus:\n"
            "    def publish(self, kind: str) -> Event:\n"
            "        ev = Event(kind=kind)\n"
            "        return ev\n\n"
            "    def close(self) -> str:\n"
            "        return 'unused sibling'\n")
        self.assertEqual(self.call("import", "src").returncode, 0)
        ctx = self.call("context", "class Event", "def publish")
        self.assertEqual(ctx.returncode, 0, ctx.stderr)
        self.assertIn("#@ events.file [v:", ctx.stdout)
        self.assertIn("#@ events.Event [v:", ctx.stdout)
        self.assertIn("#@ bus.Bus.publish [v:", ctx.stdout)
        self.assertIn("omitted 1 unchanged role: close", ctx.stdout)
        self.assertNotIn("unused sibling", ctx.stdout)

        ctx_callers = self.call("context", "Event", "--file", "src/app/events.py", "--callers")
        self.assertEqual(ctx_callers.returncode, 0, ctx_callers.stderr)
        self.assertIn("# Callers:", ctx_callers.stdout)
        self.assertIn("bus.Bus.publish: def publish(self, kind: str) -> Event:", ctx_callers.stdout)

        def ver_for(alias):
            marker = f"#@ {alias} [v:"
            return ctx.stdout.split(marker, 1)[1].split("]", 1)[0]

        patch = (
            f"#@ events.Event [v:{ver_for('events.Event')}]\n"
            "<<<\n"
            "    kind: str = 'click'\n"
            "===\n"
            "    kind: str = 'click'\n"
            "    level: str = 'wrong'\n"
            ">>>\n\n"
            f"#@ bus.Bus.publish [v:{ver_for('bus.Bus.publish')}]\n"
            "<<<\n"
            "def publish(self, kind: str) -> Event:\n"
            "===\n"
            "def publish(self, kind: str, level: str = 'info') -> Event:\n"
            ">>>\n"
            "<<<\n"
            "    ev = Event(kind=kind)\n"
            "===\n"
            "    ev = Event(kind=kind, level=level)\n"
            ">>>\n"
        )
        check_cmd = (
            f"PYTHONPATH=src {sys.executable} -c \""
            "from app.events import Event; from app.bus import Bus; "
            "assert Event().level == 'info'; "
            "assert Bus().publish('x', 'warn').level == 'warn'\""
        )
        failed = self.call("apply", "--check", check_cmd, text=patch)
        self.assertEqual(failed.returncode, 1)
        self.assertIn("Check failed", failed.stderr)
        self.assertTrue((self.cwd / ".tfe-code/last_failed_batch.json").exists())

        amend = (
            f"#@ events.Event [v:{ver_for('events.Event')}]\n"
            "<<<\n"
            "    kind: str = 'click'\n"
            "===\n"
            "    kind: str = 'click'\n"
            "    level: str = 'info'\n"
            ">>>\n"
        )
        passed = self.call("apply", "--amend", "--check", check_cmd, text=amend)
        self.assertEqual(passed.returncode, 0, passed.stderr)
        self.assertEqual(json.loads(passed.stdout)["changed"],
                         ["src/app/bus.py", "src/app/events.py"])
        self.assertFalse((self.cwd / ".tfe-code/last_failed_batch.json").exists())

    def test_conflict_and_experiment_differential(self):
        original = (ROOT.parent / "TextFoundryEngine/experiments/code-fragments-spike/"
                    "role_tree_doc/original.py")
        if not original.exists():
            original = (ROOT / "TextFoundryEngine/experiments/code-fragments-spike/"
                        "role_tree_doc/original.py")
        if not original.exists():
            self.skipTest("TextFoundryEngine spike fixture not found")
        shutil.copy2(original, self.cwd / "critic_node.py")
        result = self.call("import", "critic_node.py")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        native = json.loads(self.call("render").stdout)["critic_node.py"].encode()
        self.assertEqual(native, original.read_bytes())
        renderer = os.environ.get("TFE_CODE_RENDERER")
        if renderer:
            rendered = subprocess.run([renderer, ".tfe-code/project.json", "render-all", "0"],
                                      cwd=self.cwd, text=True, capture_output=True, check=True)
            self.assertEqual(json.loads(rendered.stdout)["critic_node.py"].encode(), original.read_bytes())
        project_path = self.cwd / ".tfe-code/project.json"
        project_path.write_text(project_path.read_text() + "\n")
        source_path = self.cwd / "critic_node.py"
        source_path.write_bytes(source_path.read_bytes() + b"\n")
        before = source_path.read_bytes()
        conflict = self.call("sync")
        self.assertNotEqual(conflict.returncode, 0)
        self.assertIn("Both project and source changed", conflict.stderr)
        self.assertEqual(source_path.read_bytes(), before)
