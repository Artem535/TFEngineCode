#include "go_import.h"

#include <cctype>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <tree_sitter/api.h>

#include "hash.h"

extern "C" const TSLanguage* tree_sitter_go(void);

namespace tfe::code {
namespace {

using Parser = std::unique_ptr<TSParser, decltype(&ts_parser_delete)>;
using Tree = std::unique_ptr<TSTree, decltype(&ts_tree_delete)>;

struct Child {
  std::string id;
  size_t start;
  size_t end;
};

std::string GoModuleId(const std::string& path) {
  std::string id = path;
  while (id.starts_with("./")) id = id.substr(2);
  if (id.ends_with(".go")) id = id.substr(0, id.size() - 3);
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
    if (!parser_ || !ts_parser_set_language(parser_.get(), tree_sitter_go())) {
      throw std::runtime_error("Cannot initialize Go parser");
    }
    tree_.reset(ts_parser_parse_string(parser_.get(), nullptr, source_.data(),
                                       static_cast<uint32_t>(source_.size())));
    if (!tree_) throw std::runtime_error("Cannot parse Go source");
    const TSNode root = ts_tree_root_node(tree_.get());
    if (HasSyntaxError(root)) throw std::runtime_error("Invalid Go syntax: " + path_);
  }

  Project Run() {
    const std::string base = GoModuleId(path_);
    AddContainer(ts_tree_root_node(tree_.get()), base + ".file", base);
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

  std::string ExtractReceiverTypeName(TSNode node) const {
    if (ts_node_is_null(node)) return "";
    const std::string_view type = ts_node_type(node);
    if (type == "type_identifier") {
      return SanitizeRolePart(Substr(node));
    }
    if (type == "pointer_type" || type == "generic_type") {
      const uint32_t count = ts_node_named_child_count(node);
      for (uint32_t i = 0; i < count; ++i) {
        const std::string sub = ExtractReceiverTypeName(ts_node_named_child(node, i));
        if (!sub.empty()) return sub;
      }
    }
    if (type == "parameter_list" || type == "parameter_declaration") {
      const TSNode type_field = ts_node_child_by_field_name(node, "type", 4);
      if (!ts_node_is_null(type_field)) {
        const std::string sub = ExtractReceiverTypeName(type_field);
        if (!sub.empty()) return sub;
      }
      const uint32_t count = ts_node_named_child_count(node);
      for (uint32_t i = 0; i < count; ++i) {
        const std::string sub = ExtractReceiverTypeName(ts_node_named_child(node, i));
        if (!sub.empty()) return sub;
      }
    }
    return "";
  }

  std::string ExtractSingleTypeSpecName(TSNode type_decl_node) const {
    TSNode spec{};
    size_t spec_count = 0;
    const uint32_t count = ts_node_named_child_count(type_decl_node);
    for (uint32_t i = 0; i < count; ++i) {
      const TSNode child = ts_node_named_child(type_decl_node, i);
      const std::string_view ctype = ts_node_type(child);
      if (ctype == "type_spec" || ctype == "type_alias") {
        spec = child;
        ++spec_count;
      }
    }
    if (spec_count != 1) return "";
    const TSNode name = ts_node_child_by_field_name(spec, "name", 4);
    if (!ts_node_is_null(name)) return SanitizeRolePart(Substr(name));
    return "";
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
    for (uint32_t i = 0; i < count; ++i) {
      const TSNode child = ts_node_named_child(node, i);
      const std::string_view type = ts_node_type(child);

      if (type == "function_declaration") {
        const TSNode name_node = ts_node_child_by_field_name(child, "name", 4);
        if (!ts_node_is_null(name_node)) {
          const std::string name = SanitizeRolePart(Substr(name_node));
          const std::string id = Unique(prefix + "." + name);
          AddContainer(child, id, id);
          children.push_back({id, ts_node_start_byte(child), ts_node_end_byte(child)});
          continue;
        }
      }

      if (type == "method_declaration") {
        const TSNode name_node = ts_node_child_by_field_name(child, "name", 4);
        const TSNode recv_node = ts_node_child_by_field_name(child, "receiver", 8);
        if (!ts_node_is_null(name_node)) {
          const std::string method_name = SanitizeRolePart(Substr(name_node));
          const std::string recv_type = ExtractReceiverTypeName(recv_node);
          const std::string role_path =
              recv_type.empty() ? (prefix + "." + method_name)
                                : (prefix + "." + recv_type + "." + method_name);
          const std::string id = Unique(role_path);
          AddContainer(child, id, id);
          children.push_back({id, ts_node_start_byte(child), ts_node_end_byte(child)});
          continue;
        }
      }

      if (type == "type_declaration") {
        const std::string type_name = ExtractSingleTypeSpecName(child);
        if (!type_name.empty()) {
          const std::string id = Unique(prefix + "." + type_name);
          AddContainer(child, id, id);
          children.push_back({id, ts_node_start_byte(child), ts_node_end_byte(child)});
          continue;
        }
      }
    }
    return children;
  }

  void AddContainer(TSNode node, const std::string& id, const std::string& prefix) {
    const std::string_view type = ts_node_type(node);
    std::vector<Child> children;
    if (type == "source_file") {
      children = AddChildren(node, prefix);
    }

    const size_t start = ts_node_start_byte(node);
    const size_t end = ts_node_end_byte(node);
    std::string text;
    size_t cursor = start;
    for (const auto& child : children) {
      if (child.start < cursor || child.end > end) {
        throw std::runtime_error("Overlapping Go roles");
      }
      text.append(source_, cursor, child.start - cursor);
      text += "{{" + child.id + "}}";
      cursor = child.end;
    }
    text.append(source_, cursor, end - cursor);

    const size_t indentation = ts_node_start_point(node).column;
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

bool IsGoFile(const std::string& path) { return path.ends_with(".go"); }

Project ImportGo(const std::string& path, const std::string& source) {
  if (path.empty() || !IsGoFile(path) || path.starts_with('/') ||
      path.find('\\') != std::string::npos || path.find("../") != std::string::npos ||
      path == ".." || path.starts_with(".tfe-code/")) {
    throw std::runtime_error("Go import path must be a safe relative .go path");
  }
  return Importer(path, source).Run();
}

void ValidateGo(const std::string& source) {
  Parser parser(ts_parser_new(), ts_parser_delete);
  if (!parser || !ts_parser_set_language(parser.get(), tree_sitter_go())) {
    throw std::runtime_error("Cannot initialize Go parser");
  }
  Tree tree(ts_parser_parse_string(parser.get(), nullptr, source.data(),
                                   static_cast<uint32_t>(source.size())),
            ts_tree_delete);
  if (!tree || HasSyntaxError(ts_tree_root_node(tree.get()))) {
    throw std::runtime_error("Candidate is not valid Go");
  }
}

}  // namespace tfe::code
