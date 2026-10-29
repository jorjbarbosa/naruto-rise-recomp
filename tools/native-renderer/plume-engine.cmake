# The backends of the native renderer: Plume at a pinned revision. A supplied
# checkout is mandatory (NATIVE_RENDERER_PLUME_SOURCE), clean at the pinned commits.
# native_renderer_plume is the D3D12 backend (NARUTORISE_NATIVE_D3D12) and
# native_renderer_plume_vulkan the Vulkan one (NARUTORISE_NATIVE_VULKAN). Only the
# submodules of the enabled backends are checked, so a D3D12 build needs no
# Vulkan sources.
set(NATIVE_RENDERER_PLUME_SOURCE "" CACHE PATH "Clean Plume checkout at the accepted revision")
if(NOT NATIVE_RENDERER_PLUME_SOURCE)
    message(FATAL_ERROR "Native bootstrap requires NATIVE_RENDERER_PLUME_SOURCE")
endif()
find_package(Git REQUIRED)
set(dep_names plume)
set(dep_dirs .)
set(dep_commits d72379344dacd3dbf9f810f92ddc87e6de1845b1)
if(NARUTORISE_NATIVE_D3D12)
    list(APPEND dep_names allocator)
    list(APPEND dep_dirs contrib/D3D12MemoryAllocator)
    list(APPEND dep_commits 9ef66bc14edd10dee0de3a545b98578363552f66)
endif()
if(NARUTORISE_NATIVE_VULKAN)
    list(APPEND dep_names volk Vulkan-Headers VulkanMemoryAllocator)
    list(APPEND dep_dirs contrib/volk contrib/Vulkan-Headers contrib/VulkanMemoryAllocator)
    list(APPEND dep_commits be3dbd49bf77052665e96b6c7484af855e7e5f67 2fa203425eb4af9dfc6b03f97ef72b0b5bcb8350
         29b35ea4232688c0f42cdff0c10848290760a417)
endif()
foreach(dep IN ZIP_LISTS dep_names dep_dirs dep_commits)
    set(dep_dir "${NATIVE_RENDERER_PLUME_SOURCE}/${dep_1}")
    # An uninitialized submodule is an empty folder, where rev-parse answers for Plume.
    file(GLOB dep_files "${dep_dir}/*")
    if(NOT dep_files)
        message(FATAL_ERROR "${dep_0} source is missing: git -C \"${NATIVE_RENDERER_PLUME_SOURCE}\" "
                            "submodule update --init ${dep_1}")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dep_dir}" rev-parse HEAD
        OUTPUT_VARIABLE dep_head OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dep_dir}" status --porcelain --untracked-files=no
        OUTPUT_VARIABLE dep_changes OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT dep_head STREQUAL dep_2 OR NOT dep_changes STREQUAL "")
        message(FATAL_ERROR "${dep_0} source must be clean at ${dep_2}")
    endif()
endforeach()
# Each backend source takes a local patch of plume-patches/, applied to a copy
# in the build folder. Source, patch and result are checked by the SHA-256 of
# their text with LF line endings, so the compiled source has one identity
# whatever the line endings of the checkout and of the copy.
function(native_renderer_text_sha256 path out)
    file(READ "${path}" text)
    string(REPLACE "\r\n" "\n" text "${text}")
    string(SHA256 sha "${text}")
    set(${out} "${sha}" PARENT_SCOPE)
endfunction()
# Copies `file` of the checkout to `dir` with `patch` applied, unless the copy
# already has the accepted result.
function(native_renderer_patched_copy file patch source_sha256 patch_sha256 patched_sha256 dir)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NATIVE_RENDERER_PLUME_SOURCE}/${file}" "${patch}")
    set(patched_now "")
    if(EXISTS "${dir}/${file}")
        native_renderer_text_sha256("${dir}/${file}" patched_now)
    endif()
    if(patched_now STREQUAL patched_sha256)
        return()
    endif()
    native_renderer_text_sha256("${NATIVE_RENDERER_PLUME_SOURCE}/${file}" source_now)
    native_renderer_text_sha256("${patch}" patch_now)
    if(NOT source_now STREQUAL source_sha256 OR NOT patch_now STREQUAL patch_sha256)
        message(FATAL_ERROR "${file} or its patch differs from the accepted identity")
    endif()
    # Both copies are written the same way, so their line endings agree.
    file(REMOVE_RECURSE "${dir}")
    foreach(copy IN ITEMS "${NATIVE_RENDERER_PLUME_SOURCE}/${file}|${file}" "${patch}|patch.diff")
        string(REPLACE "|" ";" copy "${copy}")
        list(GET copy 0 copy_from)
        list(GET copy 1 copy_to)
        file(READ "${copy_from}" copy_text)
        string(REPLACE "\r\n" "\n" copy_text "${copy_text}")
        file(WRITE "${dir}/${copy_to}" "${copy_text}")
    endforeach()
    # The build folder can sit inside a Git work tree: the ceiling keeps git apply
    # from finding it, so the patch applies to the copy as a plain patch.
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${CMAKE_BINARY_DIR}"
                            "${GIT_EXECUTABLE}" -c core.autocrlf=false apply --whitespace=nowarn patch.diff
                    WORKING_DIRECTORY "${dir}" RESULT_VARIABLE applied)
    native_renderer_text_sha256("${dir}/${file}" patched_now)
    if(NOT applied EQUAL 0 OR NOT patched_now STREQUAL patched_sha256)
        message(FATAL_ERROR "The Plume patch of ${file} did not give the accepted source")
    endif()
endfunction()
# D3D12: the adapter loop skips an adapter whose shader model query fails
# without using its released device, and the swap chain releases what a failed
# creation made and closes its frame latency handle.
if(NARUTORISE_NATIVE_D3D12)
    set(plume_d3d12_dir "${CMAKE_BINARY_DIR}/plume-d3d12")
    native_renderer_patched_copy(plume_d3d12.cpp
        "${CMAKE_CURRENT_LIST_DIR}/plume-patches/d3d12-release-and-adapter.patch"
        7738a95c6b9f6760a5dc1bde7da2b2e3ae6754033ed6242482b1c9d2d10ebc74
        283f32b936b4cc3012514bad80059f2e84b875da43edbd15fea42a152499e158
        0346a50612df6807794cd7447ba0ac7baa339d4097bdb9543467dcedc14d1c9c
        "${plume_d3d12_dir}")
    add_library(native_renderer_plume STATIC
        "${plume_d3d12_dir}/plume_d3d12.cpp"
        "${NATIVE_RENDERER_PLUME_SOURCE}/contrib/D3D12MemoryAllocator/src/D3D12MemAlloc.cpp")
    target_include_directories(native_renderer_plume PUBLIC "${NATIVE_RENDERER_PLUME_SOURCE}"
        "${NATIVE_RENDERER_PLUME_SOURCE}/contrib/D3D12MemoryAllocator/include")
    target_compile_features(native_renderer_plume PUBLIC cxx_std_17)
    target_compile_definitions(native_renderer_plume PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(native_renderer_plume PUBLIC d3d12 dxgi dxguid)
endif()
# volk and the Vulkan Memory Allocator are implemented inside plume_vulkan.cpp;
# volk loads the system loader (vulkan-1.dll) at run time, so nothing is linked.
# The Vulkan patch: the device enables timeline semaphores when it supports them.
if(NARUTORISE_NATIVE_VULKAN)
    set(plume_vulkan_dir "${CMAKE_BINARY_DIR}/plume-vulkan")
    native_renderer_patched_copy(plume_vulkan.cpp
        "${CMAKE_CURRENT_LIST_DIR}/plume-patches/vulkan-timeline-semaphore.patch"
        c81dc9aeb7544cabd12835bcc924a03184ba91263867016f41cb9f0b4804f849
        e0267d3a4f4b752f8a0d428a1f71efae03d312c0d3354cdd80d94e838c86afa4
        3e237565735ba0d06bc0d95f53579a000767e1bb8b33692c27d7a72f439f3edc
        "${plume_vulkan_dir}")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${CMAKE_CURRENT_LIST_DIR}/../../app/cmake/extend-plume-vulkan.py")
    set(plume_vulkan_wayland)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(plume_vulkan_wayland wayland)
    endif()
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../../app/cmake/extend-plume-vulkan.py"
                            "${plume_vulkan_dir}/plume_vulkan.cpp" "${CMAKE_BINARY_DIR}/plume-native-vulkan.cpp"
                            ${plume_vulkan_wayland} COMMAND_ERROR_IS_FATAL ANY)
    add_library(native_renderer_plume_vulkan STATIC "${CMAKE_BINARY_DIR}/plume-native-vulkan.cpp")
    target_include_directories(native_renderer_plume_vulkan PUBLIC "${NATIVE_RENDERER_PLUME_SOURCE}"
        "${NATIVE_RENDERER_PLUME_SOURCE}/contrib/volk"
        "${NATIVE_RENDERER_PLUME_SOURCE}/contrib/Vulkan-Headers/include"
        "${NATIVE_RENDERER_PLUME_SOURCE}/contrib/VulkanMemoryAllocator/include")
    target_compile_features(native_renderer_plume_vulkan PUBLIC cxx_std_17)
    target_compile_definitions(native_renderer_plume_vulkan PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    # On Linux, Plume's own swap chain queries the window through Xlib.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(X11 REQUIRED)
        target_link_libraries(native_renderer_plume_vulkan PUBLIC X11::X11)
    endif()
endif()
