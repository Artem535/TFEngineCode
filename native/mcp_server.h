#pragma once

#include <string>
#include <vector>

#include "workspace.h"

namespace tfe::code {

// Parses the compact `#@ ROLE [VERSION]` patch format or JSON edit batch used
// by both `tfe-code apply` and the `tfe_apply` MCP tool.
std::vector<RoleEdit> ParseApplyInput(const std::string& input);

// Returns the human-readable schema documentation for `apply` / `tfe_apply`.
std::string ApplyInputSchemaDoc();

// Runs the PhoenixMcp stdio MCP server (`tfe-code serve [--mcp]`) against `workspace`.
// `input_fd` and `output_fd` default to STDIN_FILENO (0) and STDOUT_FILENO (1).
int RunMcpServer(Workspace& workspace, int input_fd = 0, int output_fd = 1);

}  // namespace tfe::code
