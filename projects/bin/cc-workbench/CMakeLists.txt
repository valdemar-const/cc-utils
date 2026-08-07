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

# Ship assets (fonts) next to the executable — Hello ImGui loads them via its
# asset system (exe_dir/assets/...). Uses a generator expression so the path
# always resolves to wherever this target's binary lands.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
    COMMENT "Copying cc-workbench assets -> $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets")
