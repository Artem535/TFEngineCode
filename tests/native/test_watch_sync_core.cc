// Verification of real-time cross-platform file watching via efsw (Workspace::Watch)
// and cross-file reference discovery.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

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

}  // namespace

int main() {
  try {
    const fs::path root = fs::temp_directory_path() / "tfe-code-efsw-watch-test";
    fs::remove_all(root);

    Write(root / "src/app.py",
          "def compute(x: int) -> int:\n"
          "    return x + 1\n");

    {
      tfe::code::Workspace workspace(root);
      const auto imp = workspace.Import({"src"});
      Expect(imp.changed.size() == 1, "expected 1 imported file");

      // Spawn a background writer that modifies src/app.py while Watch is listening
      std::thread writer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        Write(root / "src/app.py",
              "def compute(x: int) -> int:\n"
              "    return x + 42\n"
              "\n"
              "def extra_fn() -> str:\n"
              "    return 'watched'\n");
      });

      const auto watch_res = workspace.Watch(3000, true);
      writer.join();

      Expect(watch_res.changed.size() == 1 && watch_res.changed[0] == "src/app.py",
             "Watch did not detect and sync change to src/app.py");

      const auto proj = workspace.LoadProject();
      Expect(proj.blocks.contains("src.app.extra_fn"),
             "newly added function extra_fn was not synced by Watch");
      Expect(proj.blocks.at("src.app.compute").text.find("return x + 42") != std::string::npos,
             "modified function compute was not synced by Watch");
    }

    fs::remove_all(root);
    std::cout << "watch_sync_core tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Test failed: " << e.what() << "\n";
    return 1;
  }
}
