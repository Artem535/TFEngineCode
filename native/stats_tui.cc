#include "tui.h"

#include <unistd.h>

#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
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
using ftxui::hbox;
using ftxui::Renderer;
using ftxui::ScreenInteractive;
using ftxui::text;
using ftxui::vbox;

}  // namespace

int RunStatsTui(Workspace& workspace) {
  if (!isatty(fileno(stdin)) || !isatty(fileno(stdout))) {
    std::cerr << "stats --tui requires an interactive terminal on stdin and stdout\n";
    return 2;
  }
  const auto report = workspace.GetStats();
  auto screen = ScreenInteractive::TerminalOutput();
  auto dashboard = Renderer([&] {
    std::vector<ftxui::Element> lines;
    std::ostringstream header;
    header << " tfe-code stats   calls " << report.total_calls << "   saved "
           << report.saved_tokens << " tokens   savings " << std::fixed
           << std::setprecision(1) << report.savings_percent << "%";
    lines.push_back(hbox({text(header.str()), filler()}));
    const auto chart = StatsChartRows(report);
    if (chart.empty()) {
      lines.push_back(text(" no telemetry yet; run context/apply to accrue savings"));
    }
    for (const auto& row : chart) lines.push_back(text(" " + row));
    return vbox(lines) | border;
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
