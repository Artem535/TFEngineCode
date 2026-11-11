// Structural placement contracts: bare-module parents resolve, class-marker
// punctuation is inherited, before/after anchors are exclusive, and responses
// surface the created and moved role ids.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

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

// Builds a role marker without ever spelling the double braces adjacently in
// this source: the importer reads markers in string literals too, and this
// file must not grow fake children.
std::string Marker(const std::string& id) {
  return "{" "{" + id + "}" "}";
}

struct Result {
  int code = -1;
  std::string out;
  std::string err;
};

Result Run(const fs::path& root, const fs::path& binary,
           const std::vector<std::string>& args, const std::string& stdin_text) {
  const auto out_path = root / ".tfe-code-test.stdout";
  const auto err_path = root / ".tfe-code-test.stderr";
  const auto in_path = root / ".tfe-code-test.stdin";
  Write(in_path, stdin_text);
  std::string command = "cd " + Quote(root.string()) + " && " + Quote(binary.string());
  for (const auto& arg : args) command += " " + Quote(arg);
  command += " <" + Quote(in_path.string());
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
  fs::remove(in_path);
  return result;
}

int main() {
  try {
    const char* env_binary = std::getenv("TFE_CODE_NATIVE");
    const fs::path binary = env_binary ? fs::path(env_binary) : fs::current_path() / "tfe-code";
    const fs::path root = fs::temp_directory_path() / "tfe-code-structural-placement";
    fs::remove_all(root);
    Write(root / "dialect.cc",
          "namespace demo {\n"
          "class Ancient {\n public:\n  int Run() { return 1; }\n};\n"
          "\n"
          "class AncientTest {\n public:\n  int Probe() { return 2; }\n};\n"
          "}  // namespace demo\n");
    tfe::code::Workspace workspace(root);
    workspace.Import({"dialect.cc"});

    // The importer keeps the class semicolon in the container text.
    const auto project = workspace.LoadProject();
    Expect(project.blocks.at("dialect.file").text.find(Marker("dialect.Ancient") + ";") !=
               std::string::npos,
           "expected the class marker to carry a container semicolon");

    // Bare module names resolve to the file's root role.
    const auto by_module = workspace.AddRole("dialect", "Lonely", "class Lonely {\n}");
    Expect(by_module.created.size() == 1 && by_module.created.front() == "dialect.Lonely",
           "add-role must surface the created role id");

    // A class anchored after a sibling inherits the `;` punctuation.
    const auto anchored =
        workspace.AddRole("dialect", "Fresh",
                          "class Fresh {\n public:\n  int Go() { return 3; }\n}", "Ancient",
                          std::nullopt);
    Expect(anchored.created.size() == 1 && anchored.created.front() == "dialect.Fresh",
           "anchored add must surface the created role id");
    const auto project2 = workspace.LoadProject();
    Expect(project2.blocks.at("dialect.file").text.find(Marker("dialect.Fresh") + ";") !=
               std::string::npos,
           "anchored class insertion must inherit the container semicolon");

    // A before-anchor lands above the anchor and keeps the punctuation.
    const auto early = workspace.AddRole("dialect", "Early",
                                         "class Early {\n public:\n  int Go() { return 4; }\n}",
                                         "", std::nullopt, "Ancient");
    Expect(early.created.size() == 1 && early.created.front() == "dialect.Early",
           "before-anchored add must surface the created role id");
    const auto project3 = workspace.LoadProject();
    const auto& container = project3.blocks.at("dialect.file").text;
    const auto early_pos = container.find(Marker("dialect.Early"));
    const auto ancient_pos = container.find(Marker("dialect.Ancient"));
    Expect(early_pos != std::string::npos && ancient_pos != std::string::npos &&
               early_pos < ancient_pos,
           "--before must place the marker above the anchor");
    Expect(container.find(Marker("dialect.Early") + ";") != std::string::npos,
           "before-anchored class must inherit the semicolon too");

    // The anchors are mutually exclusive.
    bool rejected = false;
    try {
      (void)workspace.AddRole("dialect", "Clash", "class Clash {\n}", "Ancient", std::nullopt,
                              "Fresh");
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    Expect(rejected, "using --after and --before together must be rejected");

    // Default append follows the all-class punctuation convention too.
    const auto appended = workspace.AddRole(
        "dialect", "Last", "class Last {\n public:\n  int Go() { return 5; }\n}");
    const auto project4 = workspace.LoadProject();
    Expect(project4.blocks.at("dialect.file").text.find(Marker("dialect.Last") + ";") !=
               std::string::npos,
           "default append must follow the all-class semicolon convention");

    // A rename in place surfaces the move.
    const auto moved = workspace.MoveRole("dialect.AncientTest", "Historic", "", "");
    Expect(moved.moved.size() == 1 &&
               moved.moved.front().find("dialect.AncientTest") != std::string::npos,
           "move-role must surface the moved role");
    // A pure rename renders byte-identical output: nothing to materialise.
    Expect(moved.changed.empty(), "a rename must not claim file writes");

    // A re-parent surfaces the move as well.
    const auto reparented = workspace.MoveRole("dialect.Historic", "", "Fresh", "");
    Expect(reparented.moved.size() == 1 &&
               reparented.moved.front().find("dialect.Fresh.Historic") != std::string::npos,
           "the re-parent must surface the new id");
    Expect(reparented.changed.size() == 1 && reparented.changed.front() == "dialect.cc",
           "the re-parent must materialise the owning file");

    // The CLI rejects combined anchors as a usage error.
    if (fs::exists(binary)) {
      const auto cli =
          Run(root, binary,
              {"add-role", "dialect", "SecondClash", "--after", "Fresh", "--before", "Last"},
              "class SecondClash {\n}");
      Expect(cli.code == 2, "combined --after/--before exit code was " +
                                std::to_string(cli.code) + ", expected 2: " + cli.err);
      Expect(cli.err.find("Usage:") != std::string::npos,
             "combined anchors did not print usage: " + cli.err);
    }

    fs::remove_all(root);
    std::cout << "structural_placement_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
