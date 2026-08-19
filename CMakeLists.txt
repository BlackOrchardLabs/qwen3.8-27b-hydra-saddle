cmake_minimum_required(VERSION 3.20)
project(dispatch_organ VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_library(dispatch_organ_core
    src/core.cpp
    src/harness_client.cpp
    src/ledger.cpp
    src/workroom.cpp
)

target_include_directories(dispatch_organ_core PUBLIC
    include
    third_party
)

target_link_libraries(dispatch_organ_core PUBLIC
    OpenSSL::Crypto
    Threads::Threads
)

target_compile_options(dispatch_organ_core PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)

add_executable(dispatch-organ src/main.cpp)
target_link_libraries(dispatch-organ PRIVATE dispatch_organ_core)

enable_testing()
add_subdirectory(third_party/swarm-harness)

add_executable(dispatch-organ-core-tests tests/core_tests.cpp)
target_link_libraries(dispatch-organ-core-tests PRIVATE dispatch_organ_core)
add_test(NAME phase_a_core COMMAND dispatch-organ-core-tests)

add_executable(dispatch-organ-workroom-tests tests/workroom_tests.cpp)
target_link_libraries(dispatch-organ-workroom-tests PRIVATE dispatch_organ_core)
add_test(NAME phase_b_workroom COMMAND dispatch-organ-workroom-tests)

add_executable(dispatch-organ-ledger-tests tests/ledger_tests.cpp)
target_link_libraries(dispatch-organ-ledger-tests PRIVATE dispatch_organ_core)
add_test(NAME phase_c_ledger COMMAND dispatch-organ-ledger-tests)

add_executable(dispatch-organ-integration-tests tests/integration_tests.cpp)
target_link_libraries(dispatch-organ-integration-tests PRIVATE dispatch_organ_core)
add_test(NAME integrated_gate_journal COMMAND dispatch-organ-integration-tests)
add_test(
    NAME phase_a_process_boundary
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_phase_a.py
        --organ $<TARGET_FILE:dispatch-organ>
        --fake-harness ${CMAKE_SOURCE_DIR}/tests/fake_harness.py
        --policy ${CMAKE_SOURCE_DIR}/config/local_qwen.json
        --job ${CMAKE_SOURCE_DIR}/fixtures/phase_a_live_job.json
)

add_test(
    NAME phase_b_process_boundary
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_phase_b.py
        --organ $<TARGET_FILE:dispatch-organ>
        --policy ${CMAKE_SOURCE_DIR}/config/local_qwen.json
        --workroom-policy ${CMAKE_SOURCE_DIR}/config/workroom.json
)

add_test(
    NAME phase_c_process_boundary
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_phase_c.py
        --organ $<TARGET_FILE:dispatch-organ>
        --fake-harness ${CMAKE_SOURCE_DIR}/tests/fake_harness.py
        --policy ${CMAKE_SOURCE_DIR}/config/local_qwen.json
        --job ${CMAKE_SOURCE_DIR}/fixtures/phase_a_live_job.json
        --workroom-policy ${CMAKE_SOURCE_DIR}/config/workroom.json
        --ledger-policy ${CMAKE_SOURCE_DIR}/config/ledger.json
)

add_test(
    NAME any_qwen_model_mode
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_any_qwen_mode.py
)
