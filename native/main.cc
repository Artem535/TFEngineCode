#include <cstdlib>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <rfl/json.hpp>

#include "mcp_server.h"
#include "tf/logger.h"
#include "workspace.h"

namespace {

std::string Usage() {
  return "Usage: tfe-code ignore | import PATH... | ls [FILE|ROLE] | "
         "context [QUERY...] [--file FILE]... [--callers|--references] | "
         "outline [FILE|ROLE] [--depth N] [--relative] | "
         "tree FILE|ROLE | show ROLE... [--json] | role FILE NAME | "
         "search QUERY... [--prefix PREFIX] [--file FILE] [--ids] [--regex] | who [PREFIX.] NAME... | "
         "prepare-batch [QUERY...] [--file FILE]... | "
         "apply [--amend] [--check COMMAND] < patch_or_json | "
         "add-role PARENT NAME [--after ROLE] [--check COMMAND] < body | "
         "move-role ROLE [NEW_NAME] [--to PARENT] [--after ROLE] [--check COMMAND] | "
         "delete-role ROLE [--check COMMAND] | "
         "stats [--json] [--reset] | "
         "render [--write] | sync | watch [--once] [--timeout-ms N] | check COMMAND | serve [--mcp] | help";
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
        } else if (std::string(argv[i]) == "--callers" || std::string(argv[i]) == "--references") {
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
        std::cout << tfe::code::ApplyInputSchemaDoc() << '\n';
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
      const auto edits = tfe::code::ParseApplyInput(input);
      std::cout << rfl::json::write(workspace.Apply(edits, check, amend)) << '\n';
    } else if (command == "add-role" && argc >= 4) {
      const std::string parent = argv[2];
      const std::string name = argv[3];
      std::string after_role;
      std::optional<std::string> check;
      for (int i = 4; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--after" && i + 1 < argc) {
          after_role = argv[++i];
        } else if (arg == "--check" && i + 1 < argc) {
          check = argv[++i];
        } else {
          throw std::invalid_argument("add-role accepts only [--after ROLE] [--check COMMAND]");
        }
      }
      const std::string text{std::istreambuf_iterator<char>(std::cin), {}};
      std::cout << rfl::json::write(workspace.AddRole(parent, name, text, after_role, check)) << '\n';
    } else if (command == "move-role" && argc >= 3) {
      const std::string role = argv[2];
      std::string new_name;
      std::string to_parent;
      std::string after_role;
      std::optional<std::string> check;
      for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--to" && i + 1 < argc) {
          to_parent = argv[++i];
        } else if (arg == "--after" && i + 1 < argc) {
          after_role = argv[++i];
        } else if (arg == "--check" && i + 1 < argc) {
          check = argv[++i];
        } else if (new_name.empty()) {
          new_name = arg;
        } else {
          throw std::invalid_argument(
              "move-role accepts ROLE [NEW_NAME] [--to PARENT] [--after ROLE] [--check COMMAND]");
        }
      }
      std::cout << rfl::json::write(
                       workspace.MoveRole(role, new_name, to_parent, after_role, check))
                << '\n';
    } else if (command == "delete-role" && argc >= 3) {
      const std::string role = argv[2];
      std::optional<std::string> check;
      for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--check" && i + 1 < argc) {
          check = argv[++i];
        } else {
          throw std::invalid_argument("delete-role accepts only ROLE [--check COMMAND]");
        }
      }
      std::cout << rfl::json::write(workspace.DeleteRole(role, check)) << '\n';
    } else if (command == "stats") {
      bool json = false;
      bool reset = false;
      for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json") json = true;
        else if (arg == "--reset") reset = true;
        else throw std::invalid_argument("stats accepts only [--json] [--reset]");
      }
      const std::string out = workspace.FormatStats(json, reset);
      std::cout << out;
      if (!out.ends_with('\n')) std::cout << '\n';
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
    } else if (command == "watch") {
      bool once = false;
      int timeout_ms = 0;
      for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--once") {
          once = true;
        } else if (arg == "--timeout-ms" && i + 1 < argc) {
          timeout_ms = std::stoi(argv[++i]);
        } else {
          throw std::invalid_argument("watch accepts only [--once] [--timeout-ms N]");
        }
      }
      std::cout << rfl::json::write(workspace.Watch(timeout_ms, once)) << '\n';
    } else if (command == "check" && argc >= 3) {
      const auto receipt = workspace.Check(Join(argc, argv, 2));
      std::cout << rfl::json::write(receipt) << '\n';
      return receipt.passed ? 0 : 1;
    } else if (command == "serve") {
      for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) != "--mcp") {
          throw std::invalid_argument("serve accepts only [--mcp]");
        }
      }
      return tfe::code::RunMcpServer(workspace);
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
