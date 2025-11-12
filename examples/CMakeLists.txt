# Demo utilities library (shared by all examples)
add_library(demo_utils STATIC demo_utils.cpp)
target_link_libraries(demo_utils PUBLIC trossen_sdk)
target_include_directories(demo_utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Trossen AI Solo demo
find_package(libtrossen_arm)
add_executable(widowxai widowxai.cpp)
target_link_libraries(widowxai demo_utils trossen_sdk libtrossen_arm)

# Stationary AI Kit demo
find_package(libtrossen_arm)
add_executable(widowxai_bimanual widowxai_bimanual.cpp)
target_link_libraries(widowxai_bimanual demo_utils trossen_sdk libtrossen_arm)