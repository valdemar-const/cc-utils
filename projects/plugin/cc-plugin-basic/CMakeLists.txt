project(cc-plugin-basic VERSION 0.1.0 LANGUAGES CXX)

cc_add_plugin(${PROJECT_NAME}
  SOURCES
    src/basic.cpp
  PUBLIC_DEPS
    cc-core::cc-core
)
