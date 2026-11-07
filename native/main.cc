#include <cstdlib>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <ctre.hpp>
#include <rfl/json.hpp>

#include "tf/logger.h"
#include "workspace.h"

namespace {

struct InputReplacement {
  std::string old;
  rfl::Rename<"new", std::string> new_text;
};

struct InputEdit {
  std::string role;
  std::string expected_version;
  std::string old;
  rfl::Rename<"new", std::string> new_text;
  // Several independent replacements in one role. Use this instead of repeating
  // the role entry, which is rejected as ambiguous: changing a signature and the
  // body call that uses it is one role with two edits, not two roles.
  std::vector<InputReplacement> replace;
  bool drop = false;
};

struct InputBatch {
  std::vector<InputEdit> edits;
};

std::string Trim(const std::string& value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t nl = text.find('\n', start);
    if (nl == std::string::npos) {
      if (start < text.size()) {
        std::string tail = text.substr(start);
        if (!tail.empty() && tail.back() == '\r') tail.pop_back();
        lines.push_back(std::move(tail));
      }
      break;
    }
    std::string line = text.substr(start, nl - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
    start = nl + 1;
  }
  return lines;
}

std::string JoinLines(const std::vector<std::string>& lines) {
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) out += '\n';
    out += lines[i];
  }
  return out;
}

// Parses the compact `#@ ROLE [VERSION]` patch format so callers do not pay
// JSON-escaping overhead or repeat 64-character hashes on multi-role edits.
std::vector<tfe::code::RoleEdit> ParsePatchInput(const std::string& input) {
  std::vector<tfe::code::RoleEdit> edits;
  std::optional<tfe::code::RoleEdit> current;
  enum class State { kOutside, kInOld, kInNew };
  State state = State::kOutside;
  std::vector<std::string> old_lines;
  std::vector<std::string> new_lines;

  auto flush = [&]() {
    if (!current) return;
    if (!current->drop && current->replace.empty()) {
      throw std::runtime_error("Apply patch role has no <<< === >>> block: " + current->role);
    }
    edits.push_back(std::move(*current));
    current.reset();
  };

  for (const auto& line : SplitLines(input)) {
    if (state == State::kOutside) {
      if (Trim(line).empty()) continue;
      if (line.starts_with("#@")) {
        flush();
        static constexpr auto kHeader =
            ctre::match<R"(^#@\s+([\w.\-]+)\s+\[(?:v:|version:)?([0-9a-zA-Z]+)\](?:\s+\[file:\s*[^\]]+\])?)">;
        if (auto [m, r, v] = kHeader(line); m) {
          tfe::code::RoleEdit next;
          next.role = r.to_string();
          std::string ver = v.to_string();
          if (ver == "DROP" || ver == "drop") {
            next.drop = true;
          } else {
            next.expected_version = std::move(ver);
          }
          current = std::move(next);
        } else {
          const std::string rest = Trim(line.substr(2));
          const size_t open = rest.find('[');
          const size_t close = open == std::string::npos ? std::string::npos : rest.find(']', open + 1);
          if (open == std::string::npos || close == std::string::npos) {
            throw std::runtime_error(
                "Invalid apply patch header (expected '#@ ROLE [VERSION]'): " + line);
          }
          const std::string role = Trim(rest.substr(0, open));
          if (role.empty()) {
            throw std::runtime_error("Apply patch header is missing role name: " + line);
          }
          throw std::runtime_error("Apply patch header is missing version for role " + role);
        }
      } else if (line == "<<<") {
        if (!current) {
          throw std::runtime_error("Apply patch '<<<' appeared before '#@ ROLE [VERSION]'");
        }
        old_lines.clear();
        state = State::kInOld;
      } else if (Trim(line) == "DROP") {
        if (!current) {
          throw std::runtime_error("Apply patch 'DROP' appeared before '#@ ROLE [VERSION]'");
        }
        current->drop = true;
      } else {
        throw std::runtime_error(
            "Unexpected line in apply patch (expected '#@ ROLE [VERSION]' or '<<<'): " + line);
      }
    } else if (state == State::kInOld) {
      if (line == "===") {
        new_lines.clear();
        state = State::kInNew;
      } else {
        old_lines.push_back(line);
      }
    } else if (state == State::kInNew) {
      if (line == ">>>") {
        current->replace.emplace_back(JoinLines(old_lines), JoinLines(new_lines));
        state = State::kOutside;
      } else {
        new_lines.push_back(line);
      }
    }
  }
  if (state != State::kOutside) {
    throw std::runtime_error("Unterminated <<< === >>> replacement block in apply patch");
  }
  flush();
  return edits;
}

std::string Usage() {
  return "Usage: tfe-code ignore | import PATH... | ls [FILE|ROLE] | "
         "context [QUERY...] [--file FILE]... [--callers] | "
         "outline [FILE|ROLE] [--depth N] [--relative] | "
         "tree FILE|ROLE | show ROLE... [--json] | role FILE NAME | "
         "search QUERY... [--prefix PREFIX] [--file FILE] [--ids] [--regex] | who [PREFIX.] NAME... | "
         "prepare-batch [QUERY...] [--file FILE]... | "
         "apply [--amend] [--check COMMAND] < patch_or_json | render [--write] | "
         "sync | check COMMAND | help";
}

// The apply payload is the one place a caller has to guess a shape, so print it
// on demand and repeat it when parsing fails. Without this an agent that sends
// the wrong field names has no way to discover the right ones except by reading
// the binary.
std::string ApplySchema() {
  return R"(tfe-code apply [--amend] [--check COMMAND] -- apply a batch of role edits from stdin

Form 1 -- Compact patch (recommended: no JSON escaping, accepts short role aliases & >=8-char version prefixes from `context`):

  #@ mod.Class.method [v:28b4e14d46db]
  <<<
      old fragment
  ===
      new fragment
  >>>

Multiple <<< === >>> blocks under one #@ header apply in order to that role.

Form 2 -- JSON object (not an array):

  {"edits":[{"role":"mod.fn","expected_version":"28b4e14d46db","old":"fragment","new":"replacement"}]}

  {"edits":[{"role":"mod.fn","expected_version":"28b4e14d46db",
             "replace":[{"old":"first fragment","new":"..."},
                        {"old":"second fragment","new":"..."}]}]}

Rules:
- Each `old` must be nonempty and match once in the dedented role text (copy from `context` or `show`).
- `role` accepts a full ID or a unique suffix (`events.Event`); `expected_version` accepts >=8 hex chars or full SHA-256.
- With `--check COMMAND`, a failing check rolls back all files and saves `.tfe-code/last_failed_batch.json`.
- With `--amend`, stdin overrides only the named roles in the last failed batch (use `#@ role [DROP]` or `"drop":true` to drop an entry).)";
}

std::string Join(int argc, char** argv, int start) {
  std::string result;
  for (int i = start; i < argc; ++i) {
    if (!result.empty()) result += ' ';
    result += argv[i];
  }
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  // Engine diagnostics go to stderr. The default is Off because the engine logs a
  // "Block not found" error for every role's first publish and an agent merges
  // stderr into the command output, which would inflate every attempt's cost.
  // Set TFE_CODE_LOG=error|info|debug to restore engine detail.
  const char* log = std::getenv("TFE_CODE_LOG");
  const std::string level = log == nullptr ? "off" : log;
  tf::Logger::init(level == "debug" ? tf::LogLevel::Debug
                  : level == "info" ? tf::LogLevel::Info
                  : level == "error" ? tf::LogLevel::Error
                                     : tf::LogLevel::Off);
  try {
    if (argc < 2) {
      std::cerr << Usage() << '\n';
      return 2;
    }
    tfe::code::Workspace workspace(std::filesystem::current_path());
    const std::string command = argv[1];
    // A help request is not a usage error: an agent that sees exit 2 concludes
    // the tool is broken and starts probing the workspace by hand.
    if (command == "help" || command == "--help" || command == "-h") {
      std::cout << Usage() << '\n';
      return 0;
    }
    if (command == "ignore" && argc == 2) {
      workspace.CreateIgnoreFile();
      std::cout << "Created .tfe-codeignore\n";
    } else if (command == "import" && argc >= 3) {
      std::vector<std::string> paths;
      for (int i = 2; i < argc; ++i) paths.push_back(argv[i]);
      std::cout << rfl::json::write(workspace.Import(paths)) << '\n';
    } else if (command == "outline") {
      std::string target;
      int depth = 99;
      bool relative = false;
      for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--depth" && i + 1 < argc) {
          depth = std::stoi(argv[++i]);
        } else if (std::string(argv[i]) == "--relative") {
          relative = true;
        } else if (target.empty()) {
          target = argv[i];
        } else {
          throw std::invalid_argument("invalid outline arguments");
        }
      }
      std::cout << workspace.Outline(target, depth, relative);
    } else if (command == "tree" && argc == 3) {
      std::cout << workspace.Tree(argv[2]) << '\n';
    } else if (command == "show" && argc >= 3) {
      bool json = false;
      std::vector<std::string> ids;
      for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--json") json = true;
        else ids.push_back(argv[i]);
      }
      if (ids.empty()) throw std::invalid_argument("show expects role ids");
      if (json) std::cout << rfl::json::write(workspace.ShowRoles(ids)) << '\n';
      else std::cout << workspace.Show(ids) << '\n';
    } else if (command == "role" && argc == 4) {
      std::cout << workspace.FindRole(argv[2], argv[3]) << '\n';
    } else if (command == "ls") {
      std::cout << workspace.List(argc >= 3 ? argv[2] : "") << '\n';
    } else if (command == "context") {
      std::vector<std::string> files;
      std::vector<std::string> terms;
      bool callers = false;
      for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--file") {
          if (i + 1 >= argc) throw std::invalid_argument("context --file expects a value");
          files.push_back(argv[++i]);
        } else if (std::string(argv[i]) == "--callers") {
          callers = true;
        } else {
          terms.push_back(argv[i]);
        }
      }
      std::cout << workspace.Context(terms, files, callers) << '\n';
    } else if (command == "prepare-batch") {
      std::vector<std::string> files;
      std::vector<std::string> terms;
      for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--file") {
          if (i + 1 >= argc) throw std::invalid_argument("prepare-batch --file expects a value");
          files.push_back(argv[++i]);
        } else {
          terms.push_back(argv[i]);
        }
      }
      std::cout << workspace.PrepareBatch(terms, files) << '\n';
    } else if (command == "search" || command == "who") {
      std::string prefix;
      std::string file;
      std::vector<std::string> terms;
      bool ids_only = false;
      bool use_regex = false;
      int start = 2;
      if (command == "who" && argc >= 3 && std::string(argv[2]).ends_with('.')) {
        prefix = argv[2];
        start = 3;
      }
      for (int i = start; i < argc; ++i) {
        if (std::string(argv[i]) == "--prefix") {
          if (i + 1 >= argc) throw std::invalid_argument("search --prefix expects a value");
          prefix = argv[++i];
        } else if (std::string(argv[i]) == "--file") {
          if (i + 1 >= argc) throw std::invalid_argument("search --file expects a value");
          file = argv[++i];
          if (file.empty()) throw std::invalid_argument("search --file expects a nonempty value");
        } else if (std::string(argv[i]) == "--ids") {
          ids_only = true;
        } else if (std::string(argv[i]) == "--regex") {
          use_regex = true;
        } else {
          terms.push_back(argv[i]);
        }
      }
      std::cout << workspace.Search(terms, prefix, file, ids_only, use_regex) << '\n';
    } else if (command == "apply") {
      if (argc == 3 && (std::string(argv[2]) == "--help" || std::string(argv[2]) == "help")) {
        std::cout << ApplySchema() << '\n';
        return 0;
      }
      bool amend = false;
      std::optional<std::string> check;
      for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--amend") {
          amend = true;
        } else if (arg == "--check" && i + 1 < argc) {
          check = argv[++i];
        } else {
          throw std::invalid_argument("apply accepts only [--amend] [--check COMMAND]");
        }
      }
      const std::string input{std::istreambuf_iterator<char>(std::cin), {}};
      const std::string trimmed = Trim(input);
      std::vector<tfe::code::RoleEdit> edits;
      if (trimmed.starts_with("#@")) {
        edits = ParsePatchInput(input);
      } else {
        const auto parsed = rfl::json::read<InputBatch, rfl::DefaultIfMissing>(input);
        if (!parsed) {
          // Name the shape instead of echoing an rfl internals message: a caller
          // that guessed the field names needs the schema, not a cast error.
          std::cerr << "Invalid apply JSON: " << parsed.error().what() << "\n\n" << ApplySchema()
                    << '\n';
          return 1;
        }
        for (const auto& edit : parsed.value().edits) {
          if (edit.role.empty()) throw std::runtime_error("apply edit is missing 'role'");
          if (!edit.drop && edit.expected_version.empty()) {
            throw std::runtime_error("apply edit is missing 'expected_version' for role " +
                                     edit.role);
          }
          tfe::code::RoleEdit role_edit{edit.role, edit.expected_version, edit.old,
                                        edit.new_text.get(), {}, edit.drop};
          for (const auto& replacement : edit.replace) {
            role_edit.replace.emplace_back(replacement.old, replacement.new_text.get());
          }
          edits.push_back(std::move(role_edit));
        }
      }
      std::cout << rfl::json::write(workspace.Apply(edits, check, amend)) << '\n';
    } else if (command == "render") {
      const bool write = argc == 3 && std::string(argv[2]) == "--write";
      if (argc != 2 && !write) throw std::invalid_argument("render accepts only --write");
      if (write) {
        // Confirm what was materialised without echoing it. Printing the file
        // contents here repeats the whole tree into the caller's context, which
        // for a real project runs to thousands of tokens for nothing.
        std::vector<std::string> written;
        for (const auto& [path, bytes] : workspace.Materialize()) written.push_back(path);
        std::cout << rfl::json::write(written) << '\n';
      } else {
        std::cout << rfl::json::write(workspace.Render()) << '\n';
      }
    } else if (command == "sync" && argc == 2) {
      std::cout << rfl::json::write(workspace.Sync()) << '\n';
    } else if (command == "check" && argc >= 3) {
      const auto receipt = workspace.Check(Join(argc, argv, 2));
      std::cout << rfl::json::write(receipt) << '\n';
      return receipt.passed ? 0 : 1;
    } else {
      std::cerr << Usage() << '\n';
      return 2;
    }
    return 0;
  } catch (const std::invalid_argument& error) {
    std::cerr << error.what() << '\n' << Usage() << '\n';
    return 2;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
