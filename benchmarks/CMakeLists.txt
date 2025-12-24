# Camera benchmark tool
add_executable(camera_benchmark camera_benchmark.cpp)
target_link_libraries(camera_benchmark trossen_sdk)

# Jetson Orin AGX platform validation benchmark
add_executable(jetson_benchmark jetson_benchmark.cpp)
target_link_libraries(jetson_benchmark trossen_sdk)
