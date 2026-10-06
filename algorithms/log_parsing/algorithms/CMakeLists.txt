cmake_minimum_required(VERSION 3.20)
project(logtop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR "logtop requires GCC 14 or newer; found ${CMAKE_CXX_COMPILER_VERSION}")
endif()

set(LOGTOP_WARNINGS -Wall -Wextra -Wpedantic -Wshadow)

add_library(logtop_core STATIC src/logtop.cpp)
target_include_directories(logtop_core PUBLIC src)
target_compile_options(logtop_core PRIVATE ${LOGTOP_WARNINGS})

add_executable(logtop src/main.cpp)
target_link_libraries(logtop PRIVATE logtop_core)
target_compile_options(logtop PRIVATE ${LOGTOP_WARNINGS})

enable_testing()

# Часть A: разбор одной строки и формат timestamp.
add_executable(logtop_parser_tests tests/parser_tests.cpp)
target_link_libraries(logtop_parser_tests PRIVATE logtop_core)
target_compile_options(logtop_parser_tests PRIVATE ${LOGTOP_WARNINGS})
add_test(NAME parser_unit COMMAND logtop_parser_tests)

# Часть B: алгоритм top-N по уже разобранным записям.
add_executable(logtop_tracker_tests tests/tracker_tests.cpp)
target_link_libraries(logtop_tracker_tests PRIVATE logtop_core)
target_compile_options(logtop_tracker_tests PRIVATE ${LOGTOP_WARNINGS})
add_test(NAME tracker_unit COMMAND logtop_tracker_tests)

# Часть C: stdin -> executable -> stdout.
add_test(NAME end_to_end COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_e2e.sh)
set_tests_properties(end_to_end PROPERTIES ENVIRONMENT "LOGTOP=$<TARGET_FILE:logtop>")
