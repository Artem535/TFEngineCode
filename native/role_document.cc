#include "role_document.h"

#include <regex>
#include <set>
#include <stdexcept>

namespace tfe::code {

std::vector<std::string> RoleChildren(const Node& node) {
  static const std::regex reference(R"(\{\{([A-Za-z0-9_-]+(?:\.[A-Za-z0-9_-]+)+)\}\})");
  std::vector<std::string> result;
  for (auto it = std::sregex_iterator(node.text.begin(), node.text.end(), reference);
       it != std::sregex_iterator(); ++it) {
    result.push_back((*it)[1].str());
  }
  return result;
}

std::vector<std::string> RoleSubtree(const Project& project, const std::string& root) {
  std::vector<std::string> result;
  std::set<std::string> seen;
  std::vector<std::string> pending{root};
  while (!pending.empty()) {
    const std::string id = pending.back();
    pending.pop_back();
    if (!seen.insert(id).second) continue;
    const auto found = project.blocks.find(id);
    if (found == project.blocks.end()) throw std::runtime_error("Missing role: " + id);
    result.push_back(id);
    const auto children = RoleChildren(found->second);
    pending.insert(pending.end(), children.rbegin(), children.rend());
  }
  return result;
}

std::string FormatRoles(const Project& project, const std::vector<std::string>& ids,
                        bool include_source_ranges) {
  std::string result;
  for (const auto& id : ids) {
    const auto found = project.blocks.find(id);
    if (found == project.blocks.end()) {
      throw std::runtime_error("Unknown role: " + id + "; use outline to list exact role IDs");
    }
    if (!result.empty()) result += "\n\n";
    result += "#@ " + id + " [version: " + found->second.version + "]";
    if (include_source_ranges && found->second.source_start_line != 0 &&
        found->second.source_end_line != 0) {
      result += " [lines: " + std::to_string(found->second.source_start_line) + "-" +
                std::to_string(found->second.source_end_line) + "]";
    }
    result += "\n" + found->second.text;
  }
  return result;
}

}  // namespace tfe::code
