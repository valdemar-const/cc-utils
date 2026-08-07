project(ccp VERSION 0.1.0 LANGUAGES CXX)

cc_add_executable(${PROJECT_NAME}
  SOURCES src/main.cpp
  PRIVATE_DEPS cc-pipeit::cc-pipeit
)
