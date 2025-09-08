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


include_directories(include)
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

target_include_directories(trossen_data_collection_sdk PUBLIC include)


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
add_executable(sleep main.cpp)
add_executable(replay replay.cpp)
add_executable(control control_script.cpp)
add_executable(teleop teleop.cpp)


target_link_libraries(control  PRIVATE ${Boost_LIBRARIES} trossen_data_collection_sdk)
target_link_libraries(sleep trossen_data_collection_sdk)
target_link_libraries(replay PRIVATE ${Boost_LIBRARIES} trossen_data_collection_sdk)
target_link_libraries(teleop PRIVATE ${Boost_LIBRARIES} trossen_data_collection_sdk)
