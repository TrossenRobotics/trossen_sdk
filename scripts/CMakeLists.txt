# ──────────────────────────────────────────────────────────────────────────────
# Scripts — standalone offline data-processing tools
#
# These executables are NOT robot control programs. They process recorded
# MCAP data and do not require a connected robot to run.
#
# The MCAP reader is compiled into trossen_sdk, which owns the single
# MCAP_IMPLEMENTATION translation unit, so no target here defines the macro.
#
# Run all tools from the repository root so relative config paths resolve:
#   ./build/scripts/replay_trossen_mcap_jointstate <mcap_file>
#   ./build/scripts/trossen_mcap_to_lerobot_v2 <mcap_file_or_folder>
# ──────────────────────────────────────────────────────────────────────────────

# TrossenMCAP joint-state replay tool
find_package(libtrossen_arm)
add_executable(replay_trossen_mcap_jointstate replay_trossen_mcap_jointstate/replay_trossen_mcap_jointstate.cpp)
target_link_libraries(replay_trossen_mcap_jointstate trossen_sdk libtrossen_arm)

# TrossenMCAP → LeRobotV2 conversion tool
add_executable(trossen_mcap_to_lerobot_v2 trossen_mcap_to_lerobot_v2/trossen_mcap_to_lerobot_v2.cpp)
target_include_directories(trossen_mcap_to_lerobot_v2 PRIVATE ${CMAKE_BINARY_DIR})
target_link_libraries(trossen_mcap_to_lerobot_v2 trossen_sdk arrow parquet protobuf ${OpenCV_LIBS})
