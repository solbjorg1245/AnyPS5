# Settings launcher (s61): the player's entry point (the release's root folder; libs/ in the prx archive),
# which libc also runs before the settings load when the game is started on its own. Built next to the
# patched .prx (build/core/libs/libs). Dear ImGui (3rdparty/imgui, MIT) on SDL2's renderer.
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/3rdparty/imgui)
add_executable(anyps5_launcher WIN32
        ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/FolderBrowser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Host.cpp
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)
target_include_directories(anyps5_launcher PRIVATE
        ${IMGUI_DIR}
        ${CMAKE_SOURCE_DIR}/core/libs
)
target_compile_definitions(anyps5_launcher PRIVATE IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
find_package(Threads REQUIRED)
target_link_libraries(anyps5_launcher PRIVATE SDL2-static Threads::Threads)
if(WIN32)
    # IFileOpenDialog (the folder picker).
    target_link_libraries(anyps5_launcher PRIVATE ole32 uuid shell32)
endif()
set_target_properties(anyps5_launcher PROPERTIES
        OUTPUT_NAME anyps5-launcher
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/core/libs/libs
)
if(MINGW)
    # It sits in libs/, away from the bundled MinGW runtime DLLs: fully static.
    target_link_options(anyps5_launcher PRIVATE -static -static-libgcc -static-libstdc++)
else()
    # SDL dlopen()s X11; a fully static executable cannot.
    string(REPLACE "-static " "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ")
    string(STRIP "${CMAKE_EXE_LINKER_FLAGS}" CMAKE_EXE_LINKER_FLAGS)
endif()
add_dependencies(libs anyps5_launcher)
