// Exit codes and stderr contracts of the native CLI.
//
// These are the documented, user-visible failure modes: argument errors must be
// distinguishable from rejected operations, and a rejected command must never
// rewrite a managed file. The test drives the real binary so it locks the CLI
// contract rather than an internal function signature.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "hash.h"
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
           const std::vector<std::string>& args,
           const std::string& stdin_text = "") {
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
  result.out = Read(out_path);
  result.err = Read(err_path);
  fs::remove(out_path);
  fs::remove(err_path);
  fs::remove(in_path);
  return result;
}

int main() {
  try {
    const char* env_binary = std::getenv("TFE_CODE_NATIVE");
    const fs::path binary = env_binary ? fs::path(env_binary) : fs::current_path() / "tfe-code";
    Expect(fs::exists(binary),
           "native CLI not found at " + binary.string() +
               "; build the tfe-code target or set TFE_CODE_NATIVE");

    const fs::path root = fs::temp_directory_path() / "tfe-code-error-contracts";
    fs::remove_all(root);
    Write(root / "a.py", "def one():\n    return 1\n");
    {
      tfe::code::Workspace workspace(root);
      workspace.Import({"a.py"});
    }

    // `ignore` is idempotent-safe: the second run is an error, must not rewrite
    // the file, and must be distinguishable from a usage error.
    {
      const auto first = Run(root, binary, {"ignore"});
      Expect(first.code == 0, "first `ignore` did not succeed: " + first.err);
      const auto ignore_path = root / ".tfe-codeignore";
      Expect(fs::exists(ignore_path), "`ignore` did not create .tfe-codeignore");
      const auto defaults = Read(ignore_path);
      const auto source_before = Read(root / "a.py");
      const auto project_before = Read(root / ".tfe-code/project.json");
      const auto second = Run(root, binary, {"ignore"});
      Expect(second.code == 1, "repeated `ignore` exit code was " +
                                   std::to_string(second.code) + ", expected 1");
      Expect(Read(ignore_path) == defaults,
             "repeated `ignore` rewrote the existing rules file");
      Expect(second.err.find(".tfe-codeignore") != std::string::npos,
             "repeated `ignore` did not name the conflicting file: " + second.err);
      Expect(second.err.find("Usage:") == std::string::npos,
             "repeated `ignore` was reported as a usage error");
      Expect(Read(root / "a.py") == source_before &&
                 Read(root / ".tfe-code/project.json") == project_before,
             "repeated `ignore` touched managed files");
    }

    // Wrong arity is a usage error (2), not a rejected operation (1).
    {
      const auto extra = Run(root, binary, {"ignore", "extra"});
      Expect(extra.code == 2, "`ignore extra` exit code was " +
                                  std::to_string(extra.code) + ", expected 2");
      Expect(extra.err.find("Usage:") != std::string::npos,
             "`ignore extra` did not print usage: " + extra.err);
      const auto none = Run(root, binary, {});
      Expect(none.code == 2, "no-argument exit code was " + std::to_string(none.code) +
                                 ", expected 2");
      Expect(none.err.find("Usage:") != std::string::npos,
             "no-argument invocation did not print usage: " + none.err);
    }

    // A stale expected version is an operation rejection: exit 1, no rewrite.
    {
      const auto source_before = Read(root / "a.py");
      const auto project_before = Read(root / ".tfe-code/project.json");
      const auto state_before = Read(root / ".tfe-code/state.json");
      const auto stale = Run(
          root, binary, {"apply"},
          R"({"edits":[{"role":"a.one","expected_version":"0000","old":"return 1","new":"return 2"}]})");
      Expect(stale.code == 1,
             "stale version exit code was " + std::to_string(stale.code) + ", expected 1");
      Expect(stale.err.find("Stale") != std::string::npos,
             "stale version did not report staleness: " + stale.err);
      Expect(stale.err.find("Usage:") == std::string::npos,
             "stale version was reported as a usage error");
      Expect(Read(root / "a.py") == source_before &&
                 Read(root / ".tfe-code/project.json") == project_before &&
                 Read(root / ".tfe-code/state.json") == state_before,
             "rejected stale apply modified the workspace");
    }

    // Malformed apply JSON is also an operation rejection with exit 1.
    {
      const auto source_before = Read(root / "a.py");
      const auto project_before = Read(root / ".tfe-code/project.json");
      const auto malformed = Run(root, binary, {"apply"}, "this is not json");
      Expect(malformed.code == 1, "invalid apply JSON exit code was " +
                                      std::to_string(malformed.code) +
                                      ", expected 1");
      Expect(malformed.err.find("JSON") != std::string::npos,
             "invalid apply JSON did not mention JSON: " + malformed.err);
      Expect(Read(root / "a.py") == source_before &&
                 Read(root / ".tfe-code/project.json") == project_before,
             "invalid apply JSON modified the workspace");
    }

    // An empty `old` is rejected with exit 1 and never rewrites the source.
    {
      const auto source_before = Read(root / "a.py");
      const auto project_before = Read(root / ".tfe-code/project.json");
      const auto empty_old = Run(root, binary, {"apply"},
                                 R"({"edits":[{"role":"a.one","expected_version":"x","old":"","new":"y"}]})");
      Expect(empty_old.code == 1, "empty `old` exit code was " +
                                      std::to_string(empty_old.code) + ", expected 1");
      Expect(empty_old.err.find("old") != std::string::npos,
             "empty `old` error did not name the field: " + empty_old.err);
      Expect(Read(root / "a.py") == source_before &&
                 Read(root / ".tfe-code/project.json") == project_before,
             "empty `old` modified the workspace");
    }
    fs::remove_all(root);

    std::cout << "error_contracts_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}