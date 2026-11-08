#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "project.h"
#include "objectbox_store.h"

namespace tfe::code {

struct RoleEdit {
  std::string role;
  std::string expected_version;
  std::string old;
  std::string new_text;
  // Several independent replacements inside one role. This exists because a
  // single old/new pair cannot express "change the signature and change the body
  // call", and duplicating the role entry instead is rejected as ambiguous.
  // Each pair must match exactly once, applied in order to the evolving text.
  std::vector<std::pair<std::string, std::string>> replace;
  // Used with `apply --amend` to remove a broken role edit from the saved draft.
  bool drop = false;
};

struct Receipt {
  std::string command;
  std::string key;
  bool passed = false;
};

struct State {
  int schema = 1;
  std::string project_hash;
  std::map<std::string, std::string> source_hashes;
  std::map<std::string, std::string> render_hashes;
  std::optional<Receipt> receipt;
};

struct ApplyResult {
  std::vector<std::string> changed;
  bool checked = false;
  bool passed = true;
};

struct CommandStats {
  uint64_t calls = 0;
  uint64_t baseline_bytes = 0;
  uint64_t emitted_bytes = 0;
  uint64_t saved_bytes = 0;
  uint64_t baseline_tokens = 0;
  uint64_t emitted_tokens = 0;
  uint64_t saved_tokens = 0;
};

struct TelemetryReport {
  uint64_t total_calls = 0;
  uint64_t baseline_bytes = 0;
  uint64_t emitted_bytes = 0;
  uint64_t saved_bytes = 0;
  uint64_t baseline_tokens = 0;
  uint64_t emitted_tokens = 0;
  uint64_t saved_tokens = 0;
  double savings_percent = 0.0;
  std::map<std::string, CommandStats> by_command;
};

class Workspace {
 public:
  explicit Workspace(std::filesystem::path root);

  void CreateIgnoreFile();
  ApplyResult Import(const std::vector<std::string>& paths);
  Project LoadProject() const;
  State LoadState() const;
  std::map<std::string, std::string> Render();
  // Renders the tree and writes every changed file, so a database-only workspace
  // can materialise its sources on demand.
  std::map<std::string, std::string> Materialize();
  std::string Outline(const std::string& target, int depth, bool relative = false);
  std::string Tree(const std::string& target);
  std::string Show(const std::vector<std::string>& ids);
  std::string FindRole(const std::string& file, const std::string& name);
  std::map<std::string, Node> ShowRoles(const std::vector<std::string>& ids);
  // With ids_only the result is one `id: matching line` per role, like
  // `grep -rn`, instead of each matching role's whole body. A wrong query then
  // costs a fraction of a wrong body-returning search.
  std::string Search(const std::vector<std::string>& terms, const std::string& prefix,
                     const std::string& file = "", bool ids_only = false,
                     bool use_regex = false);
  // Emits a ready-to-edit apply payload for the roles matching `terms`, limited to
  // `files` when given. With no terms, every role of `files` is returned: that is
  // the one-call equivalent of reading those files, and it removes the discovery
  // round trips that a query-by-query search needs.
  std::string PrepareBatch(const std::vector<std::string>& terms,
                           const std::vector<std::string>& files = {});
  // Compact plain-text slice across files/queries: includes enclosing `.file`
  // headers (imports/constants), full matched leaf roles with short unambiguous
  // aliases and 12-char version prefixes, and 1-line summaries of omitted siblings.
  // When `include_callers` is true, appends one-line calling references across
  // other roles in the project for matched symbols.
  std::string Context(const std::vector<std::string>& terms,
                      const std::vector<std::string>& files = {},
                      bool include_callers = false);

  // The cheap map a caller needs before any role command.
  // With no target it lists the managed file paths, one per line, like `find`.
  // With a file path or role id it lists that node's direct children by local
  // name, like `ls`: names only, no repeated id prefixes, no versions.
  std::string List(const std::string& target = "");
  ApplyResult Apply(const std::vector<RoleEdit>& edits,
                    const std::optional<std::string>& check_command,
                    bool amend = false);
  ApplyResult AddRole(const std::string& parent_target,
                      const std::string& name,
                      const std::string& text,
                      const std::string& after_role = "",
                      const std::optional<std::string>& check_command = std::nullopt);
  ApplyResult MoveRole(const std::string& role_target,
                       const std::string& new_name_or_id,
                       const std::string& to_parent = "",
                       const std::string& after_role = "",
                       const std::optional<std::string>& check_command = std::nullopt);
  ApplyResult DeleteRole(const std::string& role_target,
                         const std::optional<std::string>& check_command = std::nullopt);
  TelemetryReport GetStats(bool reset = false);
  std::string FormatStats(bool json = false, bool reset = false);
  Receipt Check(const std::string& command);
  ApplyResult Sync();

 private:
  struct Snapshot {
    Project project;
    State state;
    std::string serialized_project;
    std::map<std::string, std::string> sources;
    std::set<std::string> deleted_sources;
  };

  std::filesystem::path StorePath(const std::string& name) const;
  std::filesystem::path ObjectBoxDir() const;
  void SyncObjectBoxIndex(const Project& project) const;
  void RecordTelemetry(const std::string& command, uint64_t baseline_bytes,
                       uint64_t emitted_bytes) const;
  void RecoverIfNeeded() const;
  Snapshot LoadConsistent(bool recover = true) const;
  Snapshot LoadForImport() const;
  ApplyResult Commit(Snapshot before, Project candidate,
                     const std::map<std::string, std::string>& rendered,
                     const std::optional<std::string>& check_command,
                     bool write_sources);
  std::filesystem::path root_;
  // When TFE_CODE_INDEX=objectbox the role tree is persisted in the engine's
  // ObjectBox store. .tfe-code/project.json remains the crash-recovery mirror,
  // so the existing journal guarantees are unchanged; the database is the
  // working index and is rebuilt from that mirror whenever they disagree.
  bool use_objectbox_ = false;
};

}  // namespace tfe::code
