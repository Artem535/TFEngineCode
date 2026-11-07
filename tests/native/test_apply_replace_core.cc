// Multiple independent replacements inside one role. A single old/new pair cannot
// express "change the signature and the call site"; duplicating the role entry was
// rejected as ambiguous, which cost entire agent attempts.
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
  const fs::path root = fs::temp_directory_path() / "tfe-code-apply-replace-test";
  fs::remove_all(root);
  Write(root / "mod.py",
        "def publish(event_type: str):\n    return emit(event_type)\n");
  tfe::code::Workspace workspace(root);
  workspace.Import({"mod.py"});
  const auto version = workspace.LoadProject().blocks.at("mod.publish").version;

  // Two edits in one role.
  tfe::code::RoleEdit two_edits;
  two_edits.role = "mod.publish";
  two_edits.expected_version = version;
  two_edits.replace = {{"def publish(event_type: str):", "def publish(event_type: str, level: str = \"info\"):"},
                       {"return emit(event_type)", "return emit(event_type, level)"}};
  const auto result = workspace.Apply({two_edits}, std::nullopt);
  Expect(result.changed == std::vector<std::string>{"mod.py"}, "replace did not rewrite the file");
  const auto text = Read(root / "mod.py");
  Expect(text.find("level: str = \"info\"") != std::string::npos, "first replacement was lost");
  Expect(text.find("emit(event_type, level)") != std::string::npos, "second replacement was lost");

  const auto updated = workspace.LoadProject().blocks.at("mod.publish").version;

  // old/new and replace are mutually exclusive.
  tfe::code::RoleEdit mixed;
  mixed.role = "mod.publish";
  mixed.expected_version = updated;
  mixed.old = "event_type";
  mixed.replace = {{"event_type", "kind"}};
  bool rejected = false;
  try {
    (void)workspace.Apply({mixed}, std::nullopt);
  } catch (const std::exception& error) {
    rejected = std::string(error.what()).find("not both") != std::string::npos;
  }
  Expect(rejected, "mixing old/new with replace must be rejected");

  // A fragment that does not appear is reported for that fragment.
  tfe::code::RoleEdit missing;
  missing.role = "mod.publish";
  missing.expected_version = updated;
  missing.replace = {{"nowhere", "x"}};
  bool missing_rejected = false;
  try {
    (void)workspace.Apply({missing}, std::nullopt);
  } catch (const std::exception& error) {
    missing_rejected = std::string(error.what()).find("0 times") != std::string::npos;
  }
  Expect(missing_rejected, "a non-matching fragment must be reported");

  // A fragment aimed at the wrong role names the roles that do contain it.
  Write(root / "mod3.py", "import os\n\n\ndef helper():\n    return os.sep\n");
  workspace.Import({"mod3.py"});
  const auto helper_version = workspace.LoadProject().blocks.at("mod3.helper").version;
  tfe::code::RoleEdit wrong_role;
  wrong_role.role = "mod3.helper";
  wrong_role.expected_version = helper_version;
  wrong_role.replace = {{"import os", "import sys"}};
  bool suggested_role = false;
  try {
    (void)workspace.Apply({wrong_role}, std::nullopt);
  } catch (const std::exception& error) {
    const std::string message = error.what();
    suggested_role = message.find("this fragment appears in") != std::string::npos &&
                     message.find("mod3.file") != std::string::npos;
  }
  Expect(suggested_role, "a misplaced fragment must name the roles that contain it");

  // Ambiguous fragments are rejected rather than applied to the first hit.
  Write(root / "mod2.py", "def dup():\n    return same\n\ndef other():\n    return same\n");
  workspace.Import({"mod2.py"});
  const auto version2 = workspace.LoadProject().blocks.at("mod2.dup").version;
  tfe::code::RoleEdit ambiguous;
  ambiguous.role = "mod2.dup";
  ambiguous.expected_version = version2;
  ambiguous.replace = {{"same", "changed"}};
  (void)workspace.Apply({ambiguous}, std::nullopt);  // one match inside this role: fine
  const auto after = workspace.LoadProject().blocks.at("mod2.dup");
  Expect(after.text.find("changed") != std::string::npos, "single-match fragment did not apply");

  fs::remove_all(root);
  std::cout << "apply_replace_tests passed\n";
  return 0;
}