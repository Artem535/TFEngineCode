#pragma once

#include <string>
#include <vector>

namespace tfe::code {

// One positional argument or option of a command, as printed by the help
// renderer. The registry below is presentation only: CLI11 remains the parser,
// so a page must describe exactly what main.cc accepts.
struct HelpArgument {
  std::string metavar;      // "PATHS...", "[TARGET]"
  std::string description;
  bool required = false;
};

struct HelpOption {
  std::string flags;        // "--depth N", "--file FILE", "-h, --help"
  std::string description;  // may end with " [default: N]"
};

struct CommandDoc {
  std::string name;
  std::string group;        // "Workspace", "Read", "Edit", "Run"
  std::string summary;      // single index line
  std::string usage;        // everything after "tfe-code "
  std::string description;  // wrapped paragraph
  std::vector<HelpArgument> arguments;
  std::vector<HelpOption> options;
  std::vector<std::string> notes;     // extra paragraphs, e.g. the apply schema
  std::vector<std::string> examples;  // printed after "$ "
};

// Every command the CLI accepts, in help order. `help` itself is included.
const std::vector<CommandDoc>& CommandDocs();

bool IsKnownCommand(const std::string& name);

// Grouped command index for `tfe-code help`, `--help` and `-h`.
std::string RootHelp();

// One command page for `tfe-code help COMMAND`; empty when unknown.
std::string CommandHelp(const std::string& name);

// Two-line usage error trailer: a `Usage:` line (tests and humans rely on it)
// plus the pointer to the help command.
std::string UsageHint();

}  // namespace tfe::code
