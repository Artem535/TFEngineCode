#pragma once

#include <string>

#include "project.h"

namespace tfe::code {

bool IsCppFile(const std::string& path);
// Files whose parse contains tree-sitter ERROR nodes (Qt macros such as
// Q_OBJECT/emit/Q_ARG cannot be preprocessed) degrade instead of failing:
// they import as a single whole-file role and `degraded` reports them.
Project ImportCpp(const std::string& path, const std::string& source,
                  bool* degraded = nullptr);
bool CppHasSyntaxError(const std::string& source);
void ValidateCpp(const std::string& source);

}  // namespace tfe::code
