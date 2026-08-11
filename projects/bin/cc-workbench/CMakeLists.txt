project(cc-workbench VERSION 0.1.0.0 LANGUAGES CXX)

find_package(imgui_bundle REQUIRED)

cc_add_executable(${PROJECT_NAME}
  SOURCES
    src/main.cpp
    ne/builders.cpp
    ne/drawing.cpp
    ne/widgets.cpp
  PRIVATE_DEPS cc-runtime::cc-runtime cc-astit::cc-astit cc-ir::cc-ir imgui_bundle
)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Semver string (Blender-style vMaj.Min.Rev.Patch) — used by status bar.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    CC_VERSION_STRING="${PROJECT_VERSION}"
)

# Ship assets (fonts + examples) next to the executable. An always-run custom
# target (not a stamp) is used on purpose: extracted/added files often keep an
# older mtime than a stamp, which would skip the copy. copy_directory every build
# is cheap (~11 MB, local disk) and guarantees the runtime dir stays in sync.
add_custom_target(${PROJECT_NAME}_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
    VERBATIM
    COMMENT "Syncing cc-workbench assets -> $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets")
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}_assets)
