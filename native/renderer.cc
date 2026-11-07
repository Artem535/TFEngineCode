// Extracted from the TextFoundryEngine code-fragments prototype.
// MIT License; see LICENSE.
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <rfl/json.hpp>

#include "render_project.h"

int main(int argc, char** argv) {
  try {
    using namespace tfe::code;
    if (argc < 3) throw std::runtime_error(
        "Usage: tfe-code-render PROJECT.json render FILE | context FILE BLOCK | "
        "search QUERY | patch BLOCK EXPECTED_VERSION NEW_VERSION TEXT_FILE | "
        "apply-render BLOCK EXPECTED_VERSION NEW_VERSION TEXT_FILE JOBS | "
        "render-all JOBS | inspect FILE JOBS [BLOCK ...] [--uses] | "
        "find QUERY JOBS LIMIT OFFSET FILE_PREFIX | apply-many-render EDITS.json JOBS");
    const auto parsed = rfl::json::read<Project, rfl::DefaultIfMissing>(ReadFile(argv[1]));
    if (!parsed) throw std::runtime_error(parsed.error().what());
    const auto project = parsed.value();
    const std::string command = argv[2];
    if (command == "render" && argc == 4) {
      std::cout << RenderFile(project, argv[3]);
    } else if (command == "render-all" && argc == 4) {
      std::cout << rfl::json::write(RenderFiles(project, ParseJobs(argv[3]))) << '\n';
    } else if (command == "inspect" && argc >= 5) {
      bool uses = false;
      std::vector<std::string> ids;
      for (int i = 5; i < argc; ++i) {
        if (std::string(argv[i]) == "--uses") uses = true;
        else ids.push_back(argv[i]);
      }
      std::cout << rfl::json::write(Inspect(project, argv[3], ParseJobs(argv[4]), ids, uses)) << '\n';
    } else if (command == "find" && argc == 8) {
      (void)ParseJobs(argv[4]);
      const auto number = [](const std::string& text) {
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
          throw std::runtime_error("Find limit/offset must be nonnegative integers");
        }
        return std::stoull(text);
      };
      std::cout << rfl::json::write(Find(project, argv[3], number(argv[5]), number(argv[6]), argv[7])) << '\n';
    } else if (command == "apply-many-render" && argc == 5) {
      const auto edits = rfl::json::read<std::vector<Edit>>(ReadFile(argv[3]));
      if (!edits) throw std::runtime_error(edits.error().what());
      std::cout << rfl::json::write(ApplyMany(project, edits.value(), ParseJobs(argv[4]))) << '\n';
    } else if (command == "apply-render" && argc == 8) {
      const std::vector<Edit> edits{{argv[3], argv[4], argv[5], ReadFile(argv[6])}};
      std::cout << rfl::json::write(ApplyMany(project, edits, ParseJobs(argv[7]))) << '\n';
    } else if (command == "context" && argc == 5) {
      std::cout << rfl::json::write(ReadContext(project, argv[3], argv[4])) << '\n';
    } else if (command == "search" && argc == 4) {
      std::cout << rfl::json::write(Search(project, argv[3])) << '\n';
    } else if (command == "patch" && argc == 7) {
      const Edit edit{argv[3], argv[4], argv[5], ReadFile(argv[6])};
      std::cout << rfl::json::write(Patch(project, edit)) << '\n';
    } else {
      throw std::runtime_error("Invalid command or argument count");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
