# Locates an installed trossen_vr static library and headers.
#
# Sets:
#   trossen_vr_FOUND          True if both library and headers are found.
#   TROSSEN_VR_INCLUDE_DIR    Directory containing trossen_vr/network_manager.hpp.
#   TROSSEN_VR_LIBRARY        Full path to libtrossen_vr_lib.a.
#
# Exports:
#   trossen_vr::trossen_vr    Imported STATIC target. Depends on Threads
#                             and nlohmann_json so consumers inherit those
#                             transitively.

find_path(TROSSEN_VR_INCLUDE_DIR
  NAMES trossen_vr/network_manager.hpp
  PATHS /usr/local/include /usr/include
)

find_library(TROSSEN_VR_LIBRARY
  NAMES trossen_vr_lib trossen_vr
  PATHS /usr/local/lib /usr/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(trossen_vr
  REQUIRED_VARS TROSSEN_VR_LIBRARY TROSSEN_VR_INCLUDE_DIR
)

if(trossen_vr_FOUND AND NOT TARGET trossen_vr::trossen_vr)
  # Resolve transitive deps that trossen_vr exposes as PUBLIC in its build.
  find_package(Threads REQUIRED)
  find_package(nlohmann_json REQUIRED)

  add_library(trossen_vr::trossen_vr STATIC IMPORTED)
  set_target_properties(trossen_vr::trossen_vr PROPERTIES
    IMPORTED_LOCATION "${TROSSEN_VR_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${TROSSEN_VR_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "Threads::Threads;nlohmann_json::nlohmann_json"
  )
endif()

mark_as_advanced(
  TROSSEN_VR_INCLUDE_DIR
  TROSSEN_VR_LIBRARY
)
