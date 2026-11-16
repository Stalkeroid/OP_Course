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
# search_server executable — ADDED BY THE backend-api AGENT HERE.
# Fetch Crow / nlohmann_json / cpp-httplib (pinned tags) in this section and
# link the executable against search_core.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Unit tests (GoogleTest via FetchContent, pinned tag)
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
  include(FetchContent)
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
endif()
