#pragma once

#include <string>

#include "project.h"

namespace tfe::code {

Project ImportPython(const std::string& path, const std::string& source);
void ValidatePython(const std::string& source);

}  // namespace tfe::code
