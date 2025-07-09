cmake_minimum_required(VERSION 3.14)
project(TrossenDataCollectionSDK)

set(CMAKE_CXX_STANDARD 17)



# Find the libtrossen_arm package
find_package(libtrossen_arm REQUIRED)
find_package(Arrow REQUIRED)
find_package(Parquet REQUIRED)
find_package(Boost REQUIRED COMPONENTS program_options)

include_directories(include)
include_directories(${Boost_INCLUDE_DIRS})

# Build library
add_library(trossen_data_collection_sdk src/arms_move.cpp src/dataset.cpp)

target_include_directories(trossen_data_collection_sdk PUBLIC include)


# Link Arrow and libtrossen_arm to the library target


target_link_libraries(trossen_data_collection_sdk
    libtrossen_arm
    Arrow::arrow_shared
    Parquet::parquet_shared
)

# Main executable
add_executable(sleep main.cpp)

add_executable(replay replay.cpp)
add_executable(control control_script.cpp)


target_link_libraries(control  PRIVATE ${Boost_LIBRARIES} trossen_data_collection_sdk)
target_link_libraries(sleep trossen_data_collection_sdk)
target_link_libraries(replay PRIVATE ${Boost_LIBRARIES} trossen_data_collection_sdk)
