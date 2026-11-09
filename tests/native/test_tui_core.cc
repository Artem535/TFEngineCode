// `tui` dashboard data contracts: the structured TuiSnapshot feeds the FTXUI
// dashboard, and the binary degrades to a usage error without a terminal.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "tui.h"
#include "workspace.h"

namespace fs = std::filesystem;

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

void Expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string Quote(const std::string& value) {
  std::string quoted = "'";
  for (const char c : value) {
    if (c == '\'') quoted += "'\\''";
    else quoted += c;
  }
  return quoted + "'";
}

struct Result {
  int code = -1;
  std::string out;
  std::string err;
};

// Runs the native CLI in the workspace root, capturing both streams and the exit
// code without involving a shell parser for the arguments themselves.
Result Run(const fs::path& root, const fs::path& binary,
           const std::vector<std::string>& args) {
  const auto out_path = root / ".tfe-code-test.stdout";
  const auto err_path = root / ".tfe-code-test.stderr";
  std::string command = "cd " + Quote(root.string()) + " && " + Quote(binary.string());
  for (const auto& arg : args) command += " " + Quote(arg);
  command += " </dev/null";
  command += " >" + Quote(out_path.string()) + " 2>" + Quote(err_path.string());
  const int status = std::system(command.c_str());
  Result result;
  result.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  std::ifstream out_stream(out_path, std::ios::binary);
  result.out = {std::istreambuf_iterator<char>(out_stream), {}};
  std::ifstream err_stream(err_path, std::ios::binary);
  result.err = {std::istreambuf_iterator<char>(err_stream), {}};
  fs::remove(out_path);
  fs::remove(err_path);
  return result;
}

int main() {
  try {
    const fs::path root = fs::temp_directory_path() / "tfe-code-tui-test";
    fs::remove_all(root);
    Write(root / "pkg/mod.py",
          "class Widget:\n    def render(self):\n        return 1\n\ndef helper():\n    return 2\n");
    tfe::code::Workspace workspace(root);
    workspace.Import({"pkg/mod.py"});

    // The snapshot is one row per role in outline order, with depth, the full
    // version hash and 1-based source lines.
    const auto files = workspace.TuiSnapshot();
    Expect(files.size() == 1, "snapshot listed " + std::to_string(files.size()) + " files");
    Expect(files[0].path == "pkg/mod.py", "snapshot lost the file path: " + files[0].path);
    const auto& roles = files[0].roles;
    Expect(roles.size() == 4, "snapshot carried " + std::to_string(roles.size()) + " roles, expected 4");
    Expect(roles[0].id == "pkg.mod.file" && roles[0].depth == 0,
           "the first row must be the .file root at depth 0: " + roles[0].id);
    Expect(roles[1].id == "pkg.mod.Widget" && roles[1].depth == 1,
           "the class must follow at depth 1: " + roles[1].id);
    Expect(roles[2].id == "pkg.mod.Widget.render" && roles[2].depth == 2,
           "the method must nest at depth 2: " + roles[2].id);
    Expect(roles[3].id == "pkg.mod.helper" && roles[3].depth == 1,
           "the sibling function must return to depth 1: " + roles[3].id);
    Expect(roles[2].version.size() == 64,
           "the snapshot must carry the full version hash, got " +
               std::to_string(roles[2].version.size()) + " chars");
    Expect(roles[2].line_count == 2,
           "render() must count 2 lines, got " + std::to_string(roles[2].line_count));
    Expect(roles[2].source_start_line == 2 && roles[2].source_end_line == 3,
           "render() source lines must be 2-3, got " +
               std::to_string(roles[2].source_start_line) + "-" +
               std::to_string(roles[2].source_end_line));

    // A path prefix narrows the dashboard like `ls`.
    Expect(workspace.TuiSnapshot("pkg").size() == 1, "the matching prefix lost the file");
    Expect(workspace.TuiSnapshot("absent").empty(), "an unmatched prefix leaked files");

    // The role pane text shows the 12-hex version prefix and local names.
    const auto pane = tfe::code::RoleRowsText(files[0]);
    Expect(pane.find(roles[2].version.substr(0, 12)) != std::string::npos,
           "the role pane lost the version prefix");
    Expect(pane.find("Widget.render") != std::string::npos,
           "the role pane lost the local role name");
    Expect(pane.find("source 2-3") != std::string::npos,
           "the role pane lost the source range");

    // Without a terminal the command is a usage error naming the problem.
    const char* env_binary = std::getenv("TFE_CODE_NATIVE");
    const fs::path binary = env_binary ? fs::path(env_binary) : fs::current_path() / "tfe-code";
    if (fs::exists(binary)) {
      const auto guard = Run(root, binary, {"tui"});
      Expect(guard.code == 2, "non-tty `tui` exit code was " + std::to_string(guard.code) +
                                  ", expected 2: " + guard.err);
      Expect(guard.err.find("terminal") != std::string::npos,
             "non-tty `tui` did not name the missing terminal: " + guard.err);
    }

    fs::remove_all(root);
    std::cout << "tui_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
