// `tui` dashboard data contracts: the structured TuiSnapshot feeds the FTXUI
// dashboard, and the binary degrades to a usage error without a terminal.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
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

    // v2 builders: visible rows under collapsed ancestors.
    const std::vector<bool> none_collapsed(roles.size(), false);
    auto visible = tfe::code::VisibleRoleRows(roles, none_collapsed);
    Expect(visible.size() == 4, "an open tree must show every row");
    std::vector<bool> class_collapsed(roles.size(), false);
    class_collapsed[1] = true;
    visible = tfe::code::VisibleRoleRows(roles, class_collapsed);
    Expect(visible.size() == 3, "collapse keeps the node and hides only children: " +
                                    std::to_string(visible.size()));
    Expect(visible[0] == 0 && visible[1] == 1 && visible[2] == 3,
           "collapse must keep root and class, hide the method, keep the sibling");
    std::vector<bool> root_collapsed(roles.size(), false);
    root_collapsed[0] = true;
    visible = tfe::code::VisibleRoleRows(roles, root_collapsed);
    Expect(visible.size() == 1 && visible[0] == 0, "collapsing the root must hide the tree");

    // Search filter keeps matches plus their ancestor chain, outline-ordered.
    const std::set<std::string> matched{"pkg.mod.Widget.render"};
    const auto filtered = tfe::code::FilterRoleRows(roles, matched);
    Expect(filtered.size() == 3, "the search filter must keep the ancestor chain: " +
                                     std::to_string(filtered.size()));
    Expect(filtered[0] == 0 && filtered[1] == 1 && filtered[2] == 2,
           "the filter must keep .file, Widget and render in order");

    // Role detail pane: id, version prefix, range and the role body.
    const auto detail = tfe::code::RoleDetailText(
        roles[2], "def render(self):\n        return 1");
    Expect(detail.find("pkg.mod.Widget.render") != std::string::npos,
           "the detail pane lost the role id");
    Expect(detail.find(roles[2].version.substr(0, 12)) != std::string::npos,
           "the detail pane lost the version");
    Expect(detail.find("def render(self):") != std::string::npos,
           "the detail pane lost the role body");
    Expect(detail.find("source 2-3") != std::string::npos,
           "the detail pane lost the source range");

    // Stats chart rows: bars proportional to saved tokens.
    tfe::code::TelemetryReport report;
    report.by_command["apply"].saved_tokens = 100;
    report.by_command["context"].saved_tokens = 50;
    const auto count_bars = [](const std::string& row) {
      size_t count = 0;
      for (size_t pos = 0; (pos = row.find("█", pos)) != std::string::npos;
           pos += 3) {
        ++count;
      }
      return count;
    };
    const auto chart = tfe::code::StatsChartRows(report);
    Expect(chart.size() == 2, "the chart must have one row per command: " +
                                  std::to_string(chart.size()));
    Expect(chart[0].find("apply") != std::string::npos,
           "the chart row lost the command name");
    Expect(count_bars(chart[0]) == count_bars(chart[1]) * 2,
           "bars must be proportional to saved tokens");
    report.by_command["watch"].saved_tokens = 0;
    const auto chart_zero = tfe::code::StatsChartRows(report);
    Expect(chart_zero.size() == 3, "a zero-saving command must still get a row");

    // Without a terminal the command is a usage error naming the problem.
    const char* env_binary = std::getenv("TFE_CODE_NATIVE");
    const fs::path binary = env_binary ? fs::path(env_binary) : fs::current_path() / "tfe-code";
    if (fs::exists(binary)) {
      const auto guard = Run(root, binary, {"tui"});
      Expect(guard.code == 2, "non-tty `tui` exit code was " + std::to_string(guard.code) +
                                  ", expected 2: " + guard.err);
      Expect(guard.err.find("terminal") != std::string::npos,
             "non-tty `tui` did not name the missing terminal: " + guard.err);
      const auto stats_guard = Run(root, binary, {"stats", "--tui"});
      Expect(stats_guard.code == 2, "non-tty `stats --tui` exit code was " +
                                        std::to_string(stats_guard.code) +
                                        ", expected 2: " + stats_guard.err);
      Expect(stats_guard.err.find("terminal") != std::string::npos,
             "non-tty `stats --tui` did not name the missing terminal: " + stats_guard.err);
      const auto stats_mix = Run(root, binary, {"stats", "--tui", "--json"});
      Expect(stats_mix.code == 2, "`stats --tui --json` exit code was " +
                                      std::to_string(stats_mix.code) + ", expected 2: " +
                                      stats_mix.err);
      Expect(stats_mix.err.find("Usage:") != std::string::npos,
             "`stats --tui --json` is a usage error without usage text: " + stats_mix.err);
    }

    fs::remove_all(root);
    std::cout << "tui_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
