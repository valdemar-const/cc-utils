project(cc-plugin-x86_64 VERSION 0.1.0 LANGUAGES CXX)

cc_add_plugin(${PROJECT_NAME}
  SOURCES
    src/x86_64.cpp
  PUBLIC_DEPS
    cc-core::cc-core
    cc-gen::cc-gen    # cc::gen::lower / format
    cc-ir::cc-ir      # cc::ir::module (input value)
)
