#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "project.h"

namespace tfe::code {

using Graph = std::map<std::string, std::set<std::string>>;

struct Context {
  std::string file;
  File entry;
  std::map<std::string, Node> blocks;
};

struct Inspection {
  std::string status = "context";
  std::string file;
  std::map<std::string, Node> blocks;
  std::optional<std::map<std::string, std::vector<std::string>>> uses;
  std::optional<std::string> note;
};

struct Change {
  std::string before;
  std::string after;
};

struct Applied {
  std::string project;
  std::map<std::string, Change> changed;
};

struct Edit {
  std::string block;
  std::string expected_version;
  std::string version;
  std::string text;
};

struct Match {
  std::string file;
  std::string block;
  std::string version;
};

struct Found {
  std::string status = "found";
  std::vector<Match> matches;
  std::map<std::string, Node> blocks;
  std::map<std::string, File> files;
  bool more = false;
  size_t next_offset = 0;
};

std::map<std::string, std::string> RenderFiles(const Project& project,
                                               unsigned jobs = 0);
std::string RenderFile(const Project& project, const std::string& file);
Context ReadContext(const Project& project, const std::string& file,
                    const std::string& target);
Inspection Inspect(const Project& project, const std::string& file, unsigned jobs,
                   const std::vector<std::string>& ids, bool uses);
Applied ApplyMany(const Project& project, const std::vector<Edit>& edits,
                  unsigned jobs);
Found Find(const Project& project, const std::string& query, size_t limit,
           size_t offset, const std::string& prefix);
std::vector<std::string> Search(const Project& project, const std::string& query);
Project Patch(const Project& project, const Edit& edit);
unsigned ParseJobs(const std::string& value);
std::string ReadFile(const std::string& path);

}  // namespace tfe::code
