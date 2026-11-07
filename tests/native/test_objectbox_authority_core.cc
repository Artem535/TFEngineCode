// Database-authoritative workspace: with TFE_CODE_INDEX=objectbox the role tree
// lives only in the engine's ObjectBox store. Removing the recovery mirror and
// every generated source must still leave a renderable workspace, and
// materialising it again must reproduce the original bytes exactly.
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

std::string Read(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}

void Expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  const fs::path root = fs::temp_directory_path() / "tfe-code-objectbox-authority-test";
  fs::remove_all(root);
  const std::string source = "def one():\n    return 1\n";
  Write(root / "pkg/one.py", source);

  setenv("TFE_CODE_INDEX", "objectbox", 1);
  {
    tfe::code::Workspace workspace(root);
    workspace.Import({"pkg/one.py"});
    Expect(fs::exists(root / ".tfe-code/db"), "import did not create an ObjectBox store");
    Expect(workspace.Render().at("pkg/one.py") == source, "render changed the bytes");
  }

  // Drop the derived mirror and every generated file: only the database remains.
  fs::remove(root / ".tfe-code/project.json");
  fs::remove(root / ".tfe-code/state.json");
  fs::remove(root / "pkg/one.py");
  Expect(!fs::exists(root / ".tfe-code/project.json"), "mirror was not removed");

  {
    tfe::code::Workspace workspace(root);
    const auto rendered = workspace.Render();
    Expect(rendered.size() == 1 && rendered.at("pkg/one.py") == source,
           "a database-only workspace did not render the role tree");
    const auto written = workspace.Materialize();
    Expect(written.size() == 1, "materialise did not report the written file");
    Expect(Read(root / "pkg/one.py") == source,
           "materialised source differs from the original bytes");
  }

  // A second command must not rewrite an already materialised, identical file.
  {
    tfe::code::Workspace workspace(root);
    Expect(workspace.Materialize().empty(), "materialise rewrote an identical file");
  }

  unsetenv("TFE_CODE_INDEX");
  fs::remove_all(root);
  std::cout << "objectbox_authority_tests passed\n";
  return 0;
}