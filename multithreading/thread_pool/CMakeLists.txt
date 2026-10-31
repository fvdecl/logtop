add_library(thread_pool thread_pool.cpp)

target_include_directories(thread_pool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(thread_pool PUBLIC Threads::Threads)

add_executable(thread_pool_test test.cpp)
target_link_libraries(thread_pool_test PRIVATE thread_pool)

add_test(NAME thread_pool_test COMMAND thread_pool_test)
