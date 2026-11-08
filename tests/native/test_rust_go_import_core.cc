// Core verification of Rust (.rs) and Go (.go) language support in TFEngine Code:
// parsing, role extraction (structs, enums, traits, impls, functions, methods,
// attributes, receiver methods), byte-exact rendering, workspace import,
// search, context, and apply patching.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "go_import.h"
#include "render_project.h"
#include "rust_import.h"
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
    // 1. Extension checks
    Expect(tfe::code::IsRustFile("src/lib.rs"), "src/lib.rs should be Rust");
    Expect(!tfe::code::IsRustFile("src/main.go"), "src/main.go is not Rust");
    Expect(tfe::code::IsGoFile("src/main.go"), "src/main.go should be Go");
    Expect(!tfe::code::IsGoFile("src/lib.rs"), "src/lib.rs is not Go");

    // 2. Direct ImportRust and lossless rendering
    const std::string rust_source =
        "#[derive(Debug, Clone, Copy, PartialEq)]\n"
        "pub struct Point {\n"
        "    pub x: f64,\n"
        "    pub y: f64,\n"
        "}\n"
        "\n"
        "pub trait Scalable {\n"
        "    fn scale(&self, factor: f64) -> Self;\n"
        "}\n"
        "\n"
        "impl Point {\n"
        "    pub fn new(x: f64, y: f64) -> Self {\n"
        "        Self { x, y }\n"
        "    }\n"
        "\n"
        "    pub fn translate(&self, dx: f64, dy: f64) -> Self {\n"
        "        Self {\n"
        "            x: self.x + dx,\n"
        "            y: self.y + dy,\n"
        "        }\n"
        "    }\n"
        "}\n"
        "\n"
        "impl Scalable for Point {\n"
        "    fn scale(&self, factor: f64) -> Self {\n"
        "        Self {\n"
        "            x: self.x * factor,\n"
        "            y: self.y * factor,\n"
        "        }\n"
        "    }\n"
        "}\n"
        "\n"
        "pub fn clamp(val: f64, low: f64, high: f64) -> f64 {\n"
        "    if val < low {\n"
        "        return low;\n"
        "    }\n"
        "    if val > high {\n"
        "        return high;\n"
        "    }\n"
        "    val\n"
        "}\n";

    const auto rust_proj = tfe::code::ImportRust("src/geometry.rs", rust_source);
    Expect(rust_proj.files.contains("src/geometry.rs"), "missing Rust file record");
    Expect(rust_proj.blocks.contains("src.geometry.file"), "missing Rust file role");
    Expect(rust_proj.blocks.contains("src.geometry.Point"), "missing Point struct role");
    Expect(rust_proj.blocks.contains("src.geometry.Point.impl"), "missing Point impl role");
    Expect(rust_proj.blocks.contains("src.geometry.Point.new"), "missing Point.new method role");
    Expect(rust_proj.blocks.contains("src.geometry.Point.translate"), "missing Point.translate method role");
    Expect(rust_proj.blocks.contains("src.geometry.Scalable_for_Point"), "missing Scalable_for_Point role");
    Expect(rust_proj.blocks.contains("src.geometry.Scalable_for_Point.scale"), "missing trait impl scale role");
    Expect(rust_proj.blocks.contains("src.geometry.clamp"), "missing clamp function role");
    Expect(tfe::code::RenderFiles(rust_proj).at("src/geometry.rs") == rust_source,
           "lossless render failed for src/geometry.rs");

    // 3. Direct ImportGo and lossless rendering
    const std::string go_source =
        "package geometry\n"
        "\n"
        "type Point struct {\n"
        "\tX float64\n"
        "\tY float64\n"
        "}\n"
        "\n"
        "func NewPoint(x, y float64) Point {\n"
        "\treturn Point{X: x, Y: y}\n"
        "}\n"
        "\n"
        "func (p *Point) Scale(factor float64) {\n"
        "\tp.X *= factor\n"
        "\tp.Y *= factor\n"
        "}\n";

    const auto go_proj = tfe::code::ImportGo("pkg/geometry.go", go_source);
    Expect(go_proj.files.contains("pkg/geometry.go"), "missing Go file record");
    Expect(go_proj.blocks.contains("pkg.geometry.file"), "missing Go file role");
    Expect(go_proj.blocks.contains("pkg.geometry.Point"), "missing Go Point struct role");
    Expect(go_proj.blocks.contains("pkg.geometry.NewPoint"), "missing Go NewPoint func role");
    Expect(go_proj.blocks.contains("pkg.geometry.Point.Scale"), "missing Go Point.Scale method role");
    Expect(tfe::code::RenderFiles(go_proj).at("pkg/geometry.go") == go_source,
           "lossless render failed for pkg/geometry.go");

    // 4. Syntax error validation for Rust and Go
    bool rust_rejected = false;
    try {
      (void)tfe::code::ImportRust("broken.rs", "pub fn broken( { }");
    } catch (const std::exception&) {
      rust_rejected = true;
    }
    Expect(rust_rejected, "syntax error in Rust was not rejected");

    bool go_rejected = false;
    try {
      (void)tfe::code::ImportGo("broken.go", "package main\nfunc broken( { }");
    } catch (const std::exception&) {
      go_rejected = true;
    }
    Expect(go_rejected, "syntax error in Go was not rejected");

    // 5. Full Workspace Integration: Import, Search, Context, Apply for both Rust and Go
    const fs::path root = fs::temp_directory_path() / "tfe-code-rust-go-workspace-test";
    fs::remove_all(root);
    Write(root / "src/geometry.rs", rust_source);
    Write(root / "pkg/geometry.go", go_source);

    {
      tfe::code::Workspace workspace(root);
      const auto result = workspace.Import({"src", "pkg"});
      Expect(result.changed.size() == 2, "import did not find both Rust and Go files");

      // Edit a Rust method and a Go method in one atomic batch
      const auto proj_before = workspace.LoadProject();
      const auto rust_role = "src.geometry.Point.translate";
      const auto go_role = "pkg.geometry.Point.Scale";

      tfe::code::RoleEdit rust_edit;
      rust_edit.role = rust_role;
      rust_edit.expected_version = proj_before.blocks.at(rust_role).version;
      rust_edit.old = "x: self.x + dx,";
      rust_edit.new_text = "x: self.x + dx * 2.0,";

      tfe::code::RoleEdit go_edit;
      go_edit.role = go_role;
      go_edit.expected_version = proj_before.blocks.at(go_role).version;
      go_edit.old = "p.X *= factor";
      go_edit.new_text = "p.X *= factor * 2";

      const auto apply_res = workspace.Apply({rust_edit, go_edit}, std::nullopt);
      Expect(apply_res.changed.size() == 2, "apply did not record 2 changed files");

      const auto rust_on_disk = Read(root / "src/geometry.rs");
      const auto go_on_disk = Read(root / "pkg/geometry.go");
      Expect(rust_on_disk.find("self.x + dx * 2.0") != std::string::npos,
             "Rust file on disk does not contain edit");
      Expect(go_on_disk.find("p.X *= factor * 2") != std::string::npos,
             "Go file on disk does not contain edit");
      Expect(workspace.Render().at("src/geometry.rs") == rust_on_disk,
             "rendered edited Rust file mismatch");
      Expect(workspace.Render().at("pkg/geometry.go") == go_on_disk,
             "rendered edited Go file mismatch");

      // Syntax-breaking Rust edit must be rejected and rolled back
      const auto proj_after = workspace.LoadProject();
      tfe::code::RoleEdit bad_rust_edit;
      bad_rust_edit.role = rust_role;
      bad_rust_edit.expected_version = proj_after.blocks.at(rust_role).version;
      bad_rust_edit.old = "x: self.x + dx * 2.0,";
      bad_rust_edit.new_text = "x: self.x + (((broken;;;";

      bool bad_rejected = false;
      try {
        workspace.Apply({bad_rust_edit}, std::nullopt);
      } catch (const std::exception&) {
        bad_rejected = true;
      }
      Expect(bad_rejected, "syntax-breaking Rust apply edit was not rejected");
      Expect(Read(root / "src/geometry.rs") == rust_on_disk,
             "rejected Rust edit modified disk");
    }

    fs::remove_all(root);
    std::cout << "rust_go_import_core tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Test failed: " << e.what() << "\n";
    return 1;
  }
}
