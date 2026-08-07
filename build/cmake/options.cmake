option(CC_BUILD_SHARED "Build shared library variants (.so/.dylib/.dll)" ON)
option(CC_BUILD_STATIC "Build static library variants (.a/.lib, suffixed -static)" ON)
option(CC_PIC          "Generate position-independent code"                       ON)
option(CC_WERROR       "Treat compiler warnings as errors"                        OFF)

# BUILD_TESTING is honoured by the ccl test harness; GoogleTest is fetched on demand
# in dependencies.cmake when this is ON.
option(BUILD_TESTING "Build unit tests (pulls GoogleTest via CPM)" OFF)

if(NOT CC_BUILD_SHARED AND NOT CC_BUILD_STATIC)
  message(FATAL_ERROR
    "Neither CC_BUILD_SHARED nor CC_BUILD_STATIC is ON — nothing to build. "
    "Enable at least one of them.")
endif()
