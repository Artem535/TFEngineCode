// Apply transaction semantics: multi-role atomic apply, stale version rejection,
// check-command rollback, empty-old rejection and receipt invalidation.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
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
  const fs::path root = fs::temp_directory_path() / "tfe-code-apply-transaction-test";
  fs::remove_all(root);
  Write(root / "sample.py", "def greet():\n    return 'hello'\n\ndef other():\n    return 1\n");
  tfe::code::Workspace workspace(root);
  workspace.Import({"sample.py"});
  const auto project = workspace.LoadProject();
  const auto role = project.blocks.at("sample.greet");
  const auto result = workspace.Apply(
      {{"sample.greet", role.version, "'hello'", "'updated'"},
       {"sample.other", project.blocks.at("sample.other").version, "return 1", "return 2"}},
      std::nullopt);
  Expect(result.changed == std::vector<std::string>{"sample.py"}, "affected render");
  Expect(Read(root / "sample.py").find("'updated'") != std::string::npos, "apply source");
  Expect(Read(root / "sample.py").find("return 2") != std::string::npos,
         "atomic multi-role apply");
const auto before_project = Read(root / ".tfe-code/project.json");
  const auto before_source = Read(root / "sample.py");
  const auto current = workspace.LoadProject().blocks.at("sample.greet");
  bool stale = false;
  try {
    workspace.Apply({{"sample.greet", role.version, "'updated'", "'stale'"}}, std::nullopt);
  } catch (const std::exception&) {
    stale = true;
  }
  Expect(stale, "stale edit accepted");
  Expect(Read(root / ".tfe-code/project.json") == before_project, "stale project write");
  bool rollback = false;
  try {
    workspace.Apply({{"sample.greet", current.version, "'updated'", "'wrong'"}},
                    "test -f definitely-missing-check-file");
  } catch (const std::exception&) {
    rollback = true;
  }
  Expect(rollback, "failed check accepted");
  Expect(Read(root / "sample.py") == before_source, "failed check source rollback");
  Expect(Read(root / ".tfe-code/project.json") == before_project,
         "failed check project rollback");
  bool empty_old = false;
  try {
    workspace.Apply({{"sample.greet", current.version, "", "anything"}}, std::nullopt);
  } catch (const std::exception&) {
    empty_old = true;
  }
  Expect(empty_old, "empty old accepted");
  workspace.Apply({{"sample.greet", current.version, "'updated'", "'checked'"}},
                  "test -f sample.py");
  const auto checked_state = Read(root / ".tfe-code/state.json");
  workspace.Sync();
  Expect(Read(root / ".tfe-code/state.json") == checked_state,
         "clean sync invalidated receipt");
  const auto after_check = Read(root / "sample.py");
  Write(root / "sample.py", after_check + "\ndef external():\n    return 2\n");
  bool divergence = false;
  try {
    (void)workspace.LoadProject();
  } catch (const std::exception&) {
    divergence = true;
  }
  Expect(divergence, "external source edit not detected");
  workspace.Sync();
  Expect(workspace.LoadProject().blocks.contains("sample.external"),
         "sync did not import role");
  fs::remove_all(root);

  std::cout << "apply_transaction_tests passed\n";
}
