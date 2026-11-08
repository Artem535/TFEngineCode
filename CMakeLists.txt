cmake_minimum_required(VERSION 3.20)
project(TFEngineCode VERSION 0.1.0 LANGUAGES C CXX)
set(TEXTFOUNDRY_SOURCE_DIR "" CACHE PATH "TextFoundryEngine source checkout")
if(NOT EXISTS "${TEXTFOUNDRY_SOURCE_DIR}/tf/renderer.h")
  message(FATAL_ERROR "Set TEXTFOUNDRY_SOURCE_DIR to a TextFoundryEngine checkout")
endif()
set(TEXTFOUNDRY_ENGINE_BUILD_OBJECTBOX_STORE ON CACHE BOOL "" FORCE)
set(TEXTFOUNDRY_ENGINE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TEXTFOUNDRY_ENGINE_BUILD_CLI OFF CACHE BOOL "" FORCE)
add_subdirectory("${TEXTFOUNDRY_SOURCE_DIR}" engine)
find_package(OpenMP COMPONENTS CXX QUIET)
find_package(unofficial-tree-sitter CONFIG REQUIRED)
find_package(ctre CONFIG REQUIRED)
find_package(re2 CONFIG REQUIRED)
find_package(reflectcpp CONFIG REQUIRED)
find_package(folly CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(hfsm2 CONFIG REQUIRED)
include(FetchContent)
FetchContent_Declare(tree_sitter_python
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-python.git
  GIT_TAG 293fdc02038ee2bf0e2e206711b69c90ac0d413f
  GIT_SHALLOW FALSE)
FetchContent_GetProperties(tree_sitter_python)
if(NOT tree_sitter_python_POPULATED)
  FetchContent_Populate(tree_sitter_python)
endif()
add_library(tree_sitter_python_grammar STATIC
  "${tree_sitter_python_SOURCE_DIR}/src/parser.c"
  "${tree_sitter_python_SOURCE_DIR}/src/scanner.c")
target_include_directories(tree_sitter_python_grammar PUBLIC
  "${tree_sitter_python_SOURCE_DIR}/src")

FetchContent_Declare(tree_sitter_cpp
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-cpp.git
  GIT_TAG c009222808634c1014f82438d4883753516a2c24
  GIT_SHALLOW FALSE)
FetchContent_GetProperties(tree_sitter_cpp)
if(NOT tree_sitter_cpp_POPULATED)
  FetchContent_Populate(tree_sitter_cpp)
endif()
add_library(tree_sitter_cpp_grammar STATIC
  "${tree_sitter_cpp_SOURCE_DIR}/src/parser.c"
  "${tree_sitter_cpp_SOURCE_DIR}/src/scanner.c")
target_include_directories(tree_sitter_cpp_grammar PUBLIC
  "${tree_sitter_cpp_SOURCE_DIR}/src")

FetchContent_Declare(tree_sitter_typescript
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-typescript.git
  GIT_TAG 75b3874edb2dc714fb1fd77a32013d0f8699989f
  GIT_SHALLOW FALSE)
FetchContent_GetProperties(tree_sitter_typescript)
if(NOT tree_sitter_typescript_POPULATED)
  FetchContent_Populate(tree_sitter_typescript)
endif()
add_library(tree_sitter_typescript_grammar STATIC
  "${tree_sitter_typescript_SOURCE_DIR}/typescript/src/parser.c"
  "${tree_sitter_typescript_SOURCE_DIR}/typescript/src/scanner.c"
  "${tree_sitter_typescript_SOURCE_DIR}/tsx/src/parser.c"
  "${tree_sitter_typescript_SOURCE_DIR}/tsx/src/scanner.c")
target_include_directories(tree_sitter_typescript_grammar PRIVATE
  "${tree_sitter_typescript_SOURCE_DIR}/common"
  "${tree_sitter_typescript_SOURCE_DIR}/typescript/src"
  "${tree_sitter_typescript_SOURCE_DIR}/tsx/src")

FetchContent_Declare(tree_sitter_rust
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-rust.git
  GIT_TAG cad8a206f2e4194676b9699f26f6560d07130d3f
  GIT_SHALLOW FALSE)
FetchContent_GetProperties(tree_sitter_rust)
if(NOT tree_sitter_rust_POPULATED)
  FetchContent_Populate(tree_sitter_rust)
endif()
add_library(tree_sitter_rust_grammar STATIC
  "${tree_sitter_rust_SOURCE_DIR}/src/parser.c"
  "${tree_sitter_rust_SOURCE_DIR}/src/scanner.c")
target_include_directories(tree_sitter_rust_grammar PUBLIC
  "${tree_sitter_rust_SOURCE_DIR}/src")

FetchContent_Declare(tree_sitter_go
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-go.git
  GIT_TAG 3c3775faa968158a8b4ac190a7fda867fd5fb748
  GIT_SHALLOW FALSE)
FetchContent_GetProperties(tree_sitter_go)
if(NOT tree_sitter_go_POPULATED)
  FetchContent_Populate(tree_sitter_go)
endif()
add_library(tree_sitter_go_grammar STATIC
  "${tree_sitter_go_SOURCE_DIR}/src/parser.c")
target_include_directories(tree_sitter_go_grammar PUBLIC
  "${tree_sitter_go_SOURCE_DIR}/src")

set(PHOENIX_MCP_SOURCE_DIR "" CACHE PATH "Optional local PhoenixMcp source checkout")
if(PHOENIX_MCP_SOURCE_DIR AND EXISTS "${PHOENIX_MCP_SOURCE_DIR}/include/phoenix_mcp/server/mcp_server.h")
  set(phoenix_mcp_SOURCE_DIR "${PHOENIX_MCP_SOURCE_DIR}")
else()
  FetchContent_Declare(phoenix_mcp
    GIT_REPOSITORY https://github.com/Artem535/PhoenixMcp.git
    GIT_TAG f09c59d421be97fda38d3a30c39c56271c94a1bb
    GIT_SHALLOW FALSE)
  FetchContent_GetProperties(phoenix_mcp)
  if(NOT phoenix_mcp_POPULATED)
    FetchContent_Populate(phoenix_mcp)
  endif()
endif()
add_library(phoenix_mcp_stdio STATIC
  "${phoenix_mcp_SOURCE_DIR}/src/phoenix_mcp/core/error.cc"
  "${phoenix_mcp_SOURCE_DIR}/src/phoenix_mcp/protocol/dialect.cc"
  "${phoenix_mcp_SOURCE_DIR}/src/phoenix_mcp/protocol/json_rpc_codec.cc"
  "${phoenix_mcp_SOURCE_DIR}/src/phoenix_mcp/runtime/runtime.cpp"
  "${phoenix_mcp_SOURCE_DIR}/src/phoenix_mcp/server/mcp_request_handler.cpp"
  "${phoenix_mcp_SOURCE_DIR}/src/phoenix_mcp/server/server_session.cpp"
  "${phoenix_mcp_SOURCE_DIR}/src/phoenix_mcp/server/session_states.cpp"
  "${phoenix_mcp_SOURCE_DIR}/src/phoenix_mcp/server/session_manager.cpp"
  "${phoenix_mcp_SOURCE_DIR}/src/phoenix_mcp/server/server.cpp"
  "${phoenix_mcp_SOURCE_DIR}/src/phoenix_mcp/tool_registry/tool_registry.cpp"
  "${phoenix_mcp_SOURCE_DIR}/src/phoenix_mcp/transport/stdio_transport.cpp")
target_include_directories(phoenix_mcp_stdio
  PUBLIC "${phoenix_mcp_SOURCE_DIR}/include"
  PRIVATE "${phoenix_mcp_SOURCE_DIR}/src")
target_compile_definitions(phoenix_mcp_stdio
  PRIVATE GLOG_USE_GLOG_EXPORT
  PUBLIC PXM_WITH_OTEL=0)
target_link_libraries(phoenix_mcp_stdio PUBLIC
  reflectcpp::reflectcpp Folly::folly spdlog::spdlog hfsm2::hfsm2)
if(NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_link_libraries(phoenix_mcp_stdio PUBLIC atomic)
endif()

add_library(tfe_code_core STATIC
  native/cpp_import.cc
  native/go_import.cc
  native/hash.cc
  native/mcp_server.cc
  native/objectbox_store.cc
  native/python_import.cc
  native/render_project.cc
  native/role_document.cc
  native/rust_import.cc
  native/ts_import.cc
  native/workspace.cc)
target_include_directories(tfe_code_core PUBLIC native)
if(APPLE)
  target_link_libraries(tfe_code_core PUBLIC
    textfoundry_objectbox_store textfoundry_core
    unofficial::tree-sitter::tree-sitter
    tree_sitter_python_grammar tree_sitter_cpp_grammar tree_sitter_typescript_grammar tree_sitter_rust_grammar tree_sitter_go_grammar
    ctre::ctre re2::re2 phoenix_mcp_stdio)
else()
  target_link_libraries(tfe_code_core PUBLIC
    "$<LINK_GROUP:RESCAN,textfoundry_core,textfoundry_objectbox_store>"
    unofficial::tree-sitter::tree-sitter
    tree_sitter_python_grammar tree_sitter_cpp_grammar tree_sitter_typescript_grammar tree_sitter_rust_grammar tree_sitter_go_grammar
    ctre::ctre re2::re2 phoenix_mcp_stdio)
endif()
if(OpenMP_CXX_FOUND)
  target_link_libraries(tfe_code_core PUBLIC OpenMP::OpenMP_CXX)
endif()
add_executable(tfe-code-render native/renderer.cc)
target_link_libraries(tfe-code-render PRIVATE tfe_code_core)
add_executable(tfe-code native/main.cc)
target_link_libraries(tfe-code PRIVATE tfe_code_core)
if(APPLE)
  set(_tfe_rpath "@loader_path")
else()
  set(_tfe_rpath "$ORIGIN")
endif()
set_target_properties(tfe-code tfe-code-render PROPERTIES
  INSTALL_RPATH "${_tfe_rpath}"
  BUILD_RPATH "${_tfe_rpath};${CMAKE_CURRENT_BINARY_DIR}/_deps/objectbox-download-src/lib")
option(TFE_CODE_STATIC_LIBSTDCXX "Link libgcc and libstdc++ statically for portable releases" OFF)
if(TFE_CODE_STATIC_LIBSTDCXX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_link_options(tfe-code PRIVATE -static-libgcc -static-libstdc++)
  target_link_options(tfe-code-render PRIVATE -static-libgcc -static-libstdc++)
endif()
set(TFE_CODE_ADD_TESTS "" CACHE STRING
  "Semicolon-separated test module names to build; empty builds every test")
mark_as_advanced(TFE_CODE_ADD_TESTS)

include(CTest)
if(BUILD_TESTING)
  # Every tests/native/test_<module>_*.cc becomes its own executable target,
  # linked to tfe_code_core and registered through add_test. TFE_CODE_ADD_TESTS
  # narrows the registered subset so a worker can build only its own module.
  set(tfe_code_test_modules "")
  file(GLOB tfe_code_test_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native/test_*.cc")
  # The CMake-wiring suite needs compile definitions, and the two legacy suites keep
  # their historical target names, so all three are handled explicitly below.
  list(FILTER tfe_code_test_sources EXCLUDE REGEX "/test_autotests_cmake_wiring_.*\\.cc$")
  list(FILTER tfe_code_test_sources EXCLUDE REGEX "/test_(python_import|workspace)\\.cc$")
  list(SORT tfe_code_test_sources)
  foreach(test_source IN LISTS tfe_code_test_sources)
    get_filename_component(test_name "${test_source}" NAME_WE)
    string(REGEX REPLACE "^test_(.+)_[^_]+$" "\\1" test_module "${test_name}")
    if(test_module STREQUAL test_name)
      message(WARNING "Test file ${test_source} does not match test_<module>_*.cc")
      continue()
    endif()
    list(APPEND tfe_code_test_modules "${test_module}")
    if(TFE_CODE_ADD_TESTS AND NOT test_module IN_LIST TFE_CODE_ADD_TESTS)
      continue()
    endif()
    add_executable(${test_name} "${test_source}")
    target_link_libraries(${test_name} PRIVATE tfe_code_core)
    set_target_properties(${test_name} PROPERTIES
      BUILD_RPATH "${_tfe_rpath};${CMAKE_CURRENT_BINARY_DIR}/_deps/objectbox-download-src/lib")
    add_test(NAME ${test_name} COMMAND ${test_name})
    set(tfe_code_test_registered_namespace "${test_name}")
    list(APPEND tfe_code_test_registered "${tfe_code_test_registered_namespace}")
  endforeach()
  list(REMOVE_DUPLICATES tfe_code_test_modules)
  list(SORT tfe_code_test_modules)
  # The wiring suite is built explicitly (it needs compile definitions), so it is
  # not part of the glob; add its module so the manifest covers it too.
  set(tfe_code_wiring_test test_autotests_cmake_wiring_manifest)
  list(APPEND tfe_code_test_modules "autotests")
  # Legacy suites are registered later; keep their module names in the reported set.
  list(APPEND tfe_code_test_modules "python" "workspace")
  list(REMOVE_DUPLICATES tfe_code_test_modules)
  list(SORT tfe_code_test_modules)
  # Manifest for the CMake-wiring self test: one line per test suite, carrying the
  # module name, the source path and the README verification snippet.
  set(tfe_code_test_manifest "# tfe-code test manifest; generated by CMakeLists.txt\n")
  foreach(test_source IN LISTS tfe_code_test_sources)
    get_filename_component(test_name "${test_source}" NAME_WE)
    string(REGEX REPLACE "^test_(.+)_[^_]+$" "\\1" test_module "${test_name}")
    if(test_module STREQUAL test_name)
      continue()
    endif()
    file(RELATIVE_PATH test_relative "${CMAKE_CURRENT_SOURCE_DIR}" "${test_source}")
    string(APPEND tfe_code_test_manifest
      "CMSUITE:${test_module}:${test_relative}:cmake --build build --target ${test_name}"
      " && ctest --test-dir build -R ^${test_name}$ --output-on-failure\n")
  endforeach()
  if(NOT TFE_CODE_ADD_TESTS OR "autotests" IN_LIST TFE_CODE_ADD_TESTS)
    string(APPEND tfe_code_test_manifest
      "CMSUITE:autotests:tests/native/test_autotests_cmake_wiring_manifest.cc"
      ":cmake --build build --target ${tfe_code_wiring_test}"
      " && ctest --test-dir build -R ^${tfe_code_wiring_test}$ --output-on-failure\n")
  endif()
  # Legacy suites are excluded from the glob but still need a manifest row.
  foreach(legacy_pair IN ITEMS
      "python:tests/native/test_python_import.cc:python_import_tests"
      "workspace:tests/native/test_workspace.cc:workspace_tests")
    string(REPLACE ":" ";" legacy_fields "${legacy_pair}")
    list(GET legacy_fields 0 legacy_module)
    list(GET legacy_fields 1 legacy_source)
    list(GET legacy_fields 2 legacy_target)
    string(APPEND tfe_code_test_manifest
      "CMSUITE:${legacy_module}:${legacy_source}"
      ":cmake --build build --target ${legacy_target}"
      " && ctest --test-dir build -R ^${legacy_target}$ --output-on-failure\n")
  endforeach()
  string(SHA256 tfe_code_test_manifest_sha "${tfe_code_test_manifest}")
  set(tfe_code_test_manifest_path
    "${CMAKE_CURRENT_BINARY_DIR}/tests/native/tfe_code_test_manifest.txt")
  file(WRITE "${tfe_code_test_manifest_path}" "${tfe_code_test_manifest}")

  # Legacy single-file suite: registered unless the subset excludes its module.
  if(NOT TFE_CODE_ADD_TESTS OR "python" IN_LIST TFE_CODE_ADD_TESTS)
    add_executable(python_import_tests tests/native/test_python_import.cc)
    target_link_libraries(python_import_tests PRIVATE tfe_code_core)
    set_target_properties(python_import_tests PROPERTIES
      BUILD_RPATH "${_tfe_rpath};${CMAKE_CURRENT_BINARY_DIR}/_deps/objectbox-download-src/lib")
    add_test(NAME python_import_tests COMMAND python_import_tests)
    list(APPEND tfe_code_test_registered "python_import_tests")
  endif()

  # The wiring suite lives in its own module ("autotests") so a worker can build
  # it explicitly with -DTFE_CODE_ADD_TESTS=autotests.
  if(TFE_CODE_ADD_TESTS AND NOT "autotests" IN_LIST TFE_CODE_ADD_TESTS)
    message(STATUS "tfe-code wiring test skipped: autotests not requested")
  else()
    # Semicolons are CMake list separators: passing a joined list straight to
    # target_compile_definitions shatters it into several -D flags. Escape them
    # so the macro expands to one quoted string literal in C++.
    if(tfe_code_test_registered)
      list(JOIN tfe_code_test_registered ";" tfe_code_registered_joined)
      string(REPLACE ";" "\\;" tfe_code_registered_joined "${tfe_code_registered_joined}")
    else()
      set(tfe_code_registered_joined "")
    endif()
    list(JOIN tfe_code_test_modules ";" tfe_code_modules_joined)
    string(REPLACE ";" "\\;" tfe_code_modules_joined "${tfe_code_modules_joined}")
    add_executable(${tfe_code_wiring_test} tests/native/test_autotests_cmake_wiring_manifest.cc)
    target_link_libraries(${tfe_code_wiring_test} PRIVATE tfe_code_core)
    set_target_properties(${tfe_code_wiring_test} PROPERTIES
      BUILD_RPATH "${_tfe_rpath};${CMAKE_CURRENT_BINARY_DIR}/_deps/objectbox-download-src/lib")
    target_compile_definitions(${tfe_code_wiring_test} PRIVATE
      "TFE_CODE_TEST_MANIFEST=\"${tfe_code_test_manifest_path}\""
      "TFE_CODE_TEST_MANIFEST_SHA256=\"${tfe_code_test_manifest_sha}\""
      "TFE_CODE_REGISTERED_TESTS=\"${tfe_code_registered_joined}\""
      "TFE_CODE_ALL_TEST_MODULES=\"${tfe_code_modules_joined}\"")
    add_test(NAME ${tfe_code_wiring_test} COMMAND ${tfe_code_wiring_test})
  endif()

  message(STATUS "tfe-code test modules: ${tfe_code_test_modules}")
  message(STATUS "tfe-code registered tests: ${tfe_code_test_registered}")

  # test_workspace.cc was split into per-module suites. Keep it as an opt-in
  # fallback (-DTFE_CODE_ADD_TESTS=workspace) because it duplicates the coverage.
  if((TFE_CODE_ADD_TESTS AND "workspace" IN_LIST TFE_CODE_ADD_TESTS) OR
     (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/native/test_workspace.cc" AND
      NOT tfe_code_test_sources))
    add_executable(workspace_tests tests/native/test_workspace.cc)
    target_link_libraries(workspace_tests PRIVATE tfe_code_core)
    set_target_properties(workspace_tests PROPERTIES
      BUILD_RPATH "${_tfe_rpath};${CMAKE_CURRENT_BINARY_DIR}/_deps/objectbox-download-src/lib")
    add_test(NAME workspace_tests COMMAND workspace_tests)
  endif()
endif()
install(TARGETS tfe-code tfe-code-render RUNTIME DESTINATION bin)
