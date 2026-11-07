#include "python_import.h"

#include <cctype>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <tree_sitter/api.h>

#include "hash.h"

extern "C" const TSLanguage* tree_sitter_python(void);

namespace tfe::code {
namespace {

using Parser = std::unique_ptr<TSParser, decltype(&ts_parser_delete)>;
using Tree = std::unique_ptr<TSTree, decltype(&ts_tree_delete)>;

struct Child {
  std::string id;
  size_t start;
  size_t end;
};

std::string ModuleId(const std::string& path) {
  std::string id = path.ends_with(".py") ? path.substr(0, path.size() - 3) : path;
  for (char& c : id) {
    if (c == '/') c = '.';
    else if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_') c = '_';
  }
  if (id.ends_with(".__init__")) id.erase(id.size() - 9);
  return id.empty() ? "module" : id;
}

class Importer {
 public:
  Importer(std::string path, const std::string& source)
      : path_(std::move(path)), source_(source), parser_(ts_parser_new(), ts_parser_delete) {
    if (!parser_ || !ts_parser_set_language(parser_.get(), tree_sitter_python())) {
      throw std::runtime_error("Cannot initialize Python parser");
    }
    tree_.reset(ts_parser_parse_string(parser_.get(), nullptr, source_.data(),
                                       static_cast<uint32_t>(source_.size())));
    if (!tree_) throw std::runtime_error("Cannot parse Python source");
    const TSNode root = ts_tree_root_node(tree_.get());
    if (ts_node_has_error(root)) throw std::runtime_error("Invalid Python syntax: " + path_);
  }

  Project Run() {
    const std::string base = ModuleId(path_);
    AddContainer(ts_tree_root_node(tree_.get()), base + ".file", base);
    project_.files[path_] = File{base + ".file", {}};
    return std::move(project_);
  }

 private:
  std::string NodeName(TSNode node) const {
    const TSNode name = ts_node_child_by_field_name(node, "name", 4);
    if (ts_node_is_null(name)) throw std::runtime_error("Unnamed Python definition");
    return source_.substr(ts_node_start_byte(name), ts_node_end_byte(name) - ts_node_start_byte(name));
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
      TSNode definition = child;
      bool decorated = false;
      if (type == "decorated_definition") {
        definition = ts_node_child_by_field_name(child, "definition", 10);
        decorated = true;
      }
      const std::string_view definition_type = ts_node_type(definition);
      if (definition_type != "class_definition" && definition_type != "function_definition") continue;
      const std::string id = Unique(prefix + "." + NodeName(definition));
      AddContainer(decorated ? child : definition, id, id);
      children.push_back({id, ts_node_start_byte(child), ts_node_end_byte(child)});
      (void)decorated;
    }
    return children;
  }

  void AddContainer(TSNode node, const std::string& id, const std::string& prefix) {
    TSNode contents = node;
    const std::string_view type = ts_node_type(node);
    if (type == "decorated_definition") {
      contents = ts_node_child_by_field_name(node, "definition", 10);
    }
    if (ts_node_type(contents) == std::string_view("class_definition") ||
        ts_node_type(contents) == std::string_view("function_definition")) {
      contents = ts_node_child_by_field_name(contents, "body", 4);
    }
    const auto children = AddChildren(contents, prefix);
    const size_t start = ts_node_start_byte(node);
    const size_t end = ts_node_end_byte(node);
    std::string text;
    size_t cursor = start;
    for (const auto& child : children) {
      if (child.start < cursor || child.end > end) throw std::runtime_error("Overlapping Python roles");
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

Project ImportPython(const std::string& path, const std::string& source) {
  if (path.empty() || !path.ends_with(".py") || path.starts_with('/') ||
      path.find('\\') != std::string::npos || path.find("../") != std::string::npos ||
      path == ".." || path.starts_with(".tfe-code/")) {
    throw std::runtime_error("Python import path must be a safe relative .py path");
  }
  return Importer(path, source).Run();
}

void ValidatePython(const std::string& source) {
  Parser parser(ts_parser_new(), ts_parser_delete);
  if (!parser || !ts_parser_set_language(parser.get(), tree_sitter_python())) {
    throw std::runtime_error("Cannot initialize Python parser");
  }
  Tree tree(ts_parser_parse_string(parser.get(), nullptr, source.data(),
                                   static_cast<uint32_t>(source.size())), ts_tree_delete);
  if (!tree || ts_node_has_error(ts_tree_root_node(tree.get()))) {
    throw std::runtime_error("Candidate is not valid Python");
  }
}

}  // namespace tfe::code
