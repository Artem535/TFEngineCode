#include <iostream>
#include <stdexcept>
#include <string>

#include <rfl/json.hpp>

#include "hash.h"
#include "python_import.h"
#include "render_project.h"

namespace {

void Expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() {
  try {
    Expect(tfe::code::Sha256("abc") ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "SHA-256 implementation mismatch");
    const std::string source =
        "# coding: utf-8\r\n"
        "import functools\r\n"
        "\r\n"
        "@functools.cache\r\n"
        "async def greet(name: str) -> str:\r\n"
        "    \"\"\"Привет, многострочник.\"\"\"\r\n"
        "    def inner():\r\n"
        "        return f\"Привет, {name}!\"\r\n"
        "    return await inner()\r\n";
    const auto project = tfe::code::ImportPython("pkg/sample.py", source);
    Expect(project.blocks.contains("pkg.sample.file"), "missing file role");
    Expect(project.blocks.contains("pkg.sample.greet"), "missing function role");
    Expect(project.blocks.contains("pkg.sample.greet.inner"), "missing nested role");
    Expect(project.blocks.at("pkg.sample.file").source_start_line == 1 &&
           project.blocks.at("pkg.sample.file").source_end_line == 9 &&
           project.blocks.at("pkg.sample.greet").source_start_line == 4 &&
           project.blocks.at("pkg.sample.greet").source_end_line == 9 &&
           project.blocks.at("pkg.sample.greet.inner").source_start_line == 7 &&
           project.blocks.at("pkg.sample.greet.inner").source_end_line == 8,
           "decorated/nested CRLF source ranges are incorrect");
    const auto rendered = tfe::code::RenderFiles(project).at("pkg/sample.py");
    if (rendered != source) {
      const size_t at = std::min(rendered.size(), source.size());
      size_t mismatch = 0;
      while (mismatch < at && rendered[mismatch] == source[mismatch]) ++mismatch;
      std::cerr << "sizes " << source.size() << "/" << rendered.size()
                << " mismatch " << mismatch << " bytes "
                << (mismatch < source.size() ? int(source[mismatch]) : -1) << "/"
                << (mismatch < rendered.size() ? int(rendered[mismatch]) : -1) << "\n";
    }
    Expect(rendered == source, "import/render did not preserve bytes");
    const auto again = tfe::code::ImportPython("pkg/sample.py", source);
    Expect(again.blocks == project.blocks, "role ids or versions are unstable");
    bool rejected = false;
    try {
      (void)tfe::code::ImportPython("bad.py", "def broken(:\n");
    } catch (const std::exception&) {
      rejected = true;
    }
    Expect(rejected, "invalid Python was accepted");
    const std::string classes =
        "class Example:\n"
        "    def method(self):\n"
        "        if self.ready:\n\n"
        "            return 'без final newline'\n"
        "        return None";
    const auto class_project = tfe::code::ImportPython("pkg/classes.py", classes);
    Expect(class_project.blocks.contains("pkg.classes.Example.method"), "missing method role");
    Expect(tfe::code::RenderFiles(class_project).at("pkg/classes.py") == classes,
           "class/blank line/no-final-newline round trip");
    Expect(class_project.blocks.at("pkg.classes.file").source_start_line == 1 &&
           class_project.blocks.at("pkg.classes.file").source_end_line == 6 &&
           class_project.blocks.at("pkg.classes.Example").source_start_line == 1 &&
           class_project.blocks.at("pkg.classes.Example").source_end_line == 6 &&
           class_project.blocks.at("pkg.classes.Example.method").source_start_line == 2 &&
           class_project.blocks.at("pkg.classes.Example.method").source_end_line == 6,
           "class/method no-final-newline ranges are incorrect");
    const auto empty_project = tfe::code::ImportPython("empty.py", "");
    Expect(empty_project.blocks.at("empty.file").source_start_line == 0 &&
           empty_project.blocks.at("empty.file").source_end_line == 0,
           "empty source should have an unavailable range");
    const auto legacy = rfl::json::read<tfe::code::Node, rfl::DefaultIfMissing>(
        R"({"version":"v","text":"x = 1","literal":true,"choice":null})");
    if (!legacy) std::cerr << "legacy JSON error: " << legacy.error().what() << '\n';
    Expect(legacy && legacy.value().source_start_line == 0 &&
           legacy.value().source_end_line == 0,
           "legacy node JSON did not receive zero range defaults");
    std::cout << "python_import_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
