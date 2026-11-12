#pragma once

#include <string>

#include "project.h"

namespace tfe::code {

bool IsTsFile(const std::string& path);
// Files whose parse contains ERROR or MISSING nodes (Qt Linguist writes XML
// with a `.ts` extension) degrade instead of failing: they import as a single
// whole-file role and `degraded` reports them.
Project ImportTs(const std::string& path, const std::string& source,
                 bool* degraded = nullptr);
bool TsHasSyntaxError(const std::string& path, const std::string& source);
void ValidateTs(const std::string& path, const std::string& source);

}  // namespace tfe::code
