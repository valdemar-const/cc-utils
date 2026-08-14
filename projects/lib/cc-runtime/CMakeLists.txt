project(cc-runtime VERSION 0.1.0 LANGUAGES CXX)

find_package(Boost REQUIRED)

cc_add_library(${PROJECT_NAME}
  SOURCES
    src/${PROJECT_NAME}.cpp
    src/host_registry.cpp
    src/plugin_loader.cpp
    src/graph.cpp
    src/runner.cpp
    src/map_properties.cpp
    src/inline_editors.cpp
  PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PUBLIC_HEADERS
    include/cc/host_registry.hpp
    include/cc/plugin_loader.hpp
    include/cc/graph.hpp
    include/cc/runner.hpp
    include/cc/map_properties.hpp
    include/cc/inline_editors.hpp
  PUBLIC_DEPS
    cc-core::cc-core
    cc-types-filesystem::cc-types-filesystem
  PRIVATE_DEPS
    Boost::dll
)
