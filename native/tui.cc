#include "tui.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <efsw/efsw.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

namespace tfe::code {
namespace {

using ftxui::border;
using ftxui::CatchEvent;
using ftxui::Event;
using ftxui::Input;
using ftxui::filler;
using ftxui::frame;
using ftxui::hbox;
using ftxui::LESS_THAN;
using ftxui::Menu;
using ftxui::paragraph;
using ftxui::Renderer;
using ftxui::ScreenInteractive;
using ftxui::separator;
using ftxui::size;
using ftxui::text;
using ftxui::vbox;
using ftxui::WIDTH;
using ftxui::yflex;
using ftxui::yframe;

// Local names only: repeating the module prefix on every line costs the width
// the terminal does not have, and the parent chain already is the indentation.
std::string LocalName(const std::string& scope, const std::string& id) {
  const std::string prefix = scope + ".";
  return id.starts_with(prefix) ? id.substr(prefix.size()) : id;
}

}  // namespace

std::string RoleRowsText(const TuiFileRow& file) {
  // The root `.file` role names the scope; the rows below it print local names.
  std::string scope;
  if (!file.roles.empty() && file.roles.front().id.ends_with(".file")) {
    scope = file.roles.front().id.substr(0, file.roles.front().id.size() - 5);
  }
  std::string out;
  for (const auto& role : file.roles) {
    out += std::string(static_cast<size_t>(role.depth) * 2, ' ') + LocalName(scope, role.id);
    if (role.version.size() >= 12) out += "  v:" + role.version.substr(0, 12);
    out += "  " + std::to_string(role.line_count) + " lines";
    if (role.source_start_line != 0 && role.source_end_line != 0) {
      out += "  source " + std::to_string(role.source_start_line) + "-" +
             std::to_string(role.source_end_line);
    }
    if (!role.first_line.empty()) out += "  | " + role.first_line;
    out += '\n';
  }
  return out;
}

int RunTui(Workspace& workspace, const std::string& path_prefix) {
  if (!isatty(fileno(stdin)) || !isatty(fileno(stdout))) {
    std::cerr << "tui requires an interactive terminal on stdin and stdout\n";
    return 2;
  }
  auto files = workspace.TuiSnapshot(path_prefix);
  if (files.empty()) {
    std::cout << "no managed files";
    if (!path_prefix.empty()) std::cout << " matching " << path_prefix;
    std::cout << "; run `tfe-code import PATH` first\n";
    return 0;
  }
  auto screen = ScreenInteractive::TerminalOutput();

  // Tree state of the file in view: flat rows plus per-row collapse flags.
  std::vector<TuiRoleRow> rows;
  std::vector<bool> collapsed;
  std::vector<size_t> visible;
  int selected_row = 0;
  std::string scope;
  std::vector<std::string> tree_labels;

  auto build_labels = [&] {
    tree_labels.clear();
    for (const size_t index : visible) {
      const auto& row = rows.at(index);
      const bool has_children = index + 1 < rows.size() && rows[index + 1].depth > row.depth;
      std::string label = std::string(static_cast<size_t>(row.depth) * 2, ' ') +
                          (has_children ? (collapsed.at(index) ? "▸ " : "▾ ") : "  ") +
                          LocalName(scope, row.id);
      if (row.version.size() >= 12) label += "  v:" + row.version.substr(0, 12);
      tree_labels.push_back(label);
    }
  };
  auto rebuild_tree = [&] {
    visible = VisibleRoleRows(rows, collapsed);
    if (selected_row >= static_cast<int>(visible.size())) {
      selected_row = visible.empty() ? 0 : static_cast<int>(visible.size()) - 1;
    }
    build_labels();
  };

  // Search state: '/' opens the filter, Enter applies it, Esc clears it.
  bool searching = false;
  int search_focus = 0;
  std::string search_term;
  std::set<std::string> search_ids;
  auto search_input = Input(&search_term, "substring of role id or code");

  // Detail pane (right): the selected role's body, fetched once per selection.
  std::string detail_text;
  std::string detail_id;
  // A read-only viewer must survive a diverged workspace: reads fail with a
  // sync diagnostic, and the dashboard then keeps its last good view.
  std::string stale_note;
  auto refresh_detail = [&] {
    if (visible.empty()) {
      detail_text = "(no roles)";
      detail_id.clear();
      return;
    }
    const auto& row = rows.at(visible.at(static_cast<size_t>(selected_row)));
    if (row.id == detail_id) return;
    std::string body;
    try {
      const auto nodes = workspace.ShowRoles({row.id});
      const auto found = nodes.find(row.id);
      body = found == nodes.end() ? "(role vanished)" : found->second.text;
    } catch (const std::exception&) {
      stale_note = "workspace changed on disk - run sync; the view is stale";
      body = "(stale)";
    }
    detail_text = RoleDetailText(row, body);
    detail_id = row.id;
  };

  int file_selected = 0;
  auto apply_search = [&] {
    search_ids.clear();
    try {
      if (!search_term.empty()) {
        // Scoped to the file in view so the tree filter stays local and cheap.
        const auto matches =
            workspace.Search({search_term}, "",
                             files.at(static_cast<size_t>(file_selected)).path, true, false);
        for (size_t pos = 0; pos < matches.size();) {
          const auto next = matches.find('\n', pos);
          const std::string line = matches.substr(
              pos, next == std::string::npos ? std::string::npos : next - pos);
          const auto colon = line.find(": ");
          if (colon != std::string::npos) search_ids.insert(line.substr(0, colon));
          if (next == std::string::npos) break;
          pos = next + 1;
        }
      }
      visible = FilterRoleRows(rows, search_ids);
    } catch (const std::exception&) {
      stale_note = "workspace changed on disk - run sync; the view is stale";
    }
    if (selected_row >= static_cast<int>(visible.size())) selected_row = 0;
    build_labels();
  };

  // File list (left pane) and the file currently driving the tree pane.
  size_t total_roles = 0;
  std::vector<std::string> file_labels;
  file_labels.reserve(files.size());
  for (const auto& file : files) {
    total_roles += file.roles.size();
    file_labels.push_back(file.path + " (" + std::to_string(file.roles.size()) + ")");
  }
  int file_in_view = 0;
  auto file_menu = Menu(&file_labels, &file_selected);

  auto select_file = [&](int index) {
    const auto& file = files.at(static_cast<size_t>(index));
    rows = file.roles;
    collapsed.assign(rows.size(), false);
    scope = rows.empty() || !rows.front().id.ends_with(".file")
                ? ""
                : rows.front().id.substr(0, rows.front().id.size() - 5);
    rebuild_tree();
  };
  select_file(0);

  // The middle pane: a collapsible tree menu. Enter/space toggles a branch.
  auto tree_menu = Menu(&tree_labels, &selected_row);
  auto tree = CatchEvent(tree_menu, [&](const Event& event) {
    if (event != Event::Return && event != Event::Character(' ')) return false;
    if (!visible.empty()) {
      const size_t index = visible.at(static_cast<size_t>(selected_row));
      if (index + 1 < rows.size() && rows[index + 1].depth > rows[index].depth) {
        collapsed.at(index) = !collapsed.at(index);
        rebuild_tree();
      }
    }
    return true;
  });

  // Live refresh: an efsw watcher on the workspace root posts a debounced
  // rebuild through the UI event queue; the watcher thread touches nothing
  // but `screen`, so both threads stay safe.
  const Event refresh_event = Event::Special("tfe-tui-refresh");
  class RefreshListener : public efsw::FileWatchListener {
   public:
    RefreshListener(ScreenInteractive& screen, const Event& refresh_event)
        : screen_(screen), refresh_event_(refresh_event) {}
    void handleFileAction(efsw::WatchID, const std::string& directory,
                          const std::string& filename, efsw::Action,
                          const std::string&) override {
      const std::string generic = (std::filesystem::path(directory) / filename).generic_string();
      if (generic.find("/.tfe-code/") != std::string::npos) return;
      static const char* kSuffixes[] = {".py",  ".cc", ".cpp", ".cxx", ".c",  ".hpp",
                                        ".h",  ".ts", ".tsx", ".js",  ".jsx", ".rs",
                                        ".go"};
      bool managed = false;
      for (const char* suffix : kSuffixes) {
        if (generic.ends_with(suffix)) {
          managed = true;
          break;
        }
      }
      if (!managed) return;
      const auto now = std::chrono::steady_clock::now();
      if (now - last_ < std::chrono::milliseconds(400)) return;
      last_ = now;
      screen_.PostEvent(refresh_event_);
    }

   private:
    ScreenInteractive& screen_;
    const Event& refresh_event_;
    std::chrono::steady_clock::time_point last_ =
        std::chrono::steady_clock::now() - std::chrono::seconds(10);
  } listener(screen, refresh_event);

  auto full_refresh = [&] {
    try {
      files = workspace.TuiSnapshot(path_prefix);
    } catch (const std::exception&) {
      stale_note = "workspace changed on disk - run sync; the view is stale";
      return;
    }
    stale_note.clear();
    total_roles = 0;
    file_labels.clear();
    for (const auto& file : files) {
      total_roles += file.roles.size();
      file_labels.push_back(file.path + " (" + std::to_string(file.roles.size()) + ")");
    }
    if (files.empty()) {
      screen.Exit();
      return;
    }
    if (file_selected >= static_cast<int>(files.size())) file_selected = 0;
    file_in_view = file_selected;
    select_file(file_selected);
    search_term.clear();
    search_ids.clear();
    detail_id.clear();
    detail_text.clear();
  };

  efsw::FileWatcher watcher;
  watcher.addWatch(workspace.root().string(), &listener, true);
  watcher.watch();

  auto mid = ftxui::Container::Tab({tree, search_input}, &search_focus);
  auto body = ftxui::Container::Vertical({file_menu, mid});
  auto dashboard = Renderer(body, [&] {
    if (file_in_view != file_selected) {
      file_in_view = file_selected;
      select_file(file_in_view);
      search_term.clear();
      search_ids.clear();
      detail_id.clear();
      detail_text.clear();
    }
    refresh_detail();
    const auto header = hbox(
        {text(" tfe-code tui"), filler(),
         stale_note.empty()
             ? text(std::to_string(files.size()) + " files, " +
                    std::to_string(total_roles) + " roles ")
             : text(stale_note)});
    const auto footer =
        searching
            ? hbox({text(" /"), search_input->Render(), text("   Enter: filter · Esc: clear")})
            : text(" / search · Tab panes · Enter collapse · r refresh · q quit");
    const auto panes = hbox(
        {file_menu->Render() | frame | size(WIDTH, LESS_THAN, 36), separator(),
         tree->Render() | frame | size(WIDTH, LESS_THAN, 52), separator(),
         paragraph(detail_text) | yflex | yframe});
    return vbox({header, separator(), panes, separator(), footer}) | border;
  });
  auto interactive = CatchEvent(dashboard, [&](const Event& event) {
    if (event == refresh_event) {
      full_refresh();
      return true;
    }
    if (searching) {
      if (event == Event::Return) {
        apply_search();
        searching = false;
        search_focus = 0;
        return true;
      }
      if (event == Event::Escape) {
        searching = false;
        search_focus = 0;
        search_term.clear();
        return true;
      }
      return false;
    }
    if (event == Event::Character('/')) {
      searching = true;
      search_focus = 1;
      return true;
    }
    if (event == Event::Character('r')) {
      full_refresh();
      return true;
    }
    if (event == Event::Character('q') || event == Event::Escape) {
      screen.Exit();
      return true;
    }
    return false;
  });
  screen.Loop(interactive);
  return 0;
}

std::vector<size_t> VisibleRoleRows(const std::vector<TuiRoleRow>& rows,
                                    const std::vector<bool>& collapsed) {
  // A row is visible when no ancestor in its chain is collapsed; the stack
  // carries (depth, chain hidden) for the nearest open ancestor at each level.
  std::vector<size_t> visible;
  std::vector<std::pair<int, bool>> chain;
  for (size_t i = 0; i < rows.size(); ++i) {
    while (!chain.empty() && chain.back().first >= rows[i].depth) chain.pop_back();
    bool hidden = false;
    for (const auto& entry : chain) hidden = hidden || entry.second;
    if (!hidden) visible.push_back(i);
    const bool is_collapsed = i < collapsed.size() && collapsed[i];
    chain.push_back({rows[i].depth, is_collapsed || hidden});
  }
  return visible;
}

std::vector<size_t> FilterRoleRows(const std::vector<TuiRoleRow>& rows,
                                   const std::set<std::string>& matched_ids) {
  // Keep every matched row plus its full ancestor chain (the nearest previous
  // row of each shallower depth), so the tree still reads as a tree.
  std::vector<bool> keep(rows.size(), false);
  for (size_t i = 0; i < rows.size(); ++i) {
    if (!matched_ids.count(rows[i].id)) continue;
    keep[i] = true;
    int need = rows[i].depth - 1;
    for (size_t j = i; j-- > 0 && need >= 0;) {
      if (rows[j].depth == need) {
        keep[j] = true;
        --need;
      }
    }
  }
  std::vector<size_t> kept;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (keep[i]) kept.push_back(i);
  }
  return kept;
}

std::string RoleDetailText(const TuiRoleRow& role, const std::string& body) {
  std::string header = role.id;
  if (role.version.size() >= 12) header += "  v:" + role.version.substr(0, 12);
  header += "  " + std::to_string(role.line_count) + " lines";
  if (role.source_start_line != 0 && role.source_end_line != 0) {
    header += "  source " + std::to_string(role.source_start_line) + "-" +
              std::to_string(role.source_end_line);
  }
  return header + "\n\n" + body;
}

std::vector<std::string> StatsChartRows(const TelemetryReport& report) {
  // One row per command: name, a saved-tokens bar scaled to the busiest
  // command (10 cells), and the raw count. A zero-saving command keeps its
  // row so the chart shape stays stable.
  size_t max_saved = 0;
  for (const auto& [command, stats] : report.by_command) {
    (void)command;
    max_saved = std::max(max_saved, static_cast<size_t>(stats.saved_tokens));
  }
  std::vector<std::string> rows;
  for (const auto& [command, stats] : report.by_command) {
    std::string bar;
    if (stats.saved_tokens > 0 && max_saved > 0) {
      const size_t units = std::max<size_t>(1, stats.saved_tokens * 10 / max_saved);
      bar.reserve(units * 3);
      for (size_t i = 0; i < units; ++i) bar += "█";
    }
    rows.push_back(command + "  " + bar + "  " + std::to_string(stats.saved_tokens));
  }
  return rows;
}

}  // namespace tfe::code
