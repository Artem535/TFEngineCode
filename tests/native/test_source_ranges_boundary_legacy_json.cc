// Legacy Project JSON read through the real Workspace boundary.
//
// test_python_import.cc only decodes a standalone Node with rfl. The boundary
// that actually ships is Workspace::LoadProject()/Render()/Outline()/Show() over
// an on-disk .tfe-code/project.json written by an older release, i.e. without the
// source_start_line/source_end_line fields. Those must be read as 0/0 and the
// workspace must keep rendering the exact source bytes.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "hash.h"
#include "render_project.h"
#include "workspace.h"

namespace fs = std::filesystem;

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string Read(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}

void Expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// Rewrites an up-to-date workspace index as an older release would have written
// it: the two source-range fields are absent from every block, and state.json is
// re-hashed so the index stays internally consistent.
void StripSourceRanges(const fs::path& root) {
  const auto project_path = root / ".tfe-code/project.json";
  std::string project = Read(project_path);
  const std::string start_field = "\"source_start_line\":";
  const std::string end_field = "\"source_end_line\":";
  Expect(project.find(start_field) != std::string::npos,
         "import wrote no source_start_line; the legacy fixture is meaningless");
  size_t removed = 0;
  for (const auto* field : {&start_field, &end_field}) {
    for (size_t at = project.find(*field); at != std::string::npos;
         at = project.find(*field)) {
      // Drop ", \"source_*_line\":<digits>" together with its leading comma.
      const size_t comma = project.rfind(',', at);
      Expect(comma != std::string::npos, "cannot locate a source-range field boundary");
      size_t stop = at + field->size();
      Expect(stop < project.size() && project[stop] >= '0' && project[stop] <= '9',
             "source-range field is not an unsigned number");
      while (stop < project.size() && project[stop] >= '0' && project[stop] <= '9') ++stop;
      project.erase(comma, stop - comma);
      ++removed;
    }
  }
  Expect(removed == 6, "expected to strip six source-range fields");
  Expect(project.find(start_field) == std::string::npos &&
             project.find(end_field) == std::string::npos,
         "legacy fixture still carries source-range fields");
  Write(project_path, project);
  const auto state_path = root / ".tfe-code/state.json";
  std::string state = Read(state_path);
  const std::string hash_field = "\"project_hash\":\"";
  const auto hash_at = state.find(hash_field);
  Expect(hash_at != std::string::npos, "state.json has no project_hash");
  const auto hash_begin = hash_at + hash_field.size();
  state.replace(hash_begin, state.find('"', hash_begin) - hash_begin,
                tfe::code::Sha256(project));
  Write(state_path, state);
}

int main() {
  try {
    const std::string source =
        "def first():\n    return 1\n\ndef second():\n    return 2\n";
    const fs::path root = fs::temp_directory_path() / "tfe-code-legacy-project-json";

    // Produce a real index, then rewind it to the pre-source-ranges schema.
    fs::remove_all(root);
    Write(root / "legacy.py", source);
    {
      tfe::code::Workspace modern_workspace(root);
      modern_workspace.Import({"legacy.py"});
      const auto modern = modern_workspace.LoadProject();
      Expect(modern.blocks.at("legacy.first").source_start_line == 1 &&
                 modern.blocks.at("legacy.second").source_start_line == 4,
             "modern import did not record source ranges; nothing to strip");
      Expect(modern_workspace.Render().at("legacy.py") == source,
             "modern import is not byte exact");
    }
    StripSourceRanges(root);

    // The real boundary: Workspace::LoadProject() over a legacy index.
    tfe::code::Workspace workspace(root);
    const auto legacy = workspace.LoadProject();
    Expect(legacy.files.contains("legacy.py"), "legacy index lost the file mapping");
    Expect(legacy.blocks.size() == 3, "legacy index lost blocks");
    for (const auto& [id, node] : legacy.blocks) {
      Expect(node.source_start_line == 0 && node.source_end_line == 0,
             "legacy role did not receive a 0/0 range: " + id);
    }

    // Rendering the legacy index must still reproduce the original bytes exactly.
    const auto rendered = tfe::code::RenderFiles(legacy);
    Expect(rendered.at("legacy.py") == source,
           "rendering a legacy index did not reproduce the source");
    Expect(workspace.Render().at("legacy.py") == source,
           "Workspace::Render on a legacy index did not reproduce the source");

    // Read models must not invent line annotations out of the 0/0 sentinel.
    const auto outline = workspace.Outline("legacy.py", 1);
    Expect(outline.find("legacy.first (2 lines) def first():") != std::string::npos &&
               outline.find("legacy.second (2 lines) def second():") != std::string::npos,
           "outline on a legacy index produced unexpected output: " + outline);
    Expect(outline.find("[lines:") == std::string::npos &&
               outline.find("; source ") == std::string::npos,
           "outline invented a source range from the 0/0 sentinel");
    const auto shown = workspace.Show({"legacy.first"});
    Expect(shown.find("def first():") != std::string::npos,
           "show on a legacy index did not return the role body");
    Expect(shown.find("[lines:") == std::string::npos,
           "show invented a source range from the 0/0 sentinel");
    Expect(workspace.ShowRoles({"legacy.second"}).at("legacy.second").source_end_line == 0,
           "ShowRoles on a legacy index did not expose a 0/0 range");
    fs::remove_all(root);

    std::cout << "source_ranges_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}