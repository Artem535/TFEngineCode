// A failing check must not leave a published change in the engine database.
//
// In database-authoritative mode there is no project.json mirror to restore, so
// if the batch were published before the check ran, a failed check would leave
// the database holding roles the verifier rejected while the files rolled back.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "workspace.h"

namespace fs = std::filesystem;

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

void Expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  const fs::path root = fs::temp_directory_path() / "tfe-code-objectbox-check-rollback";
  fs::remove_all(root);
  const std::string original = "def one():\n    return 1\n";
  Write(root / "pkg/one.py", original);

  setenv("TFE_CODE_INDEX", "objectbox", 1);
  {
    tfe::code::Workspace workspace(root);
    workspace.Import({"pkg/one.py"});
  }

  // Drop the recovery mirror so the database is the only record of the tree.
  fs::remove(root / ".tfe-code/project.json");
  fs::remove(root / ".tfe-code/state.json");
  fs::remove(root / "pkg/one.py");
  Expect(!fs::exists(root / ".tfe-code/project.json"), "mirror was not removed");

  const std::string role = "pkg.one.one";
  bool rejected = false;
  {
    tfe::code::Workspace workspace(root);
    const auto project = workspace.LoadProject();
    Expect(project.blocks.contains(role), "database did not serve the imported role");
    const auto version = project.blocks.at(role).version;
    try {
      (void)workspace.Apply({{role, version, "return 1", "return 2"}}, std::string("false"));
    } catch (const std::exception&) {
      rejected = true;
    }
  }
  Expect(rejected, "a failing check must reject the batch");

  {
    tfe::code::Workspace workspace(root);
    const auto project = workspace.LoadProject();
    Expect(project.blocks.contains(role), "role disappeared after a rejected batch");
    Expect(project.blocks.at(role).text == "def one():\n    return 1",
           "a rejected batch left its edit published in the database");
  }

  unsetenv("TFE_CODE_INDEX");
  fs::remove_all(root);
  std::cout << "objectbox_check_rollback_tests passed\n";
  return 0;
}