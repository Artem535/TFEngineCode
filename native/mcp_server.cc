#include "mcp_server.h"

#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <ctre.hpp>
#include <rfl/Rename.hpp>
#include <rfl/json.hpp>
#include <spdlog/spdlog.h>

#include "phoenix_mcp/protocol/message_types.h"
#include "phoenix_mcp/server/mcp_request_handler.h"
#include "phoenix_mcp/server/mcp_server.h"
#include "phoenix_mcp/tool_registry/tool_registry.h"
#include "phoenix_mcp/tool_registry/utils.h"
#include "phoenix_mcp/transport/stdio_transport.h"

namespace tfe::code {

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

std::vector<RoleEdit> ParsePatchInput(const std::string& input) {
  std::vector<RoleEdit> edits;
  std::optional<RoleEdit> current;
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
            ctre::match<R"(^#@\s+([\w.\-]+)\s+\[(?:v:\s*|version:\s*)?([0-9a-zA-Z]+)\](?:\s+\[file:\s*[^\]]+\])?)">;
        if (auto [m, r, v] = kHeader(line); m) {
          RoleEdit next;
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

struct McpImportParams {
  std::vector<std::string> paths;
};

struct McpContextParams {
  std::optional<std::vector<std::string>> queries;
  std::optional<std::vector<std::string>> files;
  std::optional<bool> callers;
  std::optional<bool> references;
};

struct McpOutlineParams {
  std::optional<std::string> target;
  std::optional<int> depth;
  std::optional<bool> relative;
};

struct McpSearchParams {
  std::vector<std::string> queries;
  std::optional<std::string> prefix;
  std::optional<std::string> file;
  std::optional<bool> ids_only;
  std::optional<bool> regex;
};

struct McpShowParams {
  std::vector<std::string> roles;
  std::optional<bool> json;
};

struct McpApplyParams {
  std::string patch;
  std::optional<std::string> check;
  std::optional<bool> amend;
};

struct McpSyncParams {};

struct McpAddRoleParams {
  std::string parent;
  std::string name;
  std::string text;
  std::optional<std::string> after_role;
  std::optional<std::string> check;
};

struct McpMoveRoleParams {
  std::string role;
  std::optional<std::string> new_name;
  std::optional<std::string> to_parent;
  std::optional<std::string> after_role;
  std::optional<std::string> check;
};

struct McpDeleteRoleParams {
  std::string role;
  std::optional<std::string> check;
};

struct McpStatsParams {
  std::optional<bool> json;
  std::optional<bool> reset;
};

}  // namespace

std::string ApplyInputSchemaDoc() {
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

std::vector<RoleEdit> ParseApplyInput(const std::string& input) {
  const std::string trimmed = Trim(input);
  if (trimmed.starts_with("#@")) {
    return ParsePatchInput(input);
  }
  const auto parsed = rfl::json::read<InputBatch, rfl::DefaultIfMissing>(input);
  if (!parsed) {
    throw std::runtime_error(std::string("Invalid apply JSON: ") + parsed.error().what() + "\n\n" +
                             ApplyInputSchemaDoc());
  }
  std::vector<RoleEdit> edits;
  for (const auto& edit : parsed.value().edits) {
    if (edit.role.empty()) throw std::runtime_error("apply edit is missing 'role'");
    if (!edit.drop && edit.expected_version.empty()) {
      throw std::runtime_error("apply edit is missing 'expected_version' for role " + edit.role);
    }
    RoleEdit role_edit{edit.role, edit.expected_version, edit.old, edit.new_text.get(), {}, edit.drop};
    for (const auto& replacement : edit.replace) {
      role_edit.replace.emplace_back(replacement.old, replacement.new_text.get());
    }
    edits.push_back(std::move(role_edit));
  }
  return edits;
}

int RunMcpServer(Workspace& workspace, int input_fd, int output_fd) {
  const char* log = std::getenv("TFE_CODE_LOG");
  const std::string level = log == nullptr ? "off" : log;
  spdlog::set_level(level == "debug"   ? spdlog::level::debug
                    : level == "info"  ? spdlog::level::info
                    : level == "error" ? spdlog::level::err
                                       : spdlog::level::off);

  auto registry = std::make_unique<phoenix_mcp::tool::ToolRegistry>();

  registry->register_tool<McpImportParams>(
      "tfe_import",
      "Import Python or C/C++ files/directories into the TFEngineCode role tree",
      [&workspace](const McpImportParams& params) {
        try {
          const auto res = workspace.Import(params.paths);
          return phoenix_mcp::utils::make_text_result(rfl::json::write(res), false);
        } catch (const std::exception& e) {
          return phoenix_mcp::utils::make_text_result(e.what(), true);
        }
      });

  registry->register_tool<McpContextParams>(
      "tfe_context",
      "Read selective role context with short aliases and 12-hex versions",
      [&workspace](const McpContextParams& params) {
        try {
          const auto queries = params.queries.value_or(std::vector<std::string>{});
          const auto files = params.files.value_or(std::vector<std::string>{});
          const bool callers = params.callers.value_or(false) || params.references.value_or(false);
          return phoenix_mcp::utils::make_text_result(
              workspace.Context(queries, files, callers), false);
        } catch (const std::exception& e) {
          return phoenix_mcp::utils::make_text_result(e.what(), true);
        }
      });

  registry->register_tool<McpOutlineParams>(
      "tfe_outline",
      "Inspect role hierarchy and 1-based source line ranges",
      [&workspace](const McpOutlineParams& params) {
        try {
          const std::string target = params.target.value_or("");
          const int depth = params.depth.value_or(99);
          const bool relative = params.relative.value_or(false);
          return phoenix_mcp::utils::make_text_result(
              workspace.Outline(target, depth, relative), false);
        } catch (const std::exception& e) {
          return phoenix_mcp::utils::make_text_result(e.what(), true);
        }
      });

  registry->register_tool<McpSearchParams>(
      "tfe_search",
      "Search roles by literal or regex queries across role IDs and bodies",
      [&workspace](const McpSearchParams& params) {
        try {
          const std::string prefix = params.prefix.value_or("");
          const std::string file = params.file.value_or("");
          const bool ids_only = params.ids_only.value_or(false);
          const bool regex = params.regex.value_or(false);
          return phoenix_mcp::utils::make_text_result(
              workspace.Search(params.queries, prefix, file, ids_only, regex), false);
        } catch (const std::exception& e) {
          return phoenix_mcp::utils::make_text_result(e.what(), true);
        }
      });

  registry->register_tool<McpShowParams>(
      "tfe_show",
      "Show exact dedented role bodies and current SHA-256 versions",
      [&workspace](const McpShowParams& params) {
        try {
          if (params.json.value_or(false)) {
            return phoenix_mcp::utils::make_text_result(
                rfl::json::write(workspace.ShowRoles(params.roles)), false);
          }
          return phoenix_mcp::utils::make_text_result(workspace.Show(params.roles), false);
        } catch (const std::exception& e) {
          return phoenix_mcp::utils::make_text_result(e.what(), true);
        }
      });

  registry->register_tool<McpApplyParams>(
      "tfe_apply",
      "Atomically apply a compact #@ patch or JSON edit batch with optional check command",
      [&workspace](const McpApplyParams& params) {
        try {
          const auto edits = ParseApplyInput(params.patch);
          const auto res =
              workspace.Apply(edits, params.check, params.amend.value_or(false));
          return phoenix_mcp::utils::make_text_result(rfl::json::write(res), !res.passed);
        } catch (const std::exception& e) {
          return phoenix_mcp::utils::make_text_result(e.what(), true);
        }
      });

  registry->register_tool<McpSyncParams>(
      "tfe_sync",
      "Synchronize external edits, deletions, and exact renames from disk",
      [&workspace](const McpSyncParams&) {
        try {
          const auto res = workspace.Sync();
          return phoenix_mcp::utils::make_text_result(rfl::json::write(res), false);
        } catch (const std::exception& e) {
          return phoenix_mcp::utils::make_text_result(e.what(), true);
        }
      });

  registry->register_tool<McpAddRoleParams>(
      "tfe_add_role",
      "Add a new child role to a parent file or container role",
      [&workspace](const McpAddRoleParams& params) {
        try {
          const auto res = workspace.AddRole(
              params.parent, params.name, params.text,
              params.after_role.value_or(""), params.check);
          return phoenix_mcp::utils::make_text_result(rfl::json::write(res), !res.passed);
        } catch (const std::exception& e) {
          return phoenix_mcp::utils::make_text_result(e.what(), true);
        }
      });

  registry->register_tool<McpMoveRoleParams>(
      "tfe_move_role",
      "Rename or move a role (and its subtree) into a new name or parent container",
      [&workspace](const McpMoveRoleParams& params) {
        try {
          const auto res = workspace.MoveRole(
              params.role, params.new_name.value_or(""),
              params.to_parent.value_or(""), params.after_role.value_or(""),
              params.check);
          return phoenix_mcp::utils::make_text_result(rfl::json::write(res), !res.passed);
        } catch (const std::exception& e) {
          return phoenix_mcp::utils::make_text_result(e.what(), true);
        }
      });

  registry->register_tool<McpDeleteRoleParams>(
      "tfe_delete_role",
      "Delete a role (and its subtree) and remove its placeholder from the parent container",
      [&workspace](const McpDeleteRoleParams& params) {
        try {
          const auto res = workspace.DeleteRole(params.role, params.check);
          return phoenix_mcp::utils::make_text_result(rfl::json::write(res), !res.passed);
        } catch (const std::exception& e) {
          return phoenix_mcp::utils::make_text_result(e.what(), true);
        }
      });

  registry->register_tool<McpStatsParams>(
      "tfe_stats",
      "Report cumulative token savings telemetry across workspace operations",
      [&workspace](const McpStatsParams& params) {
        try {
          const std::string out = workspace.FormatStats(
              params.json.value_or(true), params.reset.value_or(false));
          return phoenix_mcp::utils::make_text_result(out, false);
        } catch (const std::exception& e) {
          return phoenix_mcp::utils::make_text_result(e.what(), true);
        }
      });

  phoenix_mcp::msg::types::ServerCapabilities caps{
      .tools = phoenix_mcp::msg::types::ToolsCapabilities{.list_changed = false}};
  phoenix_mcp::msg::types::Implementation info{
      .name = "tfe-code",
      .version = TFE_CODE_VERSION,
  };

  auto handler = std::make_unique<phoenix_mcp::server::McpRequestHandler>(
      caps, info,
      "TFEngineCode role-tree editor MCP server (tfe_import, tfe_context, tfe_outline, "
      "tfe_search, tfe_show, tfe_apply, tfe_sync)",
      std::move(registry));
  auto transport =
      std::make_unique<phoenix_mcp::server::StdioTransport>(input_fd, output_fd);

  phoenix_mcp::server::McpServer server{std::move(transport), std::move(handler)};
  return server.run();
}

}  // namespace tfe::code
