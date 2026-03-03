# Demo utilities library (shared by all examples)
add_library(demo_utils STATIC demo_utils.cpp)
target_link_libraries(demo_utils PUBLIC trossen_sdk)
target_include_directories(demo_utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# SO101 demo
add_executable(so101_teleop so101_lerobot.cpp)
target_link_libraries(so101_teleop demo_utils trossen_sdk)

# Trossen AI Solo demo
find_package(libtrossen_arm)
add_executable(widowxai widowxai.cpp)
target_link_libraries(widowxai demo_utils trossen_sdk libtrossen_arm)

# Trossen AI Solo demo
find_package(libtrossen_arm)
add_executable(widowxai_lerobot widowxai_lerobot.cpp)
target_link_libraries(widowxai_lerobot demo_utils trossen_sdk libtrossen_arm)

# Stationary AI Kit demo
find_package(libtrossen_arm)
add_executable(widowxai_bimanual widowxai_bimanual.cpp)
target_link_libraries(widowxai_bimanual demo_utils trossen_sdk libtrossen_arm)

# Bimanual AI Kit with RealSense demo (Stationary)
find_package(libtrossen_arm)
add_executable(trossen_stationary_ai stationary/trossen_stationary_ai.cpp)
target_link_libraries(trossen_stationary_ai demo_utils trossen_sdk libtrossen_arm)

# Solo AI Kit demo (1 leader + 1 follower + 2 cameras)
find_package(libtrossen_arm)
add_executable(trossen_solo_ai solo/trossen_solo_ai.cpp)
target_link_libraries(trossen_solo_ai demo_utils trossen_sdk libtrossen_arm)

# Backend registry demo
add_executable(backend_registry_demo backend_registry_demo.cpp)
target_link_libraries(backend_registry_demo trossen_sdk)

# Hardware registry demo
add_executable(hardware_registry_demo hardware_registry_demo.cpp)
target_link_libraries(hardware_registry_demo trossen_sdk)

# Trossen AI Solo demo Async
# TODO (shantanuparab-tr): This example is broken due to the producer registry changes.
# We need to update the example to use the new producer registry API.
# find_package(libtrossen_arm)
# add_executable(widowxai_lerobot_async widowxai_lerobot_async.cpp)
# target_link_libraries(widowxai_lerobot_async demo_utils trossen_sdk libtrossen_arm)

# Producer registry demo
add_executable(producer_registry_demo producer_registry_demo.cpp)
target_link_libraries(producer_registry_demo trossen_sdk)

# Bimanual AI Kit with Mobile Base demo
find_package(libtrossen_arm)
add_executable(trossen_mobile_ai mobile/trossen_mobile_ai.cpp)
target_link_libraries(trossen_mobile_ai demo_utils trossen_sdk libtrossen_arm)

# MCAP replay demo
find_package(libtrossen_arm)
add_executable(replay_mcap_jointstate replay_mcap_jointstate.cpp)
target_include_directories(replay_mcap_jointstate PRIVATE
  ${mcap_SOURCE_DIR}/cpp/mcap/include
)
target_compile_definitions(replay_mcap_jointstate PRIVATE MCAP_IMPLEMENTATION)
target_link_libraries(replay_mcap_jointstate demo_utils trossen_sdk libtrossen_arm zstd lz4)

# MCAP to LeRobot converter with statistics
add_executable(mcap_to_lerobot mcap_to_lerobot.cpp)
target_include_directories(mcap_to_lerobot PRIVATE
  ${mcap_SOURCE_DIR}/cpp/mcap/include
  ${CMAKE_BINARY_DIR}  # For generated protobuf headers
)
target_compile_definitions(mcap_to_lerobot PRIVATE MCAP_IMPLEMENTATION)
target_link_libraries(mcap_to_lerobot demo_utils trossen_sdk arrow parquet zstd lz4 protobuf ${OpenCV_LIBS})
