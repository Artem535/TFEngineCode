#pragma once

#include <string>

#include "project.h"

namespace tfe::code {

bool IsCppFile(const std::string& path);
Project ImportCpp(const std::string& path, const std::string& source);
void ValidateCpp(const std::string& source);

}  // namespace tfe::code
