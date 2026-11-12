#include "ts_import.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <tree_sitter/api.h>

#include "hash.h"

extern "C" const TSLanguage* tree_sitter_typescript(void);
extern "C" const TSLanguage* tree_sitter_tsx(void);

namespace tfe::code {
namespace {

using Parser = std::unique_ptr<TSParser, decltype(&ts_parser_delete)>;
using Tree = std::unique_ptr<TSTree, decltype(&ts_tree_delete)>;

struct Child {
  std::string id;
  size_t start;
  size_t end;
};

bool UsesTsxGrammar(const std::string& path) {
  return path.ends_with(".tsx") || path.ends_with(".jsx");
}

const TSLanguage* SelectLanguage(const std::string& path) {
  return UsesTsxGrammar(path) ? tree_sitter_tsx() : tree_sitter_typescript();
}

std::string TsModuleId(const std::string& path) {
  std::string id = path;
  while (id.starts_with("./")) id = id.substr(2);
  if (id.ends_with(".d.ts")) id = id.substr(0, id.size() - 5) + "_d_ts";
  else if (id.ends_with(".tsx")) id = id.substr(0, id.size() - 4) + "_tsx";
  else if (id.ends_with(".jsx")) id = id.substr(0, id.size() - 4) + "_jsx";
  else if (id.ends_with(".mjs")) id = id.substr(0, id.size() - 4) + "_mjs";
  else if (id.ends_with(".cjs")) id = id.substr(0, id.size() - 4) + "_cjs";
  else if (id.ends_with(".ts")) id = id.substr(0, id.size() - 3);
  else if (id.ends_with(".js")) id = id.substr(0, id.size() - 3);
  for (char& c : id) {
    if (c == '/') c = '.';
    else if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_') c = '_';
  }
  return id.empty() ? "module" : id;
}

bool HasSyntaxError(TSNode node) {
  if (ts_node_is_error(node) || ts_node_is_missing(node)) return true;
  const uint32_t count = ts_node_child_count(node);
  for (uint32_t i = 0; i < count; ++i) {
    if (HasSyntaxError(ts_node_child(node, i))) return true;
  }
  return false;
}

class Importer {
 public:
  Importer(std::string path, const std::string& source)
      : path_(std::move(path)), source_(source), parser_(ts_parser_new(), ts_parser_delete) {
    if (!parser_ || !ts_parser_set_language(parser_.get(), SelectLanguage(path_))) {
      throw std::runtime_error("Cannot initialize TypeScript/JavaScript parser");
    }
    tree_.reset(ts_parser_parse_string(parser_.get(), nullptr, source_.data(),
                                       static_cast<uint32_t>(source_.size())));
    if (!tree_) throw std::runtime_error("Cannot parse TypeScript/JavaScript source");
    degraded_ = HasSyntaxError(ts_tree_root_node(tree_.get()));
  }

  Project Run() {
    if (degraded_) {
      // The parse has ERROR or MISSING nodes. Qt Linguist writes XML
      // translation files with a `.ts` extension, so workspace imports must
      // keep such files usable as one whole-file role holding the exact bytes.
      const std::string base = TsModuleId(path_);
      const std::string id = base + ".file";
      project_.blocks[id] =
          Node{Sha256(source_), source_, true, std::nullopt, 0, 0};
      project_.files[path_] = File{id, {}};
      return std::move(project_);
    }
    const std::string base = TsModuleId(path_);
    AddContainer(ts_tree_root_node(tree_.get()), base + ".file", base, true);
    project_.files[path_] = File{base + ".file", {}};
    return std::move(project_);
  }

  bool degraded() const { return degraded_; }

 private:
  std::string Substr(TSNode node) const {
    const size_t start = ts_node_start_byte(node);
    const size_t end = ts_node_end_byte(node);
    return source_.substr(start, end - start);
  }

  std::string SanitizeRolePart(std::string name) const {
    for (char& c : name) {
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') {
        c = '_';
      }
    }
    return name.empty() ? "unnamed" : name;
  }

  std::string ExtractName(TSNode node) const {
    const TSNode name = ts_node_child_by_field_name(node, "name", 4);
    if (!ts_node_is_null(name)) {
      return SanitizeRolePart(Substr(name));
    }
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
      const TSNode child = ts_node_named_child(node, i);
      const std::string_view ctype = ts_node_type(child);
      if (ctype == "identifier" || ctype == "type_identifier" || ctype == "property_identifier" ||
          ctype == "private_property_identifier") {
        return SanitizeRolePart(Substr(child));
      }
    }
    return "anonymous";
  }

  // Checks if a lexical_declaration or variable_declaration defines a single named
  // arrow_function or function_expression (e.g. `const Foo = (x: number) => { ... };`).
  std::string ExtractFunctionVariableName(TSNode decl_node, TSNode* out_fn_node) const {
    const std::string_view type = ts_node_type(decl_node);
    if (type != "lexical_declaration" && type != "variable_declaration") return "";
    TSNode declarator{};
    size_t declarator_count = 0;
    const uint32_t count = ts_node_named_child_count(decl_node);
    for (uint32_t i = 0; i < count; ++i) {
      const TSNode child = ts_node_named_child(decl_node, i);
      if (ts_node_type(child) == std::string_view("variable_declarator")) {
        declarator = child;
        ++declarator_count;
      }
    }
    if (declarator_count != 1) return "";
    const TSNode name_node = ts_node_child_by_field_name(declarator, "name", 4);
    const TSNode value_node = ts_node_child_by_field_name(declarator, "value", 5);
    if (ts_node_is_null(name_node) || ts_node_is_null(value_node)) return "";
    const std::string_view name_type = ts_node_type(name_node);
    if (name_type != "identifier") return "";
    const std::string_view val_type = ts_node_type(value_node);
    if (val_type != "arrow_function" && val_type != "function_expression" &&
        val_type != "function" && val_type != "generator_function") {
      return "";
    }
    if (out_fn_node) *out_fn_node = value_node;
    return SanitizeRolePart(Substr(name_node));
  }

  std::string Unique(std::string id) {
    if (!project_.blocks.contains(id)) return id;
    for (size_t suffix = 2;; ++suffix) {
      const std::string candidate = id + "_" + std::to_string(suffix);
      if (!project_.blocks.contains(candidate)) return candidate;
    }
  }

  // A role that would be stored dedented must own every non-blank line in its
  // range up to its indentation: dedent removes that many leading spaces and
  // the renderer re-indents each emitted line to the marker column, so a line
  // with less leading whitespace (verbatim template literal content, for
  // example) would shift and break byte identity. The parent keeps such a
  // child verbatim, and because this checks the child's whole byte range, any
  // level that cannot be dedented safely is skipped before its own children
  // are considered.
  bool UnsafeDedent(TSNode node) const {
    size_t line_start = ts_node_start_byte(node);
    while (line_start > 0 && source_[line_start - 1] != '\n') --line_start;
    const size_t first_line = line_start;
    size_t indentation = 0;
    while (line_start < source_.size() &&
           (source_[line_start] == ' ' || source_[line_start] == '\t')) {
      ++indentation;
      ++line_start;
    }
    if (indentation == 0) return false;
    const size_t end = ts_node_end_byte(node);
    size_t i = first_line;
    while (i < end) {
      size_t whitespace = 0;
      size_t j = i;
      while (j < end && (source_[j] == ' ' || source_[j] == '\t')) {
        ++whitespace;
        ++j;
      }
      if (j < end && source_[j] != '\n' && whitespace < indentation) return true;
      const size_t newline = source_.find('\n', j);
      if (newline == std::string::npos || newline >= end) break;
      i = newline + 1;
    }
    return false;
  }

  TSNode UnwrapExport(TSNode node) const {
    if (ts_node_type(node) != std::string_view("export_statement")) return node;
    const TSNode decl = ts_node_child_by_field_name(node, "declaration", 11);
    if (!ts_node_is_null(decl)) return decl;
    const TSNode val = ts_node_child_by_field_name(node, "value", 5);
    if (!ts_node_is_null(val)) return val;
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
      const TSNode child = ts_node_named_child(node, i);
      const std::string_view ctype = ts_node_type(child);
      if (ctype != "decorator" && ctype != "export_clause" && ctype != "string") {
        return child;
      }
    }
    return node;
  }

  std::vector<Child> AddChildren(TSNode node, const std::string& prefix) {
    std::vector<Child> children;
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
      const TSNode child = ts_node_named_child(node, i);
      const TSNode inner = UnwrapExport(child);
      const std::string_view inner_type = ts_node_type(inner);

      // TypeScript namespace / module declaration: flatten children into prefix
      if (inner_type == "internal_module" || inner_type == "module") {
        const TSNode body = ts_node_child_by_field_name(inner, "body", 4);
        if (!ts_node_is_null(body)) {
          const auto mod_children = AddChildren(body, prefix);
          children.insert(children.end(), mod_children.begin(), mod_children.end());
        }
        continue;
      }

      if (inner_type == "class_declaration" || inner_type == "abstract_class_declaration" ||
          inner_type == "interface_declaration" || inner_type == "function_declaration" ||
          inner_type == "generator_function_declaration" || inner_type == "method_definition") {
        if (UnsafeDedent(child)) continue;
        const std::string name = ExtractName(inner);
        if (name != "anonymous") {
          const std::string id = Unique(prefix + "." + name);
          AddContainer(child, id, id);
          children.push_back({id, ts_node_start_byte(child), ts_node_end_byte(child)});
          continue;
        }
      }

      // Check for arrow function or function expression in lexical/variable declaration
      TSNode fn_node{};
      const std::string fn_var_name = ExtractFunctionVariableName(inner, &fn_node);
      if (!fn_var_name.empty()) {
        if (UnsafeDedent(child)) continue;
        const std::string id = Unique(prefix + "." + fn_var_name);
        AddContainer(child, id, id);
        children.push_back({id, ts_node_start_byte(child), ts_node_end_byte(child)});
        continue;
      }
    }
    return children;
  }

  void AddContainer(TSNode node, const std::string& id, const std::string& prefix,
                    bool whole_file = false) {
    TSNode contents = UnwrapExport(node);
    const std::string_view type = ts_node_type(contents);

    if (type == "class_declaration" || type == "abstract_class_declaration" ||
        type == "interface_declaration" || type == "function_declaration" ||
        type == "generator_function_declaration" || type == "method_definition") {
      const TSNode body = ts_node_child_by_field_name(contents, "body", 4);
      if (!ts_node_is_null(body)) contents = body;
    } else if (type == "lexical_declaration" || type == "variable_declaration") {
      TSNode fn_node{};
      if (!ExtractFunctionVariableName(contents, &fn_node).empty()) {
        const TSNode body = ts_node_child_by_field_name(fn_node, "body", 4);
        if (!ts_node_is_null(body)) contents = body;
      }
    }

    const std::string_view contents_type = ts_node_type(contents);
    std::vector<Child> children;
    if (contents_type == "program" || contents_type == "class_body" ||
        contents_type == "interface_body" || contents_type == "object_type" ||
        contents_type == "statement_block") {
      children = AddChildren(contents, prefix);
    }

    const size_t start = whole_file ? 0 : ts_node_start_byte(node);
    const size_t end = whole_file ? source_.size() : ts_node_end_byte(node);
    std::string text;
    size_t cursor = start;
    for (const auto& child : children) {
      if (child.start < cursor || child.end > end) {
        throw std::runtime_error("Overlapping TypeScript/JavaScript roles");
      }
      text.append(source_, cursor, child.start - cursor);
      text += "{{" + child.id + "}}";
      cursor = child.end;
    }
    text.append(source_, cursor, end - cursor);

    // Dedent by the indentation of the line the role starts on. Using the
    // node's start column instead would misfire when the role begins mid-line
    // (an unwrapped `const f = () => { ... };`, for example): the renderer
    // restores indentation for line-start markers only, so a mid-line column
    // would strip the body indentation and break byte identity.
    size_t indentation = 0;
    {
      size_t line_start = ts_node_start_byte(node);
      while (line_start > 0 && source_[line_start - 1] != '\n') --line_start;
      while (line_start < source_.size() &&
             (source_[line_start] == ' ' || source_[line_start] == '\t')) {
        ++indentation;
        ++line_start;
      }
    }
    if (indentation > 0) {
      std::string relative;
      relative.reserve(text.size());
      for (size_t i = 0; i < text.size(); ++i) {
        relative += text[i];
        if (text[i] != '\n' || i + 1 == text.size()) continue;
        size_t next = i + 1;
        size_t removed = 0;
        while (next < text.size() && removed < indentation &&
               (text[next] == ' ' || text[next] == '\t')) {
          ++next;
          ++removed;
        }
        i = next - 1;
      }
      text = std::move(relative);
    }
    const auto start_point = ts_node_start_point(node);
    const auto end_point = ts_node_end_point(node);
    size_t start_line = start_point.row + 1;
    size_t end_line = end_point.column > 0 ? end_point.row + 1 : end_point.row;
    if (whole_file && !source_.empty()) {
      start_line = 1;
      end_line = static_cast<size_t>(std::count(source_.begin(), source_.end(), '\n')) +
                 (source_.back() == '\n' ? 0 : 1);
    }
    if (start == end) start_line = end_line = 0;
    project_.blocks[id] = Node{Sha256(text), std::move(text), children.empty(), std::nullopt,
                               start_line, end_line};
  }

  std::string path_;
  const std::string& source_;
  bool degraded_ = false;
  Parser parser_;
  Tree tree_{nullptr, ts_tree_delete};
  Project project_;
};

}  // namespace

bool IsTsFile(const std::string& path) {
  for (const std::string_view ext : {".ts", ".tsx", ".js", ".jsx", ".mjs", ".cjs"}) {
    if (path.ends_with(ext)) return true;
  }
  return false;
}

Project ImportTs(const std::string& path, const std::string& source, bool* degraded) {
  if (path.empty() || !IsTsFile(path) || path.starts_with('/') ||
      path.find('\\') != std::string::npos || path.find("../") != std::string::npos ||
      path == ".." || path.starts_with(".tfe-code/")) {
    throw std::runtime_error("TypeScript/JavaScript import path must be a safe relative path");
  }
  Importer importer(path, source);
  if (degraded != nullptr) *degraded = importer.degraded();
  return importer.Run();
}

bool TsHasSyntaxError(const std::string& path, const std::string& source) {
  Parser parser(ts_parser_new(), ts_parser_delete);
  if (!parser || !ts_parser_set_language(parser.get(), SelectLanguage(path))) {
    throw std::runtime_error("Cannot initialize TypeScript/JavaScript parser");
  }
  Tree tree(ts_parser_parse_string(parser.get(), nullptr, source.data(),
                                   static_cast<uint32_t>(source.size())),
            ts_tree_delete);
  return !tree || HasSyntaxError(ts_tree_root_node(tree.get()));
}

void ValidateTs(const std::string& path, const std::string& source) {
  Parser parser(ts_parser_new(), ts_parser_delete);
  if (!parser || !ts_parser_set_language(parser.get(), SelectLanguage(path))) {
    throw std::runtime_error("Cannot initialize TypeScript/JavaScript parser");
  }
  Tree tree(ts_parser_parse_string(parser.get(), nullptr, source.data(),
                                   static_cast<uint32_t>(source.size())),
            ts_tree_delete);
  if (!tree || HasSyntaxError(ts_tree_root_node(tree.get()))) {
    throw std::runtime_error("Candidate is not valid TypeScript/JavaScript");
  }
}

}  // namespace tfe::code
