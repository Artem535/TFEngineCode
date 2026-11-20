#include "qml_import.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <tree_sitter/api.h>

#include "hash.h"

extern "C" const TSLanguage* tree_sitter_qmljs(void);

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

std::string QmlModuleId(const std::string& path) {
  std::string id = path;
  while (id.starts_with("./")) id = id.substr(2);
  if (id.ends_with(".qml")) id.resize(id.size() - 4);
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
    if (!parser_ ||
        !ts_parser_set_language(parser_.get(), tree_sitter_qmljs())) {
      throw std::runtime_error("Cannot initialize QML parser");
    }
    tree_.reset(ts_parser_parse_string(parser_.get(), nullptr, source_.data(),
                                       static_cast<uint32_t>(source_.size())));
    if (!tree_) throw std::runtime_error("Cannot parse QML source");
    degraded_ = HasSyntaxError(ts_tree_root_node(tree_.get()));
  }

  Project Run() {
    const std::string base = QmlModuleId(path_);
    if (degraded_) {
      // Keep a file usable even when its parse fails: one leaf role holding the
      // exact bytes, reported through `degraded` exactly like TS and C++.
      const std::string id = base + ".file";
      project_.blocks[id] = Node{Sha256(source_), source_, true, std::nullopt, 0, 0};
      project_.files[path_] = File{id, {}};
      return std::move(project_);
    }
    const std::string id = base + ".file";
    StoreRole(ts_tree_root_node(tree_.get()), id, ChildrenOf(ts_tree_root_node(tree_.get()), base),
              true);
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
  // own indentation: dedent removes that many leading characters and the
  // renderer re-indents each emitted line to the marker column, so a line with
  // less leading whitespace (a verbatim template literal, for example) would
  // shift and break byte identity. The same guard as the TS importer applies.
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

  std::string FirstIdentifier(TSNode node) const {
    const std::string_view type = ts_node_type(node);
    if (type == "identifier" || type == "property_identifier" ||
        type == "type_identifier") {
      return Substr(node);
    }
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
      const std::string found = FirstIdentifier(ts_node_named_child(node, i));
      if (!found.empty()) return found;
    }
    return {};
  }

  // `id: root` names the object; the binding itself stays inside the object
  // text instead of becoming a role of its own.
  std::string FindIdBinding(TSNode initializer) const {
    const uint32_t count = ts_node_named_child_count(initializer);
    for (uint32_t i = 0; i < count; ++i) {
      const TSNode child = ts_node_named_child(initializer, i);
      if (ts_node_type(child) != std::string_view("ui_binding")) continue;
      const TSNode name = ts_node_child_by_field_name(child, "name", 4);
      if (ts_node_is_null(name) || Substr(name) != "id") continue;
      const TSNode value = ts_node_child_by_field_name(child, "value", 5);
      if (ts_node_is_null(value)) continue;
      const std::string id = FirstIdentifier(value);
      if (!id.empty()) return SanitizeRolePart(id);
    }
    return {};
  }

  std::string ObjectName(TSNode node) const {
    const TSNode initializer = ts_node_child_by_field_name(node, "initializer", 11);
    if (!ts_node_is_null(initializer)) {
      const std::string id = FindIdBinding(initializer);
      if (!id.empty()) return id;
    }
    const TSNode type_name = ts_node_child_by_field_name(node, "type_name", 9);
    if (!ts_node_is_null(type_name)) return SanitizeRolePart(Substr(type_name));
    return "Object";
  }

  // Collects the outermost object definitions reachable from `node`, so a
  // binding such as `states: [ State { ... } ]` still yields roles for its
  // objects while the surrounding array text stays in the binding.
  void CollectObjects(TSNode node, std::vector<TSNode>* out) const {
    if (ts_node_type(node) == std::string_view("ui_object_definition")) {
      out->push_back(node);
      return;
    }
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
      CollectObjects(ts_node_named_child(node, i), out);
    }
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
        throw std::runtime_error("Overlapping QML roles");
      }
      text.append(source_, cursor, child.start - cursor);
      text += "{{" + child.id + "}}";
      cursor = child.end;
    }
    text.append(source_, cursor, end - cursor);

    // Dedent by the indentation of the line the role starts on, never by the
    // node's start column: the renderer restores indentation for line-start
    // markers only, so a mid-line column would break byte identity.
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

  void AddLeaf(TSNode node, const std::string& prefix, const std::string& name,
               std::vector<Child>* out) {
    if (UnsafeDedent(node)) return;
    const std::string id = Unique(prefix + "." + name);
    StoreRole(node, id, {});
    out->push_back({id, ts_node_start_byte(node), ts_node_end_byte(node)});
  }

  // A QML object: `Rectangle { ... }`, a `Type on signal { ... }` binding, or
  // an inline `component Name: Type { ... }`. Members become child roles.
  void AddObject(TSNode node, TSNode initializer, const std::string& name,
                 const std::string& prefix, std::vector<Child>* out) {
    if (UnsafeDedent(node)) return;
    const std::string id = Unique(prefix + "." + name);
    StoreRole(node, id, ChildrenOf(initializer, id));
    out->push_back({id, ts_node_start_byte(node), ts_node_end_byte(node)});
  }

  void AddBinding(TSNode node, const std::string& prefix, std::vector<Child>* out) {
    const TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
    if (ts_node_is_null(name_node)) return;
    const std::string name = SanitizeRolePart(Substr(name_node));
    if (name == "id") return;
    std::vector<TSNode> objects;
    const TSNode value = ts_node_child_by_field_name(node, "value", 5);
    if (!ts_node_is_null(value)) CollectObjects(value, &objects);
    if (objects.empty()) {
      AddLeaf(node, prefix, name, out);
      return;
    }
    if (UnsafeDedent(node)) return;
    const std::string id = Unique(prefix + "." + name);
    std::vector<Child> children;
    for (const TSNode object : objects) {
      const TSNode initializer = ts_node_child_by_field_name(object, "initializer", 11);
      if (ts_node_is_null(initializer)) continue;
      AddObject(object, initializer, ObjectName(object), id, &children);
    }
    StoreRole(node, id, std::move(children));
    out->push_back({id, ts_node_start_byte(node), ts_node_end_byte(node)});
  }

  void AddInlineComponent(TSNode node, const std::string& prefix, std::vector<Child>* out) {
    const TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
    const TSNode component = ts_node_child_by_field_name(node, "component", 9);
    if (ts_node_is_null(name_node) || ts_node_is_null(component)) return;
    const TSNode initializer = ts_node_child_by_field_name(component, "initializer", 11);
    if (ts_node_is_null(initializer)) return;
    AddObject(node, initializer, SanitizeRolePart(Substr(name_node)), prefix, out);
  }

  void AddMember(TSNode node, const std::string& prefix, std::vector<Child>* out) {
    const std::string_view type = ts_node_type(node);
    if (type == "ui_object_definition") {
      const TSNode initializer = ts_node_child_by_field_name(node, "initializer", 11);
      if (ts_node_is_null(initializer)) return;
      AddObject(node, initializer, ObjectName(node), prefix, out);
      return;
    }
    if (type == "ui_object_definition_binding") {
      const TSNode name = ts_node_child_by_field_name(node, "name", 4);
      const TSNode initializer = ts_node_child_by_field_name(node, "initializer", 11);
      if (ts_node_is_null(name) || ts_node_is_null(initializer)) return;
      AddObject(node, initializer, SanitizeRolePart(Substr(name)), prefix, out);
      return;
    }
    if (type == "ui_inline_component") {
      AddInlineComponent(node, prefix, out);
      return;
    }
    if (type == "ui_binding") {
      AddBinding(node, prefix, out);
      return;
    }
    if (type == "ui_property" || type == "ui_signal" || type == "ui_required") {
      const TSNode name = ts_node_child_by_field_name(node, "name", 4);
      if (ts_node_is_null(name)) return;
      AddLeaf(node, prefix, SanitizeRolePart(Substr(name)), out);
      return;
    }
    if (type == "function_declaration" || type == "generator_function_declaration") {
      const TSNode name = ts_node_child_by_field_name(node, "name", 4);
      if (ts_node_is_null(name)) return;
      AddLeaf(node, prefix, SanitizeRolePart(Substr(name)), out);
      return;
    }
    if (type == "variable_declaration") {
      // Only a single declarator becomes one role: several declarators share a
      // statement, and splitting them would leave the parent without a marker
      // for the shared `let`/`const` prefix.
      const uint32_t count = ts_node_named_child_count(node);
      std::vector<TSNode> declarators;
      for (uint32_t i = 0; i < count; ++i) {
        const TSNode child = ts_node_named_child(node, i);
        if (ts_node_type(child) == std::string_view("variable_declarator")) {
          declarators.push_back(child);
        }
      }
      if (declarators.size() != 1) return;
      const TSNode name = ts_node_child_by_field_name(declarators.front(), "name", 4);
      if (ts_node_is_null(name)) return;
      AddLeaf(node, prefix, SanitizeRolePart(Substr(name)), out);
      return;
    }
    if (type == "ui_annotated_object_member") {
      const TSNode definition = ts_node_child_by_field_name(node, "definition", 10);
      if (!ts_node_is_null(definition)) AddMember(definition, prefix, out);
    }
  }

  std::vector<Child> ChildrenOf(TSNode node, const std::string& prefix) {
    std::vector<Child> children;
    const std::string_view type = ts_node_type(node);
    if (type == "ui_object_initializer") {
      const uint32_t count = ts_node_named_child_count(node);
      for (uint32_t i = 0; i < count; ++i) {
        AddMember(ts_node_named_child(node, i), prefix, &children);
      }
    } else if (type == "program") {
      // Only root object definitions become roles here; imports, pragmas and
      // comments stay in the file role's own text.
      const uint32_t count = ts_node_named_child_count(node);
      for (uint32_t i = 0; i < count; ++i) {
        const TSNode child = ts_node_named_child(node, i);
        if (ts_node_type(child) != std::string_view("ui_object_definition")) continue;
        const TSNode initializer = ts_node_child_by_field_name(child, "initializer", 11);
        if (ts_node_is_null(initializer)) continue;
        AddObject(child, initializer, ObjectName(child), prefix, &children);
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

bool IsQmlFile(const std::string& path) { return path.ends_with(".qml"); }

Project ImportQml(const std::string& path, const std::string& source, bool* degraded) {
  if (path.empty() || !IsQmlFile(path) || path.starts_with('/') ||
      path.find('\\') != std::string::npos || path.find("../") != std::string::npos ||
      path == ".." || path.starts_with(".tfe-code/")) {
    throw std::runtime_error("QML import path must be a safe relative path");
  }
  Importer importer(path, source);
  if (degraded != nullptr) *degraded = importer.degraded();
  return importer.Run();
}

bool QmlHasSyntaxError(const std::string& path, const std::string& source) {
  (void)path;
  Parser parser(ts_parser_new(), ts_parser_delete);
  if (!parser || !ts_parser_set_language(parser.get(), tree_sitter_qmljs())) {
    throw std::runtime_error("Cannot initialize QML parser");
  }
  Tree tree(ts_parser_parse_string(parser.get(), nullptr, source.data(),
                                   static_cast<uint32_t>(source.size())),
            ts_tree_delete);
  return !tree || HasSyntaxError(ts_tree_root_node(tree.get()));
}

void ValidateQml(const std::string& path, const std::string& source) {
  if (QmlHasSyntaxError(path, source)) {
    throw std::runtime_error("Candidate is not valid QML");
  }
}

}  // namespace tfe::code
