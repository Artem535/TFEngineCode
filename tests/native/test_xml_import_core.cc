// Core verification of XML language support in TFEngine Code: parsing
// (.xml), role extraction (elements named by id/name attribute or tag,
// self-closing leaves, nesting), byte-exact rendering, the degraded fallback,
// workspace import, apply on an element and the syntax gate.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "render_project.h"
#include "workspace.h"
#include "xml_import.h"

namespace fs = std::filesystem;

namespace {

void Expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string Read(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}

}  // namespace

int main() {
  try {
    // 1. File extension detection
    Expect(tfe::code::IsXmlFile("pom.xml"), "pom.xml should be XML");
    Expect(tfe::code::IsXmlFile("config/app.xml"), "config/app.xml should be XML");
    Expect(!tfe::code::IsXmlFile("pom.xml.bak"), "pom.xml.bak is not XML");
    Expect(!tfe::code::IsXmlFile("Main.qml"), "Main.qml is not XML source");

    // 2. Element roles: id/name attribute naming, tag naming, nesting.
    const std::string pom_source =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<project id=\"demo\" xmlns=\"http://maven.apache.org/POM/4.0.0\">\n"
        "  <!-- build file -->\n"
        "  <name>demo</name>\n"
        "  <dependencies>\n"
        "    <dependency>\n"
        "      <groupId>org.example</groupId>\n"
        "      <artifactId>core</artifactId>\n"
        "      <version>1.0</version>\n"
        "    </dependency>\n"
        "    <dependency>\n"
        "      <groupId>org.example</groupId>\n"
        "      <artifactId>ui</artifactId>\n"
        "      <scope>runtime</scope>\n"
        "    </dependency>\n"
        "  </dependencies>\n"
        "  <modules>\n"
        "    <module>core</module>\n"
        "  </modules>\n"
        "</project>\n";

    const auto project = tfe::code::ImportXml("pom.xml", pom_source);
    Expect(project.files.contains("pom.xml"), "missing file record");
    Expect(project.files.at("pom.xml").root == "pom.file", "wrong root role id");
    for (const std::string& id :
         {"pom.file", "pom.demo", "pom.demo.name", "pom.demo.dependencies",
          "pom.demo.dependencies.dependency", "pom.demo.dependencies.dependency.groupId",
          "pom.demo.dependencies.dependency.artifactId",
          "pom.demo.dependencies.dependency.version",
          "pom.demo.dependencies.dependency_2",
          "pom.demo.dependencies.dependency_2.groupId",
          "pom.demo.dependencies.dependency_2.scope", "pom.demo.modules",
          "pom.demo.modules.module"}) {
      Expect(project.blocks.contains(id), "missing role " + id);
    }
    Expect(!project.blocks.at("pom.file").literal, "file role should be a container");
    Expect(project.blocks.at("pom.demo.name").literal, "leaf element should be a leaf role");
    Expect(tfe::code::RenderFiles(project).at("pom.xml") == pom_source,
           "lossless render failed for pom.xml");

    // 3. Self-closing elements, attribute values and mixed content.
    const std::string mixed_source =
        "<r id=\"root\" a=\"1\">\n"
        "  before <a/> middle <b x=\"y\">text</b> after\n"
        "  <c d=\"e\"/>\n"
        "</r>\n";
    const auto mixed_project = tfe::code::ImportXml("Mixed.xml", mixed_source);
    for (const std::string& id :
         {"Mixed.root", "Mixed.root.a", "Mixed.root.b", "Mixed.root.c"}) {
      Expect(mixed_project.blocks.contains(id), "missing role " + id);
    }
    Expect(mixed_project.blocks.at("Mixed.root.a").text == "<a/>",
           "self-closing element role text is wrong");
    Expect(tfe::code::RenderFiles(mixed_project).at("Mixed.xml") == mixed_source,
           "lossless render failed for mixed content");

    // 4. A UTF-8 BOM and a missing final newline survive.
    {
      const std::string bom = "\xEF\xBB\xBF";
      const std::string bom_source = bom + "<r>\n  <a>x</a>\n</r>";
      const auto bom_project = tfe::code::ImportXml("Bom.xml", bom_source);
      Expect(bom_project.blocks.at("Bom.file").text.starts_with(bom),
             "file role text lost the BOM prefix");
      Expect(tfe::code::RenderFiles(bom_project).at("Bom.xml") == bom_source,
             "lossless render failed for BOM-prefixed XML");
    }

    // 5. Malformed XML degrades to one leaf role instead of failing the import.
    {
      bool degraded = false;
      const auto broken =
          tfe::code::ImportXml("broken.xml", "<a><b></a>\n", &degraded);
      Expect(degraded, "broken XML was not reported as degraded");
      Expect(broken.blocks.size() == 1, "degraded XML should hold exactly one role");
      Expect(broken.blocks.at("broken.file").literal, "degraded XML role should be a leaf");
      Expect(tfe::code::RenderFiles(broken).at("broken.xml") == "<a><b></a>\n",
             "degraded XML did not render byte-exactly");
    }

    // 6. Full workspace integration: import, read, apply, syntax gate.
    const fs::path root = fs::temp_directory_path() / "tfe-code-xml-workspace-test";
    fs::remove_all(root);
    Write(root / "pom.xml", pom_source);

    {
      tfe::code::Workspace workspace(root);
      const auto result = workspace.Import({"pom.xml"});
      Expect(result.changed.size() == 1, "import did not find the .xml file");
      Expect(workspace.Render().at("pom.xml") == pom_source,
             "workspace rendered pom.xml mismatch");

      const auto search_out = workspace.Search({"runtime"}, "");
      Expect(search_out.find("pom.demo.dependencies.dependency_2.scope") != std::string::npos,
             "search did not find the second dependency scope: " + search_out);

      const auto proj_before = workspace.LoadProject();
      const auto version_role = "pom.demo.dependencies.dependency.version";
      const auto& version_before = proj_before.blocks.at(version_role);

      tfe::code::RoleEdit edit;
      edit.role = version_role;
      edit.expected_version = version_before.version;
      edit.old = "1.0";
      edit.new_text = "2.0";
      const auto apply_res = workspace.Apply({edit}, std::nullopt);
      Expect(!apply_res.changed.empty(), "apply did not record a changed file");

      const auto on_disk = Read(root / "pom.xml");
      Expect(on_disk.find("<version>2.0</version>") != std::string::npos,
             "source file on disk does not contain the edit: " + on_disk);
      Expect(workspace.Render().at("pom.xml") == on_disk,
             "rendered edited pom.xml does not match the on-disk file");

      // A syntax-breaking edit is rejected and the file stays untouched.
      const auto proj_after = workspace.LoadProject();
      const auto name_role = "pom.demo.name";
      const auto& name_after = proj_after.blocks.at(name_role);
      tfe::code::RoleEdit bad_edit;
      bad_edit.role = name_role;
      bad_edit.expected_version = name_after.version;
      bad_edit.old = "demo";
      bad_edit.new_text = "demo <";
      bool edit_rejected = false;
      try {
        workspace.Apply({bad_edit}, std::nullopt);
      } catch (const std::exception&) {
        edit_rejected = true;
      }
      Expect(edit_rejected, "syntax-breaking XML apply edit was not rejected");
      Expect(Read(root / "pom.xml") == on_disk,
             "rejected edit modified the XML source on disk");
    }

    fs::remove_all(root);
    std::cout << "xml_import_core tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Test failed: " << e.what() << "\n";
    return 1;
  }
}
