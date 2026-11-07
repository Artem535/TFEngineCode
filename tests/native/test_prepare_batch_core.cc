// prepare-batch must emit a payload that pipes straight back into apply: correct
// role ids, current versions and exact current text, so a caller only fills `new`.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <rfl/json.hpp>
#include <stdexcept>
#include <string>

#include "workspace.h"

namespace fs = std::filesystem;

struct SkeletonEdit {
  std::string role;
  std::string expected_version;
  rfl::Rename<"old", std::string> old_text;
  rfl::Rename<"new", std::string> new_text;
};

struct Skeleton {
  std::vector<SkeletonEdit> edits;
};

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

void Expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  const fs::path root = fs::temp_directory_path() / "tfe-code-prepare-batch-test";
  fs::remove_all(root);
  Write(root / "mod.py", "def one():\n    return 1\n\ndef two():\n    return 2\n");
  tfe::code::Workspace workspace(root);
  workspace.Import({"mod.py"});
  const auto project = workspace.LoadProject();

  // A literal query matches ids and text, so pick one that only the leaf carries:
// the container's text holds `{{mod.one}}`, which would otherwise also match.
  const auto raw = workspace.PrepareBatch({"return 1"});
  const auto parsed = rfl::json::read<Skeleton>(raw);
  Expect(parsed.has_value(), "prepare-batch did not emit a valid payload");
  const auto& edits = parsed.value().edits;
  Expect(edits.size() == 1, "prepare-batch matched the wrong number of roles");
  Expect(edits.front().role == "mod.one", "prepare-batch chose the wrong role");
  Expect(edits.front().expected_version == project.blocks.at("mod.one").version,
         "prepare-batch reported a stale version");
  Expect(edits.front().old_text.get() == "def one():\n    return 1",
         "prepare-batch did not carry the exact current role text as old");
  Expect(edits.front().new_text.get().empty(), "prepare-batch must leave new empty");

  // A container that merely embeds the match must not be returned: asking for a
  // method inside a class must not hand back the whole class as `old`.
  const auto nested = rfl::json::read<Skeleton>(workspace.PrepareBatch({"mod.one"}));
  Expect(nested.has_value(), "prepare-batch rejected a role-name query");
  for (const auto& edit : nested.value().edits) {
    Expect(edit.role != "mod.file", "prepare-batch returned the enclosing container");
  }

  // The skeleton must be directly applicable once `new` is filled in.
  std::vector<tfe::code::RoleEdit> applied{
      {edits.front().role, edits.front().expected_version, edits.front().old_text.get(),
       "def one():\n    return 10"}};
  const auto result = workspace.Apply(applied, std::nullopt);
  Expect(result.changed == std::vector<std::string>{"mod.py"},
         "applying the prepared skeleton did not change the file");

  // A file filter restricts the match set.
  bool rejected = false;
  try {
    (void)workspace.PrepareBatch({"return"}, {"missing.py"});
  } catch (const std::exception&) {
    rejected = true;
  }
  Expect(rejected, "an unknown file must be rejected");
  Expect(!workspace.PrepareBatch({"two"}).empty(), "prepare-batch returned nothing");

  // A description instead of a literal must not be a dead end: the error names a
  // role whose id or text carries one of the query's words.
  bool suggested = false;
  try {
    (void)workspace.PrepareBatch({"the one thing"});
  } catch (const std::exception& error) {
    const std::string message = error.what();
    suggested = message.find("try a literal identifier") != std::string::npos &&
                message.find("mod.") != std::string::npos;
  }
  Expect(suggested, "a no-match query must suggest a literal identifier");

  fs::remove_all(root);
  std::cout << "prepare_batch_tests passed\n";
  return 0;
}