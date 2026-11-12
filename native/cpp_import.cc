#include "cpp_import.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <tree_sitter/api.h>

#include "hash.h"

extern "C" const TSLanguage* tree_sitter_cpp(void);

namespace tfe::code {
namespace {

using Parser = std::unique_ptr<TSParser, decltype(&ts_parser_delete)>;
using Tree = std::unique_ptr<TSTree, decltype(&ts_tree_delete)>;

struct Child {
  std::string id;
  size_t start;
  size_t end;
};

std::string CppModuleId(const std::string& path) {
  std::string id = path;
  while (id.starts_with("./")) id = id.substr(2);
  if (id.ends_with(".cpp")) id = id.substr(0, id.size() - 4);
  else if (id.ends_with(".cc")) id = id.substr(0, id.size() - 3);
  else if (id.ends_with(".cxx")) id = id.substr(0, id.size() - 4);
  else if (id.ends_with(".c")) id = id.substr(0, id.size() - 2);
  else if (id.ends_with(".hpp")) id = id.substr(0, id.size() - 4) + "_hpp";
  else if (id.ends_with(".hxx")) id = id.substr(0, id.size() - 4) + "_hxx";
  else if (id.ends_with(".hh")) id = id.substr(0, id.size() - 3) + "_hh";
  else if (id.ends_with(".h")) id = id.substr(0, id.size() - 2) + "_h";
  for (char& c : id) {
    if (c == '/') c = '.';
    else if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_') c = '_';
  }
  return id.empty() ? "module" : id;
}

bool HasSyntaxError(TSNode node) {
  if (ts_node_is_error(node)) return true;
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
    if (!parser_ || !ts_parser_set_language(parser_.get(), tree_sitter_cpp())) {
      throw std::runtime_error("Cannot initialize C++ parser");
    }
    tree_.reset(ts_parser_parse_string(parser_.get(), nullptr, source_.data(),
                                       static_cast<uint32_t>(source_.size())));
    if (!tree_) throw std::runtime_error("Cannot parse C++ source");
    degraded_ = HasSyntaxError(ts_tree_root_node(tree_.get()));
  }

  Project Run() {
    if (degraded_) {
      // The parse has ERROR nodes. Real Qt code hits this without any real
      // syntax fault (Q_OBJECT, emit, Q_ARG), so instead of rejecting the file
      // keep it importable as one whole-file role holding the exact bytes.
      // This is the same shape a valid childless file already produces, so the
      // renderer reproduces the file byte for byte.
      const std::string base = CppModuleId(path_);
      const std::string id = base + ".file";
      const auto start_point = ts_node_start_point(ts_tree_root_node(tree_.get()));
      const auto end_point = ts_node_end_point(ts_tree_root_node(tree_.get()));
      size_t start_line = start_point.row + 1;
      size_t end_line = end_point.column > 0 ? end_point.row + 1 : end_point.row;
      if (source_.empty()) start_line = end_line = 0;
      project_.blocks[id] =
          Node{Sha256(source_), source_, true, std::nullopt, start_line, end_line};
      project_.files[path_] = File{id, {}};
      return std::move(project_);
    }
    const std::string base = CppModuleId(path_);
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

  std::string ExtractDeclaratorName(TSNode node) const {
    const std::string_view type = ts_node_type(node);
    if (type == "identifier" || type == "type_identifier" || type == "field_identifier" ||
        type == "destructor_name" || type == "operator_name") {
      return Substr(node);
    }
    if (type == "qualified_identifier") {
      std::string s = Substr(node);
      for (size_t pos = 0; (pos = s.find("::", pos)) != std::string::npos;) {
        s.replace(pos, 2, ".");
        pos += 1;
      }
      return s;
    }
    if (type == "function_declarator" || type == "pointer_declarator" ||
        type == "reference_declarator" || type == "parenthesized_declarator") {
      const TSNode inner = ts_node_child_by_field_name(node, "declarator", 10);
      if (!ts_node_is_null(inner)) return ExtractDeclaratorName(inner);
    }
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
      const TSNode child = ts_node_named_child(node, i);
      const std::string_view ctype = ts_node_type(child);
      if (ctype == "identifier" || ctype == "type_identifier" || ctype == "field_identifier" ||
          ctype == "qualified_identifier" || ctype == "function_declarator") {
        return ExtractDeclaratorName(child);
      }
    }
    return "";
  }

  std::string SanitizeRolePart(std::string name) const {
    for (char& c : name) {
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') {
        c = '_';
      }
    }
    return name.empty() ? "unnamed" : name;
  }

  std::string FunctionName(TSNode node) const {
    const TSNode declarator = ts_node_child_by_field_name(node, "declarator", 10);
    if (!ts_node_is_null(declarator)) {
      const std::string name = ExtractDeclaratorName(declarator);
      if (!name.empty()) return SanitizeRolePart(name);
    }
    return "func";
  }

  std::string ClassName(TSNode node) const {
    const TSNode name = ts_node_child_by_field_name(node, "name", 4);
    if (!ts_node_is_null(name)) return SanitizeRolePart(Substr(name));
    return "anonymous";
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
  // with less leading whitespace (preprocessor directives, verbatim raw
  // string content, column-0 closers) would shift and break byte identity.
  // The parent keeps such a child verbatim, and because this checks the
  // child's whole byte range, any level that cannot be dedented safely is
  // skipped before its own children are considered.
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

  std::vector<Child> AddChildren(TSNode node, const std::string& prefix) {
    std::vector<Child> children;
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
      const TSNode child = ts_node_named_child(node, i);
      const std::string_view type = ts_node_type(child);

      // Unwrap template_declaration
      TSNode inner = child;
      if (type == "template_declaration") {
        const uint32_t inner_count = ts_node_named_child_count(child);
        for (uint32_t j = 0; j < inner_count; ++j) {
          const TSNode cand = ts_node_named_child(child, j);
          const std::string_view ctype = ts_node_type(cand);
          if (ctype == "function_definition" || ctype == "class_specifier" ||
              ctype == "struct_specifier" || ctype == "declaration") {
            inner = cand;
            break;
          }
        }
      }

      // If namespace, flatten children directly into current container
      if (type == "namespace_definition") {
        const TSNode body = ts_node_child_by_field_name(child, "body", 4);
        if (!ts_node_is_null(body)) {
          const auto ns_children = AddChildren(body, prefix);
          children.insert(children.end(), ns_children.begin(), ns_children.end());
        }
        continue;
      }

      // Check for function_definition
      if (type == "function_definition" || ts_node_type(inner) == std::string_view("function_definition")) {
        if (UnsafeDedent(child)) continue;
        const std::string name = FunctionName(inner);
        const std::string id = Unique(prefix + "." + name);
        AddContainer(child, id, id);
        children.push_back({id, ts_node_start_byte(child), ts_node_end_byte(child)});
        continue;
      }

      // Check for class_specifier or struct_specifier inside declaration or directly
      TSNode type_spec = inner;
      if (ts_node_type(inner) == std::string_view("declaration")) {
        const uint32_t dcount = ts_node_named_child_count(inner);
        for (uint32_t k = 0; k < dcount; ++k) {
          const TSNode cand = ts_node_named_child(inner, k);
          const std::string_view ctype = ts_node_type(cand);
          if (ctype == "class_specifier" || ctype == "struct_specifier") {
            type_spec = cand;
            break;
          }
        }
      }

      const std::string_view spec_type = ts_node_type(type_spec);
      if (spec_type == "class_specifier" || spec_type == "struct_specifier") {
        if (UnsafeDedent(child)) continue;
        const std::string name = ClassName(type_spec);
        if (name != "anonymous") {
          const std::string id = Unique(prefix + "." + name);
          AddContainer(child, id, id);
          children.push_back({id, ts_node_start_byte(child), ts_node_end_byte(child)});
          continue;
        }
      }
    }
    return children;
  }

  void AddContainer(TSNode node, const std::string& id, const std::string& prefix,
                    bool whole_file = false) {
    TSNode contents = node;
    const std::string_view type = ts_node_type(node);

    if (type == "template_declaration") {
      const uint32_t count = ts_node_named_child_count(node);
      for (uint32_t i = 0; i < count; ++i) {
        const TSNode cand = ts_node_named_child(node, i);
        const std::string_view ctype = ts_node_type(cand);
        if (ctype == "class_specifier" || ctype == "struct_specifier" || ctype == "declaration") {
          contents = cand;
          break;
        }
      }
    }

    if (ts_node_type(contents) == std::string_view("declaration")) {
      const uint32_t dcount = ts_node_named_child_count(contents);
      for (uint32_t k = 0; k < dcount; ++k) {
        const TSNode cand = ts_node_named_child(contents, k);
        const std::string_view ctype = ts_node_type(cand);
        if (ctype == "class_specifier" || ctype == "struct_specifier") {
          contents = cand;
          break;
        }
      }
    }

    if (ts_node_type(contents) == std::string_view("class_specifier") ||
        ts_node_type(contents) == std::string_view("struct_specifier")) {
      const TSNode body = ts_node_child_by_field_name(contents, "body", 4);
      if (!ts_node_is_null(body)) contents = body;
    }

    const auto children = AddChildren(contents, prefix);
    const size_t start = whole_file ? 0 : ts_node_start_byte(node);
    const size_t end = whole_file ? source_.size() : ts_node_end_byte(node);
    std::string text;
    size_t cursor = start;
    for (const auto& child : children) {
      if (child.start < cursor || child.end > end) throw std::runtime_error("Overlapping C++ roles");
      text.append(source_, cursor, child.start - cursor);
      text += "{{" + child.id + "}}";
      cursor = child.end;
    }
    text.append(source_, cursor, end - cursor);

    // Dedent by the indentation of the line the role starts on. Using the
    // node's start column instead would misfire when the role begins mid-line
    // (an unwrapped one-line `template <typename T> QByteArray f() { ... }`
    // starts at the column after the template header): the renderer restores
    // indentation for line-start markers only, so a mid-line column would
    // strip the body indentation and break byte identity.
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
  Parser parser_;
  Tree tree_{nullptr, ts_tree_delete};
  bool degraded_ = false;
  Project project_;
};

}  // namespace

bool IsCppFile(const std::string& path) {
  for (const std::string_view ext : {".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hxx", ".hh"}) {
    if (path.ends_with(ext)) return true;
  }
  return false;
}

Project ImportCpp(const std::string& path, const std::string& source, bool* degraded) {
  if (path.empty() || !IsCppFile(path) || path.starts_with('/') ||
      path.find('\\') != std::string::npos || path.find("../") != std::string::npos ||
      path == ".." || path.starts_with(".tfe-code/")) {
    throw std::runtime_error("C++ import path must be a safe relative C++ source path");
  }
  Importer importer(path, source);
  if (degraded != nullptr) *degraded = importer.degraded();
  return importer.Run();
}

bool CppHasSyntaxError(const std::string& source) {
  Parser parser(ts_parser_new(), ts_parser_delete);
  if (!parser || !ts_parser_set_language(parser.get(), tree_sitter_cpp())) {
    throw std::runtime_error("Cannot initialize C++ parser");
  }
  Tree tree(ts_parser_parse_string(parser.get(), nullptr, source.data(),
                                   static_cast<uint32_t>(source.size())), ts_tree_delete);
  return !tree || HasSyntaxError(ts_tree_root_node(tree.get()));
}

void ValidateCpp(const std::string& source) {
  Parser parser(ts_parser_new(), ts_parser_delete);
  if (!parser || !ts_parser_set_language(parser.get(), tree_sitter_cpp())) {
    throw std::runtime_error("Cannot initialize C++ parser");
  }
  Tree tree(ts_parser_parse_string(parser.get(), nullptr, source.data(),
                                   static_cast<uint32_t>(source.size())), ts_tree_delete);
  if (!tree || HasSyntaxError(ts_tree_root_node(tree.get()))) {
    throw std::runtime_error("Candidate is not valid C++");
  }
}

}  // namespace tfe::code
