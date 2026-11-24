#pragma once

#include <string>

#include "project.h"

namespace tfe::code {

bool IsXmlFile(const std::string& path);
// Files whose parse contains ERROR or MISSING nodes degrade instead of failing:
// they import as a single whole-file role and `degraded` reports them, matching
// the C++, TypeScript and QML importers.
Project ImportXml(const std::string& path, const std::string& source,
                  bool* degraded = nullptr);
bool XmlHasSyntaxError(const std::string& path, const std::string& source);
void ValidateXml(const std::string& path, const std::string& source);

}  // namespace tfe::code
