cmake_minimum_required(VERSION 3.16)

find_package(libtrossen_arm REQUIRED)

add_executable(soma
  src/main.cpp
  src/soma.cpp
)

target_include_directories(soma PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(soma
  trossen_sdk
  libtrossen_arm
)
