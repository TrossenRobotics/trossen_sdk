# ──────────────────────────────────────────────────────────────────────────────
# Scripts — offline data-processing tools
#
# Mostly offline: these process recorded MCAP data and need no connected robot.
# glide_input_probe is the exception — it reads live handles — but it only reads,
# and commands nothing.
#
# Run all tools from the repository root so relative config paths resolve:
#   ./build/scripts/replay_trossen_mcap_jointstate <mcap_file>
#   ./build/scripts/trossen_mcap_to_lerobot_v2 <mcap_file_or_folder>
#   ./build/scripts/trossen_mcap_to_lerobot_v3 <mcap_file_or_folder>
#   ./build/scripts/glide_input_probe [--left IP] [--right IP]
# ──────────────────────────────────────────────────────────────────────────────

# TrossenMCAP joint-state replay tool
find_package(libtrossen_arm)
add_executable(replay_trossen_mcap_jointstate replay_trossen_mcap_jointstate/replay_trossen_mcap_jointstate.cpp)
target_include_directories(replay_trossen_mcap_jointstate PRIVATE
  ${mcap_SOURCE_DIR}/cpp/mcap/include
)
target_compile_definitions(replay_trossen_mcap_jointstate PRIVATE MCAP_IMPLEMENTATION)
target_link_libraries(replay_trossen_mcap_jointstate trossen_sdk libtrossen_arm zstd lz4)

# Glide handle input probe — bring-up tool for button bits and stick signs.
# Needs the driver's input-report API, detected at the top level; skipped rather
# than failing the build on a driver without it, since nothing else here needs it.
if(TROSSEN_HAVE_ARM_INPUT_REPORT)
  add_executable(glide_input_probe glide_input_probe/glide_input_probe.cpp)
  target_link_libraries(glide_input_probe libtrossen_arm)
else()
  message(STATUS
    "skipping glide_input_probe — libtrossen_arm has no input-report API")
endif()

# ──────────────────────────────────────────────────────────────────────────────
# Shared converter core: MCAP decode + stream alignment + feature schema.
#
# Used by both the v2 and v3 converters. This library owns the single
# MCAP_IMPLEMENTATION translation unit; the executables link against it and must
# NOT define MCAP_IMPLEMENTATION themselves (that would duplicate the symbols).
# Include dirs are PUBLIC so the converters resolve mcap_dataset_loader.hpp and
# its transitive mcap / generated-protobuf headers without restating them.
# ──────────────────────────────────────────────────────────────────────────────
add_library(lerobot_convert_common STATIC
  common/mcap_dataset_loader.cpp
  common/lerobot_v3_writer.cpp
)
target_include_directories(lerobot_convert_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/common
  ${mcap_SOURCE_DIR}/cpp/mcap/include
  ${CMAKE_BINARY_DIR}
)
# NOTE: MCAP_IMPLEMENTATION is #defined inside common/mcap_dataset_loader.cpp (the single
# implementation TU). It must NOT be set target-wide, or every source compiling the mcap
# headers would emit the implementation and collide at link.
target_link_libraries(lerobot_convert_common PUBLIC trossen_sdk arrow parquet protobuf ${OpenCV_LIBS} zstd lz4)

# TrossenMCAP → LeRobotV2 conversion tool
add_executable(trossen_mcap_to_lerobot_v2 trossen_mcap_to_lerobot_v2/trossen_mcap_to_lerobot_v2.cpp)
target_include_directories(trossen_mcap_to_lerobot_v2 PRIVATE ${CMAKE_BINARY_DIR})
target_link_libraries(trossen_mcap_to_lerobot_v2 lerobot_convert_common arrow parquet ${OpenCV_LIBS})

# TrossenMCAP → LeRobotV3 conversion tool
add_executable(trossen_mcap_to_lerobot_v3 trossen_mcap_to_lerobot_v3/trossen_mcap_to_lerobot_v3.cpp)
target_include_directories(trossen_mcap_to_lerobot_v3 PRIVATE ${CMAKE_BINARY_DIR})
target_link_libraries(trossen_mcap_to_lerobot_v3 lerobot_convert_common arrow parquet ${OpenCV_LIBS})
