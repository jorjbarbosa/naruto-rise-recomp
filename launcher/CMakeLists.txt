cmake_minimum_required(VERSION 3.25)
project(narutorise_launcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT TARGET narutorise::json)
    add_subdirectory(../third_party/nlohmann_json third_party/nlohmann_json)
endif()

set(LAUNCHER_SOURCES
    src/main.cpp
    src/app_version.cpp
    src/config_manager.cpp
    src/content_manager.cpp
    src/folders.cpp
    src/graphics_devices.cpp
    src/game_languages.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/mods/translation_data.cpp
    src/key_bindings.cpp
    src/key_names.cpp
    src/localization.cpp
    src/texture_loader.cpp
    src/theme.cpp
    src/ui.cpp
    src/widgets.cpp
    src/pages/dlc.cpp
    src/pages/game_files.cpp
    src/pages/key_mapping.cpp
    src/pages/play.cpp
    src/pages/settings.cpp
    src/pages/settings_form.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../app/src
    ${REXSDK_INSTALL_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/stb
)

target_link_libraries(narutorise_launcher PRIVATE
    SDL3::SDL3-static
    narutorise::json
)

# The release version, shown under the cover when the copy has no
# narutorise.version file (development builds).
set(NARUTORISE_VERSION_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/version.iss)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${NARUTORISE_VERSION_FILE})
file(STRINGS ${NARUTORISE_VERSION_FILE} NARUTORISE_VERSION_LINE REGEX "#define MyAppVersion")
if(NARUTORISE_VERSION_LINE MATCHES "\"([^\"]+)\"")
    target_compile_definitions(narutorise_launcher PRIVATE NARUTORISE_VERSION="${CMAKE_MATCH_1}")
endif()
if(MSVC)
    target_compile_options(narutorise_launcher PRIVATE /utf-8)
endif()

if(WIN32)
    target_link_libraries(narutorise_launcher PRIVATE
        d3d11
        d3d12
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

# Configuration regression tests can run without game files or the SDL UI.
add_executable(narutorise_config_manager_test EXCLUDE_FROM_ALL
    tests/test_config_manager.cpp
    src/config_manager.cpp
    src/key_bindings.cpp)
target_include_directories(narutorise_config_manager_test PRIVATE
    src
    ${REXSDK_INSTALL_DIR}/include)
set_target_properties(narutorise_config_manager_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# The key names the launcher writes against the SDK's, and the value format.
add_executable(narutorise_key_bindings_test EXCLUDE_FROM_ALL
    tests/test_key_bindings.cpp
    src/key_bindings.cpp
    src/key_names.cpp)
target_include_directories(narutorise_key_bindings_test PRIVATE src)
target_link_libraries(narutorise_key_bindings_test PRIVATE SDL3::SDL3-static)
set_target_properties(narutorise_key_bindings_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_executable(narutorise_localization_test EXCLUDE_FROM_ALL
    tests/test_localization.cpp
    src/localization.cpp)
target_include_directories(narutorise_localization_test PRIVATE src)
set_target_properties(narutorise_localization_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(MSVC)
    target_compile_options(narutorise_localization_test PRIVATE /utf-8)
endif()

# Exercise discovery and language selection with the same package reader as the game.
add_executable(narutorise_game_languages_test EXCLUDE_FROM_ALL
    tests/test_game_languages.cpp
    src/game_languages.cpp src/config_manager.cpp src/key_bindings.cpp src/localization.cpp
    ../app/src/mods/translation_data.cpp)
target_include_directories(narutorise_game_languages_test PRIVATE
    src ../app/src ${REXSDK_INSTALL_DIR}/include)
target_link_libraries(narutorise_game_languages_test PRIVATE narutorise::json)
set_target_properties(narutorise_game_languages_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(MSVC)
    target_compile_options(narutorise_game_languages_test PRIVATE /utf-8)
endif()

# The game executable carries the native renderer next to the Xenos plugin:
# the launcher offers the choice and the native renderer's options.
if(NARUTORISE_NATIVE_RENDERER_BOOTSTRAP)
    target_compile_definitions(narutorise_launcher PRIVATE NARUTORISE_LAUNCHER_NATIVE_RENDERER=1)
    target_compile_definitions(narutorise_config_manager_test PRIVATE NARUTORISE_LAUNCHER_NATIVE_RENDERER=1)
    if(NARUTORISE_NATIVE_VULKAN)
        target_compile_definitions(narutorise_launcher PRIVATE NARUTORISE_LAUNCHER_NATIVE_VULKAN=1)
        target_compile_definitions(narutorise_config_manager_test PRIVATE NARUTORISE_LAUNCHER_NATIVE_VULKAN=1)
        target_include_directories(narutorise_launcher PRIVATE "${NATIVE_RENDERER_PLUME_SOURCE}/contrib/Vulkan-Headers/include")
    endif()
    if(NOT NARUTORISE_NATIVE_D3D12)
        target_compile_definitions(narutorise_launcher PRIVATE NARUTORISE_LAUNCHER_NATIVE_D3D12_DISABLED=1)
        target_compile_definitions(narutorise_config_manager_test PRIVATE NARUTORISE_LAUNCHER_NATIVE_D3D12_DISABLED=1)
    endif()
endif()

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
    if(MSVC)
        target_compile_options(narutorise_installer PRIVATE /utf-8)
    endif()

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
