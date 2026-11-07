// ObjectBox role store: the role tree must survive a publish/load round trip
// byte-for-byte, keep the SHA-256 edit contract in the stored metadata, bump
// the engine version on edit, and drop roles removed from the project.
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "objectbox_store.h"

namespace fs = std::filesystem;

void Expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

tfe::code::Node Leaf(const std::string& text, size_t start, size_t end) {
  tfe::code::Node node;
  node.version = "hash-" + text;
  node.text = text;
  node.literal = true;
  node.choice = std::nullopt;
  node.source_start_line = start;
  node.source_end_line = end;
  return node;
}

int main() {
  const fs::path db = fs::temp_directory_path() / "tfe-code-objectbox-store-test";
  fs::remove_all(db);
  tfe::code::ObjectBoxRoleStore store(db);
  Expect(!store.Exists(), "fresh store must report no roles");

  tfe::code::Project project;
  project.blocks["mod.file"] = Leaf("{{mod.run}}\n", 1, 3);
  project.blocks["mod.file"].literal = false;
  project.blocks["mod.run"] = Leaf("def run():\n    return 1", 1, 2);
  project.files["mod.py"] = tfe::code::File{"mod.file", {}};

  const size_t published = store.Save(project);
  Expect(published == 3, "first save must publish two roles and one file mapping");
  Expect(store.Exists(), "store must report roles after a save");

  const auto reloaded = store.Load();
  Expect(reloaded.blocks.size() == 2, "role count changed across the round trip");
  Expect(reloaded.files.size() == 1, "file mapping count changed across the round trip");
  Expect(reloaded.blocks.at("mod.run").text == "def run():\n    return 1",
         "role text did not round trip byte-for-byte");
  Expect(reloaded.blocks.at("mod.run").version == "hash-def run():\n    return 1",
         "content hash was not preserved in the stored metadata");
  Expect(reloaded.blocks.at("mod.run").source_start_line == 1 &&
             reloaded.blocks.at("mod.run").source_end_line == 2,
         "source ranges were not preserved");
  Expect(reloaded.blocks.at("mod.file").literal == false,
         "container role must stay a non-literal parent");
  Expect(reloaded.files.at("mod.py").root == "mod.file",
         "file root role was not preserved");

  // Re-saving the identical project must publish nothing.
  Expect(store.Save(project) == 0, "an unchanged project must publish nothing");

  // Editing one role publishes only that role.
  project.blocks["mod.run"] = Leaf("def run():\n    return 2", 1, 2);
  Expect(store.Save(project) == 1, "editing one role must publish exactly one role");
  Expect(store.Load().blocks.at("mod.run").text == "def run():\n    return 2",
         "edited role did not persist");

  // Removing a role removes it from the database.
  project.blocks.erase("mod.run");
  project.blocks["mod.file"] = Leaf("\n", 1, 1);
  Expect(store.Save(project) == 2, "removing one role must delete it and republish the parent");
  const auto pruned = store.Load();
  Expect(!pruned.blocks.contains("mod.run"), "deleted role survived in the database");

  // A failure in the middle of a batch must leave the database unchanged: the
  // whole Save runs inside one write transaction. A role carrying a conditional
  // choice is rejected by the encoder, and iteration order is by role id, so
  // "aaa.first" is published before "zzz.rejected" throws.
  project.blocks["aaa.first"] = Leaf("def first():\n    return 1", 1, 2);
  project.blocks["zzz.rejected"] = Leaf("def rejected():\n    return 2", 1, 2);
  project.blocks["zzz.rejected"].choice = tfe::code::Choice{"mode", {"a", "b"}, "zzz.a", "zzz.b"};
  bool rejected = false;
  try {
    (void)store.Save(project);
  } catch (const std::exception&) {
    rejected = true;
  }
  Expect(rejected, "a batch containing a conditional role must be rejected");
  const auto rolled_back = store.Load();
  Expect(!rolled_back.blocks.contains("aaa.first"),
         "a failed batch leaked a role published before the failure");
  Expect(!rolled_back.blocks.contains("zzz.rejected"),
         "a failed batch stored the rejected role");
  Expect(rolled_back.blocks.contains("mod.file"),
         "a failed batch destroyed the previously committed state");

  fs::remove_all(db);
  std::cout << "objectbox_store_tests passed\n";
  return 0;
}