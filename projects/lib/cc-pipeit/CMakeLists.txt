project(cc-pipeit VERSION 0.1.0 LANGUAGES CXX)

find_package(Boost REQUIRED)

cc_add_library(${PROJECT_NAME}
  SOURCES
    src/${PROJECT_NAME}.cpp
    src/pipeline.cpp
  PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PUBLIC_HEADERS
    include/cc/pipeit.hpp
    include/cc/plugin.hpp
    include/cc/pipeline.hpp
  PUBLIC_DEPS
    cc-ir::cc-ir
    cc-astit::cc-astit
  PRIVATE_DEPS
    Boost::dll
)
