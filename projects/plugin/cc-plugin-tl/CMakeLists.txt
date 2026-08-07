project(cc-plugin-tl VERSION 0.1.0 LANGUAGES CXX)

cc_add_plugin(${PROJECT_NAME}
  SOURCES src/tl.cpp
  PUBLIC_DEPS
    cc-pipeit::cc-pipeit    # cc/plugin.hpp + cc/any_ast.hpp (headers; Boost stays PRIVATE to cc-pipeit)
    cc-parseit::cc-parseit  # cc::parseit::parse
    cc-astit::cc-astit      # cc::ast::tl_program / program
)
