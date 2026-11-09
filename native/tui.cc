#include "tui.h"

#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

namespace tfe::code {
namespace {

using ftxui::border;
using ftxui::CatchEvent;
using ftxui::Event;
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
  const auto files = workspace.TuiSnapshot(path_prefix);
  if (files.empty()) {
    std::cout << "no managed files";
    if (!path_prefix.empty()) std::cout << " matching " << path_prefix;
    std::cout << "; run `tfe-code import PATH` first\n";
    return 0;
  }
  size_t total_roles = 0;
  std::vector<std::string> labels;
  labels.reserve(files.size());
  for (const auto& file : files) {
    total_roles += file.roles.size();
    labels.push_back(file.path + " (" + std::to_string(file.roles.size()) + ")");
  }
  int selected = 0;
  auto screen = ScreenInteractive::TerminalOutput();
  auto menu = Menu(&labels, &selected);
  // The dashboard is a decorated menu: the menu keeps keyboard focus, and the
  // pane on the right is rebuilt from the selected file on every frame.
  auto dashboard = Renderer(menu, [&] {
    const auto header = hbox(text(" tfe-code tui"), filler(),
                             text(std::to_string(files.size()) + " files, " +
                                  std::to_string(total_roles) + " roles "));
    const auto body = hbox(
        {menu->Render() | frame | size(WIDTH, LESS_THAN, 44), separator(),
         paragraph(RoleRowsText(files.at(static_cast<size_t>(selected)))) | yflex | yframe});
    return vbox({header, separator(), body}) | border;
  });
  auto interactive = CatchEvent(dashboard, [&](const Event& event) {
    if (event == Event::Character('q') || event == Event::Escape) {
      screen.Exit();
      return true;
    }
    return false;
  });
  screen.Loop(interactive);
  return 0;
}

}  // namespace tfe::code
