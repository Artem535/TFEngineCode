// Read-model surface: outline/tree/show/show_roles/search formatting, receipt
// bookkeeping and source-range tracking across import and apply.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
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
const fs::path tree_root = fs::temp_directory_path() / "tfe-code-read-models-test";
  fs::remove_all(tree_root);
  Write(tree_root / "mod.py",
        "class Widget:\n"
        "    def render(self):\n"
        "        return 'widget'\n"
        "\n"
        "def helper():\n"
        "    return 42\n");
  tfe::code::Workspace tree_workspace(tree_root);
  tree_workspace.Import({"mod.py"});
  const auto tree_project = tree_workspace.LoadProject();
  Expect(tree_project.blocks.contains("mod.file") && tree_project.blocks.contains("mod.Widget") &&
         tree_project.blocks.contains("mod.Widget.render") &&
         tree_project.blocks.contains("mod.helper"),
         "import did not build the expected role tree");
  const auto outline = tree_workspace.Outline("mod.py", 3);
  // `mod.file` body is the rendered file: class Widget (1-3), a blank line, then
  // helper (5-6), so the role itself spans two non-empty lines.
  Expect(outline.find("mod.file (2 lines; source 1-6)") != std::string::npos,
         "outline omits the file role header");
  Expect(outline.find("  mod.Widget (2 lines; source 1-3) class Widget:") != std::string::npos,
         "outline does not nest the class role");
  Expect(outline.find("    mod.Widget.render (2 lines; source 2-3) def render(self):") !=
             std::string::npos,
         "outline does not nest the method role");
  Expect(outline.find("  mod.helper (2 lines; source 5-6) def helper():") != std::string::npos,
         "outline does not list the sibling function");
  const auto tree = tree_workspace.Tree("mod.py");
  Expect(tree.find("mod.Widget") != std::string::npos && tree.find("[lines:") == std::string::npos,
         "tree output changed shape");
  const auto shown = tree_workspace.Show({"mod.helper"});
  Expect(shown.find("[lines: 5-6]") != std::string::npos,
         "show does not display the source range");
  Expect(shown.find("return 42") != std::string::npos, "show omits the role body");
  const auto shown_roles = tree_workspace.ShowRoles({"mod.helper"});
  Expect(shown_roles.contains("mod.helper") && shown_roles.at("mod.helper").literal &&
         shown_roles.at("mod.helper").source_start_line == 5 &&
         shown_roles.at("mod.helper").source_end_line == 6,
         "show roles JSON does not expose ranges/literals");
  const auto matched = tree_workspace.Search({"widget"}, "mod.");
  Expect(matched.find("#@ mod.Widget.render [version: ") != std::string::npos,
         "search does not report the enclosing role");
  Expect(matched.find("[lines:") == std::string::npos,
         "search should not display source ranges");
  Expect(tree_workspace.Search({"absent"}, "") == "no matching roles",
         "search should report an empty result");
  fs::remove_all(tree_root);
const fs::path ranges_root = fs::temp_directory_path() / "tfe-code-source-ranges-test";
  fs::remove_all(ranges_root);
  Write(ranges_root / "ranges.py",
        "def first():\n    return 1\n\ndef second():\n    return 2\n");
  tfe::code::Workspace ranges_workspace(ranges_root);
  ranges_workspace.Import({"ranges.py"});
  auto ranges_project = ranges_workspace.LoadProject();
  Expect(ranges_project.blocks.at("ranges.first").source_start_line == 1 &&
         ranges_project.blocks.at("ranges.first").source_end_line == 2 &&
         ranges_project.blocks.at("ranges.second").source_start_line == 4 &&
         ranges_project.blocks.at("ranges.second").source_end_line == 5,
         "import did not preserve source ranges");
  const auto ranges_outline = ranges_workspace.Outline("ranges.py", 1);
  Expect(ranges_outline.find("ranges.first (2 lines; source 1-2)") != std::string::npos &&
         ranges_outline.find("ranges.second (2 lines; source 4-5)") != std::string::npos,
         "outline does not show source ranges");
  Expect(ranges_workspace.Show({"ranges.first"}).find("[lines: 1-2]") != std::string::npos,
         "show does not display source range");
  Expect(ranges_workspace.ShowRoles({"ranges.second"}).at("ranges.second").source_end_line == 5,
         "JSON roles do not expose source ranges");
  Expect(ranges_workspace.Tree("ranges.py").find("[lines:") == std::string::npos &&
         ranges_workspace.Search({"return"}, "ranges").find("[lines:") == std::string::npos,
         "tree or search changed to display source ranges");
  ranges_workspace.Apply(
      {{"ranges.first", ranges_project.blocks.at("ranges.first").version,
        "return 1", "if True:\n        return 2"}}, std::nullopt);
  ranges_project = ranges_workspace.LoadProject();
  Expect(ranges_project.blocks.at("ranges.first").source_start_line == 1 &&
         ranges_project.blocks.at("ranges.first").source_end_line == 3 &&
         ranges_project.blocks.at("ranges.second").source_start_line == 5 &&
         ranges_project.blocks.at("ranges.second").source_end_line == 6,
         "apply did not refresh source ranges");
  ranges_workspace.Apply(
      {{"ranges.first", ranges_project.blocks.at("ranges.first").version,
        "def first():", "def renamed():"}}, std::nullopt);
  ranges_project = ranges_workspace.LoadProject();
  Expect(ranges_project.blocks.at("ranges.first").source_start_line == 0 &&
         ranges_project.blocks.at("ranges.first").source_end_line == 0 &&
         ranges_project.blocks.at("ranges.second").source_start_line == 5 &&
         ranges_project.blocks.at("ranges.second").source_end_line == 6,
         "renamed role retained a stale source range");
  fs::remove_all(ranges_root);

  std::cout << "read_models_tests passed\n";
}
