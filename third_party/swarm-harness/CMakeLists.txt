cmake_minimum_required(VERSION 3.20)
project(swarm_harness VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_executable(swarm-harness
    src/main.cpp
    src/core.cpp
)

target_include_directories(swarm-harness PRIVATE
    include
    vendor
)

target_link_libraries(swarm-harness PRIVATE
    CURL::libcurl
    OpenSSL::Crypto
    Threads::Threads
)

target_compile_options(swarm-harness PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)

enable_testing()
add_test(
    NAME deterministic_acceptance
    COMMAND ${CMAKE_COMMAND} -E env
        SWARM_HARNESS_BIN=$<TARGET_FILE:swarm-harness>
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_acceptance.py
)
