#pragma once

#include <string>
#include <string_view>

namespace tfe::code {

std::string Sha256(std::string_view bytes);

}  // namespace tfe::code
