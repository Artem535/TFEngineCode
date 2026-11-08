// Verification of structural role operations (AddRole, MoveRole, DeleteRole)
// across Python and C++/TypeScript containers, and token savings telemetry (GetStats / FormatStats).

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "workspace.h"

namespace fs = std::filesystem;

namespace {

void Expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string Read(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}

}  // namespace

int main() {
  try {
    const fs::path root = fs::temp_directory_path() / "tfe-code-structural-stats-test";
    fs::remove_all(root);

    Write(root / "src/math_ops.py",
          "class Calculator:\n"
          "    def add(self, a: int, b: int) -> int:\n"
          "        return a + b\n"
          "\n"
          "    def sub(self, a: int, b: int) -> int:\n"
          "        return a - b\n"
          "\n"
          "def helper_one(x: int) -> int:\n"
          "    return x + 1\n");

    Write(root / "src/utils.py",
          "def base_util(x: int) -> int:\n"
          "    return x * 10\n");

    {
      tfe::code::Workspace workspace(root);
      const auto imp = workspace.Import({"src"});
      Expect(imp.changed.size() == 2, "expected 2 imported files");

      // 1. AddRole: add a new method `mul` to `Calculator` after `add`
      const auto add_res = workspace.AddRole(
          "Calculator", "mul",
          "def mul(self, a: int, b: int) -> int:\n    return a * b",
          "add");
      Expect(add_res.changed.size() == 1 && add_res.changed[0] == "src/math_ops.py",
             "AddRole did not update src/math_ops.py");

      const std::string after_add = Read(root / "src/math_ops.py");
      Expect(after_add.find("    def mul(self, a: int, b: int) -> int:\n        return a * b") !=
                 std::string::npos,
             "mul method not properly indented in class Calculator: " + after_add);

      // Verify project role tree contains `src.math_ops.Calculator.mul`
      const auto proj_after_add = workspace.LoadProject();
      Expect(proj_after_add.blocks.contains("src.math_ops.Calculator.mul"),
             "missing Calculator.mul role after AddRole");

      // 2. MoveRole (in-place rename): rename `Calculator.mul` -> `Calculator.multiply`
      const auto ren_res = workspace.MoveRole("Calculator.mul", "multiply");
      Expect(ren_res.changed.empty(),
             "renaming role ID without changing body text should keep rendered source identical");
      const auto proj_after_ren = workspace.LoadProject();
      Expect(!proj_after_ren.blocks.contains("src.math_ops.Calculator.mul"),
             "old role Calculator.mul still exists after rename");
      Expect(proj_after_ren.blocks.contains("src.math_ops.Calculator.multiply"),
             "new role Calculator.multiply missing after rename");

      // 3. MoveRole across files: move `helper_one` from `src/math_ops.py` to `src/utils.py`
      const auto move_res =
          workspace.MoveRole("math_ops.helper_one", "moved_helper", "src/utils.py");
      Expect(move_res.changed.size() == 2, "cross-file MoveRole should touch both files");
      Expect(Read(root / "src/math_ops.py").find("def helper_one") == std::string::npos,
             "helper_one should be removed from src/math_ops.py");
      Expect(Read(root / "src/utils.py").find("def helper_one") != std::string::npos,
             "helper_one should appear in src/utils.py");
      Expect(workspace.LoadProject().blocks.contains("src.utils.moved_helper"),
             "moved_helper role ID should exist in src.utils");

      // 4. DeleteRole: delete `Calculator.sub`
      const auto del_res = workspace.DeleteRole("Calculator.sub");
      Expect(del_res.changed.size() == 1 && del_res.changed[0] == "src/math_ops.py",
             "DeleteRole did not update src/math_ops.py");
      Expect(Read(root / "src/math_ops.py").find("def sub") == std::string::npos,
             "deleted method sub still in src/math_ops.py");
      Expect(!workspace.LoadProject().blocks.contains("src.math_ops.Calculator.sub"),
             "deleted role Calculator.sub still in blocks");

      // 5. Telemetry verification
      (void)workspace.Outline("", 2, true);
      (void)workspace.Context({"add"}, {"src/math_ops.py"}, false);
      (void)workspace.Search({"base_util"}, "", "", true, false);
      (void)workspace.Show({"Calculator.add"});

      const auto stats = workspace.GetStats(false);
      Expect(stats.total_calls >= 4, "expected at least 4 recorded telemetry calls");
      Expect(stats.baseline_bytes > 0, "expected positive baseline_bytes");
      Expect(stats.emitted_bytes > 0, "expected positive emitted_bytes");
      Expect(stats.by_command.contains("context"), "missing context in telemetry by_command");
      Expect(stats.by_command.contains("outline"), "missing outline in telemetry by_command");
      Expect(stats.by_command.contains("search"), "missing search in telemetry by_command");
      Expect(stats.by_command.contains("show"), "missing show in telemetry by_command");

      const std::string formatted = workspace.FormatStats(false, false);
      Expect(formatted.find("TFEngineCode Token Telemetry") != std::string::npos,
             "missing header in FormatStats");

      // Reset stats
      const auto after_reset = workspace.GetStats(true);
      Expect(after_reset.total_calls == 0, "expected 0 calls after reset");
    }

    fs::remove_all(root);
    std::cout << "structural_stats_core tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Test failed: " << e.what() << "\n";
    return 1;
  }
}
