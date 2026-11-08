// End-to-end stdio test for the PhoenixMcp-backed `tfe-code serve [--mcp]` server.
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <rfl/json.hpp>

#include "mcp_server.h"
#include "workspace.h"

namespace fs = std::filesystem;

namespace {

void Expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void WriteFile(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

void SendLine(int fd, const std::string& json_line) {
  const std::string payload = json_line + "\n";
  const char* ptr = payload.data();
  size_t remaining = payload.size();
  while (remaining > 0) {
    const ssize_t n = ::write(fd, ptr, remaining);
    Expect(n > 0, "Failed to write to MCP server input pipe");
    ptr += n;
    remaining -= static_cast<size_t>(n);
  }
}

std::string ReadLine(FILE* stream) {
  char* line = nullptr;
  size_t cap = 0;
  const ssize_t len = ::getline(&line, &cap, stream);
  if (len <= 0) {
    std::free(line);
    throw std::runtime_error("Unexpected EOF reading from MCP server output pipe");
  }
  std::string result(line, static_cast<size_t>(len));
  std::free(line);
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
    result.pop_back();
  }
  return result;
}

}  // namespace

int main() {
  int in_pipe[2] = {-1, -1};
  int out_pipe[2] = {-1, -1};
  std::thread server_thread;
  FILE* reader = nullptr;
  auto cleanup = [&]() {
    if (in_pipe[1] >= 0) {
      ::close(in_pipe[1]);
      in_pipe[1] = -1;
    }
    if (in_pipe[0] >= 0) {
      ::close(in_pipe[0]);
      in_pipe[0] = -1;
    }
    if (server_thread.joinable()) {
      server_thread.join();
    }
    if (reader != nullptr) {
      ::fclose(reader);
      reader = nullptr;
    }
  };

  try {
    const fs::path root = fs::temp_directory_path() / "tfe-code-mcp-server-test";
    fs::remove_all(root);
    fs::create_directories(root);
    WriteFile(root / "demo.py",
              "def greet(name: str) -> str:\n"
              "    return \"hello\"\n");

    Expect(::pipe(in_pipe) == 0, "pipe(in_pipe) failed");
    Expect(::pipe(out_pipe) == 0, "pipe(out_pipe) failed");

    tfe::code::Workspace workspace(root);
    int server_exit_code = -1;
    server_thread = std::thread([&]() {
      server_exit_code = tfe::code::RunMcpServer(workspace, in_pipe[0], out_pipe[1]);
      ::close(out_pipe[1]);
    });

    reader = ::fdopen(out_pipe[0], "r");
    Expect(reader != nullptr, "fdopen(out_pipe[0]) failed");

    // 1. initialize
    SendLine(in_pipe[1],
             R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"tfe-test","version":"1.0.0"}}})");
    const std::string init_resp = ReadLine(reader);
    Expect(init_resp.find("\"protocolVersion\"") != std::string::npos,
           "initialize response missing protocolVersion: " + init_resp);
    Expect(init_resp.find("\"tfe-code\"") != std::string::npos,
           "initialize response missing serverInfo.name tfe-code: " + init_resp);

    // 2. notifications/initialized
    SendLine(in_pipe[1], R"({"jsonrpc":"2.0","method":"notifications/initialized"})");

    // 3. tools/list
    SendLine(in_pipe[1], R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})");
    const std::string list_resp = ReadLine(reader);
    for (const char* tool_name : {"tfe_import", "tfe_context", "tfe_outline", "tfe_search",
                                  "tfe_show", "tfe_apply", "tfe_sync"}) {
      Expect(list_resp.find(tool_name) != std::string::npos,
             std::string("tools/list missing tool ") + tool_name + ": " + list_resp);
    }

    // 4. tools/call -> tfe_import
    SendLine(
        in_pipe[1],
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"tfe_import","arguments":{"paths":["demo.py"]}}})");
    const std::string import_resp = ReadLine(reader);
    Expect(import_resp.find("\"isError\":false") != std::string::npos,
           "tfe_import failed: " + import_resp);
    Expect(import_resp.find("demo.py") != std::string::npos,
           "tfe_import output missing demo.py: " + import_resp);

    // 5. tools/call -> tfe_outline
    SendLine(
        in_pipe[1],
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"tfe_outline","arguments":{"target":"demo.py"}}})");
    const std::string outline_resp = ReadLine(reader);
    Expect(outline_resp.find("\"isError\":false") != std::string::npos,
           "tfe_outline failed: " + outline_resp);
    Expect(outline_resp.find("greet") != std::string::npos,
           "tfe_outline missing greet: " + outline_resp);

    // 6. tools/call -> tfe_context
    SendLine(
        in_pipe[1],
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"tfe_context","arguments":{"queries":["greet"]}}})");
    const std::string context_resp = ReadLine(reader);
    Expect(context_resp.find("\"isError\":false") != std::string::npos,
           "tfe_context failed: " + context_resp);
    const std::string marker = "greet [v:";
    const size_t v_pos = context_resp.find(marker);
    Expect(v_pos != std::string::npos,
           "tfe_context missing greet [v:...]: " + context_resp);
    const std::string ver_prefix = context_resp.substr(v_pos + marker.size(), 12);

    // 7. tools/call -> tfe_search
    SendLine(
        in_pipe[1],
        R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"tfe_search","arguments":{"queries":["hello"]}}})");
    const std::string search_resp = ReadLine(reader);
    Expect(search_resp.find("\"isError\":false") != std::string::npos,
           "tfe_search failed: " + search_resp);
    Expect(search_resp.find("greet") != std::string::npos,
           "tfe_search missing greet: " + search_resp);

    // 8. tools/call -> tfe_show
    SendLine(
        in_pipe[1],
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"tfe_show","arguments":{"roles":["greet"]}}})");
    const std::string show_resp = ReadLine(reader);
    Expect(show_resp.find("\"isError\":false") != std::string::npos,
           "tfe_show failed: " + show_resp);
    Expect(show_resp.find("return \\\"hello\\\"") != std::string::npos,
           "tfe_show missing body: " + show_resp);

    // 9. tools/call -> tfe_apply (valid compact patch)
    const std::string patch =
        "#@ greet [v:" + ver_prefix + "]\n<<<\n    return \"hello\"\n===\n    return \"world\"\n>>>\n";
    const std::string apply_req =
        R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"tfe_apply","arguments":{"patch":)" +
        rfl::json::write(patch) + R"(}}})";
    SendLine(in_pipe[1], apply_req);
    const std::string apply_resp = ReadLine(reader);
    Expect(apply_resp.find("\"isError\":false") != std::string::npos,
           "tfe_apply failed: " + apply_resp);
    Expect(ReadFile(root / "demo.py").find("return \"world\"") != std::string::npos,
           "tfe_apply did not materialize demo.py on disk");

    // 10. tools/call -> tfe_apply with stale version returns isError:true without crashing
    const std::string bad_patch =
        "#@ greet [v:000000000000]\n<<<\n    return \"world\"\n===\n    return \"nope\"\n>>>\n";
    const std::string bad_apply_req =
        R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"tfe_apply","arguments":{"patch":)" +
        rfl::json::write(bad_patch) + R"(}}})";
    SendLine(in_pipe[1], bad_apply_req);
    const std::string bad_apply_resp = ReadLine(reader);
    Expect(bad_apply_resp.find("\"isError\":true") != std::string::npos,
           "stale tfe_apply should report isError:true: " + bad_apply_resp);

    // 11. tools/call -> tfe_sync after external edit updates the role tree
    WriteFile(root / "demo.py",
              "def greet(name: str) -> str:\n"
              "    return \"synced\"\n");
    SendLine(
        in_pipe[1],
        R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"tfe_sync","arguments":{}}})");
    const std::string sync_resp = ReadLine(reader);
    Expect(sync_resp.find("\"isError\":false") != std::string::npos,
           "tfe_sync failed: " + sync_resp);

    SendLine(
        in_pipe[1],
        R"({"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"tfe_show","arguments":{"roles":["greet"]}}})");
    const std::string show_synced_resp = ReadLine(reader);
    Expect(show_synced_resp.find("return \\\"synced\\\"") != std::string::npos,
           "tfe_show after tfe_sync did not reflect external edit: " + show_synced_resp);

    cleanup();
    Expect(server_exit_code == 0,
           "RunMcpServer returned non-zero exit code: " + std::to_string(server_exit_code));

    fs::remove_all(root);
    std::cout << "mcp_server_stdio_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    cleanup();
    std::cerr << error.what() << '\n';
    return 1;
  }
}
