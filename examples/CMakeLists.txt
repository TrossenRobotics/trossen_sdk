# Demo utilities library (shared by all examples and scripts)
add_library(demo_utils STATIC demo_utils.cpp)
target_link_libraries(demo_utils PUBLIC trossen_sdk)
target_include_directories(demo_utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# SO101 demo
# TODO(so101): Migrate to use examples/so101/config.json with DataCollectionConfig pattern.
# See examples/solo/, stationary/, mobile/ for the target pattern.
add_executable(so101_teleop so101/so101_lerobot_v2.cpp)
target_link_libraries(so101_teleop demo_utils trossen_sdk)

# Trossen AI Solo demo (1 leader + 1 follower + 2 cameras)
find_package(libtrossen_arm)
add_executable(trossen_solo_ai solo/trossen_solo_ai.cpp)
target_link_libraries(trossen_solo_ai demo_utils trossen_sdk libtrossen_arm)

# Trossen AI Stationary demo (bimanual + cameras)
find_package(libtrossen_arm)
add_executable(trossen_stationary_ai stationary/trossen_stationary_ai.cpp)
target_link_libraries(trossen_stationary_ai demo_utils trossen_sdk libtrossen_arm)

# Trossen AI Mobile demo (bimanual + mobile base + cameras)
find_package(libtrossen_arm)
add_executable(trossen_mobile_ai mobile/trossen_mobile_ai.cpp)
target_link_libraries(trossen_mobile_ai demo_utils trossen_sdk libtrossen_arm)

# WidowXAI demo (direct hardware, no session manager)
find_package(libtrossen_arm)
add_executable(widowxai widowxai.cpp)
target_link_libraries(widowxai demo_utils trossen_sdk libtrossen_arm)

# Registry demo executables
add_executable(backend_registry_demo backend_registry_demo.cpp)
target_link_libraries(backend_registry_demo trossen_sdk)

add_executable(hardware_registry_demo hardware_registry_demo.cpp)
target_link_libraries(hardware_registry_demo trossen_sdk)

add_executable(producer_registry_demo producer_registry_demo.cpp)
target_link_libraries(producer_registry_demo trossen_sdk)
