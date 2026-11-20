// Core verification of the sticky role-index mode: an explicit
// TFE_CODE_INDEX is recorded in .tfe-code/index-mode on the first successful
// write, an unset variable follows the recorded mode, an existing ObjectBox
// store without a marker is adopted with a one-off notice, unknown values are
// rejected, and `stats` reports the active mode.

#include <cstdlib>
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

void ClearEnv() { unsetenv("TFE_CODE_INDEX"); }

}  // namespace

int main() {
  try {
    const fs::path root = fs::temp_directory_path() / "tfe-code-index-mode-test";
    fs::remove_all(root);
    Write(root / "a.py", "def one():\n    return 1\n");

    // 1. An explicit mode is used and recorded by the first successful write.
    setenv("TFE_CODE_INDEX", "objectbox", 1);
    {
      tfe::code::Workspace workspace(root);
      Expect(workspace.IndexMode() == "objectbox",
             "explicit objectbox mode was not selected");
      workspace.Import({"a.py"});
    }
    ClearEnv();
    Expect(fs::exists(root / ".tfe-code" / "index-mode"),
           "the objectbox mode was not persisted");
    Expect(Read(root / ".tfe-code" / "index-mode") == "objectbox\n",
           "the persisted mode is not objectbox");

    // 2. With the variable unset the recorded mode wins, without a notice.
    {
      tfe::code::Workspace workspace(root);
      Expect(workspace.IndexMode() == "objectbox",
             "a forgotten TFE_CODE_INDEX did not fall back to the recorded mode");
      Expect(workspace.IndexNotice().empty(),
             "a recorded mode should not produce an inference notice");
      Expect(workspace.FormatStats(false).find("Index:    objectbox") != std::string::npos,
             "stats does not report the active index mode");
    }

    // 3. An explicit override wins over the recorded mode and becomes sticky.
    setenv("TFE_CODE_INDEX", "json", 1);
    {
      tfe::code::Workspace workspace(root);
      Expect(workspace.IndexMode() == "json", "explicit json mode was not selected");
      workspace.Import({"a.py"});
    }
    ClearEnv();
    Expect(Read(root / ".tfe-code" / "index-mode") == "json\n",
           "the json override was not recorded");
    {
      tfe::code::Workspace workspace(root);
      Expect(workspace.IndexMode() == "json", "the json override was not sticky");
    }

    // 4. An unknown value is a usage error instead of a silent JSON fallback.
    setenv("TFE_CODE_INDEX", "bogus", 1);
    bool rejected = false;
    try {
      tfe::code::Workspace workspace(root);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    ClearEnv();
    Expect(rejected, "an unknown TFE_CODE_INDEX value was accepted");

    // 5. A pre-marker workspace that only has the database is adopted, explained
    //    once, and recorded so the next command is quiet.
    fs::remove(root / ".tfe-code" / "index-mode");
    {
      tfe::code::Workspace workspace(root);
      Expect(workspace.IndexMode() == "objectbox",
             "an existing .tfe-code/db was ignored without a recorded mode");
      Expect(!workspace.IndexNotice().empty(),
             "inferring the mode from an existing store was not explained");
    }
    Expect(Read(root / ".tfe-code" / "index-mode") == "objectbox\n",
           "the inferred mode was not recorded");
    {
      tfe::code::Workspace workspace(root);
      Expect(workspace.IndexNotice().empty(),
             "the inference notice repeats after the mode was recorded");
    }

    fs::remove_all(root);
    std::cout << "index_mode_core tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Test failed: " << e.what() << "\n";
    return 1;
  }
}
