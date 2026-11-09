#pragma once

#include <string>

#include "workspace.h"

namespace tfe::code {

// One pane of text for the `tui` dashboard: local role names indented by depth,
// each with a 12-hex version prefix, line count and 1-based source range.
// Exposed so tests can check dashboard content without a terminal.
std::string RoleRowsText(const TuiFileRow& file);

// Runs the read-only FTXUI dashboard and blocks until the user quits with q or
// Esc. Returns 2 with a diagnostic when stdin or stdout is not a terminal.
int RunTui(Workspace& workspace, const std::string& path_prefix = "");

}  // namespace tfe::code
