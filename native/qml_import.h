#pragma once

#include <string>

#include "project.h"

namespace tfe::code {

bool IsQmlFile(const std::string& path);
// Files whose parse contains tree-sitter ERROR or MISSING nodes degrade instead
// of failing: they import as a single whole-file role and `degraded` reports
// them, matching the TypeScript and C++ importers.
Project ImportQml(const std::string& path, const std::string& source,
                  bool* degraded = nullptr);
bool QmlHasSyntaxError(const std::string& path, const std::string& source);
void ValidateQml(const std::string& path, const std::string& source);

}  // namespace tfe::code
