// Fast multi-role route tests: selective `context` reads, short version prefixes,
// unambiguous role suffixes, compact `#@` patch batches, multi-error batch
// diagnostics with target versions, `--amend` recovery, and accurate `changed`
// reporting on unmaterialised ObjectBox workspaces.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "workspace.h"

namespace fs = std::filesystem;

namespace {

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

struct CliResult {
  int code = -1;
  std::string out;
  std::string err;
};

CliResult RunCli(const fs::path& root, const fs::path& binary,
                 const std::vector<std::string>& args,
                 const std::string& stdin_text = "") {
  const auto out_path = root / ".tfe-fast-test.stdout";
  const auto err_path = root / ".tfe-fast-test.stderr";
  const auto in_path = root / ".tfe-fast-test.stdin";
  Write(in_path, stdin_text);
  std::string command = "cd " + Quote(root.string()) + " && " + Quote(binary.string());
  for (const auto& arg : args) command += " " + Quote(arg);
  command += " <" + Quote(in_path.string());
  command += " >" + Quote(out_path.string()) + " 2>" + Quote(err_path.string());
  const int status = std::system(command.c_str());
  CliResult result;
  result.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  result.out = Read(out_path);
  result.err = Read(err_path);
  fs::remove(out_path);
  fs::remove(err_path);
  fs::remove(in_path);
  return result;
}

}  // namespace

int main() {
  try {
    const char* env_binary = std::getenv("TFE_CODE_NATIVE");
    const fs::path binary = env_binary ? fs::path(env_binary) : fs::current_path() / "tfe-code";
    Expect(fs::exists(binary), "native CLI not found at " + binary.string());

    // 1. ObjectBox unmaterialised workspace reports only logically changed files.
    {
      const fs::path root = fs::temp_directory_path() / "tfe-code-fast-obj-changed";
      fs::remove_all(root);
      Write(root / "src/pkg/events.py", "def emit():\n    return 'ok'\n");
      Write(root / "src/pkg/untouched.py", "def stay():\n    return 42\n");
      setenv("TFE_CODE_INDEX", "objectbox", 1);
      {
        tfe::code::Workspace ws(root);
        ws.Import({"src"});
      }
      fs::remove_all(root / "src");
      fs::create_directories(root / "src");
      {
        tfe::code::Workspace ws(root);
        const auto ver = ws.LoadProject().blocks.at("src.pkg.events.emit").version;
        const auto res = ws.Apply(
            {{"events.emit", ver.substr(0, 8), "return 'ok'", "return 'info'"}},
            std::nullopt);
        Expect(res.changed == std::vector<std::string>{"src/pkg/events.py"},
               "db apply reported untouched files as changed: count=" +
                   std::to_string(res.changed.size()));
        Expect(fs::exists(root / "src/pkg/untouched.py"),
               "db apply did not materialise untouched sibling source for test suite");
      }
      unsetenv("TFE_CODE_INDEX");
      fs::remove_all(root);
    }

    // 2. Short version prefixes, unambiguous suffixes, multi-error batch diagnostics,
    //    and versioned FragmentSuggestion.
    const fs::path root = fs::temp_directory_path() / "tfe-code-fast-route";
    fs::remove_all(root);
    Write(root / "src/app/events.py",
          "from datetime import datetime\n\n\n"
          "class Event:\n"
          "    def kind(self) -> str:\n"
          "        return 'raw'\n");
    Write(root / "src/app/store.py",
          "import sqlite3\n\n\n"
          "class EventStore:\n"
          "    # section header\n\n"
          "    def init_schema(self):\n"
          "        return 'CREATE TABLE events(id TEXT)'\n\n"
          "    def append(self, item):\n"
          "        return item\n\n"
          "    def close(self):\n"
          "        return None\n\n"
          "    def count(self):\n"
          "        return 0\n\n"
          "    # trailing section\n"
          "    def ping(self):\n"
          "        return True\n");
    Write(root / "src/app/caller.py",
          "from app.events import Event\n\n\n"
          "def handle_click():\n"
          "    ev = Event()\n"
          "    return ev.kind()\n");
    Write(root / "src/a/dup.py", "def same():\n    return 1\n");
    Write(root / "src/b/dup.py", "def same():\n    return 2\n");

    tfe::code::Workspace workspace(root);
    workspace.Import({"src"});

    // Ambiguous suffix `dup.same` must be rejected cleanly.
    {
      const auto ver = workspace.LoadProject().blocks.at("src.a.dup.same").version;
      bool ambiguous_rejected = false;
      try {
        (void)workspace.Apply({{"dup.same", ver.substr(0, 8), "return 1", "return 9"}},
                              std::nullopt);
      } catch (const std::exception& e) {
        ambiguous_rejected = std::string(e.what()).find("Ambiguous") != std::string::npos;
      }
      Expect(ambiguous_rejected, "ambiguous suffix dup.same was not rejected");
    }

    // Multi-error reporting + version hint in FragmentSuggestion.
    {
      const auto proj = workspace.LoadProject();
      const auto kind_ver = proj.blocks.at("src.app.events.Event.kind").version;
      const auto file_ver = proj.blocks.at("src.app.events.file").version;
      tfe::code::RoleEdit e1{"events.Event.kind", kind_ver.substr(0, 10),
                             "from datetime import datetime", "from datetime import date"};
      tfe::code::RoleEdit e2{"store.EventStore.append", "deadbeef0000", "return item", "return None"};
      bool both_reported = false;
      try {
        (void)workspace.Apply({e1, e2}, std::nullopt);
      } catch (const std::exception& e) {
        const std::string msg = e.what();
        both_reported = msg.find("events.file") != std::string::npos &&
                        msg.find(file_ver.substr(0, 12)) != std::string::npos &&
                        msg.find("Stale") != std::string::npos &&
                        msg.find("EventStore.append") != std::string::npos;
      }
      Expect(both_reported, "Apply did not report all batch errors and target version in one pass");
    }

    // 3. `context` returns module header + matched leaves, omits unrelated methods,
    //    collapses blank placeholder gaps, expands directly queried classes, and
    //    supports --callers to find referencing roles in one line.
    {
      const std::string ctx = workspace.Context({"init_schema", "kind"}, {});
      Expect(ctx.find("#@ events.file [v:") != std::string::npos,
             "context missed enclosing module header for events.py: " + ctx);
      Expect(ctx.find("from datetime import datetime") != std::string::npos,
             "context missed module imports: " + ctx);
      Expect(ctx.find("#@ events.Event.kind [v:") != std::string::npos,
             "context missed matched leaf role events.Event.kind: " + ctx);
      Expect(ctx.find("#@ store.EventStore.init_schema [v:") != std::string::npos,
             "context missed matched leaf role store.EventStore.init_schema: " + ctx);
      Expect(ctx.find("omitted") != std::string::npos,
             "context did not summarise omitted sibling roles: " + ctx);
      Expect(ctx.find("return None") == std::string::npos,
             "context leaked body of unrelated sibling role EventStore.close");
      Expect(ctx.find("src.app.store.EventStore.init_schema") == std::string::npos,
             "context repeated full package prefix instead of unambiguous alias");
      Expect(ctx.find("\n\n\n") == std::string::npos,
             "context emitted consecutive blank lines from stripped placeholders: " + ctx);

      // Querying `class Event` directly (without naming `kind`) must expand `Event.kind`
      // in one call instead of forcing a second read step.
      const std::string class_ctx = workspace.Context({"class Event:"}, {"src/app/events.py"});
      Expect(class_ctx.find("#@ events.Event.kind [v:") != std::string::npos &&
                 class_ctx.find("return 'raw'") != std::string::npos,
             "querying class Event did not expand its methods: " + class_ctx);

      // context with include_callers=true must append a # Callers section with
      // one-line references to matched identifiers.
      const std::string callers_ctx = workspace.Context({"kind"}, {"src/app/events.py"}, true);
      Expect(callers_ctx.find("# Callers:") != std::string::npos,
             "context --callers missed # Callers header: " + callers_ctx);
      Expect(callers_ctx.find("caller.handle_click: return ev.kind()") != std::string::npos,
             "context --callers missed caller in caller.py: " + callers_ctx);
      Expect(callers_ctx.find("events.Event.kind") != std::string::npos,
             "context --callers missed target role definition: " + callers_ctx);

      // 3.1 RE2 regular expression search
      const std::string re_found = workspace.Search({"def\\s+kind\\(self\\)"}, "", "", false, true);
      Expect(re_found.find("src.app.events.Event.kind") != std::string::npos,
             "Search with RE2 failed to match regex pattern: " + re_found);
      const std::string re_ids = workspace.Search({"def\\s+handle_.*"}, "", "", true, true);
      Expect(re_ids.find("src.app.caller.handle_click: def handle_click():") != std::string::npos,
             "Search with RE2 --ids failed: " + re_ids);
      bool bad_regex_threw = false;
      try {
        (void)workspace.Search({"[invalid("}, "", "", false, true);
      } catch (const std::invalid_argument& e) {
        bad_regex_threw = std::string(e.what()).find("Invalid regular expression") != std::string::npos;
      }
      Expect(bad_regex_threw, "Search with invalid regex did not throw clear error");
    }

    // 4. CLI compact `#@` patch format + `--amend` recovery after a failed check.
    {
      const auto proj = workspace.LoadProject();
      const auto file_v = proj.blocks.at("src.app.events.file").version.substr(0, 12);
      const auto kind_v = proj.blocks.at("src.app.events.Event.kind").version.substr(0, 12);
      const auto init_v = proj.blocks.at("src.app.store.EventStore.init_schema").version.substr(0, 12);

      const std::string patch =
          "#@ events.file [v:" + file_v + "]\n"
          "<<<\n"
          "from datetime import datetime\n"
          "===\n"
          "from datetime import datetime, timezone\n"
          ">>>\n\n"
          "#@ events.Event.kind [" + kind_v + "]\n"
          "<<<\n"
          "    return 'raw'\n"
          "===\n"
          "    return 'wrong'\n"
          ">>>\n\n"
          "#@ store.EventStore.init_schema [" + init_v + "]\n"
          "<<<\n"
          "    return 'CREATE TABLE events(id TEXT)'\n"
          "===\n"
          "    return 'CREATE TABLE events(id TEXT, level TEXT)'\n"
          ">>>\n";

      // Check fails because Event.kind returned 'wrong' instead of 'info'.
      const auto failed = RunCli(
          root, binary,
          {"apply", "--check",
           "python3 -c \"import sys; sys.path.insert(0, 'src'); "
           "from app.events import Event; assert Event().kind() == 'info'\""},
          patch);
      Expect(failed.code == 1, "failing check must exit 1");
      Expect(Read(root / "src/app/events.py").find("timezone") == std::string::npos,
             "failed check did not roll back source files");

      // Now amend ONLY `events.Event.kind` without resending `events.file` or `store.py`!
      const std::string amend_patch =
          "#@ events.Event.kind [" + kind_v + "]\n"
          "<<<\n"
          "    return 'raw'\n"
          "===\n"
          "    return 'info'\n"
          ">>>\n";

      const auto passed = RunCli(
          root, binary,
          {"apply", "--amend", "--check",
           "python3 -c \"import sys; sys.path.insert(0, 'src'); "
           "from app.events import Event; from app.store import EventStore; "
           "assert Event().kind() == 'info'; "
           "assert 'level TEXT' in EventStore().init_schema()\""},
          amend_patch);
      Expect(passed.code == 0, "--amend batch failed: " + passed.err);
      Expect(Read(root / "src/app/events.py").find("timezone") != std::string::npos,
             "--amend lost the earlier events.file edit");
      Expect(Read(root / "src/app/store.py").find("level TEXT") != std::string::npos,
             "--amend lost the earlier store.py edit");
      Expect(!fs::exists(root / ".tfe-code/last_failed_batch.json"),
             "successful apply did not clean up last_failed_batch.json");
    }

    fs::remove_all(root);
    std::cout << "fast_route_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
