# Fetch Google Test
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

# Unit test executables
add_executable(test_timestamp test_timestamp.cpp)
target_link_libraries(test_timestamp PRIVATE trossen_sdk GTest::gtest_main)
add_test(NAME test_timestamp COMMAND test_timestamp)

add_executable(test_record test_record.cpp)
target_link_libraries(test_record PRIVATE trossen_sdk GTest::gtest_main)
add_test(NAME test_record COMMAND test_record)

add_executable(test_null_backend test_null_backend.cpp)
target_link_libraries(test_null_backend PRIVATE trossen_sdk GTest::gtest_main)
add_test(NAME test_null_backend COMMAND test_null_backend)

add_executable(test_backend_registry test_backend_registry.cpp)
target_link_libraries(test_backend_registry PRIVATE trossen_sdk GTest::gtest_main)
add_test(NAME test_backend_registry COMMAND test_backend_registry)

add_executable(test_push_producer_registry test_push_producer_registry.cpp)
target_link_libraries(test_push_producer_registry PRIVATE trossen_sdk GTest::gtest_main)
add_test(NAME test_push_producer_registry COMMAND test_push_producer_registry)

add_executable(test_push_producer_base test_push_producer_base.cpp)
target_link_libraries(test_push_producer_base PRIVATE trossen_sdk GTest::gtest_main)
add_test(NAME test_push_producer_base COMMAND test_push_producer_base)

# Enable CTest integration
include(GoogleTest)
gtest_discover_tests(test_timestamp)
gtest_discover_tests(test_record)
gtest_discover_tests(test_null_backend)
gtest_discover_tests(test_backend_registry)
gtest_discover_tests(test_push_producer_registry)
gtest_discover_tests(test_push_producer_base)
