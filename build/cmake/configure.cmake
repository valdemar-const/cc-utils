# The top-level project() is declared LANGUAGES NONE so the environment wrapper
# stays compiler-agnostic until options are resolved. Enable C++ here, once.
enable_language(CXX)
# boost.context (pulled in transitively by boost.cobalt/fiber) builds its
# x86_64 fiber trampolines as MASM assembly under MSVC; Ninja does not auto-detect
# the ASM language the way the Visual Studio generator does, so enable it here
# to make it propagate to FetchContent'd subdirectories (boost).
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  enable_language(ASM_MASM)
endif()

set(CMAKE_CXX_STANDARD          23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# Keep build artefacts out of the source tree, grouped by kind.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# Runtime-loaded plugin modules live apart from build-linked shared libs:
# conceptually they are assets discovered by the host, not libraries linked at
# build time. cc_add_plugin routes its MODULE target here on every platform.
# Keep them beside the executables (<runtime>/plugins): the host's plugin_loader
# finds them via <exe dir>/plugins, and on Windows the exe's directory is the OS
# "application directory" that resolves the plugins' shared deps (libcc-*.dll).
# An install layout may differ — it is handled via CCP_PLUGIN_PATH / install cfg.
# (Normal variable, not CACHE: it must always track CMAKE_RUNTIME_OUTPUT_DIRECTORY
# and not be frozen by a stale first-configure cache entry.)
unset(CC_PLUGIN_OUTPUT_DIRECTORY CACHE)
set(CC_PLUGIN_OUTPUT_DIRECTORY  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins")

if(CC_PIC)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

add_compile_options(
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-Wall>
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-Wextra>
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-Wpedantic>
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/W4>
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/permissive->
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/Zc:__cplusplus>
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/utf-8>
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>,$<BOOL:${CC_WERROR}>>:/WX>
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>,$<BOOL:${CC_WERROR}>>:-Werror>
)

# boost uses MSVC #pragma auto-linking that guesses the toolset tag from _MSC_VER;
# new MSVC versions confuse it (e.g. headers ask for -vc143- while CMake built
# -vc145-). Since we link boost through CMake targets, disable pragma auto-link.
add_compile_definitions($<$<COMPILE_LANGUAGE:CXX>:BOOST_ALL_NO_LIB>)

if(BUILD_TESTING)
  enable_testing()
  include(GoogleTest)
endif()

# cpm patch tool. The engine default stores the command as a single
# space-separated string (e.g. "patch -p1 -i"), which CMake COMMAND reads as one
# non-existent executable. Supply a proper ';' command list instead. Prefer `patch`
# when available (matches the Unix default), fall back to `git apply`.
find_program(CPM_PATCH_PROG patch)
if(CPM_PATCH_PROG)
  set(CPM_PATCH_COMMAND "patch;-p1;-i" CACHE STRING "" FORCE)
else()
  set(CPM_PATCH_COMMAND "git;apply" CACHE STRING "" FORCE)
endif()
