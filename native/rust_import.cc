#include "rust_import.h"

#include <cctype>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <tree_sitter/api.h>

#include "hash.h"

extern "C" const TSLanguage* tree_sitter_rust(void);

namespace tfe::code {
namespace {

using Parser = std::unique_ptr<TSParser, decltype(&ts_parser_delete)>;
using Tree = std::unique_ptr<TSTree, decltype(&ts_tree_delete)>;

struct Child {
  std::string id;
  size_t start;
  size_t end;
};

std::string RustModuleId(const std::string& path) {
  std::string id = path;
  while (id.starts_with("./")) id = id.substr(2);
  if (id.ends_with(".rs")) id = id.substr(0, id.size() - 3);
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
    if (!parser_ || !ts_parser_set_language(parser_.get(), tree_sitter_rust())) {
      throw std::runtime_error("Cannot initialize Rust parser");
    }
    tree_.reset(ts_parser_parse_string(parser_.get(), nullptr, source_.data(),
                                       static_cast<uint32_t>(source_.size())));
    if (!tree_) throw std::runtime_error("Cannot parse Rust source");
    const TSNode root = ts_tree_root_node(tree_.get());
    if (HasSyntaxError(root)) throw std::runtime_error("Invalid Rust syntax: " + path_);
  }

  Project Run() {
    const std::string base = RustModuleId(path_);
    const TSNode root = ts_tree_root_node(tree_.get());
    AddSpanContainer(root, ts_node_start_byte(root), ts_node_end_byte(root),
                     ts_node_start_point(root), ts_node_end_point(root), base + ".file", base);
    project_.files[path_] = File{base + ".file", {}};
    return std::move(project_);
  }

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

  std::string ExtractTypeName(TSNode node) const {
    if (ts_node_is_null(node)) return "";
    const std::string_view type = ts_node_type(node);
    if (type == "type_identifier" || type == "identifier" || type == "primitive_type") {
      return SanitizeRolePart(Substr(node));
    }
    if (type == "generic_type") {
      const TSNode inner = ts_node_child_by_field_name(node, "type", 4);
      if (!ts_node_is_null(inner)) return ExtractTypeName(inner);
    }
    if (type == "scoped_type_identifier") {
      const TSNode name = ts_node_child_by_field_name(node, "name", 4);
      if (!ts_node_is_null(name)) return ExtractTypeName(name);
    }
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
      const std::string sub = ExtractTypeName(ts_node_named_child(node, i));
      if (!sub.empty()) return sub;
    }
    return SanitizeRolePart(Substr(node));
  }

  std::string ExtractItemName(TSNode node) const {
    const std::string_view type = ts_node_type(node);
    if (type == "impl_item") {
      const TSNode type_node = ts_node_child_by_field_name(node, "type", 4);
      const std::string type_name = ExtractTypeName(type_node);
      return type_name.empty() ? "impl" : type_name;
    }
    const TSNode name = ts_node_child_by_field_name(node, "name", 4);
    if (!ts_node_is_null(name)) {
      return SanitizeRolePart(Substr(name));
    }
    return "anonymous";
  }

  std::string Unique(std::string id) {
    if (!project_.blocks.contains(id)) return id;
    for (size_t suffix = 2;; ++suffix) {
      const std::string candidate = id + "_" + std::to_string(suffix);
      if (!project_.blocks.contains(candidate)) return candidate;
    }
  }

  std::vector<Child> AddChildren(TSNode node, const std::string& prefix) {
    std::vector<Child> children;
    const uint32_t count = ts_node_named_child_count(node);
    bool has_attr = false;
    size_t attr_start_byte = 0;
    TSPoint attr_start_point{0, 0};

    for (uint32_t i = 0; i < count; ++i) {
      const TSNode child = ts_node_named_child(node, i);
      const std::string_view type = ts_node_type(child);

      if (type == "attribute_item") {
        if (!has_attr) {
          has_attr = true;
          attr_start_byte = ts_node_start_byte(child);
          attr_start_point = ts_node_start_point(child);
        }
        continue;
      }

      const size_t span_start = has_attr ? attr_start_byte : ts_node_start_byte(child);
      const TSPoint start_point = has_attr ? attr_start_point : ts_node_start_point(child);
      const size_t span_end = ts_node_end_byte(child);
      const TSPoint end_point = ts_node_end_point(child);
      has_attr = false;

      if (type == "impl_item") {
        const TSNode trait_node = ts_node_child_by_field_name(child, "trait", 5);
        const TSNode body = ts_node_child_by_field_name(child, "body", 4);
        const std::string type_name = ExtractItemName(child);
        if (ts_node_is_null(trait_node) && !ts_node_is_null(body) &&
            project_.blocks.contains(prefix + "." + type_name)) {
          // Inherent impl for an existing struct/enum in the same scope:
          // place its methods under `prefix.<Type>.<method>` and register the impl
          // container itself as `prefix.<Type>.impl` (or `impl_2`, etc.).
          const std::string impl_id = Unique(prefix + "." + type_name + ".impl");
          AddSpanContainer(child, span_start, span_end, start_point, end_point, impl_id,
                           prefix + "." + type_name);
          children.push_back({impl_id, span_start, span_end});
          continue;
        }
        std::string role_part = type_name;
        if (!ts_node_is_null(trait_node)) {
          const std::string trait_name = ExtractTypeName(trait_node);
          if (!trait_name.empty()) role_part = trait_name + "_for_" + type_name;
        }
        const std::string id = Unique(prefix + "." + role_part);
        AddSpanContainer(child, span_start, span_end, start_point, end_point, id, id);
        children.push_back({id, span_start, span_end});
        continue;
      }

      if (type == "struct_item" || type == "enum_item" || type == "trait_item" ||
          type == "function_item" || type == "function_signature_item" || type == "mod_item") {
        const std::string name = ExtractItemName(child);
        if (name != "anonymous") {
          const std::string id = Unique(prefix + "." + name);
          AddSpanContainer(child, span_start, span_end, start_point, end_point, id, id);
          children.push_back({id, span_start, span_end});
          continue;
        }
      }
    }
    return children;
  }

  void AddSpanContainer(TSNode item_node, size_t start, size_t end, TSPoint start_point,
                        TSPoint end_point, const std::string& id, const std::string& prefix) {
    TSNode contents = item_node;
    const std::string_view type = ts_node_type(item_node);
    if (type == "trait_item" || type == "impl_item" || type == "mod_item") {
      const TSNode body = ts_node_child_by_field_name(item_node, "body", 4);
      if (!ts_node_is_null(body)) contents = body;
    }

    const std::string_view contents_type = ts_node_type(contents);
    std::vector<Child> children;
    if (contents_type == "source_file" || contents_type == "declaration_list") {
      children = AddChildren(contents, prefix);
    }

    std::string text;
    size_t cursor = start;
    for (const auto& child : children) {
      if (child.start < cursor || child.end > end) {
        throw std::runtime_error("Overlapping Rust roles");
      }
      text.append(source_, cursor, child.start - cursor);
      text += "{{" + child.id + "}}";
      cursor = child.end;
    }
    text.append(source_, cursor, end - cursor);

    const size_t indentation = start_point.column;
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
    size_t start_line = start_point.row + 1;
    size_t end_line = end_point.column > 0 ? end_point.row + 1 : end_point.row;
    if (start == end) start_line = end_line = 0;
    project_.blocks[id] = Node{Sha256(text), std::move(text), children.empty(), std::nullopt,
                               start_line, end_line};
  }

  std::string path_;
  const std::string& source_;
  Parser parser_;
  Tree tree_{nullptr, ts_tree_delete};
  Project project_;
};

}  // namespace

bool IsRustFile(const std::string& path) { return path.ends_with(".rs"); }

Project ImportRust(const std::string& path, const std::string& source) {
  if (path.empty() || !IsRustFile(path) || path.starts_with('/') ||
      path.find('\\') != std::string::npos || path.find("../") != std::string::npos ||
      path == ".." || path.starts_with(".tfe-code/")) {
    throw std::runtime_error("Rust import path must be a safe relative .rs path");
  }
  return Importer(path, source).Run();
}

void ValidateRust(const std::string& source) {
  Parser parser(ts_parser_new(), ts_parser_delete);
  if (!parser || !ts_parser_set_language(parser.get(), tree_sitter_rust())) {
    throw std::runtime_error("Cannot initialize Rust parser");
  }
  Tree tree(ts_parser_parse_string(parser.get(), nullptr, source.data(),
                                   static_cast<uint32_t>(source.size())),
            ts_tree_delete);
  if (!tree || HasSyntaxError(ts_tree_root_node(tree.get()))) {
    throw std::runtime_error("Candidate is not valid Rust");
  }
}

}  // namespace tfe::code
