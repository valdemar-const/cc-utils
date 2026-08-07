# The top-level project() is declared LANGUAGES NONE so the environment wrapper
# stays compiler-agnostic until options are resolved. Enable C++ here, once.
enable_language(CXX)

set(CMAKE_CXX_STANDARD          23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# Keep build artefacts out of the source tree, grouped by kind.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(CC_PIC)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

add_compile_options(
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wextra>
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wpedantic>
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
  $<$<CXX_COMPILER_ID:MSVC>:/permissive->
  $<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>
  $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
  $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<BOOL:${CC_WERROR}>>:/WX>
  $<$<AND:$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>,$<BOOL:${CC_WERROR}>>:-Werror>
)

if(BUILD_TESTING)
  enable_testing()
  include(GoogleTest)
endif()
