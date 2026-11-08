// Extracted from the TextFoundryEngine code-fragments prototype.
// MIT License; see LICENSE.
#include <algorithm>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <ctre.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <rfl/json.hpp>
#ifdef _OPENMP
#include <omp.h>
#endif

#include "render_project.h"

namespace tfe::code {

namespace {
int GetProcessorCount() {
#ifdef _OPENMP
  return omp_get_num_procs();
#else
  const unsigned hw = std::thread::hardware_concurrency();
  return hw > 0 ? static_cast<int>(hw) : 1;
#endif
}
}  // namespace

std::string ReadFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("Cannot read " + path);
  return {std::istreambuf_iterator<char>(stream), {}};
}

std::string ResolveReference(std::string id, const tf::Params& params) {
  constexpr std::string_view prefix = "<PREFIX>.";
  if (id.starts_with(prefix)) {
    const auto it = params.find("PREFIX");
    if (it == params.end()) throw std::runtime_error("Missing PREFIX parameter");
    id = it->second + id.substr(prefix.size() - 1);
  }
  return id;
}

// Static superset of the active reference graph: conditions contribute both
// branches and literal nodes contribute nothing. Used to skip files that cannot
// be affected by a patch without rendering the whole snapshot.
std::set<std::string> StaticReach(const Project& project, const std::string& file) {
  const auto& entry = project.files.at(file);
  std::set<std::string> seen;
  std::vector<std::pair<std::string, tf::Params>> pending{{entry.root, entry.params}};
  while (!pending.empty()) {
    auto [id, params] = pending.back();
    pending.pop_back();
    if (!seen.insert(id).second) continue;
    const auto found = project.blocks.find(id);
    if (found == project.blocks.end()) continue;
    const Node& node = found->second;
    if (node.choice) {
      for (const auto& ref : {node.choice->then_ref, node.choice->else_ref}) {
        if (!ref.empty()) pending.emplace_back(ResolveReference(ref, params), params);
      }
      continue;
    }
    if (node.literal) continue;
    for (auto match : ctre::search_all<R"(\{\{([^{}\r\n]+)\}\})">(node.text)) {
      const std::string name = match.get<1>().to_string();
      if (name.find('.') != std::string::npos) {
        pending.emplace_back(ResolveReference(name, params), params);
      }
    }
  }
  return seen;
}

class Resolver {
 public:
  explicit Resolver(const Project& project) : project_(project) {}

  std::string Render(const std::string& file) {
    edges_.clear();
    stack_.clear();
    const auto& entry = project_.files.at(file);
    return Expand(entry.root, entry.params);
  }

  Context ReadContext(const std::string& file, const std::string& target) {
    (void)Render(file);  // Build the active reference graph, using real conditions.
    return ReadContext(file, target, edges_);
  }

  const Graph& Edges() const { return edges_; }

  Context ReadContext(const std::string& file, const std::string& target,
                      const Graph& graph) const {
    if (!graph.contains(target)) {
      throw std::runtime_error("Target is not reachable in this file: " + target);
    }
    std::set<std::string> selected{target};
    bool changed;
    do {
      changed = false;
      for (const auto& [parent, children] : graph) {
        if (std::any_of(children.begin(), children.end(), [&](const auto& child) {
              return selected.contains(child);
            })) {
          changed |= selected.insert(parent).second;
        }
      }
    } while (changed);
    // Include the target's dependencies, but not bodies of its siblings.
    std::vector<std::string> pending{target};
    while (!pending.empty()) {
      auto id = pending.back();
      pending.pop_back();
      for (const auto& child : graph.at(id)) {
        if (selected.insert(child).second) pending.push_back(child);
      }
    }
    Context result{file, project_.files.at(file), {}};
    for (const auto& id : selected) result.blocks.emplace(id, project_.blocks.at(id));
    return result;
  }

 private:
  const Project& project_;
  Graph edges_;
  std::vector<std::string> stack_;

  static std::string Select(const Choice& choice, const tf::Params& params) {
    tf::Condition condition{choice.attribute,
                            {choice.values.begin(), choice.values.end()}, false};
    tf::Conditional conditional;
    conditional.branches.push_back(
        {{condition}, {tf::Fragment::MakeStaticText(choice.then_ref)}});
    conditional.elseContent =
        std::vector{tf::Fragment::MakeStaticText(choice.else_ref)};
    tf::Composition composition("spike.choice");
    composition.InsertFragment(0, tf::Fragment::MakeConditional(conditional));
    const auto published = composition.publish({1, 0});
    if (published.is_error()) throw std::runtime_error(published.message);
    tf::RenderContext context;
    context.params = params;
    const auto rendered = tf::Renderer{}.Render(composition, context);
    if (rendered.HasError()) throw std::runtime_error(rendered.error().message);
    return rendered.value().text;
  }

  std::string Expand(const std::string& id, const tf::Params& params) {
    if (std::find(stack_.begin(), stack_.end(), id) != stack_.end()) {
      std::string chain;
      for (const auto& parent : stack_) chain += parent + " -> ";
      throw std::runtime_error("Circular reference: " + chain + id);
    }
    const auto found = project_.blocks.find(id);
    if (found == project_.blocks.end()) throw std::runtime_error("Missing block: " + id);
    if (stack_.size() >= 128) throw std::runtime_error("Prototype depth limit: 128");
    const Node& node = found->second;
    if (node.version.empty()) throw std::runtime_error("Unpinned block: " + id);
    stack_.push_back(id);
    edges_.try_emplace(id);
    std::string result;
    if (node.choice) {
      const auto child = ResolveReference(Select(*node.choice, params), params);
      if (!child.empty()) {
        edges_[id].insert(child);
        result = Expand(child, params);
      }
    } else if (node.literal) {
      result = node.text;
    } else {
      tf::RenderContext context;
      context.params = params;
      std::string source;
      size_t cursor = 0;
      size_t slot = 0;
      for (auto match : ctre::search_all<R"(\{\{([^{}\r\n]+)\}\})">(node.text)) {
        const size_t position = match.data() - node.text.data();
        source.append(node.text, cursor, position - cursor);
        const std::string name = match.get<1>().to_string();
        if (ctre::match<R"([\w\-]+(\.[\w\-]+)+)">(name)) {
          const auto child = ResolveReference(name, params);
          edges_[id].insert(child);
          auto text = Expand(child, params);
          const auto newline = node.text.rfind('\n', position);
          const auto start = newline == std::string::npos ? 0 : newline + 1;
          const auto indent = node.text.substr(start, position - start);
          std::string indented;
          for (size_t i = 0; i < text.size(); ++i) {
            indented += text[i];
            if (text[i] == '\n' && i + 1 < text.size() && text[i + 1] != '\n' &&
                text[i + 1] != '\r' && indent.find_first_not_of(" \t") == std::string::npos) {
              indented += indent;
            }
          }
          std::string key;
          do { key = "TFE_CODE_CHILD_" + std::to_string(slot++); }
          while (context.params.contains(key) || node.text.find("{{" + key + "}}") != std::string::npos);
          context.params[key] = std::move(indented);
          source += "{{" + key + "}}";
        } else {
          // Protect non-role braces from being reinterpreted as required
          // parameters in a parent that also references code roles.
          std::string key;
          do { key = "TFE_CODE_LITERAL_" + std::to_string(slot++); }
          while (context.params.contains(key) || node.text.find("{{" + key + "}}") != std::string::npos);
          const auto supplied = params.find(name);
          context.params[key] = supplied == params.end() ? match.to_string() : supplied->second;
          source += "{{" + key + "}}";
        }
        cursor = position + match.size();
      }
      source.append(node.text, cursor, std::string::npos);
      tf::Block block(id);
      block.SetTemplate(tf::Template(source));
      const auto rendered = tf::Renderer::RenderBlock(block, context);
      if (rendered.HasError()) throw std::runtime_error(rendered.error().message);
      result = rendered.value();
    }
    stack_.pop_back();
    return result;
  }
};

struct FileResult {
  std::string text;
  Graph graph;
};

unsigned ParseJobs(const std::string& value) {
  if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
    throw std::runtime_error("Worker count must be 0 (automatic) or 1..32");
  }
  const auto jobs = std::stoul(value);
  if (jobs > 32) throw std::runtime_error("Worker count must be 0 (automatic) or 1..32");
  return static_cast<unsigned>(jobs);
}

std::map<std::string, FileResult> BuildFiles(const Project& project,
                                              unsigned jobs, bool graphs) {
  std::vector<std::string> names;
  for (const auto& [name, file] : project.files) names.push_back(name);
  if (names.empty()) return {};
  const unsigned capacity = static_cast<unsigned>(std::max(1, GetProcessorCount()));
  if (jobs == 0) jobs = names.size() < 64 ? 1 : std::min(4u, capacity);
  jobs = std::min({jobs, capacity, static_cast<unsigned>(names.size())});
  std::vector<FileResult> values(names.size());
  std::vector<std::exception_ptr> errors(names.size());
  #pragma omp parallel num_threads(jobs) if(jobs > 1) default(none) shared(project, names, values, errors) firstprivate(graphs)
  {
    // The project is an immutable snapshot. Each worker owns its Resolver and
    // reference graph; no repository cache is shared between threads.
    Resolver resolver(project);
    #pragma omp for schedule(dynamic, 1)
    for (size_t index = 0; index < names.size(); ++index) {
      try {
        values[index].text = resolver.Render(names[index]);
        if (graphs) values[index].graph = resolver.Edges();
      } catch (...) {
        errors[index] = std::current_exception();
      }
    }
  }
  // Preserve deterministic file/error ordering and emit nothing on failure.
  std::map<std::string, FileResult> result;
  for (size_t i = 0; i < names.size(); ++i) {
    if (errors[i]) std::rethrow_exception(errors[i]);
    result.emplace(names[i], std::move(values[i]));
  }
  return result;
}

Applied ApplyMany(const Project& project, const std::vector<Edit>& edits, unsigned jobs) {
  if (edits.empty()) throw std::runtime_error("An edit batch must not be empty");
  std::set<std::string> changed_blocks;
  for (const auto& edit : edits) {
    if (!changed_blocks.insert(edit.block).second) throw std::runtime_error("Duplicate block in batch");
    const auto& node = project.blocks.at(edit.block);
    if (node.version != edit.expected_version) throw std::runtime_error("Stale block version");
    if (edit.version.empty() || node.version == edit.version) {
      throw std::runtime_error("A distinct nonempty new version is required");
    }
    if (node.choice) throw std::runtime_error("Patch a text block, not a choice");
  }
  Project patched = project;
  for (const auto& edit : edits) {
    auto& node = patched.blocks.at(edit.block);
    node.text = edit.text;
    node.version = edit.version;
  }
  Applied result{rfl::json::write(patched), {}};
  std::vector<std::string> names;
  for (const auto& [name, file] : project.files) names.push_back(name);
  const unsigned capacity = static_cast<unsigned>(std::max(1, GetProcessorCount()));
  if (jobs == 0) jobs = names.size() < 64 ? 1 : std::min(4u, capacity);
  jobs = std::min({jobs, capacity, static_cast<unsigned>(std::max<size_t>(1, names.size()))});
  std::vector<Change> values(names.size());
  std::vector<char> touched(names.size(), 0);
  std::vector<std::exception_ptr> errors(names.size());
  #pragma omp parallel num_threads(jobs) if(jobs > 1) default(none) shared(project, patched, names, values, touched, errors, changed_blocks)
  {
    Resolver before(project);
    Resolver after(patched);
    #pragma omp for schedule(dynamic, 1)
    for (size_t index = 0; index < names.size(); ++index) {
      try {
        bool candidate = true;
        try {
          const auto reachable = StaticReach(project, names[index]);
          candidate = std::any_of(changed_blocks.begin(), changed_blocks.end(),
                                  [&](const auto& id) { return reachable.contains(id); });
        } catch (...) {
          // Unresolved references cannot prove that a file is unaffected.
        }
        if (!candidate) continue;
        touched[index] = 1;
        values[index].before = before.Render(names[index]);
        values[index].after = after.Render(names[index]);
      } catch (...) {
        errors[index] = std::current_exception();
      }
    }
  }
  for (size_t i = 0; i < names.size(); ++i) {
    if (errors[i]) std::rethrow_exception(errors[i]);
    if (touched[i] && values[i].before != values[i].after) {
      result.changed.emplace(names[i], std::move(values[i]));
    }
  }
  return result;
}

Found Find(const Project& project, const std::string& query, size_t limit,
           size_t offset, const std::string& prefix) {
  if (query.empty()) throw std::runtime_error("Find needs a nonempty query");
  if (limit == 0 || limit > 32) throw std::runtime_error("Find limit must be 1..32");
  std::vector<std::pair<int, std::string>> candidates;
  for (const auto& [id, node] : project.blocks) {
    const bool named = id.find(query) != std::string::npos;
    if (!named && node.text.find(query) == std::string::npos) continue;
    // Exact IDs and named body fragments precede enclosing templates/siblings.
    const int rank = id == query ? 0 : named && id.ends_with(".body") ? 1
                           : named && node.literal ? 2 : named ? 3 : node.literal ? 4 : 5;
    candidates.emplace_back(rank, id);
  }
  std::sort(candidates.begin(), candidates.end());
  Found result;
  result.next_offset = offset;
  std::map<std::string, Graph> active;
  std::map<std::string, std::set<std::string>> reachable;
  size_t skipped = 0;
  int best_active = 100;
  Resolver resolver(project);
  for (const auto& [rank, id] : candidates) {
    if (rank > best_active) break;
    for (const auto& [file, entry] : project.files) {
      if (!file.starts_with(prefix)) continue;
      if (!reachable.contains(file)) reachable.emplace(file, StaticReach(project, file));
      if (!reachable.at(file).contains(id)) continue;
      if (!active.contains(file)) {
        (void)resolver.Render(file);
        active.emplace(file, resolver.Edges());
      }
      if (!active.at(file).contains(id)) continue;
      best_active = rank;
      if (skipped++ < offset) continue;
      if (result.matches.size() == limit) { result.more = true; return result; }
      const auto context = resolver.ReadContext(file, id, active.at(file));
      result.matches.push_back({file, id, project.blocks.at(id).version});
      result.files.emplace(file, entry);
      result.blocks.insert(context.blocks.begin(), context.blocks.end());
      ++result.next_offset;
    }
  }
  return result;
}

std::map<std::string, std::string> RenderFiles(const Project& project,
                                               unsigned jobs) {
  std::map<std::string, std::string> result;
  for (const auto& [name, file] : BuildFiles(project, jobs, false)) {
    result.emplace(name, file.text);
  }
  return result;
}

std::string RenderFile(const Project& project, const std::string& file) {
  return Resolver(project).Render(file);
}

Context ReadContext(const Project& project, const std::string& file,
                    const std::string& target) {
  return Resolver(project).ReadContext(file, target);
}

Inspection Inspect(const Project& project, const std::string& file, unsigned jobs,
                   const std::vector<std::string>& ids, bool uses) {
  (void)project.files.at(file);
  Resolver target_resolver(project);
  (void)target_resolver.Render(file);
  const auto& target = target_resolver.Edges();
  std::map<std::string, Graph> graphs;
  if (uses) {
    for (auto& [name, value] : BuildFiles(project, jobs, true)) {
      graphs.emplace(name, std::move(value.graph));
    }
  }
  Inspection result;
  result.file = file;
  if (uses) {
    result.uses.emplace();
    result.note = "uses lists active template references, not language-level callers";
  }
  for (const auto& id : ids) {
    if (result.uses && result.uses->size() >= 8) break;
    if (!target.contains(id)) continue;
    const auto context = target_resolver.ReadContext(file, id, target);
    result.blocks.insert(context.blocks.begin(), context.blocks.end());
    if (!result.uses) continue;
    auto& list = (*result.uses)[id];
    for (const auto& [name, graph] : graphs) {
      if (graph.contains(id)) list.push_back(name);
    }
  }
  return result;
}

std::vector<std::string> Search(const Project& project, const std::string& query) {
  std::vector<std::string> matches;
  for (const auto& [id, node] : project.blocks) {
    if (id.find(query) != std::string::npos || node.text.find(query) != std::string::npos) {
      matches.push_back(id);
    }
  }
  return matches;
}

Project Patch(const Project& project, const Edit& edit) {
  Project patched = project;
  auto& node = patched.blocks.at(edit.block);
  if (node.version != edit.expected_version) throw std::runtime_error("Stale block version");
  if (node.version == edit.version || edit.version.empty()) {
    throw std::runtime_error("A distinct nonempty new version is required");
  }
  if (node.choice) throw std::runtime_error("Patch a text block, not a choice");
  node.text = edit.text;
  node.version = edit.version;
  return patched;
}

}  // namespace tfe::code
