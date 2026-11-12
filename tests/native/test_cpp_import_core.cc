// Core verification of C++ language support in TFEngine Code:
// parsing, role extraction, template unwrapping, namespace flattening,
// byte-exact rendering, workspace import, search, and apply patching.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "cpp_import.h"
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
    Expect(tfe::code::IsCppFile("main.cc"), "main.cc should be C++");
    Expect(tfe::code::IsCppFile("util.cpp"), "util.cpp should be C++");
    Expect(tfe::code::IsCppFile("algo.cxx"), "algo.cxx should be C++");
    Expect(tfe::code::IsCppFile("legacy.c"), "legacy.c should be C++");
    Expect(tfe::code::IsCppFile("math.h"), "math.h should be C++");
    Expect(tfe::code::IsCppFile("types.hpp"), "types.hpp should be C++");
    Expect(!tfe::code::IsCppFile("script.py"), "script.py is not C++");
    Expect(!tfe::code::IsCppFile("README.md"), "README.md is not C++");

    // 2. Direct ImportCpp and lossless rendering of complex C++
    const std::string cpp_source =
        "#pragma once\n"
        "\n"
        "#include <string>\n"
        "#include <vector>\n"
        "\n"
        "namespace math::geometry {\n"
        "\n"
        "template <typename T>\n"
        "T Clamp(T value, T low, T high) {\n"
        "  if (value < low) return low;\n"
        "  if (value > high) return high;\n"
        "  return value;\n"
        "}\n"
        "\n"
        "struct Point {\n"
        "  double x = 0.0;\n"
        "  double y = 0.0;\n"
        "\n"
        "  bool operator==(const Point& other) const {\n"
        "    return x == other.x && y == other.y;\n"
        "  }\n"
        "};\n"
        "\n"
        "class Transform {\n"
        " public:\n"
        "  explicit Transform(double scale) : scale_(scale) {}\n"
        "\n"
        "  Point Apply(const Point& p) const {\n"
        "    return Point{p.x * scale_, p.y * scale_};\n"
        "  }\n"
        "\n"
        " private:\n"
        "  double scale_;\n"
        "};\n"
        "\n"
        "}  // namespace math::geometry\n";

    const auto project = tfe::code::ImportCpp("include/geometry.hpp", cpp_source);
    Expect(project.files.contains("include/geometry.hpp"), "missing file record");
    Expect(project.blocks.contains("include.geometry_hpp.file"), "missing file role");
    Expect(project.blocks.contains("include.geometry_hpp.Clamp"), "missing Clamp template function role");
    Expect(project.blocks.contains("include.geometry_hpp.Point"), "missing Point struct role");
    Expect(project.blocks.contains("include.geometry_hpp.Transform"), "missing Transform class role");
    Expect(project.blocks.contains("include.geometry_hpp.Transform.Apply"), "missing Transform::Apply method role");

    // Byte-exact rendering check
    const auto rendered_files = tfe::code::RenderFiles(project);
    Expect(rendered_files.at("include/geometry.hpp") == cpp_source,
           "lossless render failed for include/geometry.hpp");

    // A single-line template header puts the function definition mid-line; the
    // role text must stay byte-exact anyway (no dedent from the mid-line
    // column, the renderer restores indentation only at line-start markers).
    {
      const std::string template_source =
          "namespace pcm::prompter {\n"
          "\n"
          "template <typename T> QByteArray BaseSchema() {\n"
          "  static const auto schema = Close(QByteArray());\n"
          "  return schema;\n"
          "}\n"
          "\n"
          "}  // namespace pcm::prompter\n";
      const auto template_project = tfe::code::ImportCpp("src/schema.cc", template_source);
      Expect(template_project.blocks.contains("src.schema.BaseSchema"),
             "missing single-line template function role");
      const auto template_rendered = tfe::code::RenderFiles(template_project);
      Expect(template_rendered.at("src/schema.cc") == template_source,
             "lossless render failed for single-line template function");
    }

    // A dedented role must not contain preprocessor directives at column 0:
    // dedent cannot move them, but the renderer re-indents every emitted line
    // to the marker column. Such methods stay inside the class role verbatim.
    {
      const std::string preproc_source =
          "#include \"widget.h\"\n"
          "\n"
          "namespace {\n"
          "\n"
          "class Widget final {\n"
          "public:\n"
          "  void go() {\n"
          "#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)\n"
          "    begin();\n"
          "#else\n"
          "    end();\n"
          "#endif\n"
          "  }\n"
          "};\n"
          "\n"
          "}  // namespace\n";
      const auto preproc_project = tfe::code::ImportCpp("src/widget.cc", preproc_source);
      Expect(preproc_project.blocks.contains("src.widget.Widget"),
             "missing Widget class role");
      Expect(!preproc_project.blocks.contains("src.widget.Widget.go"),
             "method with column-0 directives must not be extracted");
      const auto preproc_rendered = tfe::code::RenderFiles(preproc_project);
      Expect(preproc_rendered.at("src/widget.cc") == preproc_source,
             "lossless render failed for preproc-guarded method");
    }

    // The file role owns the whole byte range: a UTF-8 BOM and any trailing
    // bytes sit before/after the translation_unit node and must survive.
    {
      const std::string bom_source =
          "\xEF\xBB\xBF#pragma once\n"
          "\n"
          "namespace math {\n"
          "double scale(double value);\n"
          "}\n";
      const auto bom_project = tfe::code::ImportCpp("src/bom.hpp", bom_source);
      Expect(bom_project.blocks.at("src.bom_hpp.file").text == bom_source,
             "file role text lost the BOM prefix or trailing bytes");
      const auto bom_rendered = tfe::code::RenderFiles(bom_project);
      Expect(bom_rendered.at("src/bom.hpp") == bom_source,
             "lossless render failed for BOM-prefixed header");
    }

    // Raw string content keeps its verbatim column-0 lines; a dedented role
    // holding such lines would be re-indented by the renderer and shifted.
    // The method stays inside the class role verbatim instead.
    {
      const std::string raw_source =
          "#include \"widget.h\"\n"
          "\n"
          "class Widget final {\n"
          "public:\n"
          "  QString banner() const {\n"
          "    return QStringLiteral(R\"(Lorem ipsum dolor sit amet,\n"
          "consectetur adipiscing elit, sed do eiusmod.\n"
          ")\");\n"
          "  }\n"
          "};\n";
      const auto raw_project = tfe::code::ImportCpp("src/raw.cc", raw_source);
      Expect(raw_project.blocks.contains("src.raw.Widget"),
             "missing Widget class role");
      Expect(!raw_project.blocks.contains("src.raw.Widget.banner"),
             "method with verbatim column-0 lines must not be extracted");
      const auto raw_rendered = tfe::code::RenderFiles(raw_project);
      Expect(raw_rendered.at("src/raw.cc") == raw_source,
             "lossless render failed for raw-string method");
    }

    // 3. Syntax errors degrade to a single whole-file role instead of failing
    //    the import: tree-sitter cannot preprocess Qt macros, so macro-heavy
    //    but compiler-valid C++ must stay importable (and byte-exact).
    {
      const auto degraded = tfe::code::ImportCpp("broken.cc", "int broken_func( { return; }");
      Expect(degraded.blocks.size() == 1, "broken file should degrade to exactly one role");
      Expect(degraded.blocks.contains("broken.file"), "broken file missing .file role");
      Expect(degraded.blocks.at("broken.file").text == "int broken_func( { return; }",
             "broken file role text is not the whole source");
      Expect(degraded.blocks.at("broken.file").literal, "degraded file role should be a leaf");
    }
    {
      // Qt-style header: Q_OBJECT without a semicolon, nullary emit, macro
      // arguments that are types, and a missing final newline.
      const std::string qt_header =
          "#pragma once\n"
          "\n"
          "#include <QDialog>\n"
          "\n"
          "class AppLockDialog final : public QDialog {\n"
          "  Q_OBJECT\n"
          "\n"
          "public:\n"
          "  explicit AppLockDialog(QWidget *parent = nullptr);\n"
          "\n"
          "private slots:\n"
          "  void unlock() { emit unlockRequested(Q_ARG(int, 2)); }\n"
          "\n"
          "signals:\n"
          "  void unlockRequested(int retries);\n"
          "}";
      bool qt_threw = false;
      try {
        const auto qt = tfe::code::ImportCpp("src/app_lock_dialog.h", qt_header);
        Expect(qt.blocks.size() == 1, "Qt-macro header should degrade to exactly one role");
        Expect(qt.blocks.contains("src.app_lock_dialog_h.file"), "Qt-macro header missing .file role");
        Expect(qt.blocks.at("src.app_lock_dialog_h.file").text == qt_header,
               "degraded Qt header role text is not byte-exact");
        const auto qt_rendered = tfe::code::RenderFiles(qt);
        Expect(qt_rendered.at("src/app_lock_dialog.h") == qt_header,
               "lossless render failed for degraded Qt header");
      } catch (const std::exception&) {
        qt_threw = true;
      }
      Expect(!qt_threw, "Qt-macro header was rejected instead of degrading");
    }

    // 4. Full Workspace Integration: Import, Outline, Search, Context, Apply
    const fs::path root = fs::temp_directory_path() / "tfe-code-cpp-workspace-test";
    fs::remove_all(root);
    Write(root / "src/geometry.hpp", cpp_source);
    Write(root / "src/main.cc",
          "#include <iostream>\n"
          "#include \"geometry.hpp\"\n"
          "\n"
          "int main() {\n"
          "  math::geometry::Transform t(2.0);\n"
          "  const auto pt = t.Apply(math::geometry::Point{3.0, 4.0});\n"
          "  std::cout << pt.x << \", \" << pt.y << std::endl;\n"
          "  return 0;\n"
          "}\n");

    {
      tfe::code::Workspace workspace(root);
      const auto result = workspace.Import({"src"});
      Expect(result.changed.size() == 2, "import did not find 2 C++ files");
      Expect(workspace.Render().at("src/geometry.hpp") == Read(root / "src/geometry.hpp"),
             "workspace rendered geometry.hpp mismatch");
      Expect(workspace.Render().at("src/main.cc") == Read(root / "src/main.cc"),
             "workspace rendered main.cc mismatch");

      // Search
      const auto search_out = workspace.Search({"Transform"}, "");
      Expect(search_out.find("src.geometry_hpp.Transform") != std::string::npos,
             "search did not find Transform role: " + search_out);

      // Context
      const auto ctx_out = workspace.Context({"Apply"}, {"src/geometry.hpp"}, true);
      Expect(ctx_out.find("geometry_hpp.Transform.Apply") != std::string::npos,
             "context did not locate Apply role: " + ctx_out);
      Expect(ctx_out.find("main.main: const auto pt = t.Apply") != std::string::npos,
             "context callers missing: " + ctx_out);

      // Apply edit to C++ method body
      const auto proj_before = workspace.LoadProject();
      const auto apply_role = "src.geometry_hpp.Transform.Apply";
      const auto& node_before = proj_before.blocks.at(apply_role);

      tfe::code::RoleEdit edit;
      edit.role = apply_role;
      edit.expected_version = node_before.version;
      edit.old = "return Point{p.x * scale_, p.y * scale_};";
      edit.new_text = "return Point{p.x * scale_ + 1.0, p.y * scale_ + 1.0};";

      const auto apply_res = workspace.Apply({edit}, std::nullopt);
      Expect(!apply_res.changed.empty(), "apply did not record changed file");

      // Verify source file on disk was modified
      const auto on_disk = Read(root / "src/geometry.hpp");
      Expect(on_disk.find("p.x * scale_ + 1.0") != std::string::npos,
             "source file on disk does not contain edit: " + on_disk);

      // Verify lossless round-trip of edited C++ file
      Expect(workspace.Render().at("src/geometry.hpp") == on_disk,
             "rendered edited geometry.hpp does not match on-disk file");

      // Verify invalid C++ syntax edit is rejected and rolled back
      const auto proj_after = workspace.LoadProject();
      const auto& node_after = proj_after.blocks.at(apply_role);
      tfe::code::RoleEdit bad_edit;
      bad_edit.role = apply_role;
      bad_edit.expected_version = node_after.version;
      bad_edit.old = "return Point{p.x * scale_ + 1.0, p.y * scale_ + 1.0};";
      bad_edit.new_text = "this is totally broken C++ syntax (((; }}}";

      bool edit_rejected = false;
      try {
        workspace.Apply({bad_edit}, std::nullopt);
      } catch (const std::exception&) {
        edit_rejected = true;
      }
      Expect(edit_rejected, "syntax-breaking apply edit was not rejected");
      Expect(Read(root / "src/geometry.hpp") == on_disk,
             "rejected edit modified the source on disk");
    }

    fs::remove_all(root);
    std::cout << "cpp_import_core tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Test failed: " << e.what() << "\n";
    return 1;
  }
}
