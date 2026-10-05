cmake_minimum_required(VERSION 3.20)
project(logtop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# std::chrono::parse и std::format для chrono есть в libstdc++ 14 и новее.
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

add_executable(logtop_tests tests/unit_tests.cpp)
target_link_libraries(logtop_tests PRIVATE logtop_core)
target_compile_options(logtop_tests PRIVATE ${LOGTOP_WARNINGS})

add_test(NAME unit COMMAND logtop_tests)
add_test(NAME end_to_end COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_e2e.sh)
set_tests_properties(end_to_end PROPERTIES ENVIRONMENT "LOGTOP=$<TARGET_FILE:logtop>")
