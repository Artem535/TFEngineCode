#include "objectbox_store.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <rfl/json.hpp>

#include "tf/engine.h"
#include "tf/objectbox-model.h"
#include "tf/objectbox_repository.h"

namespace tfe::code {
namespace {

// ObjectBox permits one open store per directory per process, so every adapter
// instance for the same path shares one backend. The adapter owns the store, so
// it can wrap a whole publish batch in a single write transaction; the engine is
// created with defer_init so it never opens a competing store of its own.
struct Backend {
  std::shared_ptr<obx::Store> store;
  std::shared_ptr<tf::Engine> engine;
};

struct EngineRegistry {
  std::mutex mutex;
  std::map<std::string, std::shared_ptr<Backend>> backends;
};

EngineRegistry& Registry() {
  static EngineRegistry registry;
  return registry;
}

std::shared_ptr<Backend> BackendFor(const std::filesystem::path& dir) {
  std::lock_guard<std::mutex> lock(Registry().mutex);
  const std::string key = dir.lexically_normal().generic_string();
  auto found = Registry().backends.find(key);
  if (found != Registry().backends.end()) return found->second;

  std::filesystem::create_directories(dir);
  obx::Options options;
  options.directory(key);
  options.model(create_obx_model());
  auto store = std::make_shared<obx::Store>(options);

  tf::EngineConfig config;
  config.default_data_path = key;
  config.defer_init = true;
  auto engine = std::make_shared<tf::Engine>(config);
  engine->SetBlockRepository(std::make_shared<tf::BlockRepository>(store));
  engine->SetCompositionRepository(std::make_shared<tf::CompositionRepository>(store));

  auto backend = std::make_shared<Backend>(Backend{store, engine});
  Registry().backends.emplace(key, backend);
  return backend;
}

constexpr char kFilePrefix[] = "file:";

struct RoleMeta {
  std::string sha256 = {};
  bool literal = true;
  int64_t source_start_line = 0;
  int64_t source_end_line = 0;
  // Block::publish rejects an empty template, but an empty role (an empty
  // __init__.py) is meaningful code. The role text is then stored as a
  // placeholder and this flag drives decoding, so no real text can be confused
  // with the placeholder.
  bool empty = false;
};

// Placeholder stored in the template when the role text is empty.
constexpr char kEmptyPlaceholder[] = "<tfe-code:empty-role>";

struct FileRecord {
  std::string root = {};
  std::map<std::string, std::string> params = {};
};

std::string RoleText(const tf::Block& block) { return block.templ().Content(); }

bool SameRole(const tf::Block& stored, const Node& node);

}  // namespace

std::string EncodeRoleMetadata(const Node& node) {
  if (node.choice.has_value()) {
    throw std::runtime_error(
        "ObjectBox store does not support conditional role choices: " +
        node.text.substr(0, 40));
  }
  return rfl::json::write(RoleMeta{node.version, node.literal,
                                   static_cast<int64_t>(node.source_start_line),
                                   static_cast<int64_t>(node.source_end_line),
                                   node.text.empty()});
}

std::string RoleTemplate(const Node& node) {
  return node.text.empty() ? std::string(kEmptyPlaceholder) : node.text;
}

Node DecodeRoleMetadata(const std::string& description, const std::string& text) {
  Node node;
  node.text = text;
  node.literal = true;
  node.choice = std::nullopt;
  node.source_start_line = 0;
  node.source_end_line = 0;
  if (description.empty()) return node;
  const auto meta = rfl::json::read<RoleMeta>(description);
  if (!meta) {
    throw std::runtime_error("Invalid role metadata in ObjectBox store: " +
                             std::string(meta.error().what()));
  }
  if (!meta.value().sha256.empty()) node.version = meta.value().sha256;
  node.literal = meta.value().literal;
  node.text = meta.value().empty ? std::string() : text;
  node.source_start_line =
      static_cast<size_t>(std::max<int64_t>(0, meta.value().source_start_line));
  node.source_end_line =
      static_cast<size_t>(std::max<int64_t>(0, meta.value().source_end_line));
  return node;
}

std::string EncodeFileRecord(const File& file) {
  FileRecord record;
  record.root = file.root;
  for (const auto& [name, value] : file.params) record.params[name] = value;
  return rfl::json::write(record);
}

File DecodeFileRecord(const std::string& payload) {
  const auto record = rfl::json::read<FileRecord>(payload);
  if (!record) {
    throw std::runtime_error("Invalid file record in ObjectBox store: " +
                             std::string(record.error().what()));
  }
  File file;
  file.root = record.value().root;
  for (const auto& [name, value] : record.value().params) file.params[name] = value;
  return file;
}

namespace {

bool SameRole(const tf::Block& stored, const Node& node) {
  const auto existing = DecodeRoleMetadata(stored.description(), RoleText(stored));
  return existing.text == node.text && existing.version == node.version &&
         existing.literal == node.literal &&
         existing.source_start_line == node.source_start_line &&
         existing.source_end_line == node.source_end_line;
}

}  // namespace

ObjectBoxRoleStore::ObjectBoxRoleStore(std::filesystem::path db_dir)
    : db_dir_(std::move(db_dir)) {}

bool ObjectBoxRoleStore::Exists() const {
  if (!std::filesystem::exists(db_dir_)) return false;
  auto engine = BackendFor(db_dir_)->engine;
  for (const auto& id : engine->ListBlocks()) {
    if (!id.starts_with(kFilePrefix)) return true;
  }
  return false;
}

Project ObjectBoxRoleStore::Load() const {
  Project project;
  auto engine = BackendFor(db_dir_)->engine;
  for (const auto& id : engine->ListBlocks()) {
    const auto block = engine->LoadBlock(id);
    if (block.HasError()) {
      throw std::runtime_error("Cannot load role from ObjectBox store: " + id);
    }
    const std::string text = RoleText(block.value());
    if (id.starts_with(kFilePrefix)) {
      project.files[id.substr(sizeof(kFilePrefix) - 1)] =
          DecodeFileRecord(text);
    } else {
      project.blocks[id] =
          DecodeRoleMetadata(block.value().description(), text);
    }
  }
  return project;
}

size_t ObjectBoxRoleStore::Save(const Project& project) const {
  auto backend = BackendFor(db_dir_);
  auto engine = backend->engine;
  size_t published = 0;

  // One write transaction for the whole batch: either every published role and
  // file mapping commits, or the store keeps its previous state. Reads inside
  // the transaction see the batch's own writes, so the delete pass below also
  // observes roles published earlier in this call.
  // The transaction rolls back through its destructor unless success() runs, so
  // a throw anywhere below leaves the database untouched.
  auto transaction = backend->store->txWrite();

  for (const auto& [id, node] : project.blocks) {
    const auto stored = engine->LoadBlock(id);
    if (!stored.HasError() && SameRole(stored.value(), node)) continue;
    tf::BlockDraftBuilder builder{tf::BlockId(id)};
    builder.WithType(tf::BlockType::Domain)
        .WithLanguage("python")
        .WithTemplate(tf::Template(RoleTemplate(node)))
        .WithDescription(EncodeRoleMetadata(node));
    const auto result =
        engine->PublishBlock(builder.build(), tf::Engine::VersionBump::Minor);
    if (result.HasError()) {
      throw std::runtime_error("Cannot publish role " + id + ": " +
                               result.error().message);
    }
    ++published;
  }

  for (const auto& [path, file] : project.files) {
    const std::string id = std::string(kFilePrefix) + path;
    const std::string payload = EncodeFileRecord(file);
    const auto stored = engine->LoadBlock(id);
    if (!stored.HasError() && RoleText(stored.value()) == payload) continue;
    tf::BlockDraftBuilder builder{tf::BlockId(id)};
    builder.WithType(tf::BlockType::Meta)
        .WithLanguage("json")
        .WithTemplate(tf::Template(payload))
        .WithDescription("file mapping");
    const auto result =
        engine->PublishBlock(builder.build(), tf::Engine::VersionBump::Minor);
    if (result.HasError()) {
      throw std::runtime_error("Cannot publish file mapping " + path + ": " +
                               result.error().message);
    }
    ++published;
  }

  // Roles removed from the project must not survive in the database.
  std::vector<std::string> stale;
  for (const auto& id : engine->ListBlocks()) {
    const bool is_file = id.starts_with(kFilePrefix);
    const std::string key =
        is_file ? id.substr(sizeof(kFilePrefix) - 1) : id;
    const bool keep =
        is_file ? project.files.contains(key) : project.blocks.contains(key);
    if (!keep) stale.push_back(id);
  }
  for (const auto& id : stale) {
    const auto removed = engine->DeleteBlock(id);
    if (removed.is_error()) {
      throw std::runtime_error("Cannot delete role " + id + ": " +
                               removed.message);
    }
    ++published;
  }
  transaction.success();
  return published;
}

}  // namespace tfe::code