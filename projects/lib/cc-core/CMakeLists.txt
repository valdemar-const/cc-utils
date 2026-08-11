project(cc-core VERSION 0.1.0 LANGUAGES CXX)

find_package(AnyAny REQUIRED)

cc_add_library(${PROJECT_NAME}
  SOURCES
    src/${PROJECT_NAME}.cpp
  PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PUBLIC_HEADERS
    include/cc/any_value.hpp
    include/cc/node.hpp
    include/cc/registry.hpp
    include/cc/view.hpp
    include/cc/host.hpp
    include/cc/node_factory.hpp
    include/cc/plugin_entry.hpp
  PUBLIC_DEPS
    anyanylib
)
