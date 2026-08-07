project(cc-workbench VERSION 0.1.0 LANGUAGES CXX)

find_package(imgui_bundle REQUIRED)

cc_add_executable(${PROJECT_NAME}
  SOURCES
    src/main.cpp
    ne/builders.cpp
    ne/drawing.cpp
    ne/widgets.cpp
  PRIVATE_DEPS cc-pipeit::cc-pipeit imgui_bundle
)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Ship assets (fonts + examples) next to the executable — Hello ImGui loads the
# fonts via its asset system (exe_dir/assets/...), and the ImFileDialog "Browse"
# view starts from exe_dir. A stamp file + CONFIGURE_DEPENDS glob re-copies
# whenever anything under assets/ changes (adding a .tl example, etc.), which a
# plain POST_BUILD would miss (it only fires on a real target rebuild).
file(GLOB_RECURSE _cc_wb_assets CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
set(_cc_wb_stamp "${CMAKE_CURRENT_BINARY_DIR}/assets.copy.stamp")
add_custom_command(OUTPUT ${_cc_wb_stamp}
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
    COMMAND ${CMAKE_COMMAND} -E touch ${_cc_wb_stamp}
    DEPENDS ${_cc_wb_assets}
    VERBATIM
    COMMENT "Copying cc-workbench assets -> $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets")
add_custom_target(${PROJECT_NAME}_assets DEPENDS ${_cc_wb_stamp})
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}_assets)
