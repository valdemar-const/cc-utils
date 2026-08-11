project(cc-plugin-tl VERSION 0.1.0 LANGUAGES CXX)

cc_add_plugin(${PROJECT_NAME}
  SOURCES
    src/tl.cpp
  PUBLIC_DEPS
    cc-core::cc-core
    cc-astit::cc-astit
    cc-parseit::cc-parseit
)
