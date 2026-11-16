# ---------------------------------------------------------------------------
# search_core: parallel core (thread pool, tokenizer, inverted index, snippet,
# index builder). No HTTP / JSON / Ollama dependencies.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_library(search_core STATIC
  src/custom_thread_pool.cpp
  src/tokenizer.cpp
  src/inverted_index.cpp
  src/snippet.cpp
  src/index_builder.cpp
)
target_include_directories(search_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(search_core PUBLIC cxx_std_20)
target_link_libraries(search_core PUBLIC Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(search_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Optional performance sanity tool (not part of `all`):
#   cmake --build build --target index_bench
add_executable(index_bench EXCLUDE_FROM_ALL tools/index_bench.cpp)
target_link_libraries(index_bench PRIVATE search_core)

# ---------------------------------------------------------------------------
# search_server executable (HTTP / WebSocket service layer).
#
# Third-party code comes from FetchContent at pinned tags. All of it is
# header-only and is used through our own INTERFACE targets (SOURCE_SUBDIR
# points to a non-existent directory so the projects' own CMakeLists.txt are
# never run: no OpenSSL / zlib / examples / tests are configured).
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

FetchContent_Declare(
  asio_src
  GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
  GIT_TAG asio-1-36-0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _no_cmake_
)
FetchContent_Declare(
  crow_src
  GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
  GIT_TAG v1.3.5
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _no_cmake_
)
FetchContent_Declare(
  nlohmann_json_src
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _no_cmake_
)
FetchContent_Declare(
  httplib_src
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.27.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _no_cmake_
)
FetchContent_MakeAvailable(asio_src crow_src nlohmann_json_src httplib_src)

add_library(third_party_headers INTERFACE)
target_include_directories(third_party_headers SYSTEM INTERFACE
  ${asio_src_SOURCE_DIR}/asio/include
  ${crow_src_SOURCE_DIR}/include
  ${nlohmann_json_src_SOURCE_DIR}/include
  ${httplib_src_SOURCE_DIR}
)
# Standalone Asio (no Boost); no SSL, no compression features of Crow/httplib.
target_compile_definitions(third_party_headers INTERFACE
  ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_link_libraries(third_party_headers INTERFACE Threads::Threads)

add_library(search_service STATIC
  src/config.cpp
  src/request_validation.cpp
  src/ollama_client.cpp
)
target_include_directories(search_service PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(search_service PUBLIC cxx_std_20)
target_link_libraries(search_service PUBLIC search_core third_party_headers)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(search_service PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Unit tests (GoogleTest via FetchContent, pinned tag)
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(core_tests
    tests/thread_pool_test.cpp
    tests/tokenizer_test.cpp
    tests/inverted_index_test.cpp
    tests/snippet_test.cpp
    tests/index_builder_test.cpp
    tests/concurrency_test.cpp
  )
  target_link_libraries(core_tests PRIVATE search_core GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(core_tests DISCOVERY_TIMEOUT 60)

  add_executable(service_tests
    tests/request_validation_test.cpp
    tests/ollama_client_test.cpp
        tests/config_test.cpp
  )
  target_link_libraries(service_tests PRIVATE search_service GTest::gtest_main)
  gtest_discover_tests(service_tests DISCOVERY_TIMEOUT 60)
endif()
