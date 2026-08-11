project(cc-plugin-basic VERSION 0.1.0 LANGUAGES CXX)

cc_add_plugin(${PROJECT_NAME}
  SOURCES
    src/basic.cpp
    src/executor.cpp
  PUBLIC_DEPS
    cc-core::cc-core
  PRIVATE_DEPS
    Boost::process
    Boost::system
)
# boost::process v2 defaults to boost::filesystem; we use std::filesystem
# everywhere else, so make v2 follow suit to avoid path-type mismatches at
# the spawn call site.
target_compile_definitions(${PROJECT_NAME} PRIVATE BOOST_PROCESS_USE_STD_FS)
