# The shader cache of the native renderer, generated in the build from the local game
# files. The XenosRecomp fork of the
# repository (tools/xenosrecomp) is built as a host tool, and build-shader-cache.py runs
# the chain inside the build directory: the containers of game/xeshader.sdb, the
# translation and DXC of each shader, the lib_6_3 libraries of the spec constants and
# the variants linked from them with the values the draw asks for, the index
# (identification only by hash) and the package that the given unit embeds
# (#embed <shader_cache.bin>, by the embed directory set here).
#
#   narutorise_shader_cache(<target> <embedding source> [SPIRV PASSES <backend folder>])
#
# SPIRV (a build with the Vulkan backend): the chain also compiles the SPIR-V of each
# shader, checks it with NARUTORISE_SPIRV_VAL (build-sdb-shader-spirv.py), does the same
# for the passes of the backend folder (build-pass-spirv.py: its *.hlsl, fxaa and the
# SDK's modules in sdk_shaders/vulkan_spirv) and packs them, encoded by smol-v
# (<target>_smolv_encode, a host tool), into shader_cache_spirv.bin with the manifest;
# the source embeds it too (NARUTORISE_SHADER_CACHE_SPIRV) and the target links the
# smol-v decoder (<target>_smolv).
#
# The packages are compressed by Zstandard: <target>_shader_compress (a host tool)
# writes them and the target links the decoder, both from the zstd of the XenosRecomp
# dependencies, so neither the build nor the renderer needs a system codec.
#
# NARUTORISE_GAME_DIR: the local game files (xeshader.sdb).
# NARUTORISE_XENOS_RECOMP_THIRDPARTY: the dependencies of XenosRecomp (fmt, xxHash, zstd,
# smol-v, dxc-bin), the submodules of the upstream checkout (tools/xenosrecomp/thirdparty).
# NARUTORISE_SHADER_STORE: the content-addressed store of the translated and compiled
# shaders, kept across builds (shader_store.py; empty: none). It derives from the game
# files, so it stays in a git-ignored folder; deleting it only makes the next build slower.
# NARUTORISE_SPIRV_VAL: spirv-val of SPIRV-Tools, for SPIRV (the Vulkan SDK's, the system's
# on Linux, or the one tools/native-renderer/vulkan/build-validation-layer.ps1 builds under
# out/vulkan-tools).
#
# NARUTORISE_DXC is the DXC of the build host from dxc-bin (dxc.exe on Windows, dxc-linux on
# Linux), and NARUTORISE_DXC_ENV the command prefix that runs it and every tool that starts
# it: dxc-linux finds libdxcompiler.so through LD_LIBRARY_PATH.
include_guard(GLOBAL)
include(ExternalProject)
enable_language(C)  # Zstandard.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

get_filename_component(_narutorise_fork "${CMAKE_CURRENT_LIST_DIR}/../xenosrecomp" ABSOLUTE)
set(NARUTORISE_XENOS_RECOMP_THIRDPARTY "${_narutorise_fork}/thirdparty"
    CACHE PATH "thirdparty directory with the XenosRecomp dependencies")
get_filename_component(_narutorise_dxc_bin "${NARUTORISE_XENOS_RECOMP_THIRDPARTY}/dxc-bin" ABSOLUTE)
if(CMAKE_HOST_WIN32)
  set(NARUTORISE_DXC "${_narutorise_dxc_bin}/bin/x64/dxc.exe")
  set(NARUTORISE_DXC_ENV)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  set(NARUTORISE_DXC "${_narutorise_dxc_bin}/bin/x64/dxc-linux")
  set(NARUTORISE_DXC_ENV "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${_narutorise_dxc_bin}/lib/x64")
else()
  message(FATAL_ERROR "The shader cache of the native renderer has no DXC for ${CMAKE_HOST_SYSTEM_NAME}")
endif()

function(narutorise_shader_cache target source)
  cmake_parse_arguments(PARSE_ARGV 2 arg "SPIRV" "PASSES" "")
  set(tools "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
  set(fork "${_narutorise_fork}")
  set(NARUTORISE_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../game" CACHE PATH "Local game files (xeshader.sdb)")
  set(NARUTORISE_SHADER_STORE "${CMAKE_CURRENT_SOURCE_DIR}/../out/shader-store"
      CACHE PATH "Store of the translated and compiled shaders kept across builds (empty: none)")
  get_filename_component(game "${NARUTORISE_GAME_DIR}" ABSOLUTE)
  get_filename_component(thirdparty "${NARUTORISE_XENOS_RECOMP_THIRDPARTY}" ABSOLUTE)
  foreach(input "${game}/xeshader.sdb" "${NARUTORISE_DXC}")
    if(NOT EXISTS "${input}")
      message(FATAL_ERROR "The shader cache of the native renderer needs ${input}")
    endif()
  endforeach()
  # Zstandard: the static library only, without the legacy formats (the packages
  # are written by this same version).
  if(NOT TARGET libzstd_static)
    set(ZSTD_BUILD_SHARED OFF)
    set(ZSTD_BUILD_STATIC ON)
    set(ZSTD_BUILD_PROGRAMS OFF)
    set(ZSTD_BUILD_TESTS OFF)
    set(ZSTD_BUILD_CONTRIB OFF)
    set(ZSTD_BUILD_DICTBUILDER OFF)
    set(ZSTD_MULTITHREAD_SUPPORT OFF)
    set(ZSTD_LEGACY_SUPPORT OFF)
    add_subdirectory("${thirdparty}/zstd/build/cmake" "${CMAKE_BINARY_DIR}/shader-zstd" EXCLUDE_FROM_ALL)
  endif()
  add_executable(${target}_shader_compress "${tools}/shader-compress.cpp")
  target_link_libraries(${target}_shader_compress PRIVATE libzstd_static)
  target_link_libraries(${target} PRIVATE libzstd_static)
  set(out "${CMAKE_CURRENT_BINARY_DIR}/shader_cache")
  set(spirv)
  set(spirv_outputs)
  set(spirv_encoder)
  set(pass_sources)
  if(arg_SPIRV)
    if(NOT arg_PASSES)
      message(FATAL_ERROR "narutorise_shader_cache: SPIRV needs PASSES <backend folder>")
    endif()
    get_filename_component(passes "${arg_PASSES}" ABSOLUTE)
    file(GLOB pass_sources CONFIGURE_DEPENDS "${passes}/*.hlsl" "${passes}/fxaa/*"
         "${passes}/sdk_shaders/vulkan_spirv/*.h")
    find_program(NARUTORISE_SPIRV_VAL spirv-val
        HINTS "$ENV{VULKAN_SDK}/Bin"
              "${CMAKE_CURRENT_SOURCE_DIR}/../out/vulkan-tools/validation-layers-1.4.363.0/src/external/Release/64/SPIRV-Tools/build/install/bin"
        DOC "spirv-val of SPIRV-Tools, for the SPIR-V of the shader cache")
    if(NOT NARUTORISE_SPIRV_VAL)
      message(FATAL_ERROR "The SPIR-V of the shader cache needs spirv-val (Vulkan SDK or "
                          "tools/native-renderer/vulkan/build-validation-layer.ps1): set NARUTORISE_SPIRV_VAL")
    endif()
    # smol-v (the XenosRecomp dependency, <target>_smolv): the build encodes the modules,
    # the renderer decodes them.
    add_library(${target}_smolv STATIC "${thirdparty}/smol-v/source/smolv.cpp")
    target_include_directories(${target}_smolv PUBLIC "${thirdparty}/smol-v/source")
    add_executable(${target}_smolv_encode "${tools}/smolv-encode.cpp")
    target_link_libraries(${target}_smolv_encode PRIVATE ${target}_smolv)
    target_link_libraries(${target} PRIVATE ${target}_smolv)
    target_compile_definitions(${target} PRIVATE NARUTORISE_SHADER_CACHE_SPIRV=1)
    set(spirv --spirv-val "${NARUTORISE_SPIRV_VAL}" --smolv-encode "$<TARGET_FILE:${target}_smolv_encode>"
              --passes "${passes}")
    set(spirv_outputs "${out}/shader_cache_spirv.bin" "${out}/shader_manifest.json")
    set(spirv_encoder ${target}_smolv_encode)  # Reruns the chain when the encoder changes.
  endif()

  # The fork as a host tool (the C compiler is the clang of the build, as C). Its DXIL
  # (the in-process cache mode, not the single-file mode the chain uses) needs dxil.dll,
  # which dxc-bin has on Windows only.
  string(REGEX REPLACE "clang\\+\\+" "clang" c_compiler "${CMAKE_CXX_COMPILER}")
  if(CMAKE_HOST_WIN32)
    set(recomp_dxil ON)
  else()
    set(recomp_dxil OFF)
  endif()
  set(recomp_build "${CMAKE_CURRENT_BINARY_DIR}/xenosrecomp")
  set(recomp "${recomp_build}/XenosRecomp/XenosRecomp${CMAKE_EXECUTABLE_SUFFIX}")
  ExternalProject_Add(${target}_xenosrecomp
      SOURCE_DIR "${fork}"
      BINARY_DIR "${recomp_build}"
      CMAKE_ARGS -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_C_COMPILER=${c_compiler}"
                 "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}" "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
                 -DXENOS_RECOMP_DXIL=${recomp_dxil} "-DXENOS_RECOMP_THIRDPARTY_ROOT=${thirdparty}"
      BUILD_COMMAND "${CMAKE_COMMAND}" --build "${recomp_build}" --target XenosRecomp
      INSTALL_COMMAND ""
      BUILD_BYPRODUCTS "${recomp}")

  set(scripts build-shader-cache.py extract-sdb-containers.py build-sdb-shader-cache.py
      build-sdb-shader-libraries.py build-sdb-shader-variants.py build-native-shader-index.py
      build-embedded-shader-cache.py shader_store.py build-sdb-shader-spirv.py spirv_module.py
      build-shader-manifest.py build-pass-spirv.py)
  list(TRANSFORM scripts PREPEND "${tools}/")
  add_custom_command(
      OUTPUT "${out}/shader_cache.bin" "${out}/shader_cache.json" ${spirv_outputs}
      COMMAND ${NARUTORISE_DXC_ENV} "${Python3_EXECUTABLE}" -I "${tools}/build-shader-cache.py" "${game}/xeshader.sdb"
              "${recomp}" "${fork}/XenosRecomp/shader_common.h" "${NARUTORISE_DXC}" "${out}"
              "${NARUTORISE_SHADER_STORE}" --compressor "$<TARGET_FILE:${target}_shader_compress>" ${spirv}
      DEPENDS "${game}/xeshader.sdb" "${recomp}" ${target}_xenosrecomp "${fork}/XenosRecomp/shader_common.h"
              ${scripts} ${spirv_encoder} ${pass_sources} ${target}_shader_compress
      COMMENT "Shader cache of the native renderer from ${game}/xeshader.sdb (XenosRecomp fork, DXC)"
      USES_TERMINAL
      VERBATIM)
  # "native" in the name: the product build already has <target>_shader_cache
  # (the Xenos plugin's cache copied from packaging/shader_cache).
  add_custom_target(${target}_native_shader_cache DEPENDS "${out}/shader_cache.bin")
  add_dependencies(${target} ${target}_native_shader_cache)
  # #embed <...> searches the embed directories of clang (--embed-dir), not -I.
  set_source_files_properties("${source}" PROPERTIES
      COMPILE_OPTIONS "-Wno-c23-extensions;--embed-dir=${out}"
      OBJECT_DEPENDS "${out}/shader_cache.bin;${spirv_outputs}")
endfunction()
