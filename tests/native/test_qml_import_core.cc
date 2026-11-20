// Core verification of QML language support in TFEngine Code: parsing
// (.qml), role extraction (objects, properties, signals, bindings, handlers,
// functions, inline components), byte-exact rendering, the degraded fallback
// for unparsable files, workspace import, search/context, apply patching, and
// the syntax gate that rejects an edit which breaks the rendered file.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "qml_import.h"
#include "render_project.h"
#include "workspace.h"

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
    Expect(tfe::code::IsQmlFile("ui/Main.qml"), "ui/Main.qml should be QML");
    Expect(!tfe::code::IsQmlFile("ui/Main.qmlc"), "ui/Main.qmlc is not source QML");
    Expect(!tfe::code::IsQmlFile("Main.qml.bak"), "Main.qml.bak is not QML");
    Expect(!tfe::code::IsQmlFile("main.py"), "main.py is not QML");

    // 2. Structural import: whole-file role plus object/member roles.
    const std::string qml_source =
        "import QtQuick 2.15\n"
        "\n"
        "Rectangle {\n"
        "    id: root\n"
        "    width: 640\n"
        "    color: \"white\"\n"
        "\n"
        "    property int counter: 0\n"
        "    signal clicked(int x)\n"
        "\n"
        "    Component.onCompleted: console.log(\"ready\")\n"
        "\n"
        "    function increment(step) {\n"
        "        counter += step\n"
        "        return counter\n"
        "    }\n"
        "\n"
        "    Text {\n"
        "        id: title\n"
        "        text: qsTr(\"Hello\")\n"
        "    }\n"
        "\n"
        "    Rectangle {\n"
        "        width: 10\n"
        "    }\n"
        "\n"
        "    MouseArea {\n"
        "        anchors.fill: parent\n"
        "        onClicked: root.clicked(mouse.x)\n"
        "    }\n"
        "}\n";

    const auto project = tfe::code::ImportQml("ui/Main.qml", qml_source);
    Expect(project.files.contains("ui/Main.qml"), "missing file record");
    Expect(project.files.at("ui/Main.qml").root == "ui.Main.file", "wrong root role id");
    for (const std::string& id :
         {"ui.Main.file", "ui.Main.root", "ui.Main.root.width", "ui.Main.root.color",
          "ui.Main.root.counter", "ui.Main.root.clicked", "ui.Main.root.Component.onCompleted",
          "ui.Main.root.increment", "ui.Main.root.title", "ui.Main.root.title.text",
          "ui.Main.root.Rectangle", "ui.Main.root.Rectangle.width", "ui.Main.root.MouseArea",
          "ui.Main.root.MouseArea.anchors.fill", "ui.Main.root.MouseArea.onClicked"}) {
      Expect(project.blocks.contains(id), "missing role " + id);
    }
    Expect(!project.blocks.contains("ui.Main.root.id"), "the id binding should stay in the object");
    Expect(!project.blocks.at("ui.Main.file").literal, "file role should be a container");
    Expect(project.blocks.at("ui.Main.root.MouseArea").literal == false,
           "object role should be a container");
    Expect(project.blocks.at("ui.Main.root.width").literal, "property role should be a leaf");
    const auto rendered = tfe::code::RenderFiles(project);
    Expect(rendered.at("ui/Main.qml") == qml_source, "lossless render failed for ui/Main.qml");

    // 3. Objects with and without an id, and objects nested in a binding array.
    const std::string states_source =
        "import QtQuick 2.15\n"
        "\n"
        "Item {\n"
        "    states: [\n"
        "        State {\n"
        "            name: \"on\"\n"
        "            PropertyChanges { target: owner; opacity: 1 }\n"
        "        }\n"
        "    ]\n"
        "\n"
        "    component Header: Text {\n"
        "        font.bold: true\n"
        "    }\n"
        "}\n";
    const auto states_project = tfe::code::ImportQml("ui/States.qml", states_source);
    for (const std::string& id :
         {"ui.States.Item", "ui.States.Item.states", "ui.States.Item.states.State",
          "ui.States.Item.states.State.name", "ui.States.Item.states.State.PropertyChanges",
          "ui.States.Item.states.State.PropertyChanges.opacity", "ui.States.Item.Header",
          "ui.States.Item.Header.font.bold"}) {
      Expect(states_project.blocks.contains(id), "missing role " + id);
    }
    Expect(tfe::code::RenderFiles(states_project).at("ui/States.qml") == states_source,
           "lossless render failed for ui/States.qml");

    // 4. A file role owns the whole byte range: a UTF-8 BOM must survive, and a
    //    file without a trailing newline keeps that shape.
    {
      const std::string bom = "\xEF\xBB\xBF";
      const std::string bom_source = bom + "Item {\n    width: 1\n}";
      const auto bom_project = tfe::code::ImportQml("Bom.qml", bom_source);
      Expect(bom_project.blocks.at("Bom.file").text.starts_with(bom),
             "file role text lost the BOM prefix");
      Expect(tfe::code::RenderFiles(bom_project).at("Bom.qml") == bom_source,
             "lossless render failed for BOM-prefixed QML");
    }

    // 4a. A role that cannot be dedented safely stays inside its parent: a
    //     template literal with a column-0 line would otherwise be re-indented.
    {
      const std::string raw_source =
          "Item {\n"
          "    property string text: `line one\n"
          "column zero`\n"
          "}\n";
      const auto raw_project = tfe::code::ImportQml("Raw.qml", raw_source);
      Expect(raw_project.blocks.contains("Raw.Item"), "missing Item role");
      Expect(!raw_project.blocks.contains("Raw.Item.text"),
             "role with verbatim column-0 lines must not be extracted");
      Expect(tfe::code::RenderFiles(raw_project).at("Raw.qml") == raw_source,
             "lossless render failed for template literal property");
    }

    // 5. Syntax errors degrade to a single whole-file role instead of failing.
    {
      bool degraded = false;
      const auto broken =
          tfe::code::ImportQml("broken.qml", "Item {\n    width: 1\n", &degraded);
      Expect(degraded, "broken QML was not reported as degraded");
      Expect(broken.blocks.size() == 1, "degraded QML should hold exactly one role");
      Expect(broken.blocks.at("broken.file").literal, "degraded QML role should be a leaf");
      Expect(tfe::code::RenderFiles(broken).at("broken.qml") == "Item {\n    width: 1\n",
             "degraded QML did not render byte-exactly");
    }

    // 6. Full workspace integration: import a directory, then read and edit.
    const fs::path root = fs::temp_directory_path() / "tfe-code-qml-workspace-test";
    fs::remove_all(root);
    Write(root / "ui/Main.qml", qml_source);

    {
      tfe::code::Workspace workspace(root);
      const auto result = workspace.Import({"ui"});
      Expect(result.changed.size() == 1, "import did not find the .qml file");
      Expect(workspace.Render().at("ui/Main.qml") == qml_source,
             "workspace rendered Main.qml mismatch");

      const auto search_out = workspace.Search({"Component.onCompleted"}, "");
      Expect(search_out.find("ui.Main.root.Component.onCompleted") != std::string::npos,
             "search did not find the handler role: " + search_out);

      const auto ctx_out = workspace.Context({"increment"}, {"ui/Main.qml"}, true);
      Expect(ctx_out.find("Main.root.increment") != std::string::npos,
             "context did not locate increment role: " + ctx_out);

      const auto proj_before = workspace.LoadProject();
      const auto color_role = "ui.Main.root.color";
      const auto& color_before = proj_before.blocks.at(color_role);

      tfe::code::RoleEdit edit;
      edit.role = color_role;
      edit.expected_version = color_before.version;
      edit.old = "\"white\"";
      edit.new_text = "\"black\"";
      const auto apply_res = workspace.Apply({edit}, std::nullopt);
      Expect(!apply_res.changed.empty(), "apply did not record a changed file");

      const auto on_disk = Read(root / "ui/Main.qml");
      Expect(on_disk.find("color: \"black\"") != std::string::npos,
             "source file on disk does not contain the edit: " + on_disk);
      Expect(workspace.Render().at("ui/Main.qml") == on_disk,
             "rendered edited Main.qml does not match the on-disk file");

      // A syntax-breaking edit is rejected and the file is left untouched.
      const auto proj_after = workspace.LoadProject();
      const auto increment_role = "ui.Main.root.increment";
      const auto& increment_after = proj_after.blocks.at(increment_role);
      tfe::code::RoleEdit bad_edit;
      bad_edit.role = increment_role;
      bad_edit.expected_version = increment_after.version;
      bad_edit.old = "return counter";
      bad_edit.new_text = "return (counter";
      bool edit_rejected = false;
      try {
        workspace.Apply({bad_edit}, std::nullopt);
      } catch (const std::exception&) {
        edit_rejected = true;
      }
      Expect(edit_rejected, "syntax-breaking QML apply edit was not rejected");
      Expect(Read(root / "ui/Main.qml") == on_disk,
             "rejected edit modified the QML source on disk");
    }

    fs::remove_all(root);
    std::cout << "qml_import_core tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Test failed: " << e.what() << "\n";
    return 1;
  }
}
