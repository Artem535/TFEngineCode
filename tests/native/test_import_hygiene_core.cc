// Import hygiene: a recursive import skips dot-directories, and a role text that
// kept a literal marker can still be replaced by the next sync instead of
// wedging the workspace on a permanent "Role id collision".
#include <cstdlib>
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

// Builds a role marker without spelling the double braces adjacently: the
// importer reads markers in string literals too, and this suite writes sources
// that must keep such a literal.
std::string Marker(const std::string& id) {
  const std::string open(2, '{');
  const std::string close(2, '}');
  return open + id + close;
}

int main() {
  try {
    // Recursive import skips dot-directories: checkouts and caches are not
    // managed sources.
    const fs::path dot_root = fs::temp_directory_path() / "tfe-code-dot-directory-test";
    fs::remove_all(dot_root);
    Write(dot_root / "main.py", "def Main():\n    return 1\n");
    Write(dot_root / ".worktrees/branch.py", "def Branch():\n    return 2\n");
    Write(dot_root / ".cache/generated.py", "def Generated():\n    return 3\n");
    tfe::code::Workspace dot_workspace(dot_root);
    dot_workspace.Import({"."});
    const auto dot_files = dot_workspace.List();
    Expect(dot_files.find(".worktrees") == std::string::npos,
           "recursive import picked up a .worktrees checkout");
    Expect(dot_files.find(".cache") == std::string::npos,
           "recursive import picked up a dot-directory");
    Expect(dot_files.find("main.py") != std::string::npos,
           "recursive import dropped the real source");
    fs::remove_all(dot_root);

    // A code role may keep a literal marker (a string literal in the source), and
    // that must not turn the next sync into a permanent "Role id collision".
    const fs::path ghost_root = fs::temp_directory_path() / "tfe-code-ghost-marker-test";
    fs::remove_all(ghost_root);
    Write(ghost_root / "ghost.py",
          "def Answer():\n    return \"" + Marker("ghost.Thing") + "\"\n");
    tfe::code::Workspace ghost_workspace(ghost_root);
    ghost_workspace.Import({"ghost.py"});
    Write(ghost_root / "ghost.py", "def Answer():\n    return \"clean\"\n");
    ghost_workspace.Sync();
    Expect(ghost_workspace.Render().at("ghost.py") == Read(ghost_root / "ghost.py"),
           "a literal marker in a role text blocked the next sync");
    fs::remove_all(ghost_root);

    std::cout << "import_hygiene_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
