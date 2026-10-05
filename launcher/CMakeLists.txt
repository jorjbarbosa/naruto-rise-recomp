cmake_minimum_required(VERSION 3.25)
project(narutorise_launcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LAUNCHER_SOURCES
    src/main.cpp
    src/config_manager.cpp
    src/content_manager.cpp
    src/localization.cpp
    src/texture_loader.cpp
    src/theme.cpp
    src/ui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/imgui/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/imgui/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/imgui/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/imgui/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/imgui/backends/imgui_impl_sdl3.cpp
)

if(WIN32)
    enable_language(RC)
    configure_file(src/launcher.rc.in ${CMAKE_CURRENT_BINARY_DIR}/launcher.rc @ONLY)
    list(APPEND LAUNCHER_SOURCES
        ${CMAKE_CURRENT_BINARY_DIR}/launcher.rc
        ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/imgui/backends/imgui_impl_dx11.cpp
    )
    add_executable(narutorise_launcher WIN32 ${LAUNCHER_SOURCES})
else()
    list(APPEND LAUNCHER_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/imgui/backends/imgui_impl_vulkan.cpp
    )
    add_executable(narutorise_launcher ${LAUNCHER_SOURCES})
endif()

get_filename_component(REXSDK_INSTALL_DIR "${rexglue_DIR}/../../.." ABSOLUTE)

target_include_directories(narutorise_launcher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${REXSDK_INSTALL_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/stb
)

target_link_libraries(narutorise_launcher PRIVATE
    SDL3::SDL3-static
)

if(WIN32)
    target_link_libraries(narutorise_launcher PRIVATE
        d3d11
        dxgi
        shell32
    )
endif()

if(COMMAND rexglue_apply_target_settings)
    rexglue_apply_target_settings(narutorise_launcher)
endif()

# Saída no diretório raiz do build, ao lado de narutorise.exe
set_target_properties(narutorise_launcher PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# Copiar assets para a pasta do executável
add_custom_target(narutorise_launcher_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/assets
            ${CMAKE_BINARY_DIR}/assets)
add_dependencies(narutorise_launcher narutorise_launcher_assets)

# ---------------------------------------------------------------------------
# Legacy wizard is opt-in; releases use packaging/installer.iss.
# ---------------------------------------------------------------------------
option(NARUTORISE_BUILD_LEGACY_INSTALLER "Build the legacy ImGui installer" OFF)
if(WIN32 AND NARUTORISE_BUILD_LEGACY_INSTALLER)
    set(INSTALLER_SOURCES
        src/installer_main.cpp
        src/installer_ui.cpp
        src/installer_payload.cpp
        src/installer_windows.cpp
        src/xiso_process.cpp
        src/localization.cpp
        src/config_manager.cpp
        src/theme.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/imgui/imgui.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/imgui/imgui_draw.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/imgui/imgui_tables.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/imgui/imgui_widgets.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/imgui/backends/imgui_impl_sdl3.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/imgui/backends/imgui_impl_dx11.cpp
    )

    add_executable(narutorise_installer WIN32 ${INSTALLER_SOURCES})

    target_include_directories(narutorise_installer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${REXSDK_INSTALL_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/stb
    )

    target_link_libraries(narutorise_installer PRIVATE
        SDL3::SDL3-static
        d3d11
        dxgi
    )

    if(COMMAND rexglue_apply_target_settings)
        rexglue_apply_target_settings(narutorise_installer)
    endif()

    set_target_properties(narutorise_installer PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

endif()

if(WIN32)
    # ISO tools belong to setup only; never ship them next to the launcher.
    add_executable(narutorise_game_paths_test EXCLUDE_FROM_ALL
        ../packaging/tests/test_game_paths.cpp)
    target_include_directories(narutorise_game_paths_test PRIVATE src)
    set_target_properties(narutorise_game_paths_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        MSVC_RUNTIME_LIBRARY MultiThreaded)
    add_executable(narutorise_setup_helper src/setup_helper.cpp src/xiso_process.cpp)
    target_link_libraries(narutorise_setup_helper PRIVATE shell32)
    set_target_properties(narutorise_setup_helper PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(COMMAND rexglue_apply_target_settings)
        rexglue_apply_target_settings(narutorise_setup_helper)
    endif()
    # Runs out of Inno's private temp directory, without an app-local VC runtime.
    set_property(TARGET narutorise_setup_helper PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
    add_custom_target(narutorise_extract_xiso ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract-xiso/extract-xiso.exe
                ${CMAKE_BINARY_DIR}/extract-xiso.exe)
    if(TARGET narutorise_installer)
        add_dependencies(narutorise_installer narutorise_extract_xiso)
    endif()
    add_dependencies(narutorise_setup_helper narutorise_extract_xiso)
endif()
