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
#   ./build/scripts/trossen_mcap_to_lerobot_v3 <mcap_file_or_folder>
#   ./build/scripts/trossen_mcap_transcode_video <mcap_file_or_folder>
# ──────────────────────────────────────────────────────────────────────────────

# TrossenMCAP joint-state replay tool
find_package(libtrossen_arm)
add_executable(replay_trossen_mcap_jointstate replay_trossen_mcap_jointstate/replay_trossen_mcap_jointstate.cpp)
target_link_libraries(replay_trossen_mcap_jointstate trossen_sdk libtrossen_arm)
# Rivet base replay needs the trossen_base library, which trossen_sdk links only
# with this option on.
if(TROSSEN_ENABLE_RIVET)
  target_compile_definitions(replay_trossen_mcap_jointstate PRIVATE TROSSEN_ENABLE_RIVET)
endif()

# TrossenMCAP → LeRobotV2 conversion tool
add_executable(trossen_mcap_to_lerobot_v2 trossen_mcap_to_lerobot_v2/trossen_mcap_to_lerobot_v2.cpp)
target_include_directories(trossen_mcap_to_lerobot_v2 PRIVATE ${CMAKE_BINARY_DIR})
target_link_libraries(trossen_mcap_to_lerobot_v2 trossen_sdk arrow parquet protobuf ${OpenCV_LIBS})

# TrossenMCAP → LeRobotV3 conversion tool
# Threads: the v3 converter prepares episodes (decode/extract/encode) on a
# worker pool while the writer aggregates them single-threaded and in order.
find_package(Threads REQUIRED)
add_executable(trossen_mcap_to_lerobot_v3 trossen_mcap_to_lerobot_v3/trossen_mcap_to_lerobot_v3.cpp)
target_include_directories(trossen_mcap_to_lerobot_v3 PRIVATE ${CMAKE_BINARY_DIR})
target_link_libraries(trossen_mcap_to_lerobot_v3 trossen_sdk arrow parquet protobuf ${OpenCV_LIBS} Threads::Threads)

# Raw-image → compressed-video MCAP transcode tool
# Needs the SDK built with -DTROSSEN_ENABLE_VIDEO_ENCODE=ON; without it the tool builds
# but reports that the build has no encoder.
add_executable(trossen_mcap_transcode_video
  trossen_mcap_transcode_video/trossen_mcap_transcode_video.cpp)
target_include_directories(trossen_mcap_transcode_video PRIVATE ${CMAKE_BINARY_DIR})
target_link_libraries(trossen_mcap_transcode_video trossen_sdk protobuf ${OpenCV_LIBS})
