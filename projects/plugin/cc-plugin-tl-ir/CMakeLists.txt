project(cc-plugin-tl-ir VERSION 0.1.0 LANGUAGES CXX)

cc_add_plugin(${PROJECT_NAME}
  SOURCES
    src/tl_ir.cpp
  PUBLIC_DEPS
    cc-core::cc-core
    cc-astit::cc-astit   # type shared_ptr<tl_program>'s typeinfo lives here
    cc-astq::cc-astq     # cc::astq::lower
    cc-ir::cc-ir         # cc::ir::module (output value)
)
