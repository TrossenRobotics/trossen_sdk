cmake_minimum_required(VERSION 3.14)
project(TrossenDataCollectionSDK)

set(CMAKE_CXX_STANDARD 17)

set(Torch_DIR "${CMAKE_SOURCE_DIR}/third_party/libtorch/share/cmake/Torch")
set(CMAKE_CUDA_COMPILER /usr/local/cuda/bin/nvcc)

find_package(Torch REQUIRED)


# Find the libtrossen_arm package
find_package(libtrossen_arm REQUIRED)
find_package(Arrow REQUIRED)
find_package(Parquet REQUIRED)
find_package(Boost REQUIRED COMPONENTS program_options)
find_package(nlohmann_json REQUIRED)
find_package(realsense2 REQUIRED)
find_package(OpenCV REQUIRED)
find_package(Torch REQUIRED)


include_directories(include)
include_directories(${Boost_INCLUDE_DIRS})




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
    ${OpenCV_LIBS}
)

# Main executable
add_executable(sleep main.cpp)
add_executable(replay replay.cpp)
add_executable(control control_script.cpp)
add_executable(realsense_main camera.cpp)
# add_executable(config config.cpp)


target_link_libraries(control  PRIVATE ${Boost_LIBRARIES} trossen_data_collection_sdk)
target_link_libraries(sleep trossen_data_collection_sdk)
target_link_libraries(replay PRIVATE ${Boost_LIBRARIES} trossen_data_collection_sdk)
target_link_libraries(realsense_main   trossen_data_collection_sdk)
# target_link_libraries(config PRIVATE ${Boost_LIBRARIES} trossen_data_collection_sdk)

add_executable(torch_test torch.cpp)
target_link_libraries(torch_test "${TORCH_LIBRARIES}")
set_property(TARGET torch_test PROPERTY CXX_STANDARD 17)