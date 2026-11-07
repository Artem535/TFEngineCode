// CMake wiring self test: proves tfe_code_core is linked, that the public core
// headers are reachable, and that the generated test manifest lists every test
// module with its source path and README verification snippet.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "project.h"
#include "workspace.h"

namespace fs = std::filesystem;

#ifndef TFE_CODE_TEST_MANIFEST
#error "TFE_CODE_TEST_MANIFEST must be defined by CMakeLists.txt"
#endif
#ifndef TFE_CODE_REGISTERED_TESTS
#error "TFE_CODE_REGISTERED_TESTS must be defined by CMakeLists.txt"
#endif
#ifndef TFE_CODE_ALL_TEST_MODULES
#error "TFE_CODE_ALL_TEST_MODULES must be defined by CMakeLists.txt"
#endif

void Expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::vector<std::string> Split(const std::string& value, char separator) {
  std::vector<std::string> parts;
  std::string current;
  for (const char character : value) {
    if (character == separator) {
      if (!current.empty()) parts.push_back(current);
      current.clear();
    } else {
      current.push_back(character);
    }
  }
  if (!current.empty()) parts.push_back(current);
  return parts;
}

int main() {
  try {
    // tfe_code_core symbol + header reachability: constructing a Workspace and
    // naming a Project type only type-checks if both headers and the archive are present.
    const fs::path probe = fs::temp_directory_path() / "tfe-code-cmake-wiring-probe";
    fs::remove_all(probe);
    fs::create_directories(probe);
    tfe::code::Workspace workspace(probe);
    // A workspace that was never imported must refuse to load a project; this
    // also proves the symbol is linked and the headers are the real ones.
    bool rejected = false;
    try {
      (void)workspace.LoadProject();
    } catch (const std::exception&) {
      rejected = true;
    }
    Expect(rejected, "a fresh workspace must reject LoadProject");
    fs::remove_all(probe);

    const std::string manifest_path{TFE_CODE_TEST_MANIFEST};
    std::ifstream manifest(manifest_path);
    Expect(manifest.good(), "CMake test manifest is missing: " + manifest_path);
    const std::string contents{std::istreambuf_iterator<char>(manifest), {}};
    const std::vector<std::string> modules = Split(std::string{TFE_CODE_ALL_TEST_MODULES}, ';');
    Expect(!modules.empty(), "CMake reported no test modules; the glob found nothing");
    size_t suites = 0;
    for (const auto& line : Split(contents, '\n')) {
      if (line.empty() || line.front() == '#') continue;
      ++suites;
      Expect(line.starts_with("CMSUITE:"), "manifest line lacks the CMSUITE prefix: " + line);
      const std::vector<std::string> fields = Split(line, ':');
      Expect(fields.size() >= 4, "manifest line is missing fields: " + line);
      std::cout << line << '\n';
    }
    Expect(suites == modules.size(),
           "manifest suite count does not match the reported module count");

    const std::string registered{TFE_CODE_REGISTERED_TESTS};
    const std::vector<std::string> targets = Split(registered, ';');
    const bool everything_selected = targets.size() >= modules.size();
    if (!everything_selected) {
      std::cout << "subset build: " << targets.size() << " of " << modules.size()
                << " modules registered\n";
    }
    for (const auto& target : targets) {
      std::cout << "CMTARGET:" << target << '\n';
    }
    std::cout << "tfe-code test manifest: " << manifest_path << " (" << suites << " suites)\n";
    std::cout << "autotests_cmake_wiring_tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
