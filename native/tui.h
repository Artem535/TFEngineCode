#pragma once

#include <set>
#include <string>
#include <vector>

#include "workspace.h"

namespace tfe::code {

// One pane of text for the `tui` dashboard: local role names indented by depth,
// each with a 12-hex version prefix, line count and 1-based source range.
// Exposed so tests can check dashboard content without a terminal.
std::string RoleRowsText(const TuiFileRow& file);

// Tree helpers behind the v2 dashboard, all pure so tests stay headless.

// Rows not hidden by a collapsed ancestor, in outline order.
std::vector<size_t> VisibleRoleRows(const std::vector<TuiRoleRow>& rows,
                                    const std::vector<bool>& collapsed);

// Matched rows plus their full ancestor chain, in outline order.
std::vector<size_t> FilterRoleRows(const std::vector<TuiRoleRow>& rows,
                                   const std::set<std::string>& matched_ids);

// The right-hand pane for one role: header (id, version, lines, range) + body.
std::string RoleDetailText(const TuiRoleRow& role, const std::string& body);

// One row per command in `report`, with a saved-tokens bar scaled to the
// busiest command (10 cells) and the raw count.
std::vector<std::string> StatsChartRows(const TelemetryReport& report);

// Runs the read-only FTXUI dashboard and blocks until the user quits with q or
// Esc. Returns 2 with a diagnostic when stdin or stdout is not a terminal.
int RunTui(Workspace& workspace, const std::string& path_prefix = "");

// Runs the read-only FTXUI token-savings chart and blocks until the user quits
// with q or Esc. Returns 2 with a diagnostic when stdin or stdout is not a
// terminal.
int RunStatsTui(Workspace& workspace);

}  // namespace tfe::code
