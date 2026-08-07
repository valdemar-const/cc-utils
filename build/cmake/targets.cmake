# ---------------------------------------------------------------------------
# cc_add_library(<name>
#   SOURCES src1 src2 ...
#   [PUBLIC_INCLUDE_DIRS dir1 ...]   # build-interface include roots
#   [PUBLIC_HEADERS h1 h2 ...]        # informational (install-time use)
#   [PUBLIC_DEPS dep1 ...]            # targets/packages linked publicly
#   [PRIVATE_DEPS dep1 ...])
#
# Produces up to two real targets (gated by CC_BUILD_SHARED / CC_BUILD_STATIC):
#   <name>          SHARED  -> lib<name>.so / lib<name>.dylib / <name>.dll
#   <name>-static   STATIC  -> lib<name>-static.a / <name>-static.lib
# with ALIAS targets for ergonomic linking:
#   <name>::shared  <name>::static  <name>::<name>   (default -> shared if built)
#
# A single export header (<name>_export.hpp, generate_export_header) is shared by
# both variants: the shared target builds with <MACRO>_EXPORTS (dllexport), the
# static target propagates <MACRO>_STATIC_DEFINE so the API macro collapses.
# ---------------------------------------------------------------------------
function(cc_add_library name)
  cmake_parse_arguments(ARG "" ""
    "SOURCES;PUBLIC_INCLUDE_DIRS;PUBLIC_HEADERS;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})

  # cc-parseit -> CC_PARSEIT
  string(TOUPPER "${name}" _macro)
  string(REPLACE "-" "_" _macro "${_macro}")
  string(MAKE_C_IDENTIFIER "${_macro}" _macro)

  set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/${name}")
  file(MAKE_DIRECTORY "${_gen_dir}")

  set(_shared_tgt "")
  set(_static_tgt "")

  # ---- shared variant ----
  if(CC_BUILD_SHARED)
    set(_shared_tgt "${name}")
    add_library("${_shared_tgt}" SHARED ${ARG_SOURCES})
    target_compile_features("${_shared_tgt}" PUBLIC cxx_std_23)
    target_include_directories("${_shared_tgt}" PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<BUILD_INTERFACE:${_gen_dir}>)
    target_link_libraries("${_shared_tgt}"
      PUBLIC  ${ARG_PUBLIC_DEPS}
      PRIVATE ${ARG_PRIVATE_DEPS})
    set_target_properties("${_shared_tgt}" PROPERTIES
      OUTPUT_NAME             "${name}"
      POSITION_INDEPENDENT_CODE ON
      CXX_VISIBILITY_PRESET   hidden
      VISIBILITY_INLINES_HIDDEN ON
      DEFINE_SYMBOL           "${_macro}_EXPORTS"
      VERSION                 "${PROJECT_VERSION}"
      SOVERSION               "${PROJECT_VERSION_MAJOR}")
    if(NOT TARGET "${name}::shared")
      add_library("${name}::shared" ALIAS "${_shared_tgt}")
    endif()
  endif()

  # ---- static variant ----
  if(CC_BUILD_STATIC)
    set(_static_tgt "${name}-static")
    add_library("${_static_tgt}" STATIC ${ARG_SOURCES})
    target_compile_features("${_static_tgt}" PUBLIC cxx_std_23)
    target_include_directories("${_static_tgt}" PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<BUILD_INTERFACE:${_gen_dir}>)
    target_link_libraries("${_static_tgt}"
      PUBLIC  ${ARG_PUBLIC_DEPS}
      PRIVATE ${ARG_PRIVATE_DEPS})
    set_target_properties("${_static_tgt}" PROPERTIES
      OUTPUT_NAME               "${name}-static"
      POSITION_INDEPENDENT_CODE ON)
    # Collapses <MACRO>_API to nothing for this target and its consumers.
    target_compile_definitions("${_static_tgt}" PUBLIC "${_macro}_STATIC_DEFINE")
    if(NOT TARGET "${name}::static")
      add_library("${name}::static" ALIAS "${_static_tgt}")
    endif()
  endif()

  # ---- shared export header (generated once, prefer the shared target) ----
  include(GenerateExportHeader)
  set(_gen_owner "")
  if(_shared_tgt)
    set(_gen_owner "${_shared_tgt}")
  elseif(_static_tgt)
    set(_gen_owner "${_static_tgt}")
  endif()
  generate_export_header("${_gen_owner}"
    BASE_NAME         "${_macro}"
    EXPORT_MACRO_NAME "${_macro}_API"
    STATIC_DEFINE     "${_macro}_STATIC_DEFINE"
    EXPORT_FILE_NAME  "${_gen_dir}/${name}_export.hpp"
    DEFINE_NO_DEPRECATED)

  # ---- default alias <name>::<name> (shared wins) ----
  if(_shared_tgt)
    set(_default "${_shared_tgt}")
  else()
    set(_default "${_static_tgt}")
  endif()
  if(NOT TARGET "${name}::${name}")
    add_library("${name}::${name}" ALIAS "${_default}")
  endif()
endfunction()

# ---------------------------------------------------------------------------
# cc_add_executable(<name> SOURCES ... [PUBLIC_DEPS ...] [PRIVATE_DEPS ...])
# ---------------------------------------------------------------------------
function(cc_add_executable name)
  cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})
  add_executable("${name}" ${ARG_SOURCES})
  target_compile_features("${name}" PRIVATE cxx_std_23)
  if(ARG_PUBLIC_DEPS OR ARG_PRIVATE_DEPS)
    target_link_libraries("${name}"
      PUBLIC  ${ARG_PUBLIC_DEPS}
      PRIVATE ${ARG_PRIVATE_DEPS})
  endif()
endfunction()

# ---------------------------------------------------------------------------
# cc_add_plugin(<name> SOURCES ... [PUBLIC_DEPS ...] [PRIVATE_DEPS ...])
#
# A MODULE library built for runtime loading (dlopen): file is exactly
# <name>.so / <name>.dll (no `lib` prefix), placed in CMAKE_LIBRARY_OUTPUT_DIRECTORY.
# ---------------------------------------------------------------------------
function(cc_add_plugin name)
  cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})
  add_library(${name} MODULE ${ARG_SOURCES})
  target_compile_features(${name} PRIVATE cxx_std_23)
  set_target_properties(${name} PROPERTIES
    OUTPUT_NAME ${name}
    PREFIX ""
    POSITION_INDEPENDENT_CODE ON)
  if(ARG_PUBLIC_DEPS OR ARG_PRIVATE_DEPS)
    target_link_libraries(${name}
      PUBLIC  ${ARG_PUBLIC_DEPS}
      PRIVATE ${ARG_PRIVATE_DEPS})
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Auto-discover project subdirectories under projects/<container>/. Adding a new
# directory with a CMakeLists.txt is enough; CONFIGURE_DEPENDS re-scans on build.
# ---------------------------------------------------------------------------
function(_cc_add_project_dirs container)
  set(_base "${PROJECT_SOURCE_DIR}/projects/${container}")
  if(NOT IS_DIRECTORY "${_base}")
    return()
  endif()
  file(GLOB _entries LIST_DIRECTORIES true CONFIGURE_DEPENDS RELATIVE "${_base}" "${_base}/*")
  foreach(_e IN LISTS _entries)
    if(IS_DIRECTORY "${_base}/${_e}" AND EXISTS "${_base}/${_e}/CMakeLists.txt")
      add_subdirectory("${_base}/${_e}" "${CMAKE_BINARY_DIR}/proj/${container}/${_e}")
    endif()
  endforeach()
endfunction()

_cc_add_project_dirs(lib)
_cc_add_project_dirs(bin)
_cc_add_project_dirs(plugin)

# ---------------------------------------------------------------------------
# Auto-discover per-target test suites under tests/<target>/ (tests enabled only).
# ---------------------------------------------------------------------------
function(_cc_add_test_dirs)
  set(_base "${PROJECT_SOURCE_DIR}/tests")
  if(NOT IS_DIRECTORY "${_base}")
    return()
  endif()
  file(GLOB _entries LIST_DIRECTORIES true CONFIGURE_DEPENDS RELATIVE "${_base}" "${_base}/*")
  foreach(_e IN LISTS _entries)
    if(IS_DIRECTORY "${_base}/${_e}" AND EXISTS "${_base}/${_e}/CMakeLists.txt")
      add_subdirectory("${_base}/${_e}" "${CMAKE_BINARY_DIR}/tests/${_e}")
    endif()
  endforeach()
endfunction()

if(BUILD_TESTING)
  _cc_add_test_dirs()
endif()
