# Locates an installed trossen_vr static library and headers.
#
# Sets:
#   trossen_vr_FOUND          True if both library and headers are found.
#   TROSSEN_VR_INCLUDE_DIR    Directory containing trossen_vr/vr_manager.hpp.
#   TROSSEN_VR_LIBRARY        Full path to libtrossen_vr.a.
#
# Exports:
#   trossen_vr::trossen_vr    Imported STATIC target. Depends on Threads,
#                             Eigen3, nlohmann_json, and websocketpp so
#                             consumers inherit those transitively — this
#                             mirrors the PUBLIC link interface declared in
#                             trossen_vr's own CMakeLists.

find_path(TROSSEN_VR_INCLUDE_DIR
  NAMES trossen_vr/vr_manager.hpp
  PATHS /usr/local/include /usr/include
)

find_library(TROSSEN_VR_LIBRARY
  NAMES trossen_vr
  PATHS /usr/local/lib /usr/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(trossen_vr
  REQUIRED_VARS TROSSEN_VR_LIBRARY TROSSEN_VR_INCLUDE_DIR
)

if(trossen_vr_FOUND AND NOT TARGET trossen_vr::trossen_vr)
  # Resolve transitive deps that trossen_vr exposes as PUBLIC in its build.
  find_package(Threads REQUIRED)
  find_package(Eigen3 3.3 REQUIRED NO_MODULE)
  find_package(nlohmann_json REQUIRED)

  # websocketpp is header-only and ships via apt without a CMake config on
  # some distros; find the include dir directly.
  find_path(TROSSEN_VR_WEBSOCKETPP_INCLUDE_DIR
    NAMES websocketpp/server.hpp
    PATHS /usr/local/include /usr/include
  )

  add_library(trossen_vr::trossen_vr STATIC IMPORTED)
  set_target_properties(trossen_vr::trossen_vr PROPERTIES
    IMPORTED_LOCATION "${TROSSEN_VR_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES
      "${TROSSEN_VR_INCLUDE_DIR};${TROSSEN_VR_WEBSOCKETPP_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES
      "Threads::Threads;Eigen3::Eigen;nlohmann_json::nlohmann_json"
  )
endif()

mark_as_advanced(
  TROSSEN_VR_INCLUDE_DIR
  TROSSEN_VR_LIBRARY
  TROSSEN_VR_WEBSOCKETPP_INCLUDE_DIR
)
