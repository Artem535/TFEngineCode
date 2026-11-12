#include "workspace.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <ranges>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include <condition_variable>
#include <mutex>
#include <efsw/efsw.hpp>
#include <re2/re2.h>
#include <rfl/json.hpp>

#include "hash.h"
#include "cpp_import.h"
#include "go_import.h"
#include "python_import.h"
#include "rust_import.h"
#include "ts_import.h"
#include "render_project.h"
#include "role_document.h"

namespace tfe::code {
namespace {

class Lock {
 public:
  explicit Lock(const std::filesystem::path& path) {
    fd_ = open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd_ < 0) throw std::runtime_error("Cannot open workspace lock");
    // Bounded wait instead of an uninterruptible one. `apply --check` holds this
    // lock while the check runs, so a check command that invokes `tfe-code`
    // itself would otherwise block on itself with no diagnostic at all.
    for (int attempt = 0; attempt < 300; ++attempt) {
      if (flock(fd_, LOCK_EX | LOCK_NB) == 0) return;
      if (errno != EWOULDBLOCK) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    close(fd_);
    throw std::runtime_error(
        "Workspace is busy: another tfe-code command holds .tfe-code/lock. A check "
        "command must not call tfe-code; use plain test commands instead.");
  }
  ~Lock() {
    if (fd_ >= 0) {
      flock(fd_, LOCK_UN);
      close(fd_);
    }
  }
 private:
  int fd_ = -1;
};

std::string ReadBinary(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("Cannot read " + path.string());
  return {std::istreambuf_iterator<char>(stream), {}};
}

void WriteBinary(const std::filesystem::path& path, const std::string& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!stream) throw std::runtime_error("Cannot write " + path.string());
}

void AtomicWrite(const std::filesystem::path& path, const std::string& bytes) {
  const auto temporary = path.string() + ".tmp-" + std::to_string(getpid());
  WriteBinary(temporary, bytes);
  std::filesystem::rename(temporary, path);
}

// Test-only crash injection. Both sides are canonicalised because the paths
// reaching this hook are not spelled the way the tests name them: source writes
// come from SafePath(), which resolves symlinks, so a workspace rooted under a
// symlinked directory (on macOS TMPDIR is /var/folders/... and /var points at
// /private/var) would otherwise never match and the hook would silently not fire.
void CrashAfterWrite(const std::filesystem::path& path) {
  const char* target = std::getenv("TFE_CODE_TEST_CRASH_AFTER");
  if (target == nullptr) return;
  std::error_code error;
  const auto expected = std::filesystem::weakly_canonical(std::filesystem::path(target), error);
  if (error) return;
  const auto written = std::filesystem::weakly_canonical(path, error);
  if (!error && written == expected) _exit(90);
}

bool IsSupportedSourceFile(const std::string& path) {
  return path.ends_with(".py") || IsCppFile(path) || IsTsFile(path) ||
         IsRustFile(path) || IsGoFile(path);
}

Project ImportSource(const std::string& path, const std::string& bytes,
                     std::vector<std::string>* degraded = nullptr) {
  if (IsCppFile(path)) {
    bool file_degraded = false;
    Project project = ImportCpp(path, bytes, &file_degraded);
    if (file_degraded && degraded != nullptr) degraded->push_back(path);
    return project;
  }
  if (IsTsFile(path)) {
    bool file_degraded = false;
    Project project = ImportTs(path, bytes, &file_degraded);
    if (file_degraded && degraded != nullptr) degraded->push_back(path);
    return project;
  }
  if (IsRustFile(path)) {
    return ImportRust(path, bytes);
  }
  if (IsGoFile(path)) {
    return ImportGo(path, bytes);
  }
  return ImportPython(path, bytes);
}

std::filesystem::path SafePath(const std::filesystem::path& root,
                               const std::string& name,
                               bool require_source = true) {
  const std::filesystem::path relative(name);
  if (name.empty() || relative.is_absolute() || (require_source && !IsSupportedSourceFile(name))) {
    throw std::runtime_error("Expected a relative source path: " + name);
  }
  for (const auto& part : relative) {
    if (part == ".." || part == ".tfe-code") {
      throw std::runtime_error("Path escapes the managed workspace: " + name);
    }
  }
  const auto path = std::filesystem::weakly_canonical(root / relative);
  const auto canonical_root = std::filesystem::weakly_canonical(root);
  auto root_it = canonical_root.begin();
  auto path_it = path.begin();
  for (; root_it != canonical_root.end() && path_it != path.end(); ++root_it, ++path_it) {
    if (*root_it != *path_it) throw std::runtime_error("Path escapes workspace: " + name);
  }
  if (root_it != canonical_root.end()) throw std::runtime_error("Path escapes workspace: " + name);
  return path;
}

std::filesystem::path JournalEntryPath(const std::filesystem::path& root,
                                       const std::string& name) {
  if (name == ".tfe-code/project.json" || name == ".tfe-code/state.json") {
    return root / std::filesystem::path(name);
  }
  return SafePath(root, name);
}

struct TransactionEntry {
  std::string path;
  std::optional<std::string> original;
};

struct TransactionJournal {
  int schema = 1;
  std::vector<TransactionEntry> entries;
};

// Mirror of the apply payload in main.cc, so prepare-batch can emit a document
// that pipes straight back into `tfe-code apply`.
struct BatchEdit {
  std::string role = {};
  std::string expected_version = {};
  rfl::Rename<"old", std::string> old_text = {};
  rfl::Rename<"new", std::string> new_text = {};
};

struct BatchPayload {
  std::vector<BatchEdit> edits = {};
};

struct SavedReplacement {
  std::string old;
  std::string new_text;
};

struct SavedRoleEdit {
  std::string role;
  std::string expected_version;
  std::string old;
  std::string new_text;
  std::vector<SavedReplacement> replace;
};

struct SavedBatch {
  std::vector<SavedRoleEdit> edits;
};

struct ResolvedRole {
  std::string id;
  std::string error;
};

// Resolves either an exact role ID or a unique dotted suffix (such as
// `events.Event` for `src.pkg.observability.events.Event`), avoiding 50-char
// package prefixes in every edit while rejecting ambiguous suffixes explicitly.
ResolvedRole TryResolveRoleId(const Project& project, const std::string& role) {
  if (project.blocks.contains(role)) return {role, ""};
  const std::string suffix = "." + role;
  std::vector<std::string> matches;
  for (const auto& [id, node] : project.blocks) {
    (void)node;
    if (id.ends_with(suffix)) matches.push_back(id);
  }
  if (matches.size() == 1) return {matches.front(), ""};
  if (matches.size() > 1) {
    std::string msg = "Ambiguous role '" + role + "'; choose a longer suffix from:";
    for (size_t i = 0; i < matches.size() && i < 6; ++i) msg += " " + matches[i];
    return {"", msg};
  }
  return {"", "Unknown role: " + role + "; use outline to list exact role IDs"};
}

// Container targets resolve file-path first, then bare module names (`dialect`
// -> `dialect.file`), then unique dotted suffixes, so structural operations
// accept the same targets `apply` does.
std::string ResolveContainerTarget(const Project& project, const std::string& target) {
  if (project.files.contains(target)) return project.files.at(target).root;
  if (project.blocks.contains(target)) return target;
  if (!target.empty()) {
    const std::string file_role = target + ".file";
    if (project.blocks.contains(file_role)) return file_role;
  }
  const auto resolved = TryResolveRoleId(project, target);
  if (!resolved.error.empty()) throw std::runtime_error(resolved.error);
  return resolved.id;
}

// Shortest module-rooted suffix that uniquely resolves back to `full_id`
// (for example `store.EventStore.init_schema` instead of repeating the full
// `src.tabletopmagnat.observability.store...` prefix).
std::string ShortestRoleAlias(const Project& project, const std::string& full_id) {
  std::string best_module;
  for (const auto& [path, file] : project.files) {
    (void)path;
    const std::string mod =
        file.root.ends_with(".file") ? file.root.substr(0, file.root.size() - 5) : file.root;
    if (full_id == file.root || full_id.starts_with(mod + ".")) {
      if (mod.size() > best_module.size()) best_module = mod;
    }
  }
  if (best_module.empty() || full_id.size() <= best_module.size() + 1) return full_id;
  const std::string local = full_id.substr(best_module.size() + 1);
  std::vector<std::string> parts;
  for (const auto& seg : std::views::split(best_module, '.')) {
    parts.emplace_back(seg.begin(), seg.end());
  }
  for (size_t count = 1; count <= parts.size(); ++count) {
    std::string candidate;
    for (size_t i = parts.size() - count; i < parts.size(); ++i) {
      candidate += parts[i] + ".";
    }
    candidate += local;
    if (TryResolveRoleId(project, candidate).id == full_id) return candidate;
  }
  return full_id;
}

bool MatchesVersion(const std::string& actual, const std::string& expected) {
  if (actual == expected) return true;
  if (expected.size() < 8 || expected.size() > actual.size()) return false;
  std::string lower;
  lower.reserve(expected.size());
  for (const unsigned char c : expected) {
    if (!std::isxdigit(c)) return false;
    lower.push_back(static_cast<char>(std::tolower(c)));
  }
  return actual.starts_with(lower);
}

// Closest managed file paths to a mistyped target, so an unknown `--file` names
// its own candidates instead of costing another round trip.
std::vector<std::string> SuggestFiles(const Project& project, const std::string& target,
                                      size_t limit = 3) {
  std::vector<std::string> candidates;
  const auto last = target.find_last_of('/');
  const std::string leaf = last == std::string::npos ? target : target.substr(last + 1);
  if (leaf.empty()) return candidates;
  for (const auto& [path, file] : project.files) {
    (void)file;
    if (path.find(leaf) != std::string::npos) candidates.push_back(path);
  }
  std::sort(candidates.begin(), candidates.end());
  if (candidates.size() > limit) candidates.resize(limit);
  return candidates;
}

// Names the roles (and their current short versions) that do contain a fragment,
// so an edit aimed at the wrong role can be retried immediately without an extra
// `show` round trip.
std::string FragmentSuggestion(const Project& project, const std::string& fragment) {
  if (fragment.size() < 3) return "";
  std::string suggestion;
  size_t found = 0;
  for (const auto& [id, node] : project.blocks) {
    if (!node.text.contains(fragment)) continue;
    if (found++ == 3) break;
    const std::string alias = ShortestRoleAlias(project, id);
    const std::string short_ver = node.version.substr(0, 12);
    if (alias == id) {
      suggestion += " " + id + " [v:" + short_ver + "]";
    } else {
      suggestion += " " + id + " (" + alias + " [v:" + short_ver + "])";
    }
  }
  if (found == 0) return "";
  return "; this fragment appears in:" + suggestion;
}

std::string UnknownFileMessage(const Project& project, const std::string& target) {
  std::string message = "Unknown file: " + target;
  const auto suggestions = SuggestFiles(project, target);
  if (!suggestions.empty()) {
    message += "; did you mean:";
    for (const auto& path : suggestions) message += " " + path;
  } else {
    message += "; run `tfe-code ls` to list managed files";
  }
  return message;
}

// Extracts non-placeholder lines from a container role while collapsing gaps left
// where `{{child}}` references were separated by blank lines.
std::string OwnLines(const Node& node) {
  std::string own;
  bool last_blank = true;  // also skips leading blank lines
  for (const auto& line_view : std::views::split(node.text, '\n')) {
    std::string line(line_view.begin(), line_view.end());
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.contains("{{")) continue;
    const bool blank = line.find_first_not_of(" \t") == std::string::npos;
    if (blank) {
      if (last_blank) continue;
      last_blank = true;
      if (!own.empty()) own += '\n';
      continue;
    }
    last_blank = false;
    if (!own.empty()) own += '\n';
    own += line;
  }
  while (!own.empty() && (own.back() == '\n' || own.back() == '\r')) own.pop_back();
  return own;
}

std::string NoMatchMessage(const Project& project,
                           const std::vector<std::string>& terms,
                           const std::set<std::string>& file_roles,
                           bool has_file_filter) {
  std::set<std::string> words;
  for (const auto& term : terms) {
    std::string word;
    for (const char c : term + " ") {
      if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
        word.push_back(c);
        continue;
      }
      if (word.size() >= 3) words.insert(word);
      word.clear();
    }
  }
  std::vector<std::pair<size_t, std::string>> ranked;
  for (const auto& [id, node] : project.blocks) {
    if (has_file_filter && !file_roles.contains(id)) continue;
    size_t hits = 0;
    for (const auto& word : words) {
      if (id.contains(word)) hits += 3;
      else if (node.text.contains(word)) hits += 1;
    }
    if (hits > 0) ranked.emplace_back(hits, id);
  }
  std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first > b.first : a.second < b.second;
  });
  std::string message = "no matching roles for";
  for (const auto& term : terms) message += " '" + term + "'";
  if (!ranked.empty()) {
    message += "; try a literal identifier from:";
    for (size_t i = 0; i < ranked.size() && i < 5; ++i) message += " " + ranked[i].second;
  }
  return message;
}

constexpr char kDefaultIgnoreRules[] =
    "# TFEngine Code recursive-import exclusions\n"
    ".git/\n"
    ".tfe-code/\n"
    ".venv/\n"
    "venv/\n"
    "env/\n"
    "__pycache__/\n"
    "build/\n"
    "dist/\n"
    "node_modules/\n";

struct IgnoreRule {
  bool directory_only;
  bool component;
  std::regex pattern;
};

std::regex IgnorePattern(const std::string& pattern, bool directory_only) {
  static const std::string special = R"(\.^$|()[]{}+?)";
  std::string source = "^";
  for (size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] == '*') {
      if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
        source += ".*";
        ++i;
      } else {
        source += "[^/]*";
      }
    } else {
      if (special.contains(pattern[i])) source += '\\';
      source += pattern[i];
    }
  }
  source += directory_only ? "(/.*)?$" : "$";
  return std::regex(source);
}

std::vector<IgnoreRule> ParseIgnoreRules(const std::string& text) {
  std::vector<IgnoreRule> rules;
  for (const auto& line_view : std::views::split(text, '\n')) {
    std::string line(line_view.begin(), line_view.end());
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos) continue;
    const auto last = line.find_last_not_of(" \t\r");
    line = line.substr(first, last - first + 1);
    if (line.starts_with('#')) continue;
    if (line.starts_with('!')) throw std::runtime_error("ignore negation is unsupported: " + line);
    std::ranges::replace(line, '\\', '/');
    const bool directory_only = line.ends_with('/');
    if (directory_only) line.pop_back();
    if (line.starts_with('/')) line.erase(line.begin());
    if (line.empty()) throw std::runtime_error("ignore rule has no path");
    const bool component = !line.contains('/');
    rules.push_back({directory_only, component, IgnorePattern(line, directory_only)});
  }
  return rules;
}

bool IsIgnored(const std::vector<IgnoreRule>& rules,
               const std::filesystem::path& relative,
               bool is_directory) {
  const std::string path = relative.generic_string();
  for (const auto& rule : rules) {
    if (!rule.component) {
      if (std::regex_match(path, rule.pattern)) return true;
      continue;
    }
    std::vector<std::string> components;
    for (const auto& part : relative) components.push_back(part.string());
    for (size_t i = 0; i < components.size(); ++i) {
      if (rule.directory_only && !is_directory && i + 1 == components.size()) continue;
      if (std::regex_match(components[i], rule.pattern)) return true;
    }
  }
  return false;
}

std::vector<IgnoreRule> LoadIgnoreRules(const std::filesystem::path& root) {
  const auto path = root / ".tfe-codeignore";
  return ParseIgnoreRules(std::filesystem::exists(path) ? ReadBinary(path) : kDefaultIgnoreRules);
}

std::vector<std::string> ExpandImportPaths(const std::filesystem::path& root,
                                           const std::vector<std::string>& inputs,
                                           bool allow_empty = false) {
  std::optional<std::vector<IgnoreRule>> rules;
  std::set<std::string> expanded;
  for (const auto& name : inputs) {
    const auto input = SafePath(root, name, false);
    if (std::filesystem::is_directory(input)) {
      if (!rules) rules = LoadIgnoreRules(root);
      for (std::filesystem::recursive_directory_iterator it(input), end; it != end; ++it) {
        const auto relative = std::filesystem::relative(it->path(), root);
        if (it->is_directory()) {
          // Every dot-directory is out of scope: `.tfe-code` holds state, while
          // `.git`, `.worktrees`, `.cache` or a virtualenv hold checkouts and
          // generated trees rather than managed sources.
          const auto name = it->path().filename().string();
          const bool hidden = name.size() > 1 && name.front() == '.';
          if (hidden || IsIgnored(*rules, relative, true)) it.disable_recursion_pending();
          continue;
        }
        if (it->is_regular_file() && IsSupportedSourceFile(relative.generic_string()) &&
            !IsIgnored(*rules, relative, false)) {
          expanded.insert(relative.generic_string());
        }
      }
    } else if (std::filesystem::is_regular_file(input) && IsSupportedSourceFile(input.generic_string())) {
      expanded.insert(std::filesystem::relative(input, root).generic_string());
    } else {
      throw std::runtime_error("Expected a relative source file or directory: " + name);
    }
  }
  if (expanded.empty() && !allow_empty) throw std::runtime_error("import found no source files");
  return {expanded.begin(), expanded.end()};
}

std::string Serialize(const Project& project) { return rfl::json::write(project); }
std::string Serialize(const State& state) { return rfl::json::write(state); }
std::string Serialize(const TransactionJournal& journal) {
  // rfl omits a null std::optional field, so a path that did not exist before the
  // commit would be written as a bare {"path": ...}. Recovery reads the journal
  // with NoOptionals, which requires the field, so such a journal would always be
  // rejected as malformed. Emit an explicit JSON null instead.
  std::string text = rfl::json::write(journal);
  std::string patched;
  patched.reserve(text.size() + journal.entries.size() * 18);
  size_t cursor = 0;
  for (const auto& entry : journal.entries) {
    if (entry.original) continue;
    const std::string needle = "{\"path\":\"" + entry.path + "\"}";
    const size_t at = text.find(needle, cursor);
    if (at == std::string::npos) continue;
    patched.append(text, cursor, at - cursor);
    patched += "{\"path\":\"" + entry.path + "\",\"original\":null}";
    cursor = at + needle.size();
  }
  patched.append(text, cursor, std::string::npos);
  return patched;
}

template <typename T>
T Parse(const std::string& bytes, const std::string& description,
        bool default_if_missing = false) {
  if (default_if_missing) {
    const auto parsed = rfl::json::read<T, rfl::DefaultIfMissing>(bytes);
    if (!parsed) throw std::runtime_error("Invalid " + description + ": " + parsed.error().what());
    return parsed.value();
  }
  const auto parsed = rfl::json::read<T>(bytes);
  if (!parsed) throw std::runtime_error("Invalid " + description + ": " + parsed.error().what());
  return parsed.value();
}

std::map<std::string, std::string> Hashes(
    const std::map<std::string, std::string>& values) {
  std::map<std::string, std::string> result;
  for (const auto& [name, bytes] : values) result[name] = Sha256(bytes);
  return result;
}

void RemoveFileRoles(Project& project, const std::string& path) {
  const auto file = project.files.find(path);
  if (file == project.files.end()) return;
  // Walk the subtree leniently. A role text may keep a marker that names no
  // role at all (a string literal in the source, for example), and that dangling
  // reference must not leave the file's other roles behind: the orphans would
  // later be rejected as a role id collision on the next import or sync.
  std::vector<std::string> pending{file->second.root};
  std::set<std::string> seen;
  while (!pending.empty()) {
    const std::string id = pending.back();
    pending.pop_back();
    if (!seen.insert(id).second) continue;
    const auto found = project.blocks.find(id);
    if (found == project.blocks.end()) continue;
    const auto children = RoleChildren(found->second);
    project.blocks.erase(found);
    pending.insert(pending.end(), children.begin(), children.end());
  }
  project.files.erase(file);
}

void MergeImported(Project& candidate, const std::string& path, Project imported) {
  RemoveFileRoles(candidate, path);
  candidate.files[path] = imported.files.at(path);
  for (auto& [id, node] : imported.blocks) {
    if (candidate.blocks.contains(id)) {
      throw std::runtime_error("Role id collision with another imported file: " + id);
    }
    candidate.blocks[id] = std::move(node);
  }
}

void RefreshSourceRanges(Project& candidate,
                         const std::map<std::string, std::string>& rendered) {
  for (auto& [id, node] : candidate.blocks) {
    node.source_start_line = 0;
    node.source_end_line = 0;
  }
  for (const auto& [path, bytes] : rendered) {
    const auto imported = ImportSource(path, bytes);
    for (const auto& [id, node] : imported.blocks) {
      const auto existing = candidate.blocks.find(id);
      if (existing == candidate.blocks.end()) continue;
      existing->second.source_start_line = node.source_start_line;
      existing->second.source_end_line = node.source_end_line;
    }
  }
}

std::string ReceiptKey(const std::string& project,
                       const std::map<std::string, std::string>& rendered,
                       const std::string& command) {
  std::string input = project + "\n" + command;
  for (const auto& [name, bytes] : rendered) input += "\n" + name + "\n" + bytes;
  return Sha256(input);
}

// Returns the check command's exit status so callers can report it. A piped
// shell command hides a failure behind the pipeline's own status, and an agent
// that cannot see the code assumes the batch passed.
int RunCheck(const std::string& command, const std::filesystem::path& root) {
  const pid_t child = fork();
  if (child < 0) throw std::runtime_error("Cannot start check process");
  if (child == 0) {
    if (chdir(root.c_str()) != 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) _exit(126);
    execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) throw std::runtime_error("Cannot wait for check process");
  }
  if (!WIFEXITED(status)) return 128 + WTERMSIG(status);
  return WEXITSTATUS(status);
}

uint64_t EstimateTokens(uint64_t bytes) {
  return bytes == 0 ? 0 : (bytes + 3) / 4;
}

uint64_t TotalSourceBytes(const std::map<std::string, std::string>& sources,
                          const std::vector<std::string>& files = {}) {
  uint64_t total = 0;
  if (files.empty()) {
    for (const auto& [path, bytes] : sources) total += bytes.size();
  } else {
    std::set<std::string> seen;
    for (const auto& file : files) {
      if (!seen.insert(file).second) continue;
      const auto it = sources.find(file);
      if (it != sources.end()) total += it->second.size();
    }
  }
  return total;
}

std::string FindOwningFile(const Project& project, const std::string& role_id) {
  for (const auto& [path, file] : project.files) {
    const std::string scope =
        file.root.ends_with(".file") ? file.root.substr(0, file.root.size() - 5) : file.root;
    if (role_id == file.root || role_id.starts_with(scope + ".")) return path;
  }
  return "";
}

std::string DetectPlaceholderIndent(const std::string& container_text,
                                    const std::string& parent_id,
                                    const std::string& file_path) {
  for (const auto& line_view : std::views::split(container_text, '\n')) {
    std::string line(line_view.begin(), line_view.end());
    const auto pos = line.find("{{");
    if (pos != std::string::npos && pos > 0 &&
        line.substr(0, pos).find_first_not_of(" \t") == std::string::npos) {
      return line.substr(0, pos);
    }
  }
  if (parent_id.ends_with(".file")) return "";
  if (file_path.ends_with(".py") || file_path.ends_with(".rs")) return "    ";
  if (file_path.ends_with(".go")) return "\t";
  return "  ";
}

std::string InsertChildPlaceholder(const std::string& parent_text,
                                   const std::string& child_id,
                                   const std::string& after_id,
                                   const std::string& indent,
                                   const std::string& before_id) {
  // Class containers keep the `;` on the marker line; the new marker inherits
  // it from the anchor sibling when one is given, or from the all-siblings
  // convention when appending (every existing marker must agree).
  std::string suffix;
  const auto marker_punctuation = [&parent_text](size_t marker_end) {
    size_t after = marker_end;
    while (after < parent_text.size() &&
           (parent_text[after] == ' ' || parent_text[after] == '\t')) {
      ++after;
    }
    return after < parent_text.size() && parent_text[after] == ';';
  };
  if (!after_id.empty() || !before_id.empty()) {
    const std::string anchor = "{{" + (after_id.empty() ? before_id : after_id) + "}}";
    const auto anchor_pos = parent_text.find(anchor);
    if (anchor_pos != std::string::npos &&
        marker_punctuation(anchor_pos + anchor.size())) {
      suffix = ";";
    }
  } else {
    bool saw_marker = false;
    bool all_semicolon = true;
    for (size_t pos = parent_text.find("{{"); pos != std::string::npos;
         pos = parent_text.find("{{", pos + 2)) {
      const auto end = parent_text.find("}}", pos);
      if (end == std::string::npos) break;
      saw_marker = true;
      if (!marker_punctuation(end + 2)) all_semicolon = false;
    }
    if (saw_marker && all_semicolon) suffix = ";";
  }
  const std::string placeholder = indent + "{{" + child_id + "}}" + suffix;
  if (!before_id.empty()) {
    const std::string anchor = "{{" + before_id + "}}";
    const auto pos = parent_text.find(anchor);
    if (pos == std::string::npos) {
      throw std::runtime_error("Parent role does not contain before-role placeholder: " +
                               before_id);
    }
    const auto newline = parent_text.rfind('\n', pos);
    const size_t line_start = newline == std::string::npos ? 0 : newline + 1;
    return parent_text.substr(0, line_start) + placeholder + "\n" +
           parent_text.substr(line_start);
  }
  if (!after_id.empty()) {
    const std::string target = "{{" + after_id + "}}";
    const auto pos = parent_text.find(target);
    if (pos == std::string::npos) {
      throw std::runtime_error("Parent role does not contain after-role placeholder: " + after_id);
    }
    const auto line_end = parent_text.find('\n', pos + target.size());
    if (line_end == std::string::npos) {
      return parent_text + "\n\n" + placeholder;
    }
    return parent_text.substr(0, line_end + 1) + "\n" + placeholder + "\n" +
           parent_text.substr(line_end + 1);
  }
  // Appending past a trailing `private:` / `protected:` would flip the new
  // child into a section its siblings do not use; land above the specifier.
  for (const char* specifier : {"private:", "protected:"}) {
    const auto spec = parent_text.rfind(specifier);
    if (spec == std::string::npos) continue;
    const auto newline = parent_text.rfind('\n', spec);
    const size_t line_start = newline == std::string::npos ? 0 : newline + 1;
    const size_t line_end = parent_text.find('\n', spec);
    const std::string line = parent_text.substr(
        line_start,
        (line_end == std::string::npos ? parent_text.size() : line_end) - line_start);
    if (line != indent + specifier) continue;
    const auto first_marker = parent_text.find("{{");
    if (first_marker == std::string::npos || first_marker > line_start) continue;
    return parent_text.substr(0, line_start) + placeholder + "\n" +
           parent_text.substr(line_start);
  }
  // Check if container ends with a closing-brace line; a trailing namespace
  // comment (`}  // namespace demo`) still counts as one.
  size_t close_line_start = std::string::npos;
  size_t scan_end = parent_text.size();
  while (scan_end > 0) {
    const auto newline = parent_text.rfind('\n', scan_end - 1);
    if (newline == std::string::npos) {
      // The first line has no leading newline to skip past.
      const std::string line = parent_text.substr(0, scan_end);
      const auto first = line.find_first_not_of(" \t\r");
      if (first != std::string::npos && line[first] != '/') {
        const auto last = line.find_last_not_of(" \t\r");
        if (line[last] == '}' ||
            (line[last] == ';' && last > 0 && line[last - 1] == '}')) {
          close_line_start = 0;
        }
      }
      break;
    }
    const size_t line_start = newline + 1;
    const std::string line = parent_text.substr(line_start, scan_end - line_start);
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos || line[first] == '/') {
      scan_end = newline;
      continue;
    }
    // A trailing comment (`}  // namespace demo`) does not change the line's
    // closing-brace role, so judge the code part only.
    const auto comment = line.find("//");
    const std::string code =
        line.substr(0, comment == std::string::npos ? std::string::npos : comment);
    const auto last = code.find_last_not_of(" \t\r");
    if (last != std::string::npos &&
        (code[last] == '}' || (code[last] == ';' && last > 0 && code[last - 1] == '}'))) {
      close_line_start = line_start;
    }
    break;
  }
  if (close_line_start != std::string::npos) {
    std::string prefix = parent_text.substr(0, close_line_start);
    if (!prefix.ends_with("\n\n") && prefix.find('{') != std::string::npos) {
      prefix += "\n";
    }
    return prefix + placeholder + "\n" + parent_text.substr(close_line_start);
  }
  if (parent_text.empty()) return placeholder + "\n";
  if (parent_text.ends_with("\n\n")) return parent_text + placeholder + "\n";
  if (parent_text.ends_with("\n")) return parent_text + "\n" + placeholder + "\n";
  return parent_text + "\n\n" + placeholder;
}

std::string RemoveChildPlaceholder(const std::string& parent_text, const std::string& child_id) {
  const std::string token = "{{" + child_id + "}}";
  const auto pos = parent_text.find(token);
  if (pos == std::string::npos) {
    throw std::runtime_error("Parent role does not reference child role: " + child_id);
  }
  size_t line_start = parent_text.rfind('\n', pos);
  line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
  size_t line_end = parent_text.find('\n', pos + token.size());
  const std::string before_on_line = parent_text.substr(line_start, pos - line_start);
  const std::string after_on_line =
      line_end == std::string::npos
          ? parent_text.substr(pos + token.size())
          : parent_text.substr(pos + token.size(), line_end - (pos + token.size()));
  if (before_on_line.find_first_not_of(" \t\r") == std::string::npos &&
      after_on_line.find_first_not_of(" \t\r") == std::string::npos) {
    size_t erase_start = line_start;
    size_t erase_end = (line_end == std::string::npos) ? parent_text.size() : line_end + 1;
    // Also collapse one adjacent blank line if present
    if (erase_end < parent_text.size() && parent_text[erase_end] == '\n') {
      ++erase_end;
    } else if (erase_start >= 2 && parent_text[erase_start - 1] == '\n' &&
               parent_text[erase_start - 2] == '\n') {
      --erase_start;
    }
    std::string out = parent_text;
    out.erase(erase_start, erase_end - erase_start);
    return out;
  }
  std::string out = parent_text;
  out.erase(pos, token.size());
  return out;
}

std::string FindParentRole(const Project& project, const std::string& child_id) {
  const std::string token = "{{" + child_id + "}}";
  for (const auto& [id, node] : project.blocks) {
    if (node.text.contains(token)) return id;
  }
  return "";
}

}  // namespace

Workspace::Workspace(std::filesystem::path root) : root_(std::move(root)) {
  std::filesystem::create_directories(root_);
  const char* index = std::getenv("TFE_CODE_INDEX");
  use_objectbox_ = index != nullptr && std::string(index) == "objectbox";
}

std::filesystem::path Workspace::ObjectBoxDir() const {
  return root_ / ".tfe-code" / "db";
}

void Workspace::SyncObjectBoxIndex(const Project& project) const {
  if (!use_objectbox_) return;
  ObjectBoxRoleStore store(ObjectBoxDir());
  store.Save(project);
}

void Workspace::RecordTelemetry(const std::string& command, uint64_t baseline_bytes,
                                uint64_t emitted_bytes) const {
  try {
    const auto path = StorePath("telemetry.json");
    TelemetryReport report;
    if (std::filesystem::exists(path)) {
      const auto parsed = rfl::json::read<TelemetryReport, rfl::DefaultIfMissing>(ReadBinary(path));
      if (parsed) report = parsed.value();
    }
    const uint64_t base_tok = EstimateTokens(baseline_bytes);
    const uint64_t emit_tok = EstimateTokens(emitted_bytes);
    const uint64_t saved_b = baseline_bytes > emitted_bytes ? (baseline_bytes - emitted_bytes) : 0;
    const uint64_t saved_t = base_tok > emit_tok ? (base_tok - emit_tok) : 0;

    report.total_calls += 1;
    report.baseline_bytes += baseline_bytes;
    report.emitted_bytes += emitted_bytes;
    report.saved_bytes += saved_b;
    report.baseline_tokens += base_tok;
    report.emitted_tokens += emit_tok;
    report.saved_tokens += saved_t;
    report.savings_percent =
        report.baseline_bytes > 0
            ? (static_cast<double>(report.saved_bytes) * 100.0 /
               static_cast<double>(report.baseline_bytes))
            : 0.0;

    auto& cmd = report.by_command[command];
    cmd.calls += 1;
    cmd.baseline_bytes += baseline_bytes;
    cmd.emitted_bytes += emitted_bytes;
    cmd.saved_bytes += saved_b;
    cmd.baseline_tokens += base_tok;
    cmd.emitted_tokens += emit_tok;
    cmd.saved_tokens += saved_t;

    AtomicWrite(path, rfl::json::write(report));
  } catch (...) {
    // Telemetry must never fail user commands.
  }
}

void Workspace::CreateIgnoreFile() {
  RecoverIfNeeded();
  const auto path = root_ / ".tfe-codeignore";
  if (std::filesystem::exists(path)) {
    throw std::runtime_error("Ignore file already exists: .tfe-codeignore");
  }
  WriteBinary(path, kDefaultIgnoreRules);
}

std::filesystem::path Workspace::StorePath(const std::string& name) const {
  return root_ / ".tfe-code" / name;
}

void Workspace::RecoverIfNeeded() const {
  const auto journal_path = StorePath("transaction.json");
  if (!std::filesystem::exists(journal_path)) return;
  Lock lock(StorePath("lock"));
  if (!std::filesystem::exists(journal_path)) return;
  const auto parsed = rfl::json::read<TransactionJournal, rfl::NoOptionals, rfl::NoExtraFields>(
      ReadBinary(journal_path));
  if (!parsed) throw std::runtime_error("Invalid transaction journal: " + parsed.error().what());
  const auto& journal = parsed.value();
  if (journal.schema != 1) throw std::runtime_error("Unsupported transaction journal schema");
  std::vector<std::pair<std::filesystem::path, std::optional<std::string>>> originals;
  std::set<std::filesystem::path> paths;
  bool has_project = false;
  bool has_state = false;
  for (const auto& entry : journal.entries) {
    const auto path = JournalEntryPath(root_, entry.path);
    if (!paths.insert(path).second) {
      throw std::runtime_error("Duplicate path in transaction journal: " + entry.path);
    }
    has_project = has_project || entry.path == ".tfe-code/project.json";
    has_state = has_state || entry.path == ".tfe-code/state.json";
    originals.emplace_back(path, entry.original);
  }
  if (!has_project || !has_state) {
    throw std::runtime_error("Transaction journal is missing project or state");
  }
  for (auto it = originals.rbegin(); it != originals.rend(); ++it) {
    if (it->second) AtomicWrite(it->first, *it->second);
    else std::filesystem::remove(it->first);
  }
  std::filesystem::remove(journal_path);
}

Workspace::Snapshot Workspace::LoadForImport() const {
  Snapshot result;
  if (!std::filesystem::exists(StorePath("project.json"))) {
    // Database-authoritative mode: the role tree lives only in ObjectBox and the
    // source files are rendered output. Nothing is imported yet when the store is
    // empty, which keeps a fresh workspace distinguishable from an indexed one.
    if (!use_objectbox_) return result;
    ObjectBoxRoleStore store(ObjectBoxDir());
    if (!store.Exists()) return result;
    result.project = store.Load();
    result.serialized_project = Serialize(result.project);
    result.state.schema = 1;
    result.state.project_hash = Sha256(result.serialized_project);
    // Only files that were already materialised take part in consistency checks;
    // a database-only workspace has none yet and that is not a divergence.
    for (const auto& [path, entry] : result.project.files) {
      const auto source = SafePath(root_, path);
      if (std::filesystem::exists(source)) result.sources[path] = ReadBinary(source);
    }
    result.state.source_hashes = Hashes(result.sources);
    result.state.render_hashes = result.state.source_hashes;
    return result;
  }
  const auto state = Parse<State>(ReadBinary(StorePath("state.json")), "workspace state");
  if (state.schema != 1) throw std::runtime_error("Unsupported workspace schema");
  result.serialized_project = ReadBinary(StorePath("project.json"));
  if (Sha256(result.serialized_project) != state.project_hash) {
    throw std::runtime_error("Workspace project changed outside the CLI");
  }
  result.project = Parse<Project>(result.serialized_project, "project", true);
  result.state = state;
  for (const auto& [path, entry] : result.project.files) {
    const auto source = SafePath(root_, path);
    if (!std::filesystem::exists(source)) {
      // Database-authoritative mode treats sources as rendered output, so a file
      // that was never materialised is pending output rather than an error.
      if (use_objectbox_) continue;
      throw std::runtime_error("Source missing; run sync: " + path);
    }
    result.sources[path] = ReadBinary(source);
    const auto expected = state.source_hashes.find(path);
    if (expected != state.source_hashes.end() &&
        Sha256(result.sources[path]) != expected->second) {
      throw std::runtime_error("Source changed; run sync: " + path);
    }
  }
  return result;
}

Workspace::Snapshot Workspace::LoadConsistent(bool recover) const {
  if (recover) RecoverIfNeeded();
  auto result = LoadForImport();
  if (result.serialized_project.empty()) throw std::runtime_error("Workspace is not imported");
  const auto rendered = RenderFiles(result.project);
  for (const auto& [path, bytes] : rendered) {
    const auto found = result.sources.find(path);
    // A managed file that was never materialised is pending output, not a
    // divergence. A file that exists must still match the snapshot exactly.
    if (found == result.sources.end()) continue;
    const auto hash = result.state.render_hashes.find(path);
    if (found->second != bytes || hash == result.state.render_hashes.end() ||
        Sha256(bytes) != hash->second) {
      throw std::runtime_error("Source diverges from TF snapshot; run sync: " + path);
    }
  }
  if (use_objectbox_) {
    // Serve the tree from the engine database. project.json stays the crash
    // recovery mirror; when the two disagree the database is rebuilt from it so
    // a stale index is never presented as the current tree.
    try {
      ObjectBoxRoleStore store(ObjectBoxDir());
      if (store.Exists()) {
        auto from_db = store.Load();
        if (Sha256(Serialize(from_db)) == result.state.project_hash) {
          result.project = std::move(from_db);
        }
      }
    } catch (const std::exception&) {
      // An unreadable database must not block reads; rebuild below instead.
    }
  }
  return result;
}

Project Workspace::LoadProject() const {
  // Read-only on purpose: a const accessor must not publish to the database, and
  // that write would also run outside the workspace lock. A stale index is served
  // as the mirror's content by LoadConsistent and republished by the next Commit.
  return LoadConsistent().project;
}
State Workspace::LoadState() const { return LoadConsistent().state; }
std::map<std::string, std::string> Workspace::Render() {
  return RenderFiles(LoadConsistent().project);
}

std::map<std::string, std::string> Workspace::Materialize() {
  const auto snapshot = LoadConsistent();
  const auto rendered = RenderFiles(snapshot.project);
  std::map<std::string, std::string> written;
  for (const auto& [path, bytes] : rendered) {
    const auto target = SafePath(root_, path);
    if (std::filesystem::exists(target) && ReadBinary(target) == bytes) continue;
    AtomicWrite(target, bytes);
    written[path] = bytes;
  }
  return written;
}

ApplyResult Workspace::Import(const std::vector<std::string>& paths) {
  if (paths.empty()) throw std::runtime_error("import expects at least one source file or directory");
  RecoverIfNeeded();
  std::filesystem::create_directories(StorePath(""));
  Lock lock(StorePath("lock"));
  const auto expanded = ExpandImportPaths(root_, paths);
  auto before = LoadForImport();
  Project candidate = before.project;
  std::vector<std::string> degraded;
  for (const auto& path : expanded) {
    const auto bytes = ReadBinary(SafePath(root_, path));
    auto imported = ImportSource(path, bytes, &degraded);
    if (RenderFiles(imported).at(path) != bytes) {
      throw std::runtime_error("Lossless import verification failed: " + path);
    }
    MergeImported(candidate, path, std::move(imported));
  }
  const auto rendered = RenderFiles(candidate);
  for (const auto& path : expanded) {
    if (rendered.at(path) != ReadBinary(SafePath(root_, path))) {
      throw std::runtime_error("Imported project does not reproduce source: " + path);
    }
  }
  auto result = Commit(std::move(before), std::move(candidate), rendered, std::nullopt, false);
  result.degraded = std::move(degraded);
  return result;
}

ApplyResult Workspace::Commit(Snapshot before, Project candidate,
                              const std::map<std::string, std::string>& rendered,
                              const std::optional<std::string>& check_command,
                              bool write_sources) {
  for (const auto& [path, bytes] : before.sources) {
    if (ReadBinary(SafePath(root_, path)) != bytes) {
      throw std::runtime_error("Concurrent source change; no files written: " + path);
    }
  }
  for (const auto& path : before.deleted_sources) {
    if (std::filesystem::exists(SafePath(root_, path))) {
      throw std::runtime_error("Concurrent source change; no files written: " + path);
    }
  }
  RefreshSourceRanges(candidate, rendered);
  const std::string serialized_project = Serialize(candidate);
  State state;
  state.project_hash = Sha256(serialized_project);
  state.source_hashes = Hashes(rendered);
  state.render_hashes = state.source_hashes;
  if (check_command) {
    state.receipt = Receipt{*check_command, ReceiptKey(serialized_project, rendered, *check_command), false};
  } else if (before.state.receipt && before.state.receipt->key ==
             ReceiptKey(serialized_project, rendered, before.state.receipt->command)) {
    state.receipt = before.state.receipt;
  }

  // Journal entries carry workspace-relative names, so keep the name next to the
  // absolute path instead of deriving it back from the path: source writes come
  // from SafePath(), which canonicalises them, and lexically_relative() against a
  // symlinked workspace root turns "a.py" into "../<real-root>/a.py", a spelling
  // that recovery then rejects as escaping the workspace.
  std::vector<std::pair<std::filesystem::path, std::string>> touched{
      {StorePath("project.json"), ".tfe-code/project.json"},
      {StorePath("state.json"), ".tfe-code/state.json"}};
  if (write_sources) {
    for (const auto& [path, bytes] : rendered) {
      if (!before.sources.contains(path) || before.sources.at(path) != bytes) {
        touched.emplace_back(SafePath(root_, path), path);
      }
    }
  }
  std::map<std::filesystem::path, std::optional<std::string>> original;
  std::map<std::filesystem::path, std::string> journal_names;
  for (const auto& [path, name] : touched) {
    journal_names[path] = name;
    if (std::filesystem::exists(path)) original[path] = ReadBinary(path);
    else original[path] = std::nullopt;
  }
  auto rollback = [&]() {
    for (const auto& [path, bytes] : original) {
      if (bytes) AtomicWrite(path, *bytes);
      else std::filesystem::remove(path);
    }
  };
  const auto journal_path = StorePath("transaction.json");
  TransactionJournal journal;
  for (const auto& [path, bytes] : original) {
    journal.entries.push_back({journal_names.at(path), bytes});
  }
  AtomicWrite(journal_path, Serialize(journal));
  try {
    if (write_sources) {
      for (const auto& [path, bytes] : rendered) {
        if (!before.sources.contains(path) || before.sources.at(path) != bytes) {
          const auto source = SafePath(root_, path);
          AtomicWrite(source, bytes);
          CrashAfterWrite(source);
        }
      }
    }
    AtomicWrite(StorePath("project.json"), serialized_project);
    CrashAfterWrite(StorePath("project.json"));
    AtomicWrite(StorePath("state.json"), Serialize(state));
    CrashAfterWrite(StorePath("state.json"));
    if (check_command) {
      const int check_exit = RunCheck(*check_command, root_);
      if (check_exit != 0) {
        throw std::runtime_error("Check failed with exit " + std::to_string(check_exit) +
                                 "; batch rolled back");
      }
      state.receipt->passed = true;
      AtomicWrite(StorePath("state.json"), Serialize(state));
      CrashAfterWrite(StorePath("state.json"));
    }
    // Publish only after the batch is fully validated. Publishing earlier leaves
    // a database-authoritative workspace holding roles that a failing check
    // rejected: the rollback below restores files, and there is no mirror to
    // rebuild the index from. A throw here rolls the database back through the
    // adapter's own transaction.
    SyncObjectBoxIndex(candidate);
    std::filesystem::remove(journal_path);
  } catch (...) {
    try {
      rollback();
      std::filesystem::remove(journal_path);
    } catch (...) {
      std::throw_with_nested(
          std::runtime_error("Rollback failed; recovery journal retained; rerun a native command"));
    }
    throw;
  }
  ApplyResult result;
  const std::map<std::string, std::string> before_rendered =
      use_objectbox_ ? RenderFiles(before.project) : std::map<std::string, std::string>{};
  std::set<std::string> changed = before.deleted_sources;
  for (const auto& [path, bytes] : before.sources) {
    if (!rendered.contains(path)) changed.insert(path);
  }
  for (const auto& [path, bytes] : rendered) {
    if (before.sources.contains(path)) {
      if (!before.state.source_hashes.contains(path) || before.sources.at(path) != bytes ||
          before.state.source_hashes.at(path) != state.source_hashes.at(path)) {
        changed.insert(path);
      }
    } else if (!use_objectbox_ || !before_rendered.contains(path) ||
               before_rendered.at(path) != bytes) {
      changed.insert(path);
    }
  }
  result.changed.assign(changed.begin(), changed.end());
  result.checked = check_command.has_value();
  return result;
}

ApplyResult Workspace::Apply(const std::vector<RoleEdit>& edits,
                             const std::optional<std::string>& check_command,
                             bool amend) {
  if (!amend && edits.empty()) throw std::runtime_error("apply expects a nonempty edit batch");
  RecoverIfNeeded();
  Lock lock(StorePath("lock"));
  auto before = LoadConsistent(false);
  const auto draft_path = StorePath("last_failed_batch.json");

  auto canonical_key = [&](const std::string& raw) -> std::string {
    const auto resolved = TryResolveRoleId(before.project, raw);
    return resolved.id.empty() ? raw : resolved.id;
  };

  std::vector<RoleEdit> effective_edits;
  if (amend) {
    if (!std::filesystem::exists(draft_path)) {
      throw std::runtime_error("No failed batch to amend; send a full apply batch");
    }
    const auto saved = Parse<SavedBatch>(ReadBinary(draft_path), "failed batch draft", true);
    for (const auto& item : saved.edits) {
      RoleEdit restored{item.role, item.expected_version, item.old, item.new_text, {}, false};
      for (const auto& rep : item.replace) restored.replace.emplace_back(rep.old, rep.new_text);
      effective_edits.push_back(std::move(restored));
    }
    for (const auto& patch : edits) {
      const std::string key = canonical_key(patch.role);
      auto it = std::ranges::find_if(effective_edits, [&](const RoleEdit& existing) {
        return canonical_key(existing.role) == key;
      });
      if (patch.drop) {
        if (it != effective_edits.end()) effective_edits.erase(it);
      } else if (it != effective_edits.end()) {
        *it = patch;
      } else {
        effective_edits.push_back(patch);
      }
    }
  } else {
    effective_edits = edits;
  }

  if (effective_edits.empty()) throw std::runtime_error("apply expects a nonempty edit batch");

  auto save_draft = [&]() {
    if (!std::filesystem::exists(StorePath("project.json")) &&
        !std::filesystem::exists(ObjectBoxDir())) {
      return;
    }
    SavedBatch out;
    for (const auto& item : effective_edits) {
      SavedRoleEdit entry{item.role, item.expected_version, item.old, item.new_text, {}};
      for (const auto& [from, to] : item.replace) entry.replace.push_back({from, to});
      out.edits.push_back(std::move(entry));
    }
    AtomicWrite(draft_path, rfl::json::write(out));
  };

  try {
    Project candidate = before.project;
    // Merging the output of several prepare-batch calls produces byte-identical
    // duplicates, which are pure noise. Collapse those, but still reject two
    // different edits to one role: that is an ambiguous batch.
    std::vector<RoleEdit> unique;
    std::map<std::string, size_t> first_seen;
    for (const auto& edit : effective_edits) {
      const std::string key = canonical_key(edit.role);
      const auto found = first_seen.find(key);
      if (found == first_seen.end()) {
        first_seen.emplace(key, unique.size());
        unique.push_back(edit);
        continue;
      }
      const auto& previous = unique[found->second];
      if (previous.expected_version != edit.expected_version || previous.old != edit.old ||
          previous.new_text != edit.new_text || previous.replace != edit.replace) {
        throw std::runtime_error("Conflicting edits for one role in a batch: " + edit.role);
      }
    }

    std::vector<std::string> errors;
    for (const auto& edit : unique) {
      // Validate the shape before anything else: an empty `old` is a field error and
      // must be reported as one even when the version is also stale.
      if (!edit.replace.empty()) {
        if (!edit.old.empty() || !edit.new_text.empty()) {
          errors.push_back("Use either old/new or replace for role " + edit.role + ", not both");
          continue;
        }
        bool empty_from = false;
        for (const auto& [from, to] : edit.replace) {
          (void)to;
          if (from.empty()) {
            errors.push_back("replacement old must be nonempty: " + edit.role);
            empty_from = true;
            break;
          }
        }
        if (empty_from) continue;
      } else if (edit.old.empty()) {
        errors.push_back("old must be nonempty: " + edit.role);
        continue;
      }

      const auto resolved = TryResolveRoleId(before.project, edit.role);
      if (!resolved.error.empty()) {
        errors.push_back(resolved.error);
        continue;
      }
      const auto found = candidate.blocks.find(resolved.id);
      if (!MatchesVersion(found->second.version, edit.expected_version)) {
        errors.push_back("Stale role version; show it again: " + edit.role + " (current: " +
                         found->second.version.substr(0, 12) + ")");
        continue;
      }
      std::string text = found->second.text;
      bool role_failed = false;
      if (!edit.replace.empty()) {
        for (const auto& [from, to] : edit.replace) {
          const auto at = text.find(from);
          if (at == std::string::npos) {
            errors.push_back("replacement matched 0 times in role " + resolved.id + ": " + from +
                             FragmentSuggestion(before.project, from));
            role_failed = true;
            continue;
          }
          if (text.find(from, at + 1) != std::string::npos) {
            errors.push_back("replacement matched more than once in role " + resolved.id + ": " +
                             from);
            role_failed = true;
            continue;
          }
          text.replace(at, from.size(), to);
        }
      } else {
        size_t count = 0;
        for (size_t at = text.find(edit.old); at != std::string::npos;
             at = text.find(edit.old, at + edit.old.size())) {
          ++count;
        }
        if (count != 1) {
          errors.push_back("old matched " + std::to_string(count) + " times in role " +
                           resolved.id + "; expected exactly 1; copy old from show " + resolved.id +
                           " --json (role text is dedented)" +
                           (count == 0 ? FragmentSuggestion(before.project, edit.old) : ""));
          role_failed = true;
        } else {
          text.replace(text.find(edit.old), edit.old.size(), edit.new_text);
        }
      }
      if (role_failed) continue;
      found->second.text = std::move(text);
      found->second.version = Sha256(found->second.text);
      found->second.literal = RoleChildren(found->second).empty();
    }

    if (!errors.empty()) {
      std::string combined;
      for (size_t i = 0; i < errors.size(); ++i) {
        if (i > 0) combined += "\n";
        combined += errors[i];
      }
      throw std::runtime_error(combined);
    }

    const auto rendered = RenderFiles(candidate);
    // An edit must not break the syntax of a file that was imported with
    // structural roles: the fallback re-import in RefreshSourceRanges would
    // silently replace its whole role tree with one undivided file role.
    // Files degraded at import time stay degraded by design and are exempt.
    for (const auto& edit : unique) {
      const auto resolved = TryResolveRoleId(before.project, edit.role);
      if (resolved.id.empty()) continue;  // already reported as a role error
      const auto path = FindOwningFile(candidate, resolved.id);
      if (path.empty() || (!IsCppFile(path) && !IsTsFile(path))) continue;
      const auto root_it = candidate.blocks.find(candidate.files.at(path).root);
      if (root_it == candidate.blocks.end()) continue;
      if (RoleChildren(root_it->second).empty()) continue;
      if (IsCppFile(path) ? CppHasSyntaxError(rendered.at(path))
                          : TsHasSyntaxError(path, rendered.at(path))) {
        throw std::runtime_error("Edit breaks syntax; nothing written: " + path);
      }
    }
    uint64_t patch_bytes = 0;
    for (const auto& edit : unique) {
      patch_bytes += edit.role.size() + edit.old.size() + edit.new_text.size();
      for (const auto& [from, to] : edit.replace) patch_bytes += from.size() + to.size();
    }
    const auto before_sources = before.sources;
    auto result = Commit(std::move(before), std::move(candidate), rendered, check_command, true);
    RecordTelemetry("apply", TotalSourceBytes(before_sources, result.changed), patch_bytes);
    std::error_code ec;
    std::filesystem::remove(draft_path, ec);
    return result;
  } catch (...) {
    save_draft();
    throw;
  }
}

Receipt Workspace::Check(const std::string& command) {
  if (command.empty()) throw std::runtime_error("check expects a command");
  RecoverIfNeeded();
  Lock lock(StorePath("lock"));
  auto before = LoadConsistent(false);
  const std::string serialized = Serialize(before.project);
  Receipt receipt{command, ReceiptKey(serialized, before.sources, command), false};
  receipt.passed = RunCheck(command, root_) == 0;
  before.state.receipt = receipt;
  AtomicWrite(StorePath("state.json"), Serialize(before.state));
  CrashAfterWrite(StorePath("state.json"));
  return receipt;
}

ApplyResult Workspace::Sync() {
  RecoverIfNeeded();
  Lock lock(StorePath("lock"));
  if (!std::filesystem::exists(StorePath("project.json"))) {
    throw std::runtime_error("Workspace is not imported");
  }
  Snapshot before;
  before.serialized_project = ReadBinary(StorePath("project.json"));
  before.project = Parse<Project>(before.serialized_project, "project", true);
  before.state = Parse<State>(ReadBinary(StorePath("state.json")), "workspace state");
  const bool project_changed = Sha256(before.serialized_project) != before.state.project_hash;
  std::vector<std::string> changed;
  for (const auto& [path, entry] : before.project.files) {
    const auto source = SafePath(root_, path);
    if (!std::filesystem::exists(source)) {
      before.deleted_sources.insert(path);
      continue;
    }
    before.sources[path] = ReadBinary(source);
    if (Sha256(before.sources[path]) != before.state.source_hashes.at(path)) changed.push_back(path);
  }
  if (project_changed && (!changed.empty() || !before.deleted_sources.empty())) {
    throw std::runtime_error("Both project and source changed; automatic merge is not supported");
  }
  if (project_changed) throw std::runtime_error("Project index changed outside the CLI");
  Project candidate = before.project;
  std::vector<std::string> degraded;
  for (const auto& path : before.deleted_sources) RemoveFileRoles(candidate, path);
  if (!before.deleted_sources.empty()) {
    std::map<std::string, std::vector<std::string>> deleted_by_hash;
    for (const auto& path : before.deleted_sources) {
      deleted_by_hash[before.state.source_hashes.at(path)].push_back(path);
    }
    std::map<std::string, std::vector<std::string>> unmanaged_by_hash;
    std::map<std::string, std::string> unmanaged_sources;
    for (const auto& path : ExpandImportPaths(root_, {"."}, true)) {
      if (before.project.files.contains(path)) continue;
      auto bytes = ReadBinary(SafePath(root_, path));
      unmanaged_by_hash[Sha256(bytes)].push_back(path);
      unmanaged_sources[path] = std::move(bytes);
    }
    for (const auto& [hash, old_paths] : deleted_by_hash) {
      const auto matches = unmanaged_by_hash.find(hash);
      if (matches == unmanaged_by_hash.end()) continue;
      if (old_paths.size() != 1 || matches->second.size() != 1) {
        throw std::runtime_error("Ambiguous rename for content hash: " + hash);
      }
      const auto& new_path = matches->second.front();
      const auto& bytes = unmanaged_sources.at(new_path);
      auto imported = ImportSource(new_path, bytes, &degraded);
      if (RenderFiles(imported).at(new_path) != bytes) {
        throw std::runtime_error("Lossless sync failed: " + new_path);
      }
      MergeImported(candidate, new_path, std::move(imported));
      before.sources[new_path] = bytes;
    }
  }
  for (const auto& path : changed) {
    auto imported = ImportSource(path, before.sources.at(path), &degraded);
    if (RenderFiles(imported).at(path) != before.sources.at(path)) {
      throw std::runtime_error("Lossless sync failed: " + path);
    }
    MergeImported(candidate, path, std::move(imported));
  }
  const auto rendered = RenderFiles(candidate);
  for (const auto& [path, bytes] : rendered) {
    if (bytes != before.sources.at(path)) throw std::runtime_error("Sync would overwrite source: " + path);
  }
  auto result = Commit(std::move(before), std::move(candidate), rendered, std::nullopt, false);
  result.degraded = std::move(degraded);
  return result;
}

std::string Workspace::Tree(const std::string& target) {
  const auto snapshot = LoadConsistent();
  std::string root = target;
  if (snapshot.project.files.contains(target)) root = snapshot.project.files.at(target).root;
  return FormatRoles(snapshot.project, RoleSubtree(snapshot.project, root));
}

std::map<std::string, Node> Workspace::ShowRoles(const std::vector<std::string>& ids) {
  const auto snapshot = LoadConsistent();
  std::map<std::string, Node> result;
  for (const auto& id : ids) {
    const auto resolved = TryResolveRoleId(snapshot.project, id);
    if (!resolved.error.empty()) throw std::runtime_error(resolved.error);
    result[resolved.id] = snapshot.project.blocks.at(resolved.id);
  }
  return result;
}

std::string Workspace::Show(const std::vector<std::string>& ids) {
  const auto snapshot = LoadConsistent();
  std::vector<std::string> resolved_ids;
  resolved_ids.reserve(ids.size());
  for (const auto& id : ids) {
    const auto resolved = TryResolveRoleId(snapshot.project, id);
    if (!resolved.error.empty()) throw std::runtime_error(resolved.error);
    resolved_ids.push_back(resolved.id);
  }
  const std::string out = FormatRoles(snapshot.project, resolved_ids, true);
  std::vector<std::string> touched_files;
  for (const auto& id : resolved_ids) {
    const std::string f = FindOwningFile(snapshot.project, id);
    if (!f.empty()) touched_files.push_back(f);
  }
  RecordTelemetry("show", TotalSourceBytes(snapshot.sources, touched_files), out.size());
  return out;
}

std::string Workspace::FindRole(const std::string& file, const std::string& name) {
  if (file.empty() || name.empty()) throw std::invalid_argument("role expects a file and nonempty name");
  const auto snapshot = LoadConsistent();
  const auto found = snapshot.project.files.find(file);
  if (found == snapshot.project.files.end()) {
    throw std::runtime_error(UnknownFileMessage(snapshot.project, file));
  }
  std::vector<std::string> matches;
  for (const auto& id : RoleSubtree(snapshot.project, found->second.root)) {
    if (id == name || id.ends_with("." + name)) matches.push_back(id);
  }
  if (matches.empty()) throw std::runtime_error("No role named " + name + " in " + file);
  if (matches.size() != 1) {
    std::string message = "Ambiguous role " + name + ":";
    for (const auto& id : matches) message += "\n" + id;
    throw std::runtime_error(message);
  }
  return FormatRoles(snapshot.project, matches, true);
}

std::string Workspace::Search(const std::vector<std::string>& terms, const std::string& prefix,
                              const std::string& file, bool ids_only, bool use_regex) {
  if (terms.empty()) throw std::invalid_argument("search expects at least one query");
  for (const auto& term : terms) {
    if (term.empty()) throw std::invalid_argument("search queries must be nonempty");
  }
  std::vector<std::unique_ptr<re2::RE2>> regexes;
  if (use_regex) {
    for (const auto& term : terms) {
      auto re = std::make_unique<re2::RE2>(term);
      if (!re->ok()) {
        throw std::invalid_argument("Invalid regular expression '" + term + "': " + re->error());
      }
      regexes.push_back(std::move(re));
    }
  }
  const auto snapshot = LoadConsistent();
  std::set<std::string> file_roles;
  if (!file.empty()) {
    const auto found = snapshot.project.files.find(file);
    if (found == snapshot.project.files.end()) {
      throw std::runtime_error(UnknownFileMessage(snapshot.project, file));
    }
    const auto roles = RoleSubtree(snapshot.project, found->second.root);
    file_roles.insert(roles.begin(), roles.end());
  }
  const std::string descendants = !prefix.empty() && prefix.back() != '.' ? prefix + '.' : prefix;
  std::vector<std::string> ids;
  for (const auto& [id, node] : snapshot.project.blocks) {
    if (!file.empty() && !file_roles.contains(id)) continue;
    if (!prefix.empty() && id != prefix && !id.starts_with(descendants)) continue;
    bool matched = false;
    if (use_regex) {
      for (const auto& re : regexes) {
        if (re2::RE2::PartialMatch(id, *re) || re2::RE2::PartialMatch(node.text, *re)) {
          matched = true;
          break;
        }
      }
    } else {
      matched = std::ranges::any_of(terms, [&](const std::string& term) {
        return id.contains(term) || node.text.contains(term);
      });
    }
    if (matched) ids.push_back(id);
  }
  if (ids.empty()) return "no matching roles";
  if (!ids_only) {
    const std::string out = FormatRoles(snapshot.project, ids);
    RecordTelemetry("search",
                    TotalSourceBytes(snapshot.sources, file.empty() ? std::vector<std::string>{} : std::vector<std::string>{file}),
                    out.size());
    return out;
  }
  // Compact form: one line per role. The first line that actually contains a
  // query is the useful one; a role matched only by its id shows its first
  // non-empty line instead.
  std::string output;
  for (const auto& id : ids) {
    const auto& node = snapshot.project.blocks.at(id);
    std::string shown;
    std::string fallback;
    for (const auto& line : std::views::split(node.text, '\n')) {
      const std::string value(line.begin(), line.end());
      if (value.find_first_not_of(" \t\r") == std::string::npos) continue;
      if (fallback.empty()) fallback = value;
      bool line_matched = false;
      if (use_regex) {
        for (const auto& re : regexes) {
          if (re2::RE2::PartialMatch(value, *re)) {
            line_matched = true;
            break;
          }
        }
      } else {
        line_matched = std::ranges::any_of(terms, [&](const std::string& term) {
          return value.contains(term);
        });
      }
      if (line_matched) {
        shown = value;
        break;
      }
    }
    if (shown.empty()) shown = fallback;
    output += id + ": " + shown + "\n";
  }
  RecordTelemetry("search",
                  TotalSourceBytes(snapshot.sources, file.empty() ? std::vector<std::string>{} : std::vector<std::string>{file}),
                  output.size());
  return output;
}

std::string Workspace::PrepareBatch(const std::vector<std::string>& terms,
                                    const std::vector<std::string>& files) {
  if (terms.empty() && files.empty()) {
    throw std::invalid_argument("prepare-batch expects queries, --file, or both");
  }
  for (const auto& term : terms) {
    if (term.empty()) throw std::invalid_argument("prepare-batch queries must be nonempty");
  }
  const auto snapshot = LoadConsistent();
  std::set<std::string> file_roles;
  for (const auto& file : files) {
    const auto found = snapshot.project.files.find(file);
    if (found == snapshot.project.files.end()) {
      throw std::runtime_error(UnknownFileMessage(snapshot.project, file));
    }
    const auto roles = RoleSubtree(snapshot.project, found->second.root);
    file_roles.insert(roles.begin(), roles.end());
  }
  std::vector<std::string> ids;
  for (const auto& [id, node] : snapshot.project.blocks) {
    if (!files.empty() && !file_roles.contains(id)) continue;
    // A role with no text cannot be addressed: apply requires a nonempty `old`,
    // and an empty role has nothing for a caller to anchor an edit on.
    if (node.text.empty()) continue;
    // With no query every role of the named files is returned, which is the
    // one-call equivalent of reading them.
    if (!terms.empty()) {
      const auto matched = [&](const std::string& term) {
        return id.contains(term) || node.text.contains(term);
      };
      if (!std::ranges::any_of(terms, matched)) continue;
    }
    ids.push_back(id);
  }
  if (ids.empty()) {
    throw std::runtime_error(
        NoMatchMessage(snapshot.project, terms, file_roles, !files.empty()));
  }
  // Literal matching also hits every container whose text embeds a match, so a
  // query for a method would return the whole enclosing class as `old`. A batch
  // wants the smallest roles that contain the term, so drop any match that has a
  // matched descendant of its own. With no query this filter must NOT run: every
  // container has matched children, and module-level lines such as imports live
  // exactly in the `.file` containers.
  std::vector<std::string> leaves;
  if (terms.empty()) {
    leaves = ids;
  } else {
    const std::set<std::string> matched(ids.begin(), ids.end());
    for (const auto& id : ids) {
      bool contains_match = false;
      for (const auto& child : RoleSubtree(snapshot.project, id)) {
        if (child != id && matched.contains(child)) {
          contains_match = true;
          break;
        }
      }
      if (!contains_match) leaves.push_back(id);
    }
  }
  std::vector<BatchEdit> edits;
  edits.reserve(leaves.size());
  for (const auto& id : leaves) {
    const auto& node = snapshot.project.blocks.at(id);
    edits.push_back({id, node.version, node.text, std::string()});
  }
  return rfl::json::write(BatchPayload{edits});
}

std::string Workspace::Context(const std::vector<std::string>& terms,
                               const std::vector<std::string>& files,
                               bool include_callers) {
  if (terms.empty() && files.empty()) {
    throw std::invalid_argument("context expects queries, --file, or both");
  }
  for (const auto& term : terms) {
    if (term.empty()) throw std::invalid_argument("context queries must be nonempty");
  }
  const auto snapshot = LoadConsistent();
  std::vector<std::string> target_files;
  std::set<std::string> file_roles;
  if (!files.empty()) {
    std::set<std::string> seen;
    for (const auto& file : files) {
      const auto found = snapshot.project.files.find(file);
      if (found == snapshot.project.files.end()) {
        throw std::runtime_error(UnknownFileMessage(snapshot.project, file));
      }
      if (seen.insert(file).second) target_files.push_back(file);
      const auto roles = RoleSubtree(snapshot.project, found->second.root);
      file_roles.insert(roles.begin(), roles.end());
    }
  } else {
    for (const auto& [path, entry] : snapshot.project.files) {
      (void)entry;
      target_files.push_back(path);
    }
  }

  std::string output;
  auto append_section = [&](const std::string& header, const std::string& body) {
    if (!output.empty()) output += "\n\n";
    output += header;
    if (!body.empty()) output += "\n" + body;
  };

  std::set<std::string> all_selected;
  std::set<std::string> all_active;

  for (const auto& path : target_files) {
    const std::string root_id = snapshot.project.files.at(path).root;
    const std::vector<std::string> subtree = RoleSubtree(snapshot.project, root_id);

    std::set<std::string> selected;
    if (terms.empty()) {
      for (const auto& id : subtree) {
        if (!snapshot.project.blocks.at(id).text.empty()) selected.insert(id);
      }
    } else {
      // First pass: check each role's own lines (ignoring `{{child}}` placeholders)
      // plus leaf role IDs so a method query never inflates into the whole class.
      for (const auto& id : subtree) {
        const auto& node = snapshot.project.blocks.at(id);
        if (node.text.empty()) continue;
        const bool is_leaf = RoleChildren(node).empty();
        const std::string own = is_leaf ? node.text : OwnLines(node);
        const bool hit = std::ranges::any_of(terms, [&](const std::string& term) {
          return (is_leaf && id.contains(term)) || own.contains(term);
        });
        if (hit) selected.insert(id);
      }
      // Second pass: if a non-file container (a class or outer function) matched
      // a query itself and none of its children matched a narrower query, expand
      // its methods so `context EventStore` or `context 'class EventDTO'` returns
      // the class's implementation in one step instead of forcing a second read.
      std::vector<std::string> to_expand;
      for (const auto& id : subtree) {
        if (id == root_id) continue;
        const auto& node = snapshot.project.blocks.at(id);
        if (node.text.empty() || RoleChildren(node).empty()) continue;
        const std::string own = OwnLines(node);
        const bool container_hit =
            selected.contains(id) ||
            std::ranges::any_of(terms, [&](const std::string& term) {
              return id.contains(term) || own.contains(term);
            });
        if (!container_hit) continue;
        const auto desc_tree = RoleSubtree(snapshot.project, id);
        bool child_selected = false;
        for (const auto& desc : desc_tree) {
          if (desc != id && selected.contains(desc)) {
            child_selected = true;
            break;
          }
        }
        if (!child_selected && desc_tree.size() <= 30) {
          for (const auto& desc : desc_tree) {
            if (!snapshot.project.blocks.at(desc).text.empty()) to_expand.push_back(desc);
          }
        } else if (!child_selected) {
          to_expand.push_back(id);
        }
      }
      selected.insert(to_expand.begin(), to_expand.end());
    }

    if (selected.empty()) continue;

    // Mark ancestors of selected roles so enclosing `.file` and `class` headers
    // provide imports, class fields, and omitted-sibling summaries.
    std::set<std::string> active = selected;
    active.insert(root_id);
    for (const auto& id : subtree) {
      for (const auto& desc : RoleSubtree(snapshot.project, id)) {
        if (selected.contains(desc)) {
          active.insert(id);
          break;
        }
      }
    }
    all_selected.insert(selected.begin(), selected.end());
    all_active.insert(active.begin(), active.end());

    for (const auto& id : subtree) {
      if (!active.contains(id)) continue;
      const auto& node = snapshot.project.blocks.at(id);
      if (node.text.empty()) continue;
      const std::string alias = ShortestRoleAlias(snapshot.project, id);
      std::string header = "#@ " + alias + " [v:" + node.version.substr(0, 12) + "]";
      if (id == root_id) header += " [file: " + path + "]";

      const auto children = RoleChildren(node);
      if (children.empty()) {
        append_section(header, node.text);
        continue;
      }

      std::string body = OwnLines(node);
      if (body.empty()) {
        // Container has only `{{child}}` references: keep the raw text so an edit
        // adding the first module-level import still has a valid anchor line.
        body = node.text;
      }
      std::vector<std::string> omitted;
      const std::string prefix =
          (id.ends_with(".file") ? id.substr(0, id.size() - 5) : id) + ".";
      for (const auto& child : children) {
        if (!active.contains(child)) {
          omitted.push_back(child.starts_with(prefix) ? child.substr(prefix.size()) : child);
        }
      }
      if (!omitted.empty()) {
        if (!body.empty()) body += "\n";
        body += "# omitted " + std::to_string(omitted.size()) + " unchanged role" +
                (omitted.size() == 1 ? ": " : "s: ");
        for (size_t i = 0; i < omitted.size(); ++i) {
          if (i > 0) body += ", ";
          body += omitted[i];
        }
      }
      append_section(header, body);
    }
  }

  if (output.empty()) {
    throw std::runtime_error(
        NoMatchMessage(snapshot.project, terms, file_roles, !files.empty()));
  }

  if (include_callers) {
    std::set<std::string> candidate_symbols;
    static const std::set<std::string> kKeywords = {
        "class", "def", "async", "return", "self", "this", "function", "fn",
        "let", "var", "const", "void", "int", "str", "bool", "float", "import", "from"
    };
    if (!terms.empty()) {
      for (const auto& term : terms) {
        std::string current;
        for (char c : term) {
          if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            current += c;
          } else {
            if (!current.empty()) {
              if (!kKeywords.contains(current)) {
                candidate_symbols.insert(current);
              }
              current.clear();
            }
          }
        }
        if (!current.empty() && !kKeywords.contains(current)) {
          candidate_symbols.insert(current);
        }
      }
    } else {
      for (const auto& id : all_selected) {
        if (id.ends_with(".file")) continue;
        const auto dot = id.rfind(".");
        const std::string name = (dot == std::string::npos) ? id : id.substr(dot + 1);
        if (!name.empty() && !kKeywords.contains(name)) {
          candidate_symbols.insert(name);
        }
      }
    }

    std::string callers_body;
    size_t caller_count = 0;
    auto is_word_char = [](char c) {
      return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };

    for (const auto& [id, node] : snapshot.project.blocks) {
      if (all_active.contains(id)) continue;
      if (node.text.empty()) continue;
      const bool is_leaf = RoleChildren(node).empty();
      const std::string text = is_leaf ? node.text : OwnLines(node);
      if (text.empty()) continue;

      for (const auto& line_view : std::views::split(text, '\n')) {
        std::string line(line_view.begin(), line_view.end());
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.contains("{{")) continue;

        const auto first_non_space = line.find_first_not_of(" \t");
        if (first_non_space == std::string::npos) continue;
        const auto last_non_space = line.find_last_not_of(" \t");
        const std::string trimmed =
            line.substr(first_non_space, last_non_space - first_non_space + 1);

        if (trimmed.starts_with("#") || trimmed.starts_with("//")) continue;

        bool matched = false;
        for (const auto& sym : candidate_symbols) {
          size_t pos = 0;
          while ((pos = line.find(sym, pos)) != std::string::npos) {
            const bool before_ok = (pos == 0 || !is_word_char(line[pos - 1]));
            const bool after_ok =
                (pos + sym.size() == line.size() || !is_word_char(line[pos + sym.size()]));
            if (before_ok && after_ok) {
              matched = true;
              break;
            }
            pos += sym.size();
          }
          if (matched) break;
        }

        if (matched) {
          const std::string alias = ShortestRoleAlias(snapshot.project, id);
          if (!callers_body.empty()) callers_body += "\n";
          callers_body += "# " + alias + ": " + trimmed;
          ++caller_count;
          break;
        }
      }
      if (caller_count >= 50) break;
    }

    if (!callers_body.empty()) {
      append_section("# Callers:", callers_body);
    }
  }

  RecordTelemetry("context", TotalSourceBytes(snapshot.sources, target_files), output.size());
  return output;
}

std::string Workspace::List(const std::string& target) {
  const auto snapshot = LoadConsistent();
  std::string output;
  if (target.empty()) {
    // Like `find`: the managed file paths. This is the map a caller needs before
    // any role command, and it is what an agent otherwise probes for with `ls`
    // and `find` on the rendered sources.
    for (const auto& path : snapshot.project.files) output += path.first + "\n";
    return output;
  }
  std::string id = target;
  std::string scope = target;
  const auto file = snapshot.project.files.find(target);
  if (file != snapshot.project.files.end()) {
    id = file->second.root;
    scope = id.ends_with(".file") ? id.substr(0, id.size() - 5) : id;
  }
  const auto found = snapshot.project.blocks.find(id);
  if (found == snapshot.project.blocks.end()) {
    const auto resolved = TryResolveRoleId(snapshot.project, target);
    if (!resolved.id.empty()) {
      id = resolved.id;
      scope = id.ends_with(".file") ? id.substr(0, id.size() - 5) : id;
    }
  }
  const auto found_resolved = snapshot.project.blocks.find(id);
  if (found_resolved == snapshot.project.blocks.end()) {
    // Packages are not roles: `src.tabletopmagnat.observability` is only a
    // namespace. Treat an unknown dotted target as a directory and list the next
    // segment of every role beneath it, so the namespace stays navigable.
    // A directory path is a prefix too: `ls src/pkg` lists what `ls src.pkg`
    // does, so a caller does not have to translate paths into ids first.
    std::string dotted = target;
    std::replace(dotted.begin(), dotted.end(), '/', '.');
    const std::string prefix = dotted + ".";
    std::set<std::string> segments;
    for (const auto& [role_id, node] : snapshot.project.blocks) {
      (void)node;
      if (!role_id.starts_with(prefix)) continue;
      const std::string rest = role_id.substr(prefix.size());
      const std::string segment = rest.substr(0, rest.find('.'));
      // A module's own `….file` role is the module itself, already listed one
      // level up; it must not appear as a child called "file".
      if (segment == "file" && role_id.ends_with(".file")) continue;
      segments.insert(segment);
    }
    if (segments.empty()) {
      throw std::runtime_error("Unknown file, role or prefix: " + target +
                               "; run `tfe-code ls` for files");
    }
    for (const auto& segment : segments) output += segment + "\n";
    return output;
  }
  // Like `ls`: local names only. Repeating a 40-character id prefix on every line
  // is what made `outline` cost 22 KB where a listing needs a few hundred bytes.
  std::set<std::string> names;
  for (const auto& child : RoleChildren(found_resolved->second)) {
    const std::string prefix = scope + ".";
    names.insert(child.starts_with(prefix) ? child.substr(prefix.size()) : child);
  }
  for (const auto& name : names) output += name + "\n";
  return output;
}

std::string Workspace::Outline(const std::string& target, int depth, bool relative) {
  if (depth < 0) throw std::runtime_error("depth must be nonnegative");
  const auto snapshot = LoadConsistent();
  std::vector<std::string> roots;
  if (target.empty()) {
    for (const auto& [path, file] : snapshot.project.files) roots.push_back(file.root);
  } else if (snapshot.project.files.contains(target)) {
    roots.push_back(snapshot.project.files.at(target).root);
  } else {
    roots.push_back(target);
  }
  std::string output;
  for (const auto& root : roots) {
    // Relative mode prints names below the queried node instead of repeating a
    // 40-character id prefix on every line, which is most of the output size.
    std::string scope;
    if (relative) {
      if (root.ends_with(".file")) {
        scope = root.substr(0, root.size() - 5);
      } else {
        const auto dot = root.find_last_of('.');
        scope = dot == std::string::npos ? "" : root.substr(0, dot);
      }
    }
    const auto name_of = [&](const std::string& id) {
      if (scope.empty()) return id;
      const std::string prefix = scope + ".";
      return id.starts_with(prefix) ? id.substr(prefix.size()) : id;
    };
    std::vector<std::pair<std::string, int>> pending{{root, 0}};
    while (!pending.empty()) {
      const auto [id, level] = pending.back();
      pending.pop_back();
      const auto found = snapshot.project.blocks.find(id);
      if (found == snapshot.project.blocks.end()) throw std::runtime_error("Unknown role: " + id + "; use outline to list exact role IDs");
      std::string first;
      size_t lines = 0;
      for (const auto& line : std::views::split(found->second.text, '\n')) {
        std::string value(line.begin(), line.end());
        if (!value.empty() && value != "\r") ++lines;
        if (first.empty() && value.find_first_not_of(" \t\r") != std::string::npos &&
            !value.contains("{{")) first = value;
      }
      output += std::string(static_cast<size_t>(level) * 2, ' ') + name_of(id) + " (" +
                std::to_string(lines) + " lines";
      if (found->second.source_start_line != 0 && found->second.source_end_line != 0) {
        output += "; source " + std::to_string(found->second.source_start_line) + "-" +
                  std::to_string(found->second.source_end_line);
      }
      output += ") " + first + "\n";
      if (level < depth) {
        const auto children = RoleChildren(found->second);
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
          pending.emplace_back(*it, level + 1);
        }
      }
    }
  }
  std::vector<std::string> outline_files;
  if (snapshot.project.files.contains(target)) outline_files.push_back(target);
  RecordTelemetry("outline", TotalSourceBytes(snapshot.sources, outline_files), output.size());
  return output;
}

std::vector<TuiFileRow> Workspace::TuiSnapshot(const std::string& path_prefix) {
  const auto snapshot = LoadConsistent();
  std::vector<TuiFileRow> rows;
  for (const auto& [path, file] : snapshot.project.files) {
    if (!path_prefix.empty() && !path.starts_with(path_prefix)) continue;
    TuiFileRow row;
    row.path = path;
    std::vector<std::pair<std::string, int>> pending{{file.root, 0}};
    while (!pending.empty()) {
      const auto [id, level] = pending.back();
      pending.pop_back();
      const auto found = snapshot.project.blocks.find(id);
      if (found == snapshot.project.blocks.end()) {
        throw std::runtime_error("Unknown role: " + id + "; run `tfe-code ls` for files");
      }
      const auto& node = found->second;
      TuiRoleRow role;
      role.id = id;
      role.depth = level;
      role.version = node.version;
      role.source_start_line = node.source_start_line;
      role.source_end_line = node.source_end_line;
      // Same line accounting as `outline`: blank lines and lone CRs do not count.
      for (const auto& line : std::views::split(node.text, '\n')) {
        std::string value(line.begin(), line.end());
        if (!value.empty() && value != "\r") ++role.line_count;
        if (role.first_line.empty() && value.find_first_not_of(" \t\r") != std::string::npos &&
            !value.contains("{{")) {
          role.first_line = value;
        }
      }
      row.roles.push_back(std::move(role));
      const auto children = RoleChildren(node);
      for (auto it = children.rbegin(); it != children.rend(); ++it) {
        pending.emplace_back(*it, level + 1);
      }
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

ApplyResult Workspace::AddRole(const std::string& parent_target,
                               const std::string& name,
                               const std::string& text,
                               const std::string& after_role,
                               const std::optional<std::string>& check_command,
                               const std::string& before_role) {
  if (parent_target.empty() || name.empty() || text.empty()) {
    throw std::invalid_argument("add-role expects parent, nonempty name, and nonempty role text");
  }
  if (!after_role.empty() && !before_role.empty()) {
    throw std::invalid_argument("add-role accepts either --after or --before, not both");
  }
  RecoverIfNeeded();
  Lock lock(StorePath("lock"));
  auto before = LoadConsistent(false);
  Project candidate = before.project;

  const std::string parent_id = ResolveContainerTarget(candidate, parent_target);

  const std::string scope =
      parent_id.ends_with(".file") ? parent_id.substr(0, parent_id.size() - 5) : parent_id;
  const std::string child_id = scope + "." + name;
  if (candidate.blocks.contains(child_id)) {
    throw std::runtime_error("Role already exists: " + child_id);
  }

  std::string after_id;
  if (!after_role.empty()) {
    if (candidate.blocks.contains(scope + "." + after_role)) {
      after_id = scope + "." + after_role;
    } else {
      const auto resolved = TryResolveRoleId(candidate, after_role);
      if (!resolved.error.empty()) throw std::runtime_error(resolved.error);
      after_id = resolved.id;
    }
  }

  std::string before_id;
  if (!before_role.empty()) {
    if (candidate.blocks.contains(scope + "." + before_role)) {
      before_id = scope + "." + before_role;
    } else {
      const auto resolved = TryResolveRoleId(candidate, before_role);
      if (!resolved.error.empty()) throw std::runtime_error(resolved.error);
      before_id = resolved.id;
    }
  }

  auto& parent_node = candidate.blocks.at(parent_id);
  const std::string file_path = FindOwningFile(candidate, parent_id);
  const std::string indent = DetectPlaceholderIndent(parent_node.text, parent_id, file_path);
  parent_node.text =
      InsertChildPlaceholder(parent_node.text, child_id, after_id, indent, before_id);
  parent_node.version = Sha256(parent_node.text);
  parent_node.literal = false;

  std::string clean_text = text;
  while (!clean_text.empty() && (clean_text.back() == '\n' || clean_text.back() == '\r')) {
    clean_text.pop_back();
  }
  Node child_node;
  child_node.text = std::move(clean_text);
  child_node.version = Sha256(child_node.text);
  child_node.literal = RoleChildren(child_node).empty();
  candidate.blocks[child_id] = std::move(child_node);

  const auto rendered = RenderFiles(candidate);
  auto result = Commit(std::move(before), std::move(candidate), rendered, check_command, true);
  result.created.push_back(child_id);
  return result;
}

ApplyResult Workspace::MoveRole(const std::string& role_target,
                                const std::string& new_name_or_id,
                                const std::string& to_parent,
                                const std::string& after_role,
                                const std::optional<std::string>& check_command,
                                const std::string& before_role) {
  if (role_target.empty() || (new_name_or_id.empty() && to_parent.empty())) {
    throw std::invalid_argument("move-role expects role and a new name or --to parent");
  }
  if (!after_role.empty() && !before_role.empty()) {
    throw std::invalid_argument("move-role accepts either --after or --before, not both");
  }
  RecoverIfNeeded();
  Lock lock(StorePath("lock"));
  auto before = LoadConsistent(false);
  Project candidate = before.project;

  const auto resolved_old = TryResolveRoleId(candidate, role_target);
  if (!resolved_old.error.empty()) throw std::runtime_error(resolved_old.error);
  const std::string old_id = resolved_old.id;
  if (old_id.ends_with(".file")) {
    throw std::runtime_error("Cannot move or rename a .file container role: " + old_id);
  }

  const std::string old_parent_id = FindParentRole(candidate, old_id);
  if (old_parent_id.empty()) {
    throw std::runtime_error("Cannot locate parent container for role: " + old_id);
  }

  std::string new_parent_id = old_parent_id;
  if (!to_parent.empty()) {
    new_parent_id = ResolveContainerTarget(candidate, to_parent);
  }

  const std::string new_scope =
      new_parent_id.ends_with(".file") ? new_parent_id.substr(0, new_parent_id.size() - 5)
                                       : new_parent_id;
  const auto last_dot = old_id.rfind('.');
  const std::string old_leaf = last_dot == std::string::npos ? old_id : old_id.substr(last_dot + 1);
  std::string new_leaf = new_name_or_id.empty() ? old_leaf : new_name_or_id;
  if (new_leaf.starts_with(new_scope + ".")) {
    new_leaf = new_leaf.substr(new_scope.size() + 1);
  }
  const std::string new_id = new_scope + "." + new_leaf;
  if (new_id != old_id && candidate.blocks.contains(new_id)) {
    throw std::runtime_error("Target role already exists: " + new_id);
  }

  std::string after_id;
  if (!after_role.empty()) {
    if (candidate.blocks.contains(new_scope + "." + after_role)) {
      after_id = new_scope + "." + after_role;
    } else {
      const auto resolved_after = TryResolveRoleId(candidate, after_role);
      if (!resolved_after.error.empty()) throw std::runtime_error(resolved_after.error);
      after_id = resolved_after.id;
    }
  }

  std::string before_id;
  if (!before_role.empty()) {
    if (candidate.blocks.contains(new_scope + "." + before_role)) {
      before_id = new_scope + "." + before_role;
    } else {
      const auto resolved_before = TryResolveRoleId(candidate, before_role);
      if (!resolved_before.error.empty()) throw std::runtime_error(resolved_before.error);
      before_id = resolved_before.id;
    }
  }

  if (old_parent_id == new_parent_id && after_id.empty() && before_id.empty()) {
    // In-place rename in the same parent container
    auto& parent_node = candidate.blocks.at(old_parent_id);
    const std::string old_tok = "{{" + old_id + "}}";
    const std::string new_tok = "{{" + new_id + "}}";
    const auto pos = parent_node.text.find(old_tok);
    if (pos == std::string::npos) {
      throw std::runtime_error("Parent role does not reference child role: " + old_id);
    }
    parent_node.text.replace(pos, old_tok.size(), new_tok);
    parent_node.version = Sha256(parent_node.text);
  } else {
    auto& old_parent_node = candidate.blocks.at(old_parent_id);
    old_parent_node.text = RemoveChildPlaceholder(old_parent_node.text, old_id);
    old_parent_node.version = Sha256(old_parent_node.text);
    old_parent_node.literal = RoleChildren(old_parent_node).empty();

    auto& new_parent_node = candidate.blocks.at(new_parent_id);
    const std::string file_path = FindOwningFile(candidate, new_parent_id);
    const std::string indent =
        DetectPlaceholderIndent(new_parent_node.text, new_parent_id, file_path);
    new_parent_node.text =
        InsertChildPlaceholder(new_parent_node.text, new_id, after_id, indent, before_id);
    new_parent_node.version = Sha256(new_parent_node.text);
    new_parent_node.literal = false;
  }

  if (new_id != old_id) {
    const auto subtree = RoleSubtree(candidate, old_id);
    std::vector<std::pair<std::string, Node>> moved;
    for (const auto& sub_id : subtree) {
      std::string mapped_id =
          (sub_id == old_id) ? new_id : (new_id + sub_id.substr(old_id.size()));
      Node node = candidate.blocks.at(sub_id);
      const std::string old_prefix = "{{" + old_id + ".";
      const std::string new_prefix = "{{" + new_id + ".";
      for (size_t pos = 0; (pos = node.text.find(old_prefix, pos)) != std::string::npos;) {
        node.text.replace(pos, old_prefix.size(), new_prefix);
        pos += new_prefix.size();
      }
      node.version = Sha256(node.text);
      moved.emplace_back(std::move(mapped_id), std::move(node));
    }
    for (const auto& sub_id : subtree) candidate.blocks.erase(sub_id);
    for (auto& [mapped_id, node] : moved) candidate.blocks[mapped_id] = std::move(node);
  }

  const auto rendered = RenderFiles(candidate);
  auto result = Commit(std::move(before), std::move(candidate), rendered, check_command, true);
  result.moved.push_back(old_id + " -> " + new_id);
  return result;
}

ApplyResult Workspace::DeleteRole(const std::string& role_target,
                                  const std::optional<std::string>& check_command) {
  if (role_target.empty()) throw std::invalid_argument("delete-role expects a role id");
  RecoverIfNeeded();
  Lock lock(StorePath("lock"));
  auto before = LoadConsistent(false);
  Project candidate = before.project;

  const auto resolved = TryResolveRoleId(candidate, role_target);
  if (!resolved.error.empty()) throw std::runtime_error(resolved.error);
  const std::string role_id = resolved.id;
  if (role_id.ends_with(".file")) {
    throw std::runtime_error("Cannot delete a .file container role: " + role_id);
  }

  const std::string parent_id = FindParentRole(candidate, role_id);
  if (parent_id.empty()) {
    throw std::runtime_error("Cannot locate parent container for role: " + role_id);
  }

  auto& parent_node = candidate.blocks.at(parent_id);
  parent_node.text = RemoveChildPlaceholder(parent_node.text, role_id);
  parent_node.version = Sha256(parent_node.text);
  parent_node.literal = RoleChildren(parent_node).empty();

  const auto subtree = RoleSubtree(candidate, role_id);
  for (const auto& sub_id : subtree) candidate.blocks.erase(sub_id);

  const auto rendered = RenderFiles(candidate);
  return Commit(std::move(before), std::move(candidate), rendered, check_command, true);
}

TelemetryReport Workspace::GetStats(bool reset) {
  RecoverIfNeeded();
  const auto path = StorePath("telemetry.json");
  if (reset) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return TelemetryReport{};
  }
  if (!std::filesystem::exists(path)) return TelemetryReport{};
  const auto parsed = rfl::json::read<TelemetryReport, rfl::DefaultIfMissing>(ReadBinary(path));
  if (!parsed) return TelemetryReport{};
  return parsed.value();
}

std::string Workspace::FormatStats(bool json, bool reset) {
  const auto report = GetStats(reset);
  if (json) return rfl::json::write(report);
  std::ostringstream out;
  out << "TFEngineCode Token Telemetry\n";
  out << "Calls: " << report.total_calls << "\n";
  out << "Baseline: " << report.baseline_tokens << " tokens (" << report.baseline_bytes
      << " bytes)\n";
  out << "Emitted:  " << report.emitted_tokens << " tokens (" << report.emitted_bytes
      << " bytes)\n";
  out << "Saved:    " << report.saved_tokens << " tokens (" << report.saved_bytes << " bytes, ";
  out.setf(std::ios::fixed);
  out.precision(1);
  out << report.savings_percent << "%)\n";
  if (!report.by_command.empty()) {
    out << "\nBy Command:\n";
    for (const auto& [cmd, stats] : report.by_command) {
      const double pct =
          stats.baseline_bytes > 0
              ? (static_cast<double>(stats.saved_bytes) * 100.0 /
                 static_cast<double>(stats.baseline_bytes))
              : 0.0;
      out << "  " << cmd << ": " << stats.calls << " calls, saved " << stats.saved_tokens
          << " tokens (" << pct << "%)\n";
    }
  }
  return out.str();
}

ApplyResult Workspace::Watch(int timeout_ms, bool once) {
  RecoverIfNeeded();
  if (!std::filesystem::exists(StorePath("project.json")) &&
      !std::filesystem::exists(ObjectBoxDir())) {
    throw std::runtime_error("Workspace is not imported");
  }

  class WorkspaceWatchListener final : public efsw::FileWatchListener {
   public:
    WorkspaceWatchListener(std::filesystem::path root, std::vector<IgnoreRule> rules,
                           std::mutex& mu, std::condition_variable& cv, bool& dirty)
        : root_(std::move(root)),
          rules_(std::move(rules)),
          mu_(mu),
          cv_(cv),
          dirty_(dirty) {}

    void handleFileAction(efsw::WatchID /*watchid*/, const std::string& dir,
                          const std::string& filename, efsw::Action /*action*/,
                          const std::string& /*oldFilename*/) override {
      if (filename.empty() || filename.contains(".tmp-")) return;
      const std::filesystem::path full = std::filesystem::path(dir) / filename;
      std::error_code ec;
      const auto rel = std::filesystem::relative(full, root_, ec);
      if (ec || rel.empty()) return;
      const std::string generic = rel.generic_string();
      // Mirror the recursive-import rule: no dot-directory is a managed source.
      // A dot *file* stays managed, exactly as `import` keeps it.
      const std::filesystem::path relative_path(generic);
      const bool hidden = std::ranges::any_of(relative_path.parent_path(), [](const auto& part) {
        const auto name = part.string();
        return name.size() > 1 && name.front() == '.';
      });
      if (generic.starts_with("..") || hidden) return;
      if (!IsSupportedSourceFile(generic)) return;
      if (IsIgnored(rules_, rel, false)) return;
      {
        std::lock_guard<std::mutex> lock(mu_);
        dirty_ = true;
      }
      cv_.notify_all();
    }

   private:
    std::filesystem::path root_;
    std::vector<IgnoreRule> rules_;
    std::mutex& mu_;
    std::condition_variable& cv_;
    bool& dirty_;
  };

  std::mutex mu;
  std::condition_variable cv;
  bool dirty = false;
  const auto rules = LoadIgnoreRules(root_);
  WorkspaceWatchListener listener(root_, rules, mu, cv, dirty);

  efsw::FileWatcher watcher;
  const efsw::WatchID watch_id = watcher.addWatch(root_.string(), &listener, true);
  if (watch_id < 0) {
    throw std::runtime_error("Cannot initialize efsw watcher for workspace: " +
                             efsw::Errors::Log::getLastErrorLog());
  }
  watcher.watch();

  ApplyResult cumulative;
  std::set<std::string> all_changed;
  const auto start_time = std::chrono::steady_clock::now();

  while (true) {
    {
      std::unique_lock<std::mutex> lock(mu);
      if (timeout_ms > 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
        const auto remaining = std::chrono::milliseconds(timeout_ms) - elapsed;
        if (remaining <= std::chrono::milliseconds(0)) break;
        if (!cv.wait_for(lock, remaining, [&] { return dirty; })) break;
      } else {
        cv.wait(lock, [&] { return dirty; });
      }
      dirty = false;
    }
    // Debounce bursty editor/IDE writes before running Sync()
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    {
      std::lock_guard<std::mutex> lock(mu);
      dirty = false;
    }
    try {
      const auto res = Sync();
      for (const auto& path : res.changed) all_changed.insert(path);
    } catch (...) {
      // Transient partial writes or syntax errors during typing should not kill the watcher
      if (once) {
        watcher.removeWatch(watch_id);
        throw;
      }
    }
    if (once) break;
  }

  watcher.removeWatch(watch_id);
  cumulative.changed.assign(all_changed.begin(), all_changed.end());
  return cumulative;
}

}  // namespace tfe::code
