#pragma once

#include <filesystem>
#include <string>

#include "project.h"

namespace tfe::code {

// Persists the role tree in the TextFoundryEngine ObjectBox store instead of
// .tfe-code/project.json, so the code itself lives in the engine database.
//
// Mapping, one Block per role:
//   block id    = role id
//   template    = role text, verbatim
//   description = JSON metadata: {"sha256", "literal", "source_start_line",
//                 "source_end_line"} -- the engine Version is major.minor, so
//                 the content hash the edit contract relies on is kept here.
//   type        = Domain, language = "python"
//
// The file -> root role mapping is stored as one Meta block per file, so a
// render can rebuild the whole Project from the database alone.
//
// Each Save publishes only changed roles. A new role publishes 1.0; an edited
// role publishes the next minor version, which gives the engine's version
// history for free while leaving the hash-based optimistic-concurrency contract
// in apply unchanged.
class ObjectBoxRoleStore {
 public:
  explicit ObjectBoxRoleStore(std::filesystem::path db_dir);

  // True when the store already holds at least one role.
  bool Exists() const;

  // Rebuilds the Project from the database.
  Project Load() const;

  // Publishes every role and file mapping whose current text or metadata
  // differs from the latest stored version. Returns the number of published
  // roles plus file mappings.
  size_t Save(const Project& project) const;

  const std::filesystem::path& DbDir() const noexcept { return db_dir_; }

 private:
  std::filesystem::path db_dir_;
};

// Metadata carried in Block::description alongside the role.
std::string EncodeRoleMetadata(const Node& node);
Node DecodeRoleMetadata(const std::string& description, const std::string& text);

// JSON payload carrying a file's root role and params.
std::string EncodeFileRecord(const File& file);
File DecodeFileRecord(const std::string& payload);

}  // namespace tfe::code