# narutorise - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(narutorise LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable AMD FidelityFX present effects (CAS/FSR) for the D3D12 presenter.
set(REXGLUE_ENABLE_FIDELITYFX ON CACHE BOOL "Enable AMD FidelityFX integration" FORCE)

include(generated/rexglue.cmake)

# Sources
set(NARUTORISE_SOURCES
    src/main.cpp
    src/intro_skip.cpp
    src/ultrawide.cpp
)

if(WIN32)
    enable_language(RC)
    configure_file(src/narutorise.rc.in ${CMAKE_CURRENT_BINARY_DIR}/narutorise.rc @ONLY)
    list(APPEND NARUTORISE_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/narutorise.rc)
    add_executable(narutorise WIN32 ${NARUTORISE_SOURCES})
else()
    add_executable(narutorise ${NARUTORISE_SOURCES})
endif()

rexglue_setup_target(narutorise GPU_PLUGINS xenos)

# Runtime config: narutorise.toml ao lado do exe. Um target ALL separado
# garante a sincronização mesmo quando somente o TOML muda e o executável não
# precisa ser relinkado (POST_BUILD, sozinho, não cobre esse caso).
add_custom_target(narutorise_runtime_config ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/narutorise.toml
            $<TARGET_FILE_DIR:narutorise>/narutorise.toml)
add_dependencies(narutorise narutorise_runtime_config)

# Bundled shader cache: seeded from packaging/ into the build output on first run.
add_custom_target(narutorise_shader_cache ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/shader_cache
            $<TARGET_FILE_DIR:narutorise>/shader_cache)
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

# Cross-platform Launcher (SDL3 + Dear ImGui)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../launcher ${CMAKE_CURRENT_BINARY_DIR}/launcher)

