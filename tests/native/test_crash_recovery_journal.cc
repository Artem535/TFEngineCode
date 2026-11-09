// Crash recovery: an interrupted commit leaves a write-ahead journal that the
// next command rolls back; malformed journals must be rejected without touching
// managed files or recovery evidence.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
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
for (const auto& crash_target : {fs::path("a.py"), fs::path(".tfe-code/project.json"),
                                   fs::path(".tfe-code/state.json")}) {
    const fs::path crash_root = fs::temp_directory_path() /
                                ("tfe-code-crash-" + crash_target.filename().string());
    fs::remove_all(crash_root);
    Write(crash_root / "a.py", "def one():\n    return 1\n");
    Write(crash_root / "b.py", "def two():\n    return 2\n");
    tfe::code::Workspace crash_workspace(crash_root);
    crash_workspace.Import({"a.py", "b.py"});
    const auto crash_project = crash_workspace.LoadProject();
    const std::vector<tfe::code::RoleEdit> edits{
        {"a.one", crash_project.blocks.at("a.one").version, "return 1", "return 10"},
        {"b.two", crash_project.blocks.at("b.two").version, "return 2", "return 20"}};
    const auto before_a = Read(crash_root / "a.py");
    const auto before_b = Read(crash_root / "b.py");
    const auto before_project = Read(crash_root / ".tfe-code/project.json");
    const auto before_state = Read(crash_root / ".tfe-code/state.json");
    const auto absolute_target = crash_root / crash_target;
    const pid_t child = fork();
    Expect(child >= 0, "cannot fork crash test");
    if (child == 0) {
      setenv("TFE_CODE_TEST_CRASH_AFTER", absolute_target.c_str(), 1);
      try {
        crash_workspace.Apply(edits, std::nullopt);
      } catch (const std::exception&) {
        _exit(1);
      }
      _exit(0);
    }
    int status = 0;
    Expect(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == 90,
           "commit did not terminate at the requested write boundary");
    Expect(fs::exists(crash_root / ".tfe-code/transaction.json"),
           "interrupted commit did not leave a recovery journal");
    Expect(crash_workspace.Render().at("a.py") == before_a &&
           crash_workspace.Render().at("b.py") == before_b &&
           Read(crash_root / ".tfe-code/project.json") == before_project &&
           Read(crash_root / ".tfe-code/state.json") == before_state &&
           !fs::exists(crash_root / ".tfe-code/transaction.json"),
           "next workspace command did not restore the pre-commit snapshot");
    const auto retried = crash_workspace.Apply(edits, std::nullopt);
    Expect(retried.changed == std::vector<std::string>{"a.py", "b.py"},
           "recovered batch could not be retried");
    fs::remove_all(crash_root);
  }

  // The crash hook must match a workspace reached through a symlinked root.
  // Source writes are canonicalised by SafePath(), so before the hook compared
  // canonical paths the injection point was never reached on macOS, where TMPDIR
  // lives under /var (a symlink to /private/var) and the crash tests silently
  // applied the batch instead of dying at the requested boundary.
  {
    const fs::path symlink_real_root = fs::temp_directory_path() / "tfe-code-crash-symlink-real";
    const fs::path symlink_root = fs::temp_directory_path() / "tfe-code-crash-symlink";
    fs::remove_all(symlink_root);
    fs::remove_all(symlink_real_root);
    fs::create_directories(symlink_real_root);
    fs::create_directory_symlink(symlink_real_root, symlink_root);
    Write(symlink_root / "a.py", "def one():\n    return 1\n");
    tfe::code::Workspace symlinked_workspace(symlink_root);
    symlinked_workspace.Import({"a.py"});
    const auto symlinked_project = symlinked_workspace.LoadProject();
    const std::vector<tfe::code::RoleEdit> symlinked_edits{
        {"a.one", symlinked_project.blocks.at("a.one").version, "return 1", "return 10"}};
    const auto symlinked_before = Read(symlink_root / "a.py");
    const auto symlinked_target = symlink_root / "a.py";
    const pid_t child = fork();
    Expect(child >= 0, "cannot fork crash test");
    if (child == 0) {
      setenv("TFE_CODE_TEST_CRASH_AFTER", symlinked_target.c_str(), 1);
      try {
        symlinked_workspace.Apply(symlinked_edits, std::nullopt);
      } catch (const std::exception&) {
        _exit(1);
      }
      _exit(0);
    }
    int status = 0;
    Expect(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
               WEXITSTATUS(status) == 90,
           "crash hook ignored a symlinked workspace root");
    Expect(fs::exists(symlink_root / ".tfe-code/transaction.json"),
           "interrupted commit through a symlinked root left no recovery journal");
    Expect(symlinked_workspace.Render().at("a.py") == symlinked_before &&
               !fs::exists(symlink_root / ".tfe-code/transaction.json"),
           "symlinked-root recovery did not restore the pre-commit snapshot");
    fs::remove_all(symlink_root);
    fs::remove_all(symlink_real_root);
  }

const std::vector<std::pair<std::string, std::string>> malformed_journals{
      {"missing_original",
       R"({"schema":1,"entries":[{"path":".tfe-code/project.json"},{"path":".tfe-code/state.json","original":null}]})"},
      {"empty_entries", R"({"schema":1,"entries":[]})"},
      {"missing_state",
       R"({"schema":1,"entries":[{"path":".tfe-code/project.json","original":null}]})"},
      {"alias_duplicate",
       R"({"schema":1,"entries":[{"path":".tfe-code/project.json","original":null},{"path":".tfe-code/state.json","original":null},{"path":"a.py","original":null},{"path":"./a.py","original":null}]})"},
      {"extra_field",
       R"({"schema":1,"entries":[{"path":".tfe-code/project.json","original":null,"unexpected":true},{"path":".tfe-code/state.json","original":null}]})"}};
  for (const auto& [name, contents] : malformed_journals) {
    const fs::path malformed_root = fs::temp_directory_path() / ("tfe-code-journal-" + name);
    fs::remove_all(malformed_root);
    Write(malformed_root / "a.py", "def one():\n    return 1\n");
    tfe::code::Workspace malformed_workspace(malformed_root);
    malformed_workspace.Import({"a.py"});
    const auto before_a = Read(malformed_root / "a.py");
    const auto before_project = Read(malformed_root / ".tfe-code/project.json");
    const auto before_state = Read(malformed_root / ".tfe-code/state.json");
    const auto journal = malformed_root / ".tfe-code/transaction.json";
    Write(journal, contents);
    bool rejected = false;
    try {
      (void)malformed_workspace.Render();
    } catch (const std::exception&) {
      rejected = true;
    }
    Expect(rejected && Read(journal) == contents && Read(malformed_root / "a.py") == before_a &&
           Read(malformed_root / ".tfe-code/project.json") == before_project &&
           Read(malformed_root / ".tfe-code/state.json") == before_state,
           "malformed journal changed recovery evidence or managed files: " + name);
    fs::remove_all(malformed_root);
  }

  std::cout << "crash_recovery_tests passed\n";
}
