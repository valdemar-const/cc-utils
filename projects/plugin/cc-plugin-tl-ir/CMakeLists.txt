project(cc-plugin-tl-ir VERSION 0.1.0 LANGUAGES CXX)

cc_add_plugin(${PROJECT_NAME}
  SOURCES src/tl_ir.cpp
  PUBLIC_DEPS
    cc-pipeit::cc-pipeit   # cc/plugin.hpp + cc/any_ast.hpp
    cc-astq::cc-astq       # cc::astq::lower
    cc-astit::cc-astit     # cc::ast::tl_program (shared typeinfo)
    cc-ir::cc-ir
)
