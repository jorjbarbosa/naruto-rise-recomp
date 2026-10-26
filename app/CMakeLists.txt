# narutorise - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(narutorise LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable AMD FidelityFX present effects (CAS/FSR) for the D3D12 presenter.
if(WIN32)
    set(REXGLUE_ENABLE_FIDELITYFX ON CACHE BOOL "Enable AMD FidelityFX integration" FORCE)
endif()

include(generated/rexglue.cmake)

# Sources
set(NARUTORISE_SOURCES
    src/main.cpp
    src/intro_skip.cpp
    src/ultrawide.cpp
    src/save_thumbnail.cpp
    src/engine_free.cpp
    src/input_replay.cpp
    src/graphics_device.cpp
    src/crash_report.cpp
    src/game_image.cpp
    src/window_title.cpp
    src/mouse_camera.cpp
    src/mods/translation_data.cpp
    src/mods/translation_mod.cpp
    # The menus: RmlUi over the SDK's immediate drawing, with their files embedded,
    # and the setup screen before the game.
    src/menu/render_interface.cpp src/menu/assets.cpp src/menu/ui.cpp
    src/menu/setup_gate.cpp src/menu/setup_screen.cpp
    src/menu/game_choice.cpp
    src/menu/dlc.cpp
)
# The files that menu/assets.cpp embeds (#embed): a change rebuilds it.
file(GLOB_RECURSE NARUTORISE_UI_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ui/*)
set_source_files_properties(src/menu/assets.cpp PROPERTIES OBJECT_DEPENDS "${NARUTORISE_UI_FILES}")

# The host system services (gpu/common/platform.h), and what exists on
# Windows only: the Xenos adapter and the discrete GPU exports, and Wine.
if(WIN32)
    list(APPEND NARUTORISE_SOURCES src/gpu/common/platform_win32.cpp src/graphics_device_win32.cpp src/wine.cpp)
else()
    list(APPEND NARUTORISE_SOURCES src/gpu/common/platform_posix.cpp)
endif()

add_subdirectory(../third_party/nlohmann_json third_party/nlohmann_json)

# The UI of the menus: RmlUi 6.3 over FreeType 2.15.0 (third_party submodules),
# both static. FreeType without its optional dependencies (the fonts are plain
# TTF) and RmlUi without samples; RmlUi takes the FreeType target, not a
# FreeType of the system.
block()
    set(BUILD_SHARED_LIBS OFF)
    foreach(_dependency IN ITEMS ZLIB BZIP2 PNG HARFBUZZ BROTLI HVF)
        set(FT_DISABLE_${_dependency} ON)
    endforeach()
    add_subdirectory(../third_party/freetype third_party/freetype EXCLUDE_FROM_ALL)
    add_library(Freetype::Freetype ALIAS freetype)
    set(CMAKE_DISABLE_FIND_PACKAGE_Freetype ON)
    set(RMLUI_SAMPLES OFF)
    set(RMLUI_FONT_ENGINE freetype)
    add_subdirectory(../third_party/rmlui third_party/rmlui EXCLUDE_FROM_ALL)
endblock()

# The native renderer (opt-in): its own D3D boundary and engine, chosen at
# launch in place of the Xenos plugin (renderer = "native" in narutorise.toml).
# The executable keeps the Xenos plugin as the other renderer.
option(NARUTORISE_NATIVE_RENDERER_BOOTSTRAP "Build the native renderer next to the Xenos plugin" OFF)
# The host graphics APIs of the native renderer, apart from the SDK's
# REXGLUE_USE_VULKAN (the Xenos plugin's). Native builds include both APIs by
# default on Windows, and the menu offers the same backends as the game.
# Elsewhere the native renderer is the only one, on Vulkan: no D3D12 and no
# Xenos plugin.
if(WIN32)
    option(NARUTORISE_NATIVE_D3D12 "Build the D3D12 backend of the native renderer" ON)
    set(NARUTORISE_GPU_PLUGINS GPU_PLUGINS xenos)
else()
    set(NARUTORISE_NATIVE_D3D12 OFF)
    set(NARUTORISE_GPU_PLUGINS)
    if(NOT NARUTORISE_NATIVE_RENDERER_BOOTSTRAP)
        message(FATAL_ERROR "Outside Windows the game draws with the native renderer: "
                            "set NARUTORISE_NATIVE_RENDERER_BOOTSTRAP=ON")
    endif()
endif()
option(NARUTORISE_NATIVE_VULKAN "Build the Vulkan backend of the native renderer" ON)
if(NARUTORISE_NATIVE_RENDERER_BOOTSTRAP AND NOT NARUTORISE_NATIVE_D3D12 AND NOT NARUTORISE_NATIVE_VULKAN)
    message(FATAL_ERROR "Enable at least one native backend: NARUTORISE_NATIVE_D3D12 or NARUTORISE_NATIVE_VULKAN")
endif()
if(NARUTORISE_NATIVE_RENDERER_BOOTSTRAP)
    add_executable(native_draw_storage_test src/gpu/common/tests/draw_storage_test.cpp)
    target_include_directories(native_draw_storage_test PRIVATE src)
    add_executable(native_descriptor_writes_test src/gpu/backend/vulkan/tests/descriptor_writes_test.cpp)
    target_include_directories(native_descriptor_writes_test PRIVATE src)
    list(APPEND NARUTORISE_SOURCES
         # The host: configuration, guest kernel exports and the frame lifecycle.
         src/gpu/host/host.cpp src/gpu/host/kernel_video.cpp src/gpu/host/kernel_threading.cpp
         src/gpu/host/kernel_modules.cpp src/gpu/host/journal.cpp src/gpu/host/stop.cpp src/gpu/host/engines.cpp
         src/gpu/host/graphics.cpp src/gpu/host/frame_completion.cpp src/gpu/host/guest_hooks.cpp src/gpu/host/exports.cpp
         src/gpu/host/title_loading.cpp
         # The backend: the engine and its D3D12 backend, the session's failure channel, the
         # pipeline identities and set, the dispatcher and the title-thread witness.
         src/gpu/backend/engine.cpp src/gpu/backend/failure_channel.cpp
         src/gpu/backend/pipeline_keys.cpp src/gpu/backend/pipeline_set.cpp src/gpu/backend/dispatcher.cpp
         # The D3D boundary of the native renderer, one unit per responsibility.
         src/gpu/d3d/hooks.cpp src/gpu/d3d/device.cpp src/gpu/d3d/resources.cpp
         src/gpu/d3d/pass.cpp src/gpu/d3d/draw.cpp src/gpu/d3d/state.cpp
         src/gpu/d3d/present.cpp src/gpu/d3d/frame.cpp src/gpu/d3d/close.cpp src/gpu/d3d/query.cpp
         src/gpu/shaders/shaders.cpp src/gpu/shaders/package.cpp src/gpu/shaders/pipelines.cpp
         src/gpu/textures/textures.cpp src/gpu/textures/write_watch.cpp src/gpu/textures/formats.cpp
         src/gpu/textures/layout.cpp
         src/gpu/buffers/buffers.cpp src/gpu/buffers/vertex_layout.cpp
         src/gpu/diagnostics/frame_state.cpp src/gpu/diagnostics/census.cpp
         src/gpu/diagnostics/frame_trace.cpp src/gpu/diagnostics/start_evidence.cpp
         src/gpu/diagnostics/frame_capture.cpp src/gpu/diagnostics/measure.cpp
         src/gpu/diagnostics/motion.cpp
         src/gpu/diagnostics/shader_keys.cpp
         # The UI layer of the Present, the SDK's immediate drawing over it and the UI
         # screen before the game.
         src/gpu/ui/layer.cpp src/gpu/ui/immediate_drawer.cpp src/gpu/ui/screen.cpp
         # The logic tick and the transforms of the state interpolation.
         src/gpu/interpolation/tick.cpp src/gpu/interpolation/transforms.cpp
         # The graphics options, under the SDK names.
         src/gpu/settings.cpp
         # The shader cache embedded with #embed, generated by the build.
         src/gpu/shaders/shader_cache_data.cpp)
    # The diagnostics that need Windows (minidumps, page protection with a
    # vectored handler, suspended thread stacks, the swapchain proof's window
    # sizes, the CDB witness of the title thread); elsewhere they do nothing.
    if(WIN32)
        list(APPEND NARUTORISE_SOURCES src/gpu/diagnostics/crash_dump.cpp src/gpu/diagnostics/sampler.cpp
             src/gpu/diagnostics/heap_watch.cpp src/gpu/diagnostics/present_proof.cpp
             src/gpu/backend/title_witness.cpp)
    else()
        list(APPEND NARUTORISE_SOURCES src/gpu/diagnostics/inert_posix.cpp)
    endif()
endif()
if(NARUTORISE_NATIVE_RENDERER_BOOTSTRAP AND NARUTORISE_NATIVE_D3D12)
    list(APPEND NARUTORISE_SOURCES src/gpu/backend/d3d12/backend.cpp)
endif()
# The Vulkan backend of the engine, its completion service and the check of the
# SPIR-V contract.
set(NARUTORISE_NATIVE_VULKAN_SOURCES src/gpu/backend/vulkan/backend.cpp src/gpu/backend/vulkan/completion.cpp
    src/gpu/backend/vulkan/shader_contract.cpp)
if(NARUTORISE_NATIVE_RENDERER_BOOTSTRAP AND NARUTORISE_NATIVE_VULKAN)
    list(APPEND NARUTORISE_SOURCES ${NARUTORISE_NATIVE_VULKAN_SOURCES})
endif()

if(WIN32)
    enable_language(RC)
    configure_file(src/narutorise.rc.in ${CMAKE_CURRENT_BINARY_DIR}/narutorise.rc @ONLY)
    list(APPEND NARUTORISE_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/narutorise.rc)
    add_executable(narutorise WIN32 ${NARUTORISE_SOURCES})
    # graphics_device.cpp: the adapters by name.
    target_link_libraries(narutorise PRIVATE d3d12 dxgi ole32 shell32 uuid)
else()
    add_executable(narutorise ${NARUTORISE_SOURCES})
endif()
target_link_libraries(narutorise PRIVATE narutorise::json RmlUi::Core)
add_library(narutorise_menu_components STATIC src/menu/text.cpp src/menu/panel.cpp src/menu/pad_input.cpp
            src/controls/config.cpp
            src/controls/service.cpp
            src/controls/runtime.cpp
            src/controls/capture.cpp
            src/menu/controls.cpp src/menu/controls_text.cpp
            src/menu/input_gate.cpp src/menu/shortcut.cpp
            src/menu/settings.cpp
            src/menu/localization.cpp src/menu/config_manager.cpp
            src/menu/key_bindings.cpp src/menu/key_names.cpp
            src/menu/game_languages.cpp src/menu/graphics_devices.cpp src/mods/translation_data.cpp)
target_sources(narutorise_menu_components PRIVATE src/menu/content_manager.cpp src/menu/folders.cpp)
target_sources(narutorise_menu_components PRIVATE src/menu/update_checker.cpp)
target_sources(narutorise_menu_components PRIVATE src/menu/game_logo.cpp)
target_include_directories(narutorise_menu_components PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/stb)
target_include_directories(narutorise_menu_components PUBLIC src)
target_link_libraries(narutorise_menu_components PUBLIC rex::runtime RmlUi::Core narutorise::json)
if(NARUTORISE_NATIVE_RENDERER_BOOTSTRAP)
    target_compile_definitions(narutorise_menu_components PUBLIC NARUTORISE_MENU_NATIVE_RENDERER=1)
endif()
if(NARUTORISE_NATIVE_VULKAN)
    target_compile_definitions(narutorise_menu_components PUBLIC NARUTORISE_NATIVE_VULKAN=1 NARUTORISE_MENU_NATIVE_VULKAN=1)
    target_include_directories(narutorise_menu_components PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/vulkan-headers/include)
endif()

add_executable(narutorise_dlc_test EXCLUDE_FROM_ALL tests/dlc_test.cpp)
add_executable(narutorise_controls_config_test EXCLUDE_FROM_ALL tests/controls_config_test.cpp)
target_link_libraries(narutorise_controls_config_test PRIVATE narutorise_menu_components)
add_executable(narutorise_controls_service_test EXCLUDE_FROM_ALL tests/controls_service_test.cpp)
target_link_libraries(narutorise_controls_service_test PRIVATE narutorise_menu_components)
if(WIN32)
    # Windows links SDL statically in the runtime DLL and the test executable.
    # Compile this driver unit beside the virtual fixture so both use one SDL
    # instance; virtual devices cannot cross two static SDL copies.
    target_sources(narutorise_controls_service_test PRIVATE ../sdk/src/input/sdl/sdl_input_driver.cpp)
endif()
add_executable(narutorise_controls_runtime_test EXCLUDE_FROM_ALL tests/controls_runtime_test.cpp)
target_link_libraries(narutorise_controls_runtime_test PRIVATE narutorise_menu_components)
add_executable(narutorise_controls_capture_test EXCLUDE_FROM_ALL tests/controls_capture_test.cpp)
target_link_libraries(narutorise_controls_capture_test PRIVATE narutorise_menu_components)
# Rebuild the existing configuration/key-name regressions against the same
# sources and SDK as the game, rather than running old launcher binaries.
add_executable(narutorise_controls_legacy_config_test EXCLUDE_FROM_ALL tests/config_manager_test.cpp)
target_link_libraries(narutorise_controls_legacy_config_test PRIVATE narutorise_menu_components)
add_executable(narutorise_controls_key_names_test EXCLUDE_FROM_ALL tests/key_bindings_test.cpp)
target_link_libraries(narutorise_controls_key_names_test PRIVATE narutorise_menu_components)
add_executable(narutorise_controls_replay_test EXCLUDE_FROM_ALL tests/controls_replay_test.cpp src/input_replay.cpp)
target_link_libraries(narutorise_controls_replay_test PRIVATE narutorise_menu_components)
target_link_libraries(narutorise_dlc_test PRIVATE narutorise_menu_components)
if(WIN32)
    target_link_libraries(narutorise_dlc_test PRIVATE shell32 ole32)
endif()
if(WIN32)
    add_executable(narutorise_menu_input_test EXCLUDE_FROM_ALL tests/menu_input_test.cpp)
    target_link_libraries(narutorise_menu_input_test PRIVATE narutorise_menu_components)
endif()
add_executable(narutorise_updates_test EXCLUDE_FROM_ALL tests/update_checker_test.cpp)
target_link_libraries(narutorise_updates_test PRIVATE narutorise_menu_components)
if(NOT NARUTORISE_NATIVE_D3D12)
    target_compile_definitions(narutorise_menu_components PUBLIC NARUTORISE_NATIVE_D3D12_DISABLED=1 NARUTORISE_MENU_NATIVE_D3D12_DISABLED=1)
endif()
if(WIN32)
    target_sources(narutorise_menu_components PRIVATE src/wine.cpp)
    target_link_libraries(narutorise_menu_components PUBLIC d3d12 dxgi ole32 shell32 uuid winhttp)
endif()
target_link_libraries(narutorise PRIVATE narutorise_menu_components)

add_executable(narutorise_translation_test EXCLUDE_FROM_ALL
    tests/translation_test.cpp src/mods/translation_data.cpp)
target_include_directories(narutorise_translation_test PRIVATE src)
target_link_libraries(narutorise_translation_test PRIVATE rex::runtime narutorise::json)

add_executable(narutorise_game_image_test EXCLUDE_FROM_ALL
    tests/game_image_test.cpp src/game_image.cpp)
target_include_directories(narutorise_game_image_test PRIVATE src)
target_link_libraries(narutorise_game_image_test PRIVATE rex::runtime)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(narutorise_posix_wait_test EXCLUDE_FROM_ALL tests/posix_wait_test.cpp)
    target_link_libraries(narutorise_posix_wait_test PRIVATE rex::runtime)
endif()

add_executable(narutorise_game_logo_test EXCLUDE_FROM_ALL tests/game_logo_test.cpp)
target_link_libraries(narutorise_game_logo_test PRIVATE narutorise_menu_components)

add_executable(narutorise_setup_gate_test EXCLUDE_FROM_ALL
    tests/setup_gate_test.cpp src/menu/setup_gate.cpp)
target_include_directories(narutorise_setup_gate_test PRIVATE src)
if(WIN32)
    target_link_libraries(narutorise_setup_gate_test PRIVATE user32)
endif()

add_executable(narutorise_game_choice_test EXCLUDE_FROM_ALL
    tests/game_choice_test.cpp src/menu/game_choice.cpp src/game_image.cpp)
target_include_directories(narutorise_game_choice_test PRIVATE src)
target_link_libraries(narutorise_game_choice_test PRIVATE rex::runtime narutorise_menu_components)
if(WIN32)
    target_link_libraries(narutorise_game_choice_test PRIVATE ole32 shell32 uuid)
endif()

add_executable(narutorise_validate_translation_package EXCLUDE_FROM_ALL
    ../tools/localization/validate-package.cpp src/mods/translation_data.cpp)
target_include_directories(narutorise_validate_translation_package PRIVATE src)
target_link_libraries(narutorise_validate_translation_package PRIVATE rex::runtime narutorise::json)

if(NARUTORISE_NATIVE_RENDERER_BOOTSTRAP)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../tools/native-renderer/plume-engine.cmake)
    rexglue_setup_target(narutorise ${NARUTORISE_GPU_PLUGINS})
    if(NARUTORISE_NATIVE_D3D12)
        target_link_libraries(narutorise PRIVATE native_renderer_plume)
    else()
        target_compile_definitions(narutorise PRIVATE NARUTORISE_NATIVE_D3D12_DISABLED=1)
    endif()
    target_compile_definitions(narutorise PRIVATE NARUTORISE_NATIVE_RENDERER_BOOTSTRAP=1)
    # The shader cache generated from the local game files (game/xeshader.sdb),
    # with the SPIR-V of the shaders and of the backend's passes when the Vulkan
    # backend is built.
    include(${CMAKE_CURRENT_SOURCE_DIR}/../tools/native-renderer/shader-cache.cmake)
    set(_narutorise_shader_spirv)
    if(NARUTORISE_NATIVE_VULKAN)
        set(_narutorise_shader_spirv SPIRV PASSES src/gpu/backend)
    endif()
    narutorise_shader_cache(narutorise src/gpu/shaders/shader_cache_data.cpp ${_narutorise_shader_spirv})
    if(NARUTORISE_NATIVE_VULKAN)
        target_link_libraries(narutorise PRIVATE native_renderer_plume_vulkan)
        target_compile_definitions(narutorise PRIVATE NARUTORISE_NATIVE_VULKAN=1)
        # The engine with both backends for the host tests (no guest code), with
        # the test hooks and the embedded shader packages (the Vulkan backend
        # checks the SPIR-V contract): the common tests run on the API of
        # REX_NATIVE_TEST_API (backend/tests/test_backend.h).
        add_library(native_engine_test_units STATIC src/gpu/backend/engine.cpp
                    ${NARUTORISE_NATIVE_VULKAN_SOURCES} src/gpu/backend/failure_channel.cpp
                    src/gpu/backend/pipeline_keys.cpp src/gpu/backend/pipeline_set.cpp src/gpu/backend/dispatcher.cpp
                    src/gpu/shaders/package.cpp src/gpu/shaders/shader_cache_data.cpp)
        target_include_directories(native_engine_test_units PUBLIC src)
        # The frame events and the host services of the engine (gpu/common).
        if(WIN32)
            target_sources(native_engine_test_units PRIVATE src/gpu/common/platform_win32.cpp)
        else()
            target_sources(native_engine_test_units PRIVATE src/gpu/common/platform_posix.cpp)
        endif()
        target_link_libraries(native_engine_test_units PUBLIC native_renderer_plume_vulkan
                              narutorise_smolv libzstd_static fmt::fmt rex::runtime)
        if(NARUTORISE_NATIVE_D3D12)
            target_sources(native_engine_test_units PRIVATE src/gpu/backend/d3d12/backend.cpp)
            target_link_libraries(native_engine_test_units PUBLIC native_renderer_plume)
        else()
            target_compile_definitions(native_engine_test_units PUBLIC NARUTORISE_NATIVE_D3D12_DISABLED=1)
        endif()
        target_compile_definitions(native_engine_test_units PUBLIC NARUTORISE_NATIVE_VULKAN=1
                                   NATIVE_RENDERER_ENGINE_FAULT_TEST=1 NARUTORISE_SHADER_CACHE_SPIRV=1
                                   _CRT_SECURE_NO_WARNINGS)
        add_dependencies(native_engine_test_units narutorise_native_shader_cache)
        # The presentation tests open a Win32 window of their own.
        set(_narutorise_engine_tests latch lifetime failure translation resource depth_resolve depth_only)
        set(_narutorise_vulkan_tests completion shader_contract draw_cache)
        if(WIN32)
            list(APPEND _narutorise_engine_tests present_edge reconfigure ui)
            list(APPEND _narutorise_vulkan_tests present)
        endif()
        foreach(_narutorise_engine_test IN LISTS _narutorise_engine_tests)
            add_executable(native_engine_${_narutorise_engine_test}_test
                           src/gpu/backend/tests/${_narutorise_engine_test}_test.cpp)
            target_link_libraries(native_engine_${_narutorise_engine_test}_test PRIVATE native_engine_test_units)
        endforeach()
        if(WIN32)
            # The UI layer and the SDK's immediate drawing over it, through a dispatcher.
            add_executable(native_engine_ui_layer_test src/gpu/ui/tests/layer_test.cpp src/gpu/ui/layer.cpp
                           src/gpu/ui/immediate_drawer.cpp src/menu/setup_gate.cpp)
            target_link_libraries(native_engine_ui_layer_test PRIVATE native_engine_test_units rex::runtime)
            # The menus' UI over it: RmlUi, the embedded fonts and documents and the render interface.
            add_executable(native_engine_menu_test src/menu/tests/ui_test.cpp src/menu/ui.cpp src/menu/assets.cpp
                           src/menu/render_interface.cpp src/menu/setup_screen.cpp src/gpu/ui/layer.cpp
                           src/gpu/ui/immediate_drawer.cpp src/gpu/ui/screen.cpp src/menu/dlc.cpp
                           src/menu/game_choice.cpp src/game_image.cpp)
            target_link_libraries(native_engine_menu_test PRIVATE native_engine_test_units rex::runtime RmlUi::Core narutorise_menu_components)
        endif()
        # The Vulkan tests: the completion service (held submissions, injected
        # failures and the race) and the check of the SPIR-V contract.
        foreach(_narutorise_engine_test IN LISTS _narutorise_vulkan_tests)
            add_executable(native_engine_${_narutorise_engine_test}_test
                           src/gpu/backend/vulkan/tests/${_narutorise_engine_test}_test.cpp)
            target_link_libraries(native_engine_${_narutorise_engine_test}_test PRIVATE native_engine_test_units)
        endforeach()
        set(_draw_cache_shaders "${CMAKE_CURRENT_BINARY_DIR}/draw-cache-test")
        file(MAKE_DIRECTORY "${_draw_cache_shaders}")
        foreach(_stage IN ITEMS vs ps)
            if(_stage STREQUAL "vs")
                set(_invert -fvk-invert-y)
                set(_symbol kCacheVs)
            else()
                set(_invert)
                set(_symbol kCachePs)
            endif()
            add_custom_command(OUTPUT "${_draw_cache_shaders}/draw_cache_${_stage}.h"
                COMMAND ${NARUTORISE_DXC_ENV} "${NARUTORISE_DXC}"
                    -T ${_stage}_6_0 -E ${_stage}_main -HV 2021 -spirv -fspv-target-env=vulkan1.2
                    -fvk-use-dx-layout -fvk-stage-io-order=decl -fspv-entrypoint-name=main ${_invert}
                    -Vn ${_symbol} -Fh "${_draw_cache_shaders}/draw_cache_${_stage}.h"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/backend/vulkan/tests/draw_cache.hlsl"
                DEPENDS src/gpu/backend/vulkan/tests/draw_cache.hlsl "${NARUTORISE_DXC}" VERBATIM)
            target_sources(native_engine_draw_cache_test PRIVATE "${_draw_cache_shaders}/draw_cache_${_stage}.h")
        endforeach()
        target_include_directories(native_engine_draw_cache_test PRIVATE "${_draw_cache_shaders}")
        if(NARUTORISE_NATIVE_D3D12)
            add_executable(native_engine_bindings_test src/gpu/backend/d3d12/tests/bindings_test.cpp)
            target_link_libraries(native_engine_bindings_test PRIVATE native_engine_test_units)
            foreach(_stage IN ITEMS vs ps)
                add_custom_command(OUTPUT "${_draw_cache_shaders}/bindings_${_stage}.h"
                    COMMAND "${_draw_cache_thirdparty}/dxc-bin/bin/x64/dxc.exe"
                        -T ${_stage}_6_0 -E ${_stage}_main -HV 2021
                        -Vn kBindings_${_stage} -Fh "${_draw_cache_shaders}/bindings_${_stage}.h"
                        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/backend/d3d12/tests/bindings.hlsl"
                    DEPENDS src/gpu/backend/d3d12/tests/bindings.hlsl
                            "${_draw_cache_thirdparty}/dxc-bin/bin/x64/dxc.exe" VERBATIM)
                target_sources(native_engine_bindings_test PRIVATE "${_draw_cache_shaders}/bindings_${_stage}.h")
            endforeach()
            target_include_directories(native_engine_bindings_test PRIVATE "${_draw_cache_shaders}")
        endif()
        # Host test of the Vulkan device (no guest code): clear, upload, copy and
        # readback, with a bounded wait on the submission.
        add_executable(native_vulkan_transfer_test src/gpu/backend/vulkan/tests/transfer_test.cpp)
        target_include_directories(native_vulkan_transfer_test PRIVATE src)
        target_link_libraries(native_vulkan_transfer_test PRIVATE native_renderer_plume_vulkan)
        target_compile_definitions(native_vulkan_transfer_test PRIVATE _CRT_SECURE_NO_WARNINGS)
        # Host test of the embedded shader packages (no guest code, no GPU): every
        # SPIR-V module decodes to the SHA-256 of its index line.
        add_executable(native_shader_package_test src/gpu/shaders/tests/package_test.cpp
                       src/gpu/shaders/package.cpp src/gpu/shaders/shader_cache_data.cpp)
        target_include_directories(native_shader_package_test PRIVATE src)
        target_link_libraries(native_shader_package_test PRIVATE narutorise_smolv libzstd_static fmt::fmt)
        target_compile_definitions(native_shader_package_test PRIVATE NARUTORISE_SHADER_CACHE_SPIRV=1 NOMINMAX
                                   WIN32_LEAN_AND_MEAN)
        add_dependencies(native_shader_package_test narutorise_native_shader_cache)
    endif()
else()
    rexglue_setup_target(narutorise ${NARUTORISE_GPU_PLUGINS})
endif()

# The mouse camera inside each script module, the base game's and the DLCs':
# the orbit routine of the exploration camera in each (mouse_camera_script.cpp).
foreach(_narutorise_module IN ITEMS ai2c:sub_882227F8 ai2c1:sub_88228C18 ai2c2:sub_88234D28 ai2c3:sub_88235D10)
    string(REPLACE ":" ";" _narutorise_module "${_narutorise_module}")
    list(GET _narutorise_module 0 _narutorise_module_name)
    list(GET _narutorise_module 1 _narutorise_orbit)
    if(TARGET narutorise_${_narutorise_module_name})
        target_sources(narutorise_${_narutorise_module_name} PRIVATE src/mouse_camera_script.cpp)
        target_compile_definitions(narutorise_${_narutorise_module_name} PRIVATE
            NARUTORISE_CAMERA_ORBIT=${_narutorise_orbit})
    endif()
endforeach()

# The release version, in the window title when the copy has no
# narutorise.version file (development builds).
set(NARUTORISE_GAME_VERSION_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/version.iss)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${NARUTORISE_GAME_VERSION_FILE})
file(STRINGS ${NARUTORISE_GAME_VERSION_FILE} NARUTORISE_GAME_VERSION_LINE REGEX "#define MyAppVersion")
if(NARUTORISE_GAME_VERSION_LINE MATCHES "\"([^\"]+)\"")
    target_compile_definitions(narutorise PRIVATE NARUTORISE_VERSION="${CMAKE_MATCH_1}")
endif()

# Runtime config: narutorise.toml ao lado do exe. Um target ALL separado
# garante a sincronização mesmo quando somente o TOML muda e o executável não
# precisa ser relinkado (POST_BUILD, sozinho, não cobre esse caso).
add_custom_target(narutorise_runtime_config ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/narutorise.toml
            $<TARGET_FILE_DIR:narutorise>/narutorise.toml)
add_dependencies(narutorise narutorise_runtime_config)

# Bundled Xenos shader storage (555307E5.xsh/.xpso), seeded into the user cache
# on first run. It holds the game's shader microcode, so it stays out of the
# repository: a local folder provides it (packaging/private/shader_cache by
# default). Without it the game still runs; Xenos translates shaders on demand.
# The folder is checked at build time, so files added after configuring count.
set(NARUTORISE_SHADER_STORAGE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/private/shader_cache"
    CACHE PATH "Folder with the Xenos shader storage files (555307E5.*) to bundle")
add_custom_target(narutorise_shader_cache ALL
    COMMAND ${CMAKE_COMMAND}
            "-DSOURCE=${NARUTORISE_SHADER_STORAGE_DIR}"
            "-DDEST=$<TARGET_FILE_DIR:narutorise>/shader_cache"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_shader_storage.cmake
    VERBATIM)
add_dependencies(narutorise narutorise_shader_cache)

# AMD FidelityFX runtime DLL (when the SDK was built with FidelityFX enabled).
if(WIN32)
    set(_narutorise_fidelityfx_dll "${rexglue_DIR}/../../../bin/amd_fidelityfx_dx12.dll")
    if(EXISTS "${_narutorise_fidelityfx_dll}")
        add_custom_target(narutorise_fidelityfx_dll ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_narutorise_fidelityfx_dll}"
                    $<TARGET_FILE_DIR:narutorise>/amd_fidelityfx_dx12.dll)
        add_dependencies(narutorise narutorise_fidelityfx_dll)
    endif()
endif()

# Host regressions migrated from the retired launcher.
foreach(_case IN ITEMS config_manager key_bindings localization game_languages update_checker)
    add_executable(narutorise_${_case}_test EXCLUDE_FROM_ALL tests/${_case}_test.cpp)
    target_link_libraries(narutorise_${_case}_test PRIVATE narutorise_menu_components)
endforeach()

# The SDK runtime next to the executable on Linux, where the executable finds it
# through its $ORIGIN RUNPATH; the SDK stages it itself only on Windows and macOS.
if(UNIX AND NOT APPLE AND TARGET rex::runtime)
    add_custom_command(TARGET narutorise POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:rex::runtime> $<TARGET_FILE_DIR:narutorise>
        VERBATIM)
endif()
