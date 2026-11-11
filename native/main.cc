#include <CLI/CLI.hpp>
#include <rfl/json.hpp>

#include <cstdlib>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "mcp_server.h"
#include "tf/logger.h"
#include "tui.h"
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
         "add-role PARENT NAME [TARGET_CHILD_ROLE] [--before|--after] [--check COMMAND] < body | "
         "move-role ROLE [NEW_NAME] [--to PARENT] [TARGET_CHILD_ROLE] [--before|--after] "
         "[--check COMMAND] | "
         "delete-role ROLE [--check COMMAND] | "
         "stats [--json] [--reset] [--tui] | "
         "render [--write] | tui [PATH_PREFIX] | sync | watch [--once] [--timeout-ms N] | "
         "check COMMAND | serve [--mcp] | help";
}

// CLI11 would otherwise install its own generated help screens and swallow dash
// tokens that belong to the user's command line, like the `-c` of
// `tfe-code check bash -c true`. This CLI keeps one flat usage line, and
// `help|--help|-h` is a first-token pre-check in main().
void DisableHelpFlag(CLI::App& app) { app.set_help_flag(""); }

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
    // A help request is not a usage error: an agent that sees exit 2 concludes
    // the tool is broken and starts probing the workspace by hand.
    if (argc >= 2) {
      const std::string first = argv[1];
      if (first == "help" || first == "--help" || first == "-h") {
        std::cout << Usage() << '\n';
        return 0;
      }
    }
    CLI::App app("TFEngine Code workspace CLI", "tfe-code");
    DisableHelpFlag(app);
    app.require_subcommand(1);
    auto sub = [&app](const std::string& name) -> CLI::App& {
      CLI::App& command = *app.add_subcommand(name, "");
      DisableHelpFlag(command);
      return command;
    };

    tfe::code::Workspace workspace(std::filesystem::current_path());
    int exit_code = 0;

    auto& ignore_cmd = sub("ignore");
    ignore_cmd.callback([&] {
      workspace.CreateIgnoreFile();
      std::cout << "Created .tfe-codeignore\n";
    });

    std::vector<std::string> import_paths;
    auto& import_cmd = sub("import");
    import_cmd.add_option("PATHS", import_paths, "source files or directories")->required();
    import_cmd.callback([&] {
      std::cout << rfl::json::write(workspace.Import(import_paths)) << '\n';
    });

    std::string ls_target;
    auto& ls_cmd = sub("ls");
    ls_cmd.add_option("TARGET", ls_target, "file, role or package prefix");
    ls_cmd.callback([&] { std::cout << workspace.List(ls_target) << '\n'; });

    std::vector<std::string> context_terms;
    std::vector<std::string> context_files;
    bool context_callers = false;
    bool context_references = false;
    auto& context_cmd = sub("context");
    context_cmd.add_option("QUERY", context_terms, "literal terms");
    context_cmd.add_option("--file", context_files, "limit to these files");
    context_cmd.add_flag("--callers", context_callers, "append calling references");
    context_cmd.add_flag("--references", context_references, "alias of --callers");
    context_cmd.callback([&] {
      std::cout << workspace.Context(context_terms, context_files,
                                     context_callers || context_references) << '\n';
    });

    std::string outline_target;
    int outline_depth = 99;
    bool outline_relative = false;
    auto& outline_cmd = sub("outline");
    outline_cmd.add_option("TARGET", outline_target, "file or role");
    outline_cmd.add_option("--depth", outline_depth, "maximum nesting depth");
    outline_cmd.add_flag("--relative", outline_relative, "print names below the target");
    outline_cmd.callback([&] {
      std::cout << workspace.Outline(outline_target, outline_depth, outline_relative);
    });

    std::string tree_target;
    auto& tree_cmd = sub("tree");
    tree_cmd.add_option("TARGET", tree_target, "file or role")->required();
    tree_cmd.callback([&] { std::cout << workspace.Tree(tree_target) << '\n'; });

    std::vector<std::string> show_ids;
    bool show_json = false;
    auto& show_cmd = sub("show");
    show_cmd.add_option("IDS", show_ids, "role ids")->required();
    show_cmd.add_flag("--json", show_json, "emit JSON");
    show_cmd.callback([&] {
      if (show_json) std::cout << rfl::json::write(workspace.ShowRoles(show_ids)) << '\n';
      else std::cout << workspace.Show(show_ids) << '\n';
    });

    std::string role_file;
    std::string role_name;
    auto& role_cmd = sub("role");
    role_cmd.add_option("FILE", role_file, "source file")->required();
    role_cmd.add_option("NAME", role_name, "local role name")->required();
    role_cmd.callback([&] { std::cout << workspace.FindRole(role_file, role_name) << '\n'; });

    std::vector<std::string> search_terms;
    std::optional<std::string> search_prefix;
    std::optional<std::string> search_file;
    bool search_ids = false;
    bool search_regex = false;
    // `search` and `who` share every option; `who` additionally reads a dotted
    // prefix positionally, matching the historical `who [PREFIX.] NAME...`.
    auto& search_cmd = sub("search");
    search_cmd.add_option("QUERY", search_terms, "literal terms");
    search_cmd.add_option("--prefix", search_prefix, "role id prefix");
    search_cmd.add_option("--file", search_file, "limit to one file");
    search_cmd.add_flag("--ids", search_ids, "one line per role");
    search_cmd.add_flag("--regex", search_regex, "RE2 regex matching");
    search_cmd.callback([&] {
      if (search_file.has_value() && search_file->empty()) {
        throw std::invalid_argument("search --file expects a nonempty value");
      }
      std::cout << workspace.Search(search_terms, search_prefix.value_or(""),
                                    search_file.value_or(""), search_ids, search_regex) << '\n';
    });

    std::string who_prefix;
    auto& who_cmd = sub("who");
    who_cmd.add_option("PREFIX", who_prefix, "dotted role prefix ending with '.'");
    who_cmd.add_option("QUERY", search_terms, "literal terms");
    who_cmd.add_option("--prefix", search_prefix, "role id prefix");
    who_cmd.add_option("--file", search_file, "limit to one file");
    who_cmd.add_flag("--ids", search_ids, "one line per role");
    who_cmd.add_flag("--regex", search_regex, "RE2 regex matching");
    who_cmd.callback([&] {
      // An explicit --prefix wins; a positional prefix is only picked up when it
      // ends with a dot, otherwise it is just the first search term.
      if (!who_prefix.empty() && who_prefix.ends_with('.') && !search_prefix.has_value()) {
        search_prefix = who_prefix;
      } else if (!who_prefix.empty()) {
        search_terms.insert(search_terms.begin(), who_prefix);
      }
      if (search_file.has_value() && search_file->empty()) {
        throw std::invalid_argument("search --file expects a nonempty value");
      }
      std::cout << workspace.Search(search_terms, search_prefix.value_or(""),
                                    search_file.value_or(""), search_ids, search_regex) << '\n';
    });

    std::vector<std::string> batch_terms;
    std::vector<std::string> batch_files;
    auto& prepare_cmd = sub("prepare-batch");
    prepare_cmd.add_option("QUERY", batch_terms, "literal terms");
    prepare_cmd.add_option("--file", batch_files, "limit to these files");
    prepare_cmd.callback([&] {
      std::cout << workspace.PrepareBatch(batch_terms, batch_files) << '\n';
    });

    bool apply_amend = false;
    bool apply_schema_help = false;
    std::optional<std::string> apply_check;
    auto& apply_cmd = sub("apply");
    apply_cmd.add_flag("--amend", apply_amend, "edit the saved failed batch");
    apply_cmd.add_option("--check", apply_check, "verify with a command while holding the lock");
    apply_cmd.add_flag("--help", apply_schema_help, "print the JSON input schema");
    apply_cmd.allow_extras();
    apply_cmd.callback([&] {
      const auto extras = apply_cmd.remaining();
      const bool bare_help = extras.size() == 1 && extras[0] == "help" && !apply_amend &&
                             !apply_check.has_value() && !apply_schema_help;
      if (bare_help || (apply_schema_help && extras.empty())) {
        std::cout << tfe::code::ApplyInputSchemaDoc() << '\n';
        return;
      }
      if (!extras.empty()) {
        throw std::invalid_argument("apply accepts only [--amend] [--check COMMAND]");
      }
      const std::string input{std::istreambuf_iterator<char>(std::cin), {}};
      const auto edits = tfe::code::ParseApplyInput(input);
      std::cout << rfl::json::write(workspace.Apply(edits, apply_check, apply_amend)) << '\n';
    });

    std::string add_parent;
    std::string add_name;
    std::string add_target;
    std::string add_after;
    std::string add_before;
    std::optional<std::string> add_check;
    auto& add_cmd = sub("add-role");
    add_cmd.add_option("PARENT", add_parent, "parent role")->required();
    add_cmd.add_option("NAME", add_name, "new role name")->required();
    add_cmd.add_option("TARGET_CHILD_ROLE", add_target, "anchor sibling for placement");
    add_cmd.add_option("--after", add_after, "insert after this sibling");
    add_cmd.add_option("--before", add_before, "insert before this sibling");
    add_cmd.add_option("--check", add_check, "verify with a command");
    add_cmd.callback([&] {
      if (!add_after.empty() && !add_before.empty()) {
        throw std::invalid_argument("add-role accepts either --after or --before, not both");
      }
      // A positional anchor picks the sibling; the flag picks the direction.
      const std::string anchor = add_target.empty() ? add_after : add_target;
      const bool place_before = !add_before.empty();
      const std::string text{std::istreambuf_iterator<char>(std::cin), {}};
      std::cout << rfl::json::write(
                       workspace.AddRole(add_parent, add_name, text,
                                         place_before ? "" : anchor, add_check,
                                         place_before ? anchor : ""))
                << '\n';
    });

    std::string move_role;
    std::string move_new_name;
    std::string move_target;
    std::string move_to;
    std::string move_after;
    std::string move_before;
    std::optional<std::string> move_check;
    auto& move_cmd = sub("move-role");
    move_cmd.add_option("ROLE", move_role, "role to move")->required();
    move_cmd.add_option("NEW_NAME", move_new_name, "optional rename");
    move_cmd.add_option("TARGET_CHILD_ROLE", move_target, "anchor sibling for placement");
    move_cmd.add_option("--to", move_to, "new parent role");
    move_cmd.add_option("--after", move_after, "insert after this sibling");
    move_cmd.add_option("--before", move_before, "insert before this sibling");
    move_cmd.add_option("--check", move_check, "verify with a command");
    move_cmd.callback([&] {
      if (!move_after.empty() && !move_before.empty()) {
        throw std::invalid_argument("move-role accepts either --after or --before, not both");
      }
      const std::string anchor = move_target.empty() ? move_after : move_target;
      const bool place_before = !move_before.empty();
      std::cout << rfl::json::write(
                       workspace.MoveRole(move_role, move_new_name, move_to,
                                          place_before ? "" : anchor, move_check,
                                          place_before ? anchor : ""))
                << '\n';
    });

    std::string delete_role;
    std::optional<std::string> delete_check;
    auto& delete_cmd = sub("delete-role");
    delete_cmd.add_option("ROLE", delete_role, "role to delete")->required();
    delete_cmd.add_option("--check", delete_check, "verify with a command");
    delete_cmd.callback([&] {
      std::cout << rfl::json::write(workspace.DeleteRole(delete_role, delete_check)) << '\n';
    });

    bool stats_json = false;
    bool stats_reset = false;
    bool stats_tui = false;
    auto& stats_cmd = sub("stats");
    stats_cmd.add_flag("--json", stats_json, "emit JSON");
    stats_cmd.add_flag("--reset", stats_reset, "reset the counters");
    stats_cmd.add_flag("--tui", stats_tui, "interactive token-savings chart");
    stats_cmd.callback([&] {
      if (stats_tui && (stats_json || stats_reset)) {
        throw std::invalid_argument("stats accepts only one of [--json] [--reset] [--tui]");
      }
      if (stats_tui) {
        exit_code = tfe::code::RunStatsTui(workspace);
        return;
      }
      const std::string out = workspace.FormatStats(stats_json, stats_reset);
      std::cout << out;
      if (!out.ends_with('\n')) std::cout << '\n';
    });

    bool render_write = false;
    auto& render_cmd = sub("render");
    render_cmd.add_flag("--write", render_write, "materialise the rendered files");
    render_cmd.callback([&] {
      if (render_write) {
        // Confirm what was materialised without echoing it. Printing the file
        // contents here repeats the whole tree into the caller's context, which
        // for a real project runs to thousands of tokens for nothing.
        std::vector<std::string> written;
        for (const auto& [path, bytes] : workspace.Materialize()) written.push_back(path);
        std::cout << rfl::json::write(written) << '\n';
      } else {
        std::cout << rfl::json::write(workspace.Render()) << '\n';
      }
    });

    std::string tui_prefix;
    auto& tui_cmd = sub("tui");
    tui_cmd.add_option("PATH_PREFIX", tui_prefix, "restrict the dashboard to a path prefix");
    tui_cmd.callback([&] { exit_code = tfe::code::RunTui(workspace, tui_prefix); });

    auto& sync_cmd = sub("sync");
    sync_cmd.callback([&] { std::cout << rfl::json::write(workspace.Sync()) << '\n'; });

    bool watch_once = false;
    int watch_timeout_ms = 0;
    auto& watch_cmd = sub("watch");
    watch_cmd.add_flag("--once", watch_once, "exit after one sync pass");
    watch_cmd.add_option("--timeout-ms", watch_timeout_ms, "poll timeout in milliseconds");
    watch_cmd.callback([&] {
      std::cout << rfl::json::write(workspace.Watch(watch_timeout_ms, watch_once)) << '\n';
    });

    auto& check_cmd = sub("check");
    // The whole tail after `check` is the checked command line: dash tokens
    // belong to it, not to this CLI's parsing.
    check_cmd.allow_extras();
    check_cmd.callback([&] {
      const auto args = check_cmd.remaining();
      if (args.empty()) throw std::invalid_argument("check expects a command");
      std::string command;
      for (const auto& arg : args) {
        if (!command.empty()) command += ' ';
        command += arg;
      }
      const auto receipt = workspace.Check(command);
      std::cout << rfl::json::write(receipt) << '\n';
      exit_code = receipt.passed ? 0 : 1;
    });

    bool serve_mcp = false;
    auto& serve_cmd = sub("serve");
    serve_cmd.add_flag("--mcp", serve_mcp, "explicit marker for the MCP stdio server");
    serve_cmd.callback([&] { exit_code = tfe::code::RunMcpServer(workspace); });

    app.parse(argc, argv);
    return exit_code;
  } catch (const CLI::ParseError& error) {
    std::cerr << error.what() << '\n' << Usage() << '\n';
    return 2;
  } catch (const std::invalid_argument& error) {
    std::cerr << error.what() << '\n' << Usage() << '\n';
    return 2;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
