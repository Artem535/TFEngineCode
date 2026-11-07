// Import and sync behaviour: byte-exact import, recursive discovery, ignore
// rules, deletion/rename reconciliation and literal brace preservation.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "workspace.h"

namespace fs = std::filesystem;

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string Read(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}

void Expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  const fs::path root = fs::temp_directory_path() / "tfe-code-import-sync-test";
  fs::remove_all(root);
  Write(root / "sample.py", "def greet():\n    return 'hello'\n\ndef other():\n    return 1\n");
  tfe::code::Workspace workspace(root);
  workspace.Import({"sample.py"});
  const auto project = workspace.LoadProject();
  const auto role = project.blocks.at("sample.greet");
  Expect(role.text.find("return 'hello'") != std::string::npos, "show role body");
  Expect(workspace.Render().at("sample.py") == Read(root / "sample.py"), "exact import");
  fs::remove_all(root);
const fs::path recursive_root = fs::temp_directory_path() / "tfe-code-recursive-import-test";
  fs::remove_all(recursive_root);
  Write(recursive_root / "pkg/a.py", "def first():\n    return 'first needle'\n");
  Write(recursive_root / "pkg/b.py", "def second():\n    return 'second needle'\n");
  Write(recursive_root / "pkg/notes.txt", "not Python\n");
  Write(recursive_root / ".venv/skipped.py", "def skipped():\n    pass\n");
  Write(recursive_root / "build/skipped.py", "def skipped():\n    pass\n");
  fs::create_directories(recursive_root / "empty");
  const fs::path outside_root = fs::temp_directory_path() / "tfe-code-recursive-outside";
  fs::remove_all(outside_root);
  Write(outside_root / "escape.py", "def escape():\n    pass\n");
  fs::create_directory_symlink(outside_root, recursive_root / "linked");
  tfe::code::Workspace recursive(recursive_root);
  const auto imported = recursive.Import({"."});
  Expect(imported.changed == std::vector<std::string>{"pkg/a.py", "pkg/b.py"},
         "recursive import expands, sorts and deduplicates Python files");
  Expect(recursive.Render().at("pkg/a.py") == Read(recursive_root / "pkg/a.py"),
         "recursive import remains byte exact");
  Expect(recursive.Import({"pkg/a.py", "pkg/a.py"}).changed.empty(),
         "explicit duplicate import is deduplicated");
  const auto found = recursive.Search({"needle"}, "pkg.a");
  Expect(found.find("#@ pkg.a.first [version: ") != std::string::npos,
         "search returns role and version");
  Expect(found.find("pkg.b") == std::string::npos, "search prefix filters roles");
  const auto either = recursive.Search({"first", "second"}, "pkg.");
  Expect(either.find("#@ pkg.a.first") != std::string::npos &&
         either.find("#@ pkg.b.second") != std::string::npos,
         "search supports any literal query");
  Expect(recursive.Search({"absent"}, "") == "no matching roles", "empty search result");
  bool empty_directory = false;
  try {
    recursive.Import({"empty"});
  } catch (const std::exception&) {
    empty_directory = true;
  }
  Expect(empty_directory, "directory without Python files accepted");
  bool escaping_import = false;
  try {
    recursive.Import({"../outside.py"});
  } catch (const std::exception&) {
    escaping_import = true;
  }
  Expect(escaping_import, "parent traversal import accepted");
  fs::remove_all(recursive_root);
const fs::path ignore_root = fs::temp_directory_path() / "tfe-code-ignore-test";
  fs::remove_all(ignore_root);
  tfe::code::Workspace ignore_workspace(ignore_root);
  ignore_workspace.CreateIgnoreFile();
  const auto defaults = Read(ignore_root / ".tfe-codeignore");
  Expect(defaults.find(".venv/\n") != std::string::npos &&
         defaults.find("build/\n") != std::string::npos,
         "ignore command writes default rules");
  bool existing_ignore = false;
  try {
    ignore_workspace.CreateIgnoreFile();
  } catch (const std::exception&) {
    existing_ignore = true;
  }
  Expect(existing_ignore, "ignore command overwrote existing rules");
  Expect(Read(ignore_root / ".tfe-codeignore") == defaults,
         "repeated ignore command changed existing rules");
  Write(ignore_root / ".tfe-codeignore",
        "# project-specific exclusions\npkg/b.py\nbuild/\n*.tmp.py\n");
  Write(ignore_root / "pkg/a.py", "def included():\n    return 1\n");
  Write(ignore_root / "pkg/b.py", "def exact_excluded():\n    pass\n");
  Write(ignore_root / "pkg/c.tmp.py", "def wildcard_excluded():\n    pass\n");
  Write(ignore_root / "build/skipped.py", "def directory_excluded():\n    pass\n");
  Write(ignore_root / "venv/now_included.py", "def custom_defaults():\n    return 2\n");
  const auto filtered = ignore_workspace.Import({"."});
  Expect(filtered.changed == std::vector<std::string>{"pkg/a.py", "venv/now_included.py"},
         "custom ignore rules did not replace defaults during recursive import");
  fs::remove_all(ignore_root);

  const fs::path deleted_root = fs::temp_directory_path() / "tfe-code-sync-deleted-test";
  fs::remove_all(deleted_root);
  Write(deleted_root / "keep.py", "def kept():\n    return 1\n");
  Write(deleted_root / "deleted.py", "def gone():\n    return 2\n");
  tfe::code::Workspace deleted_workspace(deleted_root);
  deleted_workspace.Import({"keep.py", "deleted.py"});
  const auto kept_bytes = Read(deleted_root / "keep.py");
  fs::remove(deleted_root / "deleted.py");
  Write(deleted_root / "unrelated.py", "def unmanaged():\n    return 3\n");
  const auto deletion = deleted_workspace.Sync();
  Expect(deletion.changed == std::vector<std::string>{"deleted.py"},
         "sync did not report the deleted managed source");
  Expect(!deleted_workspace.LoadProject().files.contains("deleted.py") &&
         !deleted_workspace.LoadProject().blocks.contains("deleted.gone"),
         "sync retained deleted file roles");
  Expect(!deleted_workspace.LoadProject().files.contains("unrelated.py"),
         "sync imported an unrelated new source");
  Expect(deleted_workspace.Render().size() == 1 &&
         deleted_workspace.Render().at("keep.py") == kept_bytes,
         "sync changed retained source bytes");
  Expect(!deleted_workspace.LoadState().source_hashes.contains("deleted.py"),
         "sync retained deleted source hashes");
  Expect(deleted_workspace.Sync().changed.empty(), "repeated deletion sync is not clean");
  fs::remove_all(deleted_root);

  const fs::path empty_root = fs::temp_directory_path() / "tfe-code-sync-empty-test";
  fs::remove_all(empty_root);
  Write(empty_root / "only.py", "def only():\n    pass\n");
  tfe::code::Workspace empty_workspace(empty_root);
  empty_workspace.Import({"only.py"});
  fs::remove(empty_root / "only.py");
  Expect(empty_workspace.Sync().changed == std::vector<std::string>{"only.py"},
         "sync did not delete the final managed source");
  Expect(empty_workspace.LoadProject().files.empty() && empty_workspace.Render().empty(),
         "sync did not produce an empty project");
  fs::remove_all(empty_root);
const fs::path rename_root = fs::temp_directory_path() / "tfe-code-sync-rename-test";
  fs::remove_all(rename_root);
  Write(rename_root / "old.py", "def moved():\n    return 'same bytes'\n");
  Write(rename_root / "stay.py", "def stay():\n    return 1\n");
  tfe::code::Workspace rename_workspace(rename_root);
  rename_workspace.Import({"old.py", "stay.py"});
  fs::rename(rename_root / "old.py", rename_root / "new.py");
  const auto renamed_bytes = Read(rename_root / "new.py");
  Write(rename_root / "unrelated.py", "def unmanaged():\n    return 2\n");
  const auto renamed = rename_workspace.Sync();
  Expect(renamed.changed == std::vector<std::string>{"new.py", "old.py"},
         "sync did not report both sides of an exact rename");
  const auto renamed_project = rename_workspace.LoadProject();
  Expect(!renamed_project.files.contains("old.py") && renamed_project.files.contains("new.py") &&
         renamed_project.blocks.contains("new.moved") &&
         !renamed_project.files.contains("unrelated.py"),
         "sync did not reconcile exact rename paths and roles");
  Expect(rename_workspace.Render().at("new.py") == renamed_bytes &&
         Read(rename_root / "new.py") == renamed_bytes,
         "sync changed renamed source bytes");
  Expect(rename_workspace.Sync().changed.empty(), "repeated rename sync is not clean");
  fs::remove_all(rename_root);

  const fs::path many_root = fs::temp_directory_path() / "tfe-code-sync-many-to-one-test";
  fs::remove_all(many_root);
  const std::string duplicate = "def duplicate():\n    return 1\n";
  Write(many_root / "a.py", duplicate);
  Write(many_root / "b.py", duplicate);
  tfe::code::Workspace many_workspace(many_root);
  many_workspace.Import({"a.py", "b.py"});
  fs::remove(many_root / "a.py");
  fs::remove(many_root / "b.py");
  Write(many_root / "new.py", duplicate);
  const auto many_project = Read(many_root / ".tfe-code/project.json");
  const auto many_state = Read(many_root / ".tfe-code/state.json");
  std::string many_error;
  try {
    many_workspace.Sync();
  } catch (const std::exception& error) {
    many_error = error.what();
  }
  Expect(many_error.find("Ambiguous rename") != std::string::npos &&
         Read(many_root / ".tfe-code/project.json") == many_project &&
         Read(many_root / ".tfe-code/state.json") == many_state,
         "many-to-one rename ambiguity changed the workspace");
  fs::remove_all(many_root);

  const fs::path one_root = fs::temp_directory_path() / "tfe-code-sync-one-to-many-test";
  fs::remove_all(one_root);
  Write(one_root / "old.py", duplicate);
  tfe::code::Workspace one_workspace(one_root);
  one_workspace.Import({"old.py"});
  fs::remove(one_root / "old.py");
  Write(one_root / "a.py", duplicate);
  Write(one_root / "b.py", duplicate);
  const auto one_project = Read(one_root / ".tfe-code/project.json");
  const auto one_state = Read(one_root / ".tfe-code/state.json");
  std::string one_error;
  try {
    one_workspace.Sync();
  } catch (const std::exception& error) {
    one_error = error.what();
  }
  Expect(one_error.find("Ambiguous rename") != std::string::npos &&
         Read(one_root / ".tfe-code/project.json") == one_project &&
         Read(one_root / ".tfe-code/state.json") == one_state,
         "one-to-many rename ambiguity changed the workspace");
  fs::remove_all(one_root);

  const fs::path literal_root = fs::temp_directory_path() / "tfe-code-dotted-brace-literal-test";
  fs::remove_all(literal_root);
  Write(literal_root / "literal.py", "payload = \"{{not.a_role}}\"\n");
  tfe::code::Workspace literal_workspace(literal_root);
  literal_workspace.Import({"literal.py"});
  Expect(literal_workspace.Render().at("literal.py") == Read(literal_root / "literal.py"),
         "dotted brace literal was not imported byte-exactly");
  Write(literal_root / "literal.py", "payload = \"{{other.role}}\"\n");
  literal_workspace.Sync();
  Expect(literal_workspace.Render().at("literal.py") == Read(literal_root / "literal.py"),
         "dotted brace literal was not synced byte-exactly");
  fs::remove_all(literal_root);

  std::cout << "import_sync_tests passed\n";
}
