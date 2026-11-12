// Core verification of TypeScript and JavaScript language support in TFEngine Code:
// parsing (.ts, .tsx, .js, .jsx), role extraction (classes, interfaces, functions,
// arrow functions, methods, exported declarations), byte-exact rendering,
// workspace import, search, context, and apply patching.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "render_project.h"
#include "ts_import.h"
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
    Expect(tfe::code::IsTsFile("src/index.ts"), "src/index.ts should be TS");
    Expect(tfe::code::IsTsFile("src/App.tsx"), "src/App.tsx should be TSX");
    Expect(tfe::code::IsTsFile("src/util.js"), "src/util.js should be JS");
    Expect(tfe::code::IsTsFile("src/View.jsx"), "src/View.jsx should be JSX");
    Expect(tfe::code::IsTsFile("src/esm.mjs"), "src/esm.mjs should be MJS");
    Expect(tfe::code::IsTsFile("src/cjs.cjs"), "src/cjs.cjs should be CJS");
    Expect(!tfe::code::IsTsFile("script.py"), "script.py is not TS/JS");
    Expect(!tfe::code::IsTsFile("main.cc"), "main.cc is not TS/JS");

    // 2. Direct ImportTs and lossless rendering of TypeScript (.ts)
    const std::string ts_source =
        "export interface Point {\n"
        "  x: number;\n"
        "  y: number;\n"
        "}\n"
        "\n"
        "export function clamp(value: number, low: number, high: number): number {\n"
        "  if (value < low) return low;\n"
        "  if (value > high) return high;\n"
        "  return value;\n"
        "}\n"
        "\n"
        "export const scalePoint = (p: Point, factor: number): Point => {\n"
        "  return { x: p.x * factor, y: p.y * factor };\n"
        "};\n"
        "\n"
        "export class Transform {\n"
        "  private scale: number;\n"
        "\n"
        "  constructor(scale: number) {\n"
        "    this.scale = scale;\n"
        "  }\n"
        "\n"
        "  apply(p: Point): Point {\n"
        "    return scalePoint(p, this.scale);\n"
        "  }\n"
        "}\n";

    const auto project = tfe::code::ImportTs("src/geometry.ts", ts_source);
    Expect(project.files.contains("src/geometry.ts"), "missing file record");
    Expect(project.blocks.contains("src.geometry.file"), "missing file role");
    Expect(project.blocks.contains("src.geometry.Point"), "missing Point interface role");
    Expect(project.blocks.contains("src.geometry.clamp"), "missing clamp function role");
    Expect(project.blocks.contains("src.geometry.scalePoint"), "missing scalePoint arrow function role");
    Expect(project.blocks.contains("src.geometry.Transform"), "missing Transform class role");
    Expect(project.blocks.contains("src.geometry.Transform.constructor"), "missing constructor role");
    Expect(project.blocks.contains("src.geometry.Transform.apply"), "missing Transform.apply method role");

    // Byte-exact rendering check for .ts
    const auto rendered_files = tfe::code::RenderFiles(project);
    Expect(rendered_files.at("src/geometry.ts") == ts_source,
           "lossless render failed for src/geometry.ts");

    // 3. Direct ImportTs and lossless rendering of TSX (.tsx)
    const std::string tsx_source =
        "import React from 'react';\n"
        "\n"
        "export interface BadgeProps {\n"
        "  label: string;\n"
        "}\n"
        "\n"
        "export const Badge = ({ label }: BadgeProps) => {\n"
        "  return <span className=\"badge\">{label}</span>;\n"
        "};\n";

    const auto tsx_project = tfe::code::ImportTs("src/Badge.tsx", tsx_source);
    Expect(tsx_project.blocks.contains("src.Badge_tsx.file"), "missing tsx file role");
    Expect(tsx_project.blocks.contains("src.Badge_tsx.BadgeProps"), "missing BadgeProps role");
    Expect(tsx_project.blocks.contains("src.Badge_tsx.Badge"), "missing Badge component role");
    Expect(tfe::code::RenderFiles(tsx_project).at("src/Badge.tsx") == tsx_source,
           "lossless render failed for src/Badge.tsx");

    // 4. Syntax errors degrade to a single whole-file role instead of failing
    //    the import: `.ts` collides with Qt Linguist translation files, so a
    //    workspace import must stay usable when they are present.
    {
      bool degraded = false;
      const auto linguist =
          tfe::code::ImportTs("translation/app_en.ts",
                              "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                              "<!DOCTYPE TS>\n"
                              "<TS version=\"2.1\">\n"
                              "<context><name>main</name></context>\n"
                              "</TS>\n",
                              &degraded);
      Expect(degraded, "Qt Linguist XML was not reported as degraded");
      Expect(linguist.blocks.size() == 1, "degraded .ts should hold exactly one role");
      Expect(linguist.blocks.at("translation.app_en.file").literal,
             "degraded .ts role should be a leaf");
      Expect(tfe::code::RenderFiles(linguist).at("translation/app_en.ts")
                 .starts_with("<?xml"),
             "degraded .ts did not render byte-exactly");
    }
    {
      bool degraded = false;
      const auto broken = tfe::code::ImportTs("broken.ts", "export function broken( { return 1; }",
                                              &degraded);
      Expect(degraded, "broken TypeScript was not reported as degraded");
      Expect(broken.blocks.at("broken.file").text == "export function broken( { return 1; }",
             "broken .ts role text is not the whole source");
    }

    // 4a. The file role owns the whole byte range: a UTF-8 BOM must survive.
    {
      const std::string bom_source =
          "\xEF\xBB\xBFexport const one = 1;\n";
      const auto bom_project = tfe::code::ImportTs("src/bom.ts", bom_source);
      Expect(bom_project.blocks.at("src.bom.file").text == bom_source,
             "file role text lost the BOM prefix");
      Expect(tfe::code::RenderFiles(bom_project).at("src/bom.ts") == bom_source,
             "lossless render failed for BOM-prefixed .ts");
    }

    // 4b. A dedented role must not contain verbatim column-0 lines: template
    // literal content keeps its raw column, and the renderer would re-indent
    // it to the marker column. Such methods stay inside the class verbatim.
    {
      const std::string raw_source =
          "export class Banner {\n"
          "  text(): string {\n"
          "    return `Lorem ipsum dolor sit amet,\n"
          "consectetur adipiscing elit, sed do eiusmod.`;\n"
          "  }\n"
          "}\n";
      const auto raw_project = tfe::code::ImportTs("src/banner.ts", raw_source);
      Expect(raw_project.blocks.contains("src.banner.Banner"),
             "missing Banner class role");
      Expect(!raw_project.blocks.contains("src.banner.Banner.text"),
             "method with verbatim column-0 lines must not be extracted");
      Expect(tfe::code::RenderFiles(raw_project).at("src/banner.ts") == raw_source,
             "lossless render failed for template literal method");
    }

    // 5. Full Workspace Integration: Import, Outline, Search, Context, Apply
    const fs::path root = fs::temp_directory_path() / "tfe-code-ts-workspace-test";
    fs::remove_all(root);
    Write(root / "src/geometry.ts", ts_source);
    Write(root / "src/Badge.tsx", tsx_source);
    Write(root / "src/helper.js",
          "import { Transform } from './geometry';\n"
          "\n"
          "export function runDemo() {\n"
          "  const t = new Transform(2);\n"
          "  return t.apply({ x: 3, y: 4 });\n"
          "}\n");

    {
      tfe::code::Workspace workspace(root);
      const auto result = workspace.Import({"src"});
      Expect(result.changed.size() == 3, "import did not find 3 TS/TSX/JS files");
      Expect(workspace.Render().at("src/geometry.ts") == ts_source,
             "workspace rendered geometry.ts mismatch");
      Expect(workspace.Render().at("src/Badge.tsx") == tsx_source,
             "workspace rendered Badge.tsx mismatch");

      // Search
      const auto search_out = workspace.Search({"scalePoint"}, "");
      Expect(search_out.find("src.geometry.scalePoint") != std::string::npos,
             "search did not find scalePoint role: " + search_out);

      // Context with callers across files
      const auto ctx_out = workspace.Context({"apply"}, {"src/geometry.ts"}, true);
      Expect(ctx_out.find("geometry.Transform.apply") != std::string::npos,
             "context did not locate apply role: " + ctx_out);
      Expect(ctx_out.find("helper.runDemo: return t.apply") != std::string::npos,
             "context callers missing for apply: " + ctx_out);

      // Apply edit to TypeScript method body
      const auto proj_before = workspace.LoadProject();
      const auto apply_role = "src.geometry.Transform.apply";
      const auto& node_before = proj_before.blocks.at(apply_role);

      tfe::code::RoleEdit edit;
      edit.role = apply_role;
      edit.expected_version = node_before.version;
      edit.old = "return scalePoint(p, this.scale);";
      edit.new_text = "return scalePoint(p, this.scale * 2);";

      const auto apply_res = workspace.Apply({edit}, std::nullopt);
      Expect(!apply_res.changed.empty(), "apply did not record changed file");

      const auto on_disk = Read(root / "src/geometry.ts");
      Expect(on_disk.find("this.scale * 2") != std::string::npos,
             "source file on disk does not contain edit: " + on_disk);
      Expect(workspace.Render().at("src/geometry.ts") == on_disk,
             "rendered edited geometry.ts does not match on-disk file");

      // Verify invalid TypeScript syntax edit is rejected and rolled back
      const auto proj_after = workspace.LoadProject();
      const auto& node_after = proj_after.blocks.at(apply_role);
      tfe::code::RoleEdit bad_edit;
      bad_edit.role = apply_role;
      bad_edit.expected_version = node_after.version;
      bad_edit.old = "return scalePoint(p, this.scale * 2);";
      bad_edit.new_text = "return scalePoint(((broken syntax;;;";

      bool edit_rejected = false;
      try {
        workspace.Apply({bad_edit}, std::nullopt);
      } catch (const std::exception&) {
        edit_rejected = true;
      }
      Expect(edit_rejected, "syntax-breaking TS apply edit was not rejected");
      Expect(Read(root / "src/geometry.ts") == on_disk,
             "rejected edit modified the TS source on disk");
    }

    fs::remove_all(root);
    std::cout << "ts_import_core tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Test failed: " << e.what() << "\n";
    return 1;
  }
}
