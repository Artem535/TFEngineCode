// `ls` is the cheap map: file paths with no argument, local child names with
// one, and next-segment listing for a package prefix, which is not a role.
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

void Expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void ExpectContains(const std::string& haystack, const std::string& needle,
                    const std::string& message) {
  if (haystack.find(needle) == std::string::npos) {
    throw std::runtime_error(message + " (got: " + haystack.substr(0, 120) + ")");
  }
}

int main() {
  const fs::path root = fs::temp_directory_path() / "tfe-code-list-test";
  fs::remove_all(root);
  Write(root / "pkg/mod.py", "class Widget:\n    def render(self):\n        return 1\n\ndef helper():\n    return 2\n");
  tfe::code::Workspace workspace(root);
  workspace.Import({"pkg/mod.py"});

  // No argument: the managed file paths, like `find`.
  const auto files = workspace.List();
  ExpectContains(files, "pkg/mod.py", "ls with no argument did not list the file");
  ExpectContains(files, "pkg/mod.py\n", "each path must be on its own line");
  Expect(files.find("pkg.mod") == std::string::npos, "ls listed role ids instead of paths");

  // A file path: its direct children by local name, like `ls`.
  const auto children = workspace.List("pkg/mod.py");
  ExpectContains(children, "Widget", "ls on a file omitted the class");
  ExpectContains(children, "helper", "ls on a file omitted the function");
  Expect(children.find("pkg.mod") == std::string::npos, "ls repeated the id prefix");

  // A role id: that role's children.
  const auto methods = workspace.List("pkg.mod.Widget");
  ExpectContains(methods, "render", "ls on a class omitted the method");
  Expect(methods.find("helper") == std::string::npos, "ls on a class listed a sibling");

  // A package prefix is not a role; it must behave like a directory.
  const auto package = workspace.List("pkg");
  ExpectContains(package, "mod", "ls on a package prefix did not list the module");
  Expect(package.find("file") == std::string::npos, "the module's own .file role leaked in");

  // A directory path works as a prefix too, not only the dotted role form.
  const fs::path nested_root = fs::temp_directory_path() / "tfe-code-list-dir-test";
  fs::remove_all(nested_root);
  Write(nested_root / "pkg/sub/deep.py",
        "class Deep:\n    def probe(self):\n        return 3\n");
  tfe::code::Workspace nested_workspace(nested_root);
  nested_workspace.Import({"pkg/sub"});
  const auto by_directory = nested_workspace.List("pkg/sub");
  ExpectContains(by_directory, "deep", "ls on a directory prefix omitted the module");
  const auto by_dotted = nested_workspace.List("pkg.sub");
  Expect(by_dotted == by_directory, "the directory form must match the dotted form");
  fs::remove_all(nested_root);

  bool rejected = false;
  try {
    (void)workspace.List("nope/nope.py");
  } catch (const std::exception&) {
    rejected = true;
  }
  Expect(rejected, "an unknown target must be rejected");

  // search --ids: one compact line per role instead of a whole body.
  const auto bodies = workspace.Search({"return 1"}, "", "", false);
  const auto compact = workspace.Search({"return 1"}, "", "", true);
  Expect(compact.size() < bodies.size(), "the compact form must be smaller than the bodies");
  ExpectContains(compact, "pkg.mod.Widget.render", "compact search dropped a matching role");
  ExpectContains(compact, "return 1", "compact search did not show the matching line");
  Expect(compact.find("#@ ") == std::string::npos, "compact search still emitted role headers");

  // A misspelled --file names its own candidates instead of costing a round trip.
  bool suggested = false;
  try {
    (void)workspace.Search({"return"}, "", "obs/mod.py");
  } catch (const std::exception& error) {
    const std::string message = error.what();
    suggested = message.find("did you mean") != std::string::npos &&
                message.find("pkg/mod.py") != std::string::npos;
  }
  Expect(suggested, "an unknown --file must suggest the closest managed path");
  bool batch_suggested = false;
  try {
    (void)workspace.PrepareBatch({"return"}, {"obs/mod.py"});
  } catch (const std::exception& error) {
    batch_suggested = std::string(error.what()).find("did you mean") != std::string::npos;
  }
  Expect(batch_suggested, "prepare-batch must suggest paths too");

  // outline --relative drops the repeated id prefix.
  const auto absolute_outline = workspace.Outline("pkg/mod.py", 3, false);
  const auto relative_outline = workspace.Outline("pkg/mod.py", 3, true);
  Expect(relative_outline.size() < absolute_outline.size(),
         "relative outline must be smaller than the absolute one");
  ExpectContains(relative_outline, "Widget", "relative outline lost the class name");
  Expect(relative_outline.find("pkg.mod.Widget") == std::string::npos,
         "relative outline repeated the id prefix");

  fs::remove_all(root);
  std::cout << "navigation_tests passed\n";
  return 0;
}