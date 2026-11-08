#pragma once

#include <string>

#include "project.h"

namespace tfe::code {

bool IsRustFile(const std::string& path);
Project ImportRust(const std::string& path, const std::string& source);
void ValidateRust(const std::string& source);

}  // namespace tfe::code
