cmake_minimum_required(VERSION 3.20)
project(parallel_search_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(BUILD_TESTING "Build the GoogleTest unit tests (core_tests)" ON)
option(ENABLE_TSAN "Build everything with ThreadSanitizer" OFF)

if(ENABLE_TSAN)
  # Applied globally so GoogleTest and all targets are instrumented too.
  add_compile_options(-fsanitize=thread -g -O1 -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

if(BUILD_TESTING)
  enable_testing()
endif()

add_subdirectory(server)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/cli/CMakeLists.txt)
  add_subdirectory(cli)
endif()
