// Core verification of the CLI help registry: every command has one page with
// usage, arguments, options, examples and exit codes; the index lists every
// command exactly once under a known group; unknown commands are rejected; and
// the rendered pages stay within the wrapping width without ANSI escapes when
// stdout is not a terminal (ctest runs with a pipe).

#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "cli_help.h"

namespace {

void Expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

size_t LongestLine(const std::string& text) {
  size_t longest = 0;
  size_t current = 0;
  for (const char c : text) {
    current = c == '\n' ? 0 : current + 1;
    if (current > longest) longest = current;
  }
  return longest;
}

void ExpectContains(const std::string& haystack, const std::string& needle,
                    const std::string& where) {
  if (haystack.find(needle) == std::string::npos) {
    throw std::runtime_error("missing '" + needle + "' in " + where + ":\n" + haystack);
  }
}

// Collapses runs of whitespace so a wrapped paragraph can be matched against
// its unwrapped source text.
std::string Normalize(const std::string& text) {
  std::string out;
  bool space = false;
  for (const char c : text) {
    if (c == '\n' || c == '\t' || c == ' ') {
      space = true;
      continue;
    }
    if (space && !out.empty()) out += ' ';
    space = false;
    out += c;
  }
  return out;
}

}  // namespace

int main() {
  try {
    const auto& docs = tfe::code::CommandDocs();
    Expect(!docs.empty(), "command registry is empty");

    // 1. The index is grouped, lists every command, and documents itself.
    const std::string root = tfe::code::RootHelp();
    ExpectContains(root, "Usage: tfe-code COMMAND [ARGS] [OPTIONS]", "root help");
    ExpectContains(root, "tfe-code help COMMAND", "root help");
    ExpectContains(root, "Options", "root help");
    ExpectContains(root, "Examples", "root help");
    Expect(LongestLine(root) <= 88, "root help exceeds the wrapping width");
    Expect(root.find('\x1b') == std::string::npos, "root help leaked ANSI escapes into a pipe");

    const std::set<std::string> groups = {"Workspace", "Read", "Edit", "Run", "Help"};
    std::set<std::string> names;
    for (const auto& doc : docs) {
      Expect(!doc.name.empty(), "command with an empty name");
      Expect(names.insert(doc.name).second, "duplicate command name: " + doc.name);
      Expect(groups.contains(doc.group), "unknown group for " + doc.name + ": " + doc.group);
      ExpectContains(root, doc.name, "root help");
      ExpectContains(root, doc.group, "root help");
      Expect(tfe::code::IsKnownCommand(doc.name), "IsKnownCommand missed " + doc.name);
    }

    // 2. Every command has a complete page.
    for (const auto& doc : docs) {
      const std::string page = tfe::code::CommandHelp(doc.name);
      Expect(!page.empty(), "no page for " + doc.name);
      ExpectContains(Normalize(page), Normalize("Usage: tfe-code " + doc.usage),
                     doc.name + " page");
      ExpectContains(Normalize(page), Normalize(doc.description), doc.name + " page");
      ExpectContains(page, "Exit codes", doc.name + " page");
      Expect(!doc.summary.empty(), doc.name + " has no index summary");
      Expect(!doc.examples.empty(), doc.name + " has no examples");
      for (const auto& example : doc.examples) {
        Expect(example.starts_with("tfe-code "), doc.name + " example without a command: " + example);
        ExpectContains(page, example, doc.name + " page");
      }
      for (const auto& argument : doc.arguments) {
        ExpectContains(page, argument.metavar, doc.name + " page");
      }
      for (const auto& option : doc.options) {
        ExpectContains(page, option.flags, doc.name + " page");
      }
      Expect(LongestLine(page) <= 88, doc.name + " page exceeds the wrapping width");
      Expect(page.find('\x1b') == std::string::npos,
             doc.name + " page leaked ANSI escapes into a pipe");
    }

    // 3. Unknown commands are rejected, and lookups are exact.
    Expect(tfe::code::CommandHelp("frobnicate").empty(), "unknown command produced a page");
    Expect(tfe::code::CommandHelp("").empty(), "empty command produced a page");
    Expect(!tfe::code::IsKnownCommand("Help"), "command lookup must be case-sensitive");
    Expect(!tfe::code::IsKnownCommand("frobnicate"), "unknown command reported as known");

    // 4. The usage trailer keeps the `Usage:` line the error contracts rely on.
    const std::string hint = tfe::code::UsageHint();
    ExpectContains(hint, "Usage:", "usage hint");
    ExpectContains(hint, "tfe-code help", "usage hint");

    // 5. Behavior that must stay special-cased in main.cc.
    const std::string check = tfe::code::CommandHelp("check");
    ExpectContains(Normalize(check), Normalize("tfe-code help check"), "check page");
    Expect(check.find("-h, --help") == std::string::npos,
           "check page advertises a --help flag that belongs to the checked command");
    const std::string apply = tfe::code::CommandHelp("apply");
    ExpectContains(Normalize(apply), Normalize("JSON input schema"), "apply page");
    ExpectContains(apply, "--amend", "apply page");
    ExpectContains(apply, "--check COMMAND", "apply page");

    std::cout << "cli_help_core tests passed (" << docs.size() << " commands)\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Test failed: " << e.what() << "\n";
    return 1;
  }
}
