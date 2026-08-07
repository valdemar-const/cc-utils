project(cc-plugin-x86_64 VERSION 0.1.0 LANGUAGES CXX)

cc_add_plugin(${PROJECT_NAME}
  SOURCES src/x86_64.cpp
  PUBLIC_DEPS cc-pipeit::cc-pipeit   # headers only
)
