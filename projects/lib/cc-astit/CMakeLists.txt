project(cc-astit VERSION 0.1.0 LANGUAGES CXX)

cc_add_library(${PROJECT_NAME}
  SOURCES
    src/${PROJECT_NAME}.cpp
  PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PUBLIC_HEADERS
    include/cc/any_ast.hpp
    include/cc/astit.hpp
)
