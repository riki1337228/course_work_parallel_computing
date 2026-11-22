cmake_minimum_required(VERSION 3.20)

project(course_work_parallel_computing
    VERSION 1.0.0
    DESCRIPTION "Parallel inverted-index search engine with Ollama query expansion"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(BUILD_API "Build Drogon-based server, CLI and integration tests" ON)
option(BUILD_TESTS "Build GoogleTest test suites" ON)
option(ENABLE_TSAN "Build with ThreadSanitizer (-fsanitize=thread)" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(ENABLE_TSAN)
    message(STATUS "ThreadSanitizer enabled")
    add_compile_options(-fsanitize=thread -g -O1 -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)

if(BUILD_API)
    find_package(Drogon CONFIG REQUIRED)
endif()

add_subdirectory(backend)

if(BUILD_API)
    add_subdirectory(cli)
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
