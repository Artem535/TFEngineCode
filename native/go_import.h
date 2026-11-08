#pragma once

#include <string>

#include "project.h"

namespace tfe::code {

bool IsGoFile(const std::string& path);
Project ImportGo(const std::string& path, const std::string& source);
void ValidateGo(const std::string& source);

}  // namespace tfe::code
