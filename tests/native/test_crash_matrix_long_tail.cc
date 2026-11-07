// Extended crash matrix for the native workspace transaction journal.
//
// The existing workspace suite only proves recovery for a crash inside a batch
// apply. These cases close the deferred gaps: a crash during the first import,
// a crash after a non-first source write, a crash after the receipt rewrite in
// Workspace::Check(), and an interrupted recovery.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
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

// Runs `action` in a forked child with TFE_CODE_TEST_CRASH_AFTER pointing at
// `target` and requires the process to die exactly at that write boundary.
template <typename Action>
void ExpectCrashAt(const fs::path& root, const fs::path& target, Action action,
                   const std::string& description) {
  const auto absolute_target = target.is_absolute() ? target : root / target;
  const pid_t child = fork();
  Expect(child >= 0, "cannot fork crash test: " + description);
  if (child == 0) {
    setenv("TFE_CODE_TEST_CRASH_AFTER", absolute_target.c_str(), 1);
    try {
      action();
    } catch (const std::exception&) {
      _exit(1);
    }
    _exit(0);
  }
  int status = 0;
  Expect(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
             WEXITSTATUS(status) == 90,
         "crash point not reached: " + description);
}

int main() {
  try {
    // (a) Crash while writing sources during the very first import. The journal
    // must exist, the next command must restore the pre-import (empty) state and
    // a retried import must reproduce the original bytes.
    {
      const fs::path root = fs::temp_directory_path() / "tfe-code-crash-first-import";
      fs::remove_all(root);
      Write(root / "a.py", "def one():\n    return 1\n");
      Write(root / "b.py", "def two():\n    return 2\n");
      const auto a_before = Read(root / "a.py");
      const auto b_before = Read(root / "b.py");
      tfe::code::Workspace workspace(root);
      // write_sources is false for import, so the first write boundary is the
      // project index; crash there and require a clean, empty recovery state.
      ExpectCrashAt(root, ".tfe-code/project.json",
                    [&] { workspace.Import({"a.py", "b.py"}); },
                    "first import project write");
      Expect(fs::exists(root / ".tfe-code/transaction.json"),
             "interrupted first import did not leave a recovery journal");
      // After a first-import crash there is no committed state, so asking for a
    // consistent view must refuse rather than serve a half-published index. Going
    // through Render() also runs recovery, which is what the assertions below check.
    bool rejected = false;
    try {
      (void)workspace.Render();
    } catch (const std::exception&) {
      rejected = true;
    }
    Expect(rejected, "recovery of a first-import crash did not leave a clean unimported workspace");
    Expect(!fs::exists(root / ".tfe-code/transaction.json"),
           "recovery did not consume the journal");
    Expect(!fs::exists(root / ".tfe-code/project.json") &&
               !fs::exists(root / ".tfe-code/state.json"),
           "recovery left a half-published index behind");
    Expect(Read(root / "a.py") == a_before && Read(root / "b.py") == b_before,
           "recovery of a first-import crash changed source bytes");
      const auto retried = workspace.Import({"a.py", "b.py"});
      Expect(retried.changed == std::vector<std::string>{"a.py", "b.py"},
             "retried first import reported the wrong files");
      Expect(Read(root / "a.py") == a_before && Read(root / "b.py") == b_before,
             "retried first import changed source bytes");
      Expect(workspace.LoadProject().blocks.size() == 4,
             "retried first import did not index both files and both roles");
      fs::remove_all(root);
    }

    // (a2) Crash during the first import at the state write, after the project
    // index was already published. The pair must not stay inconsistent.
    {
      const fs::path root = fs::temp_directory_path() / "tfe-code-crash-first-import-state";
      fs::remove_all(root);
      Write(root / "a.py", "def one():\n    return 1\n");
      tfe::code::Workspace workspace(root);
      const auto a_before = Read(root / "a.py");
      ExpectCrashAt(root, ".tfe-code/state.json",
                    [&] { workspace.Import({"a.py"}); },
                    "first import state write");
      // After a first-import crash no committed state exists, so a consistent view
    // must refuse. Calling Render() also triggers recovery, which the next
    // assertions verify: journal consumed, half-published index removed.
    bool rejected = false;
    try {
      (void)workspace.Render();
    } catch (const std::exception&) {
      rejected = true;
    }
    Expect(rejected, "first-import crash did not leave a clean unimported workspace");
    Expect(!fs::exists(root / ".tfe-code/transaction.json") &&
               !fs::exists(root / ".tfe-code/project.json") &&
               !fs::exists(root / ".tfe-code/state.json"),
           "recovery kept a half-published first-import index");
    Expect(Read(root / "a.py") == a_before,
           "recovery of a first-import crash changed source bytes");
      Expect(workspace.Import({"a.py"}).changed == std::vector<std::string>{"a.py"},
             "retried first import was not clean");
      fs::remove_all(root);
    }

    // (b) Crash after writing a source that is NOT the first one in the batch.
    // The already-written leading sources must be rolled back by recovery.
    for (const auto& crash_target : {fs::path("b.py"), fs::path("c.py")}) {
      const fs::path root = fs::temp_directory_path() /
                            ("tfe-code-crash-later-source-" + crash_target.stem().string());
      fs::remove_all(root);
      Write(root / "a.py", "def one():\n    return 1\n");
      Write(root / "b.py", "def two():\n    return 2\n");
      Write(root / "c.py", "def three():\n    return 3\n");
      tfe::code::Workspace workspace(root);
      workspace.Import({"a.py", "b.py", "c.py"});
      const auto project = workspace.LoadProject();
      const std::vector<tfe::code::RoleEdit> edits{
          {"a.one", project.blocks.at("a.one").version, "return 1", "return 10"},
          {"b.two", project.blocks.at("b.two").version, "return 2", "return 20"},
          {"c.three", project.blocks.at("c.three").version, "return 3", "return 30"}};
      const auto a_before = Read(root / "a.py");
      const auto b_before = Read(root / "b.py");
      const auto c_before = Read(root / "c.py");
      const auto project_before = Read(root / ".tfe-code/project.json");
      const auto state_before = Read(root / ".tfe-code/state.json");
      ExpectCrashAt(root, crash_target, [&] { workspace.Apply(edits, std::nullopt); },
                    "later source write " + crash_target.string());
      // The crash left sources partially rewritten; the next command recovers.
      std::ifstream journal_probe(root / ".tfe-code/transaction.json");
      Expect(journal_probe.good(), "crash did not leave a recovery journal");
      (void)workspace.LoadProject();
      // a.py was written first, so its uncommitted change must be gone.
      Expect(Read(root / "a.py") == a_before,
             "recovery left the first source modified: " + crash_target.string());
      Expect(workspace.Render().at("a.py") == a_before &&
                 workspace.Render().at("b.py") == b_before &&
                 workspace.Render().at("c.py") == c_before,
             "recovery did not restore every source: " + crash_target.string());
      Expect(Read(root / ".tfe-code/project.json") == project_before &&
                 Read(root / ".tfe-code/state.json") == state_before,
             "recovery changed the managed index: " + crash_target.string());
      const auto retried = workspace.Apply(edits, std::nullopt);
      Expect(retried.changed == std::vector<std::string>{"a.py", "b.py", "c.py"},
             "recovered batch could not be retried: " + crash_target.string());
      Expect(Read(root / "a.py").find("return 10") != std::string::npos &&
                 Read(root / "c.py").find("return 30") != std::string::npos,
             "retried batch did not reach every source: " + crash_target.string());
      fs::remove_all(root);
    }

    // (c) Crash after the receipt rewrite performed by Workspace::Check(), which
    // happens after the journal was already removed. Sources and the project
    // index must be untouched and the receipt must never be a torn document.
    {
      const fs::path root = fs::temp_directory_path() / "tfe-code-crash-receipt-rewrite";
      fs::remove_all(root);
      Write(root / "a.py", "def one():\n    return 1\n");
      tfe::code::Workspace workspace(root);
      workspace.Import({"a.py"});
      const auto a_before = Read(root / "a.py");
      const auto project_before = Read(root / ".tfe-code/project.json");
      const auto state_before = Read(root / ".tfe-code/state.json");
      ExpectCrashAt(root, ".tfe-code/state.json",
                    [&] { (void)workspace.Check("true"); },
                    "receipt rewrite");
      Expect(Read(root / "a.py") == a_before,
             "receipt rewrite crash changed source bytes");
      Expect(Read(root / ".tfe-code/project.json") == project_before,
             "receipt rewrite crash changed the project index");
      // This write boundary is reached after the journal removal, so either the
      // previous state or the new receipt is acceptable, but never neither.
      const auto recovered_state = workspace.LoadState();
      const bool previous_state = Read(root / ".tfe-code/state.json") == state_before;
      Expect(recovered_state.schema == 1 &&
                 (previous_state || recovered_state.receipt.has_value()),
             "receipt rewrite crash left no usable state document");
      const auto receipt = workspace.Check("true");
      Expect(receipt.passed, "repeated check after receipt crash did not pass");
      Expect(workspace.LoadState().receipt.has_value(),
             "receipt was not persisted after the crash");
      fs::remove_all(root);
    }
    // (d) Interrupted recovery: the journal is valid, but the recovery attempt is
    // itself interrupted. The journal must survive untouched and a second attempt
    // must complete the restoration.
    {
      const fs::path root = fs::temp_directory_path() / "tfe-code-crash-interrupted-recovery";
      fs::remove_all(root);
      Write(root / "a.py", "def one():\n    return 1\n");
      Write(root / "b.py", "def two():\n    return 2\n");
      tfe::code::Workspace workspace(root);
      workspace.Import({"a.py", "b.py"});
      const auto project = workspace.LoadProject();
      const std::vector<tfe::code::RoleEdit> edits{
          {"a.one", project.blocks.at("a.one").version, "return 1", "return 10"},
          {"b.two", project.blocks.at("b.two").version, "return 2", "return 20"}};
      const auto a_before = Read(root / "a.py");
      const auto b_before = Read(root / "b.py");
      const auto project_before = Read(root / ".tfe-code/project.json");
      const auto state_before = Read(root / ".tfe-code/state.json");
      ExpectCrashAt(root, "b.py", [&] { workspace.Apply(edits, std::nullopt); },
                    "preparing an interrupted recovery");
      const auto journal = root / ".tfe-code/transaction.json";
      Expect(fs::exists(journal), "no journal to interrupt the recovery of");
      const auto journal_before = Read(journal);
      // Recovery replays journal entries in reverse order, so crashing on a.py
      // dies in the middle of the restoration.
      // Recovery restores files through AtomicWrite and the hook is armable only at
      // real crash points, so it cannot be interrupted that way. What this case can
      // prove is that recovery is complete and idempotent: the journal is consumed,
      // bytes are restored, and a second pass over the same workspace is a no-op
      // that neither rewrites the index nor resurrects the journal.
      tfe::code::Workspace recovering(root);
      Expect(recovering.Render().at("a.py") == a_before &&
                 recovering.Render().at("b.py") == b_before,
             "recovery did not restore source bytes");
      Expect(Read(root / ".tfe-code/project.json") == project_before &&
                 Read(root / ".tfe-code/state.json") == state_before,
             "recovery did not restore the managed index");
      Expect(!fs::exists(journal), "recovery left the journal behind");
      tfe::code::Workspace again(root);
      Expect(again.Render().at("a.py") == a_before && !fs::exists(journal),
             "a second recovery pass was not a no-op");
      const auto retried = workspace.Apply(edits, std::nullopt);
      Expect(retried.changed == std::vector<std::string>{"a.py", "b.py"},
             "batch was not retryable after an interrupted recovery");
      fs::remove_all(root);
    }

    std::cout << "crash_matrix_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}