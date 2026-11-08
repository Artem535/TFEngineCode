#pragma once

#include <string>

#include "project.h"

namespace tfe::code {

bool IsTsFile(const std::string& path);
Project ImportTs(const std::string& path, const std::string& source);
void ValidateTs(const std::string& path, const std::string& source);

}  // namespace tfe::code
