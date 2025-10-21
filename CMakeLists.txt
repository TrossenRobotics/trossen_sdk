# TODO [TDS-17] Cleanup and organize CMake
cmake_minimum_required(VERSION 3.25.0)
project(TrossenDataCollectionSDK)

set(CMAKE_CXX_STANDARD 17)
set(realsense2_DIR "/usr/lib/x86_64-linux-gnu/cmake/realsense2")

include(FetchContent)


# Find the libtrossen_arm package
find_package(libtrossen_arm REQUIRED)
find_package(Arrow REQUIRED)
find_package(Parquet REQUIRED)
find_package(Boost REQUIRED COMPONENTS program_options)
find_package(nlohmann_json REQUIRED)
find_package(realsense2 REQUIRED)
find_package(OpenCV REQUIRED)


include_directories(${Boost_INCLUDE_DIRS})

FetchContent_Declare(
  spdlog
  SYSTEM
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG 6fa36017cfd5731d617e1a934f0e5ea9c4445b13 # 1.15.3
)
set(SPDLOG_BUILD_PIC ON)
FetchContent_MakeAvailable(spdlog)


# Collect all source files
set( SOURCE_FILES
    src/trossen_ai_robot_devices/trossen_ai_robot.cpp
    src/trossen_ai_robot_devices/trossen_ai_driver.cpp
    src/trossen_ai_robot_devices/trossen_ai_cameras.cpp
    src/trossen_dataset/dataset.cpp
    src/trossen_sdk_utils/control_utils.cpp
)
# Build library
add_library(trossen_data_collection_sdk SHARED ${SOURCE_FILES})

target_include_directories(trossen_data_collection_sdk
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)


# Link Arrow and libtrossen_arm to the library target

target_link_libraries(trossen_data_collection_sdk
    libtrossen_arm
    Arrow::arrow_shared
    Parquet::parquet_shared
    nlohmann_json::nlohmann_json
    realsense2::realsense2
    spdlog::spdlog
    ${OpenCV_LIBS}
)

# Main executable
include(CMakePackageConfigHelpers)

# Install library and headers
install(TARGETS trossen_data_collection_sdk
  EXPORT TrossenDataCollectionSDKTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include
)
install(DIRECTORY include/ DESTINATION include)

# Export targets for use from the build tree
export(EXPORT TrossenDataCollectionSDKTargets
  FILE "${CMAKE_CURRENT_BINARY_DIR}/TrossenDataCollectionSDKTargets.cmake"
  NAMESPACE TrossenDataCollectionSDK::
)

# Ensure spdlog is exported if it's a dependency
get_target_property(_spdlog_imported spdlog IMPORTED)
if(NOT _spdlog_imported)
  install(TARGETS spdlog
    EXPORT TrossenDataCollectionSDKTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
  )
endif()

# Generate and install a config file
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/trossen_data_collection_sdkConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/trossen_data_collection_sdkConfig.cmake"
  INSTALL_DESTINATION lib/cmake/trossen_data_collection_sdk
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/trossen_data_collection_sdkConfig.cmake"
  DESTINATION lib/cmake/trossen_data_collection_sdk
)
install(EXPORT TrossenDataCollectionSDKTargets
  FILE TrossenDataCollectionSDKTargets.cmake
  NAMESPACE TrossenDataCollectionSDK::
  DESTINATION lib/cmake/trossen_data_collection_sdk
)
add_executable(sleep scripts/sleep.cpp)
add_executable(replay scripts/replay.cpp)
add_executable(record scripts/record.cpp)
add_executable(teleop scripts/teleop.cpp)
add_executable(video_conversion scripts/video_conversion.cpp)


target_link_libraries(record  PRIVATE ${Boost_LIBRARIES} trossen_data_collection_sdk)
target_link_libraries(sleep  PRIVATE ${Boost_LIBRARIES} trossen_data_collection_sdk)
target_link_libraries(replay PRIVATE ${Boost_LIBRARIES} trossen_data_collection_sdk)
target_link_libraries(teleop PRIVATE ${Boost_LIBRARIES} trossen_data_collection_sdk)
target_link_libraries(video_conversion PRIVATE ${Boost_LIBRARIES} trossen_data_collection_sdk)

