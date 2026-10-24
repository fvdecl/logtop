add_library(scheduler scheduler.cpp)

target_include_directories(scheduler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(scheduler PUBLIC Threads::Threads)

add_executable(scheduler_test test.cpp)
target_link_libraries(scheduler_test PRIVATE scheduler)

add_test(NAME scheduler_test COMMAND scheduler_test)
