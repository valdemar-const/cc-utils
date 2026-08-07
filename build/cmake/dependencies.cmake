# Project-managed dependencies (registrations + Find<Name>.cmake are regenerated
# by `cpm generate` from build/cmake/modules/3rdparty/deps.toml).
set(CPM_3RDPARTY_DIR "${PROJECT_SOURCE_DIR}/build/cmake/modules/3rdparty")
list(APPEND CMAKE_MODULE_PATH "${CPM_3RDPARTY_DIR}")
include("${CPM_3RDPARTY_DIR}/3rdparty.cmake")

# GoogleTest (tests only). Resolved by cpm: the loc tier uses the preloaded archive
# in $CPM_PRELOAD (offline); the git tier fires when CPM_FETCH is set.
if(BUILD_TESTING)
  find_package(GTest REQUIRED)
endif()
