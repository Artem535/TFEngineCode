#include "cli_help.h"

#include <unistd.h>

#include <cstdlib>
#include <sstream>
#include <string_view>

namespace tfe::code {
namespace {

constexpr size_t kWidth = 88;
constexpr size_t kIndexColumn = 18;
constexpr size_t kFieldColumn = 24;

bool UseColor() {
  static const bool use = [] {
    if (std::getenv("NO_COLOR") != nullptr) return false;
    const char* term = std::getenv("TERM");
    if (term != nullptr && std::string_view(term) == "dumb") return false;
    return isatty(STDOUT_FILENO) != 0;
  }();
  return use;
}

std::string Styled(const std::string& text, const char* code) {
  if (!UseColor() || text.empty()) return text;
  return std::string(code) + text + "\x1b[0m";
}

std::string Bold(const std::string& text) { return Styled(text, "\x1b[1m"); }
std::string Cyan(const std::string& text) { return Styled(text, "\x1b[36m"); }
std::string Dim(const std::string& text) { return Styled(text, "\x1b[2m"); }

std::string Spaces(size_t count) { return std::string(count, ' '); }

std::string Pad(const std::string& text, size_t width) {
  return text.size() >= width ? text : text + Spaces(width - text.size());
}

// Wraps `text` to kWidth, prefixing every line with `indent` spaces. Existing
// newlines start a new paragraph line without adding a blank line.
std::string Wrap(const std::string& text, size_t indent) {
  const std::string prefix = Spaces(indent);
  std::string out;
  std::string line;
  auto flush = [&] {
    if (line.empty()) {
      out += '\n';
      return;
    }
    if (!out.empty() && out.back() != '\n') out += '\n';
    out += prefix + line + '\n';
    line.clear();
  };
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && text[i] == ' ') ++i;
    size_t j = i;
    while (j < text.size() && text[j] != ' ' && text[j] != '\n') ++j;
    if (j > i) {
      const std::string word = text.substr(i, j - i);
      if (!line.empty() && prefix.size() + line.size() + 1 + word.size() > kWidth) {
        if (!out.empty() && out.back() != '\n') out += '\n';
        out += prefix + line + '\n';
        line.clear();
      }
      if (!line.empty()) line += ' ';
      line += word;
    }
    if (j < text.size() && text[j] == '\n') {
      flush();
      ++j;
    }
    i = j;
  }
  if (!line.empty()) {
    if (!out.empty() && out.back() != '\n') out += '\n';
    out += prefix + line + '\n';
  }
  return out;
}

std::string Field(const std::string& left, const std::string& right, size_t column) {
  const std::string head = "  " + left;
  if (head.size() + 2 >= column) return head + "\n" + Wrap(right, column);
  std::string out;
  std::string line = head + Spaces(column - head.size());
  size_t line_length = column;
  bool first = true;
  size_t i = 0;
  while (i < right.size()) {
    while (i < right.size() && right[i] == ' ') ++i;
    size_t j = i;
    while (j < right.size() && right[j] != ' ') ++j;
    if (j == i) break;
    const std::string word = right.substr(i, j - i);
    i = j;
    if (!first && line_length + 1 + word.size() > kWidth) {
      out += line + '\n';
      line = Spaces(column);
      line_length = column;
    }
    if (!first) {
      line += ' ';
      ++line_length;
    }
    line += word;
    line_length += word.size();
    first = false;
  }
  return out + line;
}

// "Usage:" plus a synopsis that hangs at seven spaces when it wraps.
std::string UsageLine(const std::string& synopsis) {
  constexpr size_t kIndent = 7;  // length of "Usage: "
  std::string out = Bold("Usage:") + " ";
  size_t line = kIndent;
  bool first = true;
  size_t i = 0;
  while (i < synopsis.size()) {
    while (i < synopsis.size() && synopsis[i] == ' ') ++i;
    size_t j = i;
    while (j < synopsis.size() && synopsis[j] != ' ') ++j;
    if (j == i) break;
    const std::string word = synopsis.substr(i, j - i);
    if (!first && line + 1 + word.size() > kWidth) {
      out += "\n" + Spaces(kIndent) + word;
      line = kIndent + word.size();
    } else {
      if (!first) {
        out += ' ';
        ++line;
      }
      out += word;
      line += word.size();
    }
    first = false;
    i = j;
  }
  return out + "\n\n";
}

CommandDoc Doc(std::string name, std::string group, std::string summary, std::string usage,
               std::string description) {
  CommandDoc doc;
  doc.name = std::move(name);
  doc.group = std::move(group);
  doc.summary = std::move(summary);
  doc.usage = std::move(usage);
  doc.description = std::move(description);
  return doc;
}

HelpOption HelpFlag() { return {"-h, --help", "Show this help and exit."}; }

std::vector<CommandDoc> BuildDocs() {
  std::vector<CommandDoc> docs;

  auto ignore = Doc("ignore", "Workspace", "Write the default .tfe-codeignore rules.",
                    "ignore",
                    "Write the default .tfe-codeignore rules. An existing file is left "
                    "untouched and the command fails with exit 1.");
  ignore.examples = {"tfe-code ignore"};
  docs.push_back(ignore);

  auto import = Doc("import", "Workspace", "Import source files or directories as roles.",
                    "import PATHS...",
                    "Import source files or directories in place as a role tree. Directories "
                    "expand recursively in sorted order without following directory symlinks; "
                    "`.tfe-code` is never imported and unsupported files are skipped. Prints a "
                    "JSON receipt with `changed`, `degraded`, `created` and `moved`.");
  import.arguments = {{"PATHS...", "Source files or directories.", true}};
  import.examples = {"tfe-code import src", "tfe-code import native/workspace.cc tests"};
  docs.push_back(import);

  auto sync = Doc("sync", "Workspace", "Reconcile external source edits with the role tree.",
                  "sync",
                  "Reconcile source files edited outside the CLI with the role tree. Prints a "
                  "JSON receipt; ambiguous renames are rejected rather than guessed.");
  sync.examples = {"tfe-code sync"};
  docs.push_back(sync);

  auto watch = Doc("watch", "Workspace", "Sync source changes while they happen.",
                   "watch [--once] [--timeout-ms N]",
                   "Watch managed sources and sync them into the role tree as they change. "
                   "Uses efsw (inotify on Linux, FSEvents on macOS).");
  watch.options = {{"--once", "Exit after one sync pass."},
                   {"--timeout-ms N", "Poll timeout in milliseconds. [default: 0]"},
                   HelpFlag()};
  watch.examples = {"tfe-code watch", "tfe-code watch --once --timeout-ms 500"};
  docs.push_back(watch);

  auto ls = Doc("ls", "Read", "List files and roles under a target.",
                "ls [TARGET]",
                "List the files and roles under a file, role or package prefix. With no target, "
                "lists the whole workspace.");
  ls.arguments = {{"[TARGET]", "File, role id or package prefix; omit for the workspace."}};
  ls.options = {HelpFlag()};
  ls.examples = {"tfe-code ls", "tfe-code ls native/workspace.cc", "tfe-code ls src.geometry"};
  docs.push_back(ls);

  auto context = Doc("context", "Read", "Select roles with their headers and callers.",
                     "context [QUERY...] [--file FILE]... [--callers]",
                     "Select roles matching literal terms and print them with their enclosing "
                     "headers and versions. `--callers` appends one-line references from across "
                     "the project, so an edit can see its call sites.");
  context.arguments = {{"[QUERY...]", "Literal terms to match; repeatable."}};
  context.options = {{"--file FILE", "Limit to roles reachable from this file. Repeatable."},
                     {"--callers", "Append one-line caller references."},
                     {"--references", "Alias of --callers."},
                     HelpFlag()};
  context.examples = {"tfe-code context apply --file native/workspace.cc",
                      "tfe-code context \"role id\" --callers"};
  docs.push_back(context);

  auto outline = Doc("outline", "Read", "Print a compact role outline.",
                     "outline [TARGET] [--depth N] [--relative]",
                     "Print the role outline of a file or role: ids, line counts, source line "
                     "ranges and the first line of each role. Pass `--relative` for names below "
                     "the target instead of full role ids.");
  outline.arguments = {{"[TARGET]", "File or role id; omit for the whole workspace."}};
  outline.options = {{"--depth N", "Maximum nesting depth. [default: 99]"},
                     {"--relative", "Print names below the target."},
                     HelpFlag()};
  outline.examples = {"tfe-code outline native/workspace.cc",
                      "tfe-code outline src.geometry --relative --depth 2"};
  docs.push_back(outline);

  auto tree = Doc("tree", "Read", "Print the full role tree of a file or role.",
                  "tree TARGET", "Print the full role tree, including empty parents and their "
                  "nesting depth.");
  tree.arguments = {{"TARGET", "File or role id.", true}};
  tree.options = {HelpFlag()};
  tree.examples = {"tfe-code tree src.geometry", "tfe-code tree native/workspace.cc"};
  docs.push_back(tree);

  auto show = Doc("show", "Read", "Print exact role text and versions.",
                  "show IDS... [--json]",
                  "Print the exact text and version of one or more roles. The first line is a "
                  "`#@ role [version:HASH]` header that `apply` accepts directly, so the output "
                  "can be edited in place and piped back.");
  show.arguments = {{"IDS...", "Role ids; repeatable.", true}};
  show.options = {{"--json", "Emit structured JSON instead of text."}, HelpFlag()};
  show.examples = {"tfe-code show src.geometry.clamp",
                   "tfe-code show src.geometry.clamp --json"};
  docs.push_back(show);

  auto role = Doc("role", "Read", "Look up one role in a file.",
                  "role FILE NAME",
                  "Look up one role in a file by exact role id or unique dotted suffix.");
  role.arguments = {{"FILE", "Source file that owns the role.", true},
                    {"NAME", "Role id or unique dotted suffix.", true}};
  role.options = {HelpFlag()};
  role.examples = {"tfe-code role native/workspace.cc ImportSource",
                   "tfe-code role src/geometry.ts Transform.apply"};
  docs.push_back(role);

  auto search = Doc("search", "Read", "Search role ids and text.",
                    "search QUERY... [--prefix PREFIX] [--file FILE] [--ids] [--regex]",
                    "Search role ids and role text for literal terms. Matching is case-sensitive; "
                    "`--regex` switches the query to RE2 and `--ids` prints one role id per line.");
  search.arguments = {{"QUERY...", "Literal terms to match; repeatable."}};
  search.options = {{"--prefix PREFIX", "Restrict to role ids that start with this prefix."},
                    {"--file FILE", "Limit to roles reachable from this file."},
                    {"--ids", "Print one role id per line."},
                    {"--regex", "Treat the query as a RE2 regular expression."},
                    HelpFlag()};
  search.examples = {"tfe-code search clamp", "tfe-code search std::move --file native/workspace.cc",
                     "tfe-code search 'Apply[A-Za-z]+' --regex --ids"};
  docs.push_back(search);

  auto who = Doc("who", "Read", "Search with a positional dotted prefix.",
                 "who [PREFIX] [QUERY...] [--ids]",
                 "Search like `search`, but accept a dotted role prefix positionally "
                 "(`who src.pkg. name`). A positional prefix is only recognised when it ends "
                 "with a dot; an explicit `--prefix` wins.");
  who.arguments = {{"[PREFIX]", "Dotted role prefix ending with '.'."},
                   {"[QUERY...]", "Literal terms to match; repeatable."}};
  who.options = {{"--prefix PREFIX", "Role id prefix; wins over the positional prefix."},
                 {"--file FILE", "Limit to roles reachable from this file."},
                 {"--ids", "Print one role id per line."},
                 {"--regex", "Treat the query as a RE2 regular expression."},
                 HelpFlag()};
  who.examples = {"tfe-code who src.geometry. clamp", "tfe-code who clamp --ids"};
  docs.push_back(who);

  auto prepare = Doc("prepare-batch", "Read", "Prepare fragments for an apply batch.",
                     "prepare-batch [QUERY...] [--file FILE]...",
                     "Print a compact `#@ role [v:HASH]` patch skeleton for the roles matching the "
                     "query, so an agent fills in fragments instead of re-emitting whole role "
                     "bodies.");
  prepare.arguments = {{"[QUERY...]", "Literal terms; omit to prepare every role in --file."}};
  prepare.options = {{"--file FILE", "Limit to these files. Repeatable."}, HelpFlag()};
  prepare.examples = {"tfe-code prepare-batch apply --file native/workspace.cc",
                      "tfe-code prepare-batch needle --file src/app.py"};
  docs.push_back(prepare);

  auto apply = Doc("apply", "Edit", "Apply a compact patch or JSON edit batch.",
                   "apply [--amend] [--check COMMAND]",
                   "Read a compact `#@ role [v:HASH]` patch or a JSON edit batch from stdin and "
                   "apply it atomically. Every edit must match exactly once in the role and carry "
                   "the current version; a failed batch is saved so `--amend` can retry it.");
  apply.options = {{"--amend", "Edit the saved failed batch instead of a fresh one."},
                   {"--check COMMAND",
                    "Run this command while the workspace lock is held; a failure rolls the "
                    "batch back."},
                   {"--help", "Print the JSON input schema instead of this page."}};
  apply.notes = {"Send fragments, not whole role bodies: one role may carry several "
                 "`<<< old === new >>>` blocks, and byte-identical duplicate entries collapse "
                 "silently. `tfe-code apply help` prints the same JSON schema as `--help`."};
  apply.examples = {"tfe-code apply < patch.txt",
                    "tfe-code apply --check \"ctest --test-dir build\" < batch.json",
                    "tfe-code apply --amend < fixes.patch"};
  docs.push_back(apply);

  auto add = Doc("add-role", "Edit", "Insert a new role under a parent.",
                 "add-role PARENT NAME [TARGET_CHILD_ROLE] [--after|--before SIBLING]",
                 "Insert a new role under a parent, with the body read from stdin. Placement "
                 "defaults to the end; `--after`/`--before` anchor it to a sibling, and a "
                 "positional TARGET_CHILD_ROLE picks the anchor while the flag picks the "
                 "direction.");
  add.arguments = {{"PARENT", "Parent role id or file path.", true},
                   {"NAME", "Name of the new role.", true},
                   {"[TARGET_CHILD_ROLE]", "Anchor sibling for placement."}};
  add.options = {{"--after SIBLING", "Insert after this sibling."},
                 {"--before SIBLING", "Insert before this sibling."},
                 {"--check COMMAND", "Verify with a command while holding the lock."},
                 HelpFlag()};
  add.examples = {"tfe-code add-role pkg.mod new_helper < body.txt",
                  "tfe-code add-role pkg.mod new_helper existing --after"};
  docs.push_back(add);

  auto move = Doc("move-role", "Edit", "Move or rename a role.",
                  "move-role ROLE [NEW_NAME] [TARGET_CHILD_ROLE] [--to PARENT]",
                  "Move a role to another parent and optionally rename it. Placement defaults to "
                  "the end of the new parent; `--after`/`--before` choose the position.");
  move.arguments = {{"ROLE", "Role to move.", true},
                    {"[NEW_NAME]", "Optional new local name."},
                    {"[TARGET_CHILD_ROLE]", "Anchor sibling for placement."}};
  move.options = {{"--to PARENT", "New parent role id or file path."},
                  {"--after SIBLING", "Insert after this sibling."},
                  {"--before SIBLING", "Insert before this sibling."},
                  {"--check COMMAND", "Verify with a command while holding the lock."},
                  HelpFlag()};
  move.examples = {"tfe-code move-role pkg.mod.old new_name",
                   "tfe-code move-role pkg.mod.fn --to pkg.other --before first"};
  docs.push_back(move);

  auto remove = Doc("delete-role", "Edit", "Delete a role and its subtree.",
                    "delete-role ROLE [--check COMMAND]",
                    "Delete a role and its whole subtree, then re-render the owning file.");
  remove.arguments = {{"ROLE", "Role to delete.", true}};
  remove.options = {{"--check COMMAND", "Verify with a command while holding the lock."},
                    HelpFlag()};
  remove.examples = {"tfe-code delete-role pkg.mod.gone",
                     "tfe-code delete-role pkg.mod.gone --check \"pytest -q\""};
  docs.push_back(remove);

  auto check = Doc("check", "Run", "Run a command and record the receipt.",
                   "check COMMAND...",
                   "Run a command and record the receipt in the workspace state. The receipt is "
                   "printed as JSON and a failing command exits 1.");
  check.arguments = {{"COMMAND...", "Command and arguments; passed through verbatim.", true}};
  check.notes = {"Everything after `check` is the command line. Dash options belong to that "
                 "command, so `check bash -c true` works and `--help` is not intercepted here; use "
                 "`tfe-code help check` for this page."};
  check.examples = {"tfe-code check bash -c true",
                    "tfe-code check cmake --build build --parallel"};
  docs.push_back(check);

  auto render = Doc("render", "Run", "Render the role tree.",
                    "render [--write]",
                    "Render the role tree and print the file map as JSON. With `--write`, "
                    "materialise the rendered bytes on disk and print only the written paths.");
  render.options = {{"--write", "Write the rendered files instead of printing them."}, HelpFlag()};
  render.examples = {"tfe-code render", "tfe-code render --write"};
  docs.push_back(render);

  auto stats = Doc("stats", "Run", "Token-savings statistics.",
                   "stats [--json] [--reset] [--tui]",
                   "Report token-savings statistics for the workspace, reset the counters, or open "
                   "the interactive chart. The three modes are mutually exclusive.");
  stats.options = {{"--json", "Emit JSON."},
                   {"--reset", "Reset the counters."},
                   {"--tui", "Open the interactive token-savings chart."},
                   HelpFlag()};
  stats.examples = {"tfe-code stats", "tfe-code stats --json", "tfe-code stats --tui"};
  docs.push_back(stats);

  auto tui = Doc("tui", "Run", "Interactive dashboard.",
                 "tui [PATH_PREFIX]",
                 "Open the interactive dashboard: role tree, statistics and live refresh when "
                 "managed source changes.");
  tui.arguments = {{"[PATH_PREFIX]", "Restrict the dashboard to a path prefix."}};
  tui.options = {HelpFlag()};
  tui.examples = {"tfe-code tui", "tfe-code tui native/"};
  docs.push_back(tui);

  auto serve = Doc("serve", "Run", "MCP stdio server.",
                   "serve [--mcp]",
                   "Run the MCP stdio server so MCP clients can call the `tfe_*` tools: import, "
                   "ls, context, outline, search, show, prepare-batch, apply, sync, add-role, "
                   "move-role, delete-role and stats.");
  serve.options = {{"--mcp", "Explicit marker for the MCP stdio server."}, HelpFlag()};
  serve.examples = {"tfe-code serve --mcp"};
  docs.push_back(serve);

  auto help = Doc("help", "Help", "Print the command index or one command page.",
                  "help [COMMAND]",
                  "Print the grouped command index, or the usage, arguments, options, examples and "
                  "exit codes of one command. `tfe-code COMMAND --help` prints the same page.");
  help.arguments = {{"[COMMAND]", "Command to describe; omit for the command index."}};
  help.options = {HelpFlag()};
  help.examples = {"tfe-code help", "tfe-code help apply"};
  docs.push_back(help);

  return docs;
}

struct ExitCode {
  const char* code;
  const char* text;
};

const ExitCode kExitCodes[] = {
    {"0", "Success."},
    {"1", "Domain or engine error, including a failed check command."},
    {"2", "Usage error: unknown command, missing or extra arguments, malformed patch."},
};

}  // namespace

const std::vector<CommandDoc>& CommandDocs() {
  static const std::vector<CommandDoc> docs = BuildDocs();
  return docs;
}

bool IsKnownCommand(const std::string& name) {
  for (const auto& doc : CommandDocs()) {
    if (doc.name == name) return true;
  }
  return false;
}

std::string RootHelp() {
  std::ostringstream out;
  out << UsageLine("tfe-code COMMAND [ARGS] [OPTIONS]");
  out << Wrap("TFEngine Code workspace CLI: read, patch and verify source as named roles "
              "(Python, C/C++, TypeScript/JavaScript, QML, Rust and Go).",
              2);
  out << '\n';

  std::vector<std::string> groups;
  for (const auto& doc : CommandDocs()) {
    bool seen = false;
    for (const auto& group : groups) seen = seen || group == doc.group;
    if (!seen) groups.push_back(doc.group);
  }
  for (const auto& group : groups) {
    out << Bold(group) << '\n';
    for (const auto& doc : CommandDocs()) {
      if (doc.group != group) continue;
      const std::string name = "  " + doc.name;
      if (name.size() < kIndexColumn) {
        out << Cyan(name) << Spaces(kIndexColumn - name.size())
            << Dim(doc.summary) << '\n';
      } else {
        out << Cyan(name) << '\n' << Wrap(doc.summary, kIndexColumn);
      }
    }
    out << '\n';
  }

  out << Bold("Options") << '\n';
  out << "  " << Cyan(Pad("-h, --help", kIndexColumn - 2)) << Dim("Show this help and exit.")
      << "\n\n";

  out << Bold("Examples") << '\n';
  for (const std::string& example : {"tfe-code import src",
                                     "tfe-code context apply --file native/workspace.cc",
                                     "tfe-code apply < patch.txt"}) {
    out << "  " << Dim("$ ") << example << '\n';
  }
  out << '\n';
  out << "Run " << Cyan("tfe-code help COMMAND") << " for arguments, options and examples of "
      << "one command.\n";
  return out.str();
}

std::string CommandHelp(const std::string& name) {
  const CommandDoc* doc = nullptr;
  for (const auto& candidate : CommandDocs()) {
    if (candidate.name == name) doc = &candidate;
  }
  if (doc == nullptr) return {};

  std::ostringstream out;
  out << UsageLine("tfe-code " + doc->usage);
  out << Wrap(doc->description, 2) << '\n';

  if (!doc->arguments.empty()) {
    out << Bold("Arguments") << '\n';
    for (const auto& arg : doc->arguments) {
      std::string description = arg.description;
      if (arg.required && !description.ends_with(".")) description += ".";
      if (arg.required) description += " Required.";
      out << Field(arg.metavar, Dim(description), kFieldColumn) << '\n';
    }
    out << '\n';
  }

  if (!doc->options.empty()) {
    out << Bold("Options") << '\n';
    for (const auto& option : doc->options) {
      out << Field(option.flags, Dim(option.description), kFieldColumn) << '\n';
    }
    out << '\n';
  }

  if (!doc->notes.empty()) {
    out << Bold("Notes") << '\n';
    for (const auto& note : doc->notes) out << Wrap(note, 2);
    out << '\n';
  }

  if (!doc->examples.empty()) {
    out << Bold("Examples") << '\n';
    for (const std::string& example : doc->examples) out << "  " << Dim("$ ") << example << '\n';
    out << '\n';
  }

  out << Bold("Exit codes") << '\n';
  for (const auto& entry : kExitCodes) {
    out << Field(entry.code, entry.text, 10) << '\n';
  }
  return out.str();
}

std::string UsageHint() {
  return std::string("Usage: tfe-code COMMAND [ARGS] [OPTIONS]\n") +
         "Try 'tfe-code help' for help, or 'tfe-code help COMMAND' for one command.";
}

}  // namespace tfe::code
