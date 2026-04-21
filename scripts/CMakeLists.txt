# ──────────────────────────────────────────────────────────────────────────────
# Scripts — offline data-processing tools
#
# These executables are NOT robot control programs. They process recorded
# MCAP data and do not require a connected robot to run.
#
# Run all tools from the repository root so relative config paths resolve:
#   ./build/scripts/replay_trossen_mcap_jointstate <mcap_file>
#   ./build/scripts/trossen_mcap_to_lerobot_v2 <mcap_file_or_folder>
# ──────────────────────────────────────────────────────────────────────────────

# TrossenMCAP joint-state replay tool
find_package(libtrossen_arm)
add_executable(replay_trossen_mcap_jointstate replay_trossen_mcap_jointstate/replay_trossen_mcap_jointstate.cpp)
target_include_directories(replay_trossen_mcap_jointstate PRIVATE
  ${mcap_SOURCE_DIR}/cpp/mcap/include
)
target_compile_definitions(replay_trossen_mcap_jointstate PRIVATE MCAP_IMPLEMENTATION)
target_link_libraries(replay_trossen_mcap_jointstate trossen_sdk libtrossen_arm zstd lz4)

# TrossenMCAP → LeRobotV2 conversion tool
add_executable(trossen_mcap_to_lerobot_v2 trossen_mcap_to_lerobot_v2/trossen_mcap_to_lerobot_v2.cpp)
target_include_directories(trossen_mcap_to_lerobot_v2 PRIVATE
  ${mcap_SOURCE_DIR}/cpp/mcap/include
  ${CMAKE_BINARY_DIR}
)
target_compile_definitions(trossen_mcap_to_lerobot_v2 PRIVATE MCAP_IMPLEMENTATION)
target_link_libraries(trossen_mcap_to_lerobot_v2 trossen_sdk arrow parquet zstd lz4 protobuf ${OpenCV_LIBS})

# Camera discovery tool
find_package(realsense2 REQUIRED)
add_executable(find_cameras find_cameras/find_cameras.cpp)
target_link_libraries(find_cameras realsense2::realsense2 ${OpenCV_LIBS})
