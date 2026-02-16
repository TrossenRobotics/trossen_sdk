cmake_minimum_required(VERSION 3.16)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(Threads REQUIRED)
find_package(OpenCV REQUIRED)

# Try to find nlohmann_json
find_package(nlohmann_json QUIET)

# Fetch cpp-httplib
include(FetchContent)
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.14.3
)
FetchContent_MakeAvailable(httplib)

# Add backend executable
add_executable(trossen_backend
    main.cpp
    src/config_manager.cpp
    src/hardware_helpers.cpp
    src/session_actions.cpp
)

# Include directories
target_include_directories(trossen_backend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include
    ${OpenCV_INCLUDE_DIRS}
    /usr/local/include
    /usr/local/include/ftservo
)

# Link libraries
target_link_libraries(trossen_backend
    trossen_sdk
    httplib::httplib
    nlohmann_json::nlohmann_json
    Threads::Threads
    ssl
    crypto
    ${OpenCV_LIBS}
)

# Install target
install(TARGETS trossen_backend
    RUNTIME DESTINATION bin
)
