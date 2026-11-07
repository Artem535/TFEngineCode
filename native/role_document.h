#pragma once

#include <string>
#include <vector>

#include "project.h"

namespace tfe::code {

std::vector<std::string> RoleChildren(const Node& node);
std::string FormatRoles(const Project& project, const std::vector<std::string>& ids,
                        bool include_source_ranges = false);
std::vector<std::string> RoleSubtree(const Project& project, const std::string& root);

}  // namespace tfe::code
