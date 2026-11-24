#include "xml_import.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <tree_sitter/api.h>

#include "hash.h"

extern "C" const TSLanguage* tree_sitter_xml(void);

namespace tfe::code {
namespace {

using Parser = std::unique_ptr<TSParser, decltype(&ts_parser_delete)>;
using Tree = std::unique_ptr<TSTree, decltype(&ts_tree_delete)>;

struct Child {
  std::string id;
  size_t start;
  size_t end;
};

bool HasSyntaxError(TSNode node) {
  if (ts_node_is_error(node) || ts_node_is_missing(node)) return true;
  const uint32_t count = ts_node_child_count(node);
  for (uint32_t i = 0; i < count; ++i) {
    if (HasSyntaxError(ts_node_child(node, i))) return true;
  }
  return false;
}

std::string XmlModuleId(const std::string& path) {
  std::string id = path;
  while (id.starts_with("./")) id = id.substr(2);
  if (id.ends_with(".xml")) id.resize(id.size() - 4);
  for (char& c : id) {
    if (c == '/') c = '.';
    else if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_') c = '_';
  }
  return id.empty() ? "module" : id;
}

class Importer {
 public:
  Importer(std::string path, const std::string& source)
      : path_(std::move(path)), source_(source), parser_(ts_parser_new(), ts_parser_delete) {
    if (!parser_ || !ts_parser_set_language(parser_.get(), tree_sitter_xml())) {
      throw std::runtime_error("Cannot initialize XML parser");
    }
    tree_.reset(ts_parser_parse_string(parser_.get(), nullptr, source_.data(),
                                       static_cast<uint32_t>(source_.size())));
    if (!tree_) throw std::runtime_error("Cannot parse XML source");
    degraded_ = HasSyntaxError(ts_tree_root_node(tree_.get()));
  }

  Project Run() {
    const std::string base = XmlModuleId(path_);
    if (degraded_) {
      const std::string id = base + ".file";
      project_.blocks[id] = Node{Sha256(source_), source_, true, std::nullopt, 0, 0};
      project_.files[path_] = File{id, {}};
      return std::move(project_);
    }
    const std::string id = base + ".file";
    StoreRole(ts_tree_root_node(tree_.get()), id,
              ChildrenOf(ts_tree_root_node(tree_.get()), base), true);
    project_.files[path_] = File{id, {}};
    return std::move(project_);
  }

  bool degraded() const { return degraded_; }

 private:
  std::string Substr(TSNode node) const {
    const size_t start = ts_node_start_byte(node);
    const size_t end = ts_node_end_byte(node);
    return source_.substr(start, end - start);
  }

  static std::string SanitizeRolePart(std::string name) {
    for (char& c : name) {
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') {
        c = '_';
      }
    }
    return name.empty() ? "unnamed" : name;
  }

  std::string Unique(std::string id) {
    if (!project_.blocks.contains(id)) return id;
    for (size_t suffix = 2;; ++suffix) {
      const std::string candidate = id + "_" + std::to_string(suffix);
      if (!project_.blocks.contains(candidate)) return candidate;
    }
  }

  // A role stored dedented must own every non-blank line in its range up to its
  // own indentation; otherwise the renderer re-indents a line that had less
  // leading whitespace and breaks byte identity.
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

  TSNode TagOf(TSNode element) const {
    const uint32_t count = ts_node_named_child_count(element);
    for (uint32_t i = 0; i < count; ++i) {
      const TSNode child = ts_node_named_child(element, i);
      const std::string_view type = ts_node_type(child);
      if (type == "STag" || type == "EmptyElemTag") return child;
    }
    return TSNode{};
  }

  std::string TagName(TSNode element) const {
    const TSNode tag = TagOf(element);
    if (ts_node_is_null(tag)) return "element";
    const uint32_t count = ts_node_named_child_count(tag);
    for (uint32_t i = 0; i < count; ++i) {
      const TSNode child = ts_node_named_child(tag, i);
      if (ts_node_type(child) == std::string_view("Name")) {
        return SanitizeRolePart(Substr(child));
      }
    }
    return "element";
  }

  // `id`/`name` attributes name the element; the value keeps its quotes in the
  // node range, so strip them.
  std::string AttributeName(TSNode element) const {
    const TSNode tag = TagOf(element);
    if (ts_node_is_null(tag)) return {};
    const uint32_t count = ts_node_named_child_count(tag);
    for (uint32_t i = 0; i < count; ++i) {
      const TSNode attribute = ts_node_named_child(tag, i);
      if (ts_node_type(attribute) != std::string_view("Attribute")) continue;
      std::string name;
      TSNode value{};
      const uint32_t parts = ts_node_named_child_count(attribute);
      for (uint32_t j = 0; j < parts; ++j) {
        const TSNode part = ts_node_named_child(attribute, j);
        const std::string_view type = ts_node_type(part);
        if (type == "Name" && name.empty()) name = Substr(part);
        else if (type == "AttValue") value = part;
      }
      if (name != "id" && name != "name" && name != "ID" && name != "Id") continue;
      if (ts_node_is_null(value)) continue;
      std::string text = Substr(value);
      while (!text.empty() && (text.front() == '"' || text.front() == '\'')) text.erase(0, 1);
      while (!text.empty() && (text.back() == '"' || text.back() == '\'')) text.pop_back();
      if (!text.empty()) return SanitizeRolePart(text);
    }
    return {};
  }

  std::string ElementName(TSNode element) const {
    const std::string named = AttributeName(element);
    return named.empty() ? TagName(element) : named;
  }

  TSNode ContentOf(TSNode element) const {
    const uint32_t count = ts_node_named_child_count(element);
    for (uint32_t i = 0; i < count; ++i) {
      const TSNode child = ts_node_named_child(element, i);
      if (ts_node_type(child) == std::string_view("content")) return child;
    }
    return TSNode{};
  }

  void StoreRole(TSNode node, const std::string& id, std::vector<Child> children,
                 bool whole_file = false) {
    const size_t start = whole_file ? 0 : ts_node_start_byte(node);
    const size_t end = whole_file ? source_.size() : ts_node_end_byte(node);
    std::sort(children.begin(), children.end(),
              [](const Child& a, const Child& b) { return a.start < b.start; });
    std::string text;
    size_t cursor = start;
    for (const auto& child : children) {
      if (child.start < cursor || child.end > end) {
        throw std::runtime_error("Overlapping XML roles");
      }
      text.append(source_, cursor, child.start - cursor);
      text += "{{" + child.id + "}}";
      cursor = child.end;
    }
    text.append(source_, cursor, end - cursor);

    size_t indentation = 0;
    if (!whole_file) {
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

  void AddElement(TSNode element, const std::string& prefix, std::vector<Child>* out) {
    if (UnsafeDedent(element)) return;
    const std::string id = Unique(prefix + "." + ElementName(element));
    std::vector<Child> children;
    const uint32_t count = ts_node_named_child_count(element);
    for (uint32_t i = 0; i < count; ++i) {
      const TSNode child = ts_node_named_child(element, i);
      if (ts_node_type(child) != std::string_view("content")) continue;
      const uint32_t inner = ts_node_named_child_count(child);
      for (uint32_t j = 0; j < inner; ++j) {
        const TSNode candidate = ts_node_named_child(child, j);
        if (ts_node_type(candidate) != std::string_view("element")) continue;
        AddElement(candidate, id, &children);
      }
    }
    StoreRole(element, id, std::move(children));
    out->push_back({id, ts_node_start_byte(element), ts_node_end_byte(element)});
  }

  std::vector<Child> ChildrenOf(TSNode document, const std::string& prefix) {
    std::vector<Child> children;
    const uint32_t count = ts_node_named_child_count(document);
    for (uint32_t i = 0; i < count; ++i) {
      const TSNode child = ts_node_named_child(document, i);
      if (ts_node_type(child) == std::string_view("element")) {
        AddElement(child, prefix, &children);
      }
    }
    return children;
  }

  std::string path_;
  const std::string& source_;
  bool degraded_ = false;
  Parser parser_;
  Tree tree_{nullptr, ts_tree_delete};
  Project project_;
};

}  // namespace

bool IsXmlFile(const std::string& path) { return path.ends_with(".xml"); }

Project ImportXml(const std::string& path, const std::string& source, bool* degraded) {
  if (path.empty() || !IsXmlFile(path) || path.starts_with('/') ||
      path.find('\\') != std::string::npos || path.find("../") != std::string::npos ||
      path == ".." || path.starts_with(".tfe-code/")) {
    throw std::runtime_error("XML import path must be a safe relative path");
  }
  Importer importer(path, source);
  if (degraded != nullptr) *degraded = importer.degraded();
  return importer.Run();
}

bool XmlHasSyntaxError(const std::string& path, const std::string& source) {
  (void)path;
  Parser parser(ts_parser_new(), ts_parser_delete);
  if (!parser || !ts_parser_set_language(parser.get(), tree_sitter_xml())) {
    throw std::runtime_error("Cannot initialize XML parser");
  }
  Tree tree(ts_parser_parse_string(parser.get(), nullptr, source.data(),
                                   static_cast<uint32_t>(source.size())),
            ts_tree_delete);
  return !tree || HasSyntaxError(ts_tree_root_node(tree.get()));
}

void ValidateXml(const std::string& path, const std::string& source) {
  if (XmlHasSyntaxError(path, source)) {
    throw std::runtime_error("Candidate is not valid XML");
  }
}

}  // namespace tfe::code
