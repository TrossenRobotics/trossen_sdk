find_package(libtrossen_arm REQUIRED)

# Trossen Solo AI — 1 leader + 1 follower arm + 2 cameras
add_executable(trossen_solo_ai trossen_solo_ai/trossen_solo_ai.cpp)
target_link_libraries(trossen_solo_ai trossen_sdk libtrossen_arm)

# Trossen Stationary AI — bimanual (2 leader + 2 follower) + 4 cameras
add_executable(trossen_stationary_ai trossen_stationary_ai/trossen_stationary_ai.cpp)
target_link_libraries(trossen_stationary_ai trossen_sdk libtrossen_arm)

# Trossen Mobile AI — bimanual + SLATE mobile base + 3 cameras
add_executable(trossen_mobile_ai trossen_mobile_ai/trossen_mobile_ai.cpp)
target_link_libraries(trossen_mobile_ai trossen_sdk libtrossen_arm)
