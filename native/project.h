#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "tf/renderer.h"

namespace tfe::code {

struct Choice {
  std::string attribute;
  std::vector<std::string> values;
  std::string then_ref;
  std::string else_ref;
  bool operator==(const Choice&) const = default;
};

struct Node {
  std::string version;
  std::string text;
  bool literal;
  std::optional<Choice> choice;
  size_t source_start_line = 0;
  size_t source_end_line = 0;
  bool operator==(const Node&) const = default;
};

struct File {
  std::string root;
  tf::Params params;
};

struct Project {
  std::map<std::string, Node> blocks;
  std::map<std::string, File> files;
};

}  // namespace tfe::code
