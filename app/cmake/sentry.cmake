# The release archive includes Crashpad and its dependencies; GitHub's source
# archives do not. Pin both version and content for reproducible builds.
option(NARUTORISE_ENABLE_SENTRY "Build optional, opt-in Sentry crash reporting" ON)
set(NARUTORISE_SENTRY_DSN "https://a63d5e2aa01f37d95830c319efe87b5a@o4512325452496896.ingest.us.sentry.io/4512325773426688"
    CACHE STRING "Public Sentry ingestion DSN (not an auth token)")
set(NARUTORISE_SENTRY_ENVIRONMENT "production" CACHE STRING "Sentry environment for this build")
# The reporting: the SDK, the facts of the session that the crashes carry and
# the facts read from the SDK's log.
add_library(narutorise_reporting STATIC src/sentry_reporting.cpp src/diagnostics/report_context.cpp
    src/diagnostics/log_facts.cpp)
target_link_libraries(narutorise_reporting PRIVATE rex::runtime)
target_link_libraries(narutorise_reporting PRIVATE narutorise_incidents)
target_link_libraries(narutorise PRIVATE narutorise_reporting)
if(NOT NARUTORISE_ENABLE_SENTRY)
    return()
endif()

include(FetchContent)
set(NARUTORISE_SENTRY_SHARED OFF CACHE INTERNAL "Sentry runtime DLL is required" FORCE)
set(_sentry_external_windows OFF)
if(WIN32 AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
    # Crashpad supports the MSVC frontend on Windows. The game's clang++ driver
    # uses the same ABI but needs its own flags, so build the SDK separately.
    set(_sentry_external_windows ON)
    set(NARUTORISE_SENTRY_SHARED ON CACHE INTERNAL "Sentry runtime DLL is required" FORCE)
    set(_sentry_source_subdir SOURCE_SUBDIR _narutorise_external_build)
endif()
set(SENTRY_BACKEND crashpad CACHE STRING "" FORCE)
set(SENTRY_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SENTRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SENTRY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SENTRY_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SENTRY_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(CRASHPAD_ZLIB_SYSTEM OFF CACHE BOOL "" FORCE)
FetchContent_Declare(sentry_native
    URL https://github.com/getsentry/sentry-native/releases/download/0.17.4/sentry-native.zip
    URL_HASH SHA256=804811557b5d42ddd51cb1241bf851964d75547a4eb7b5ad97b73a4de965ab25
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE ${_sentry_source_subdir})
FetchContent_MakeAvailable(sentry_native)
if(_sentry_external_windows)
    include(ExternalProject)
    get_filename_component(_sentry_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(NARUTORISE_SENTRY_COMPILER NAMES clang-cl
        HINTS "${_sentry_compiler_dir}" REQUIRED)
    set(_sentry_install "${CMAKE_CURRENT_BINARY_DIR}/sentry-sdk")
    ExternalProject_Add(narutorise_sentry_external
        SOURCE_DIR "${sentry_native_SOURCE_DIR}"
        BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/sentry-sdk-build"
        DOWNLOAD_COMMAND ""
        CMAKE_ARGS
            "-DCMAKE_C_COMPILER=${NARUTORISE_SENTRY_COMPILER}"
            "-DCMAKE_CXX_COMPILER=${NARUTORISE_SENTRY_COMPILER}"
            -DCMAKE_BUILD_TYPE=RelWithDebInfo
            "-DCMAKE_INSTALL_PREFIX=${_sentry_install}"
            -DSENTRY_BACKEND=crashpad -DSENTRY_BUILD_SHARED_LIBS=ON
            -DSENTRY_BUILD_TESTS=OFF -DSENTRY_BUILD_EXAMPLES=OFF
            -DSENTRY_BUILD_BENCHMARKS=OFF -DSENTRY_ENABLE_INSTALL=ON
            -DCRASHPAD_ZLIB_SYSTEM=OFF
        BUILD_BYPRODUCTS "${_sentry_install}/lib/sentry.lib"
            "${_sentry_install}/bin/sentry.dll" "${_sentry_install}/bin/crashpad_handler.exe"
            "${_sentry_install}/bin/crashpad_wer.dll")
    add_library(narutorise_sentry_sdk SHARED IMPORTED)
    set_target_properties(narutorise_sentry_sdk PROPERTIES
        IMPORTED_IMPLIB "${_sentry_install}/lib/sentry.lib"
        IMPORTED_LOCATION "${_sentry_install}/bin/sentry.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${sentry_native_SOURCE_DIR}/include")
    add_dependencies(narutorise_reporting narutorise_sentry_external)
    target_link_libraries(narutorise_reporting PRIVATE narutorise_sentry_sdk)
    set(_sentry_handler "${_sentry_install}/bin/crashpad_handler.exe")
    set(_sentry_wer "${_sentry_install}/bin/crashpad_wer.dll")
    set(_sentry_runtime_dependency narutorise_sentry_external)
else()
    target_link_libraries(narutorise_reporting PRIVATE sentry::sentry)
    set(_sentry_handler "$<TARGET_FILE:crashpad::handler>")
    set(_sentry_runtime_dependency crashpad::handler)
    if(WIN32)
        set(_sentry_wer "$<TARGET_FILE:crashpad::wer>")
    endif()
endif()
target_compile_definitions(narutorise_reporting PRIVATE NARUTORISE_ENABLE_SENTRY=1
    NARUTORISE_SENTRY_DSN="${NARUTORISE_SENTRY_DSN}"
    NARUTORISE_SENTRY_ENVIRONMENT="${NARUTORISE_SENTRY_ENVIRONMENT}")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
        OUTPUT_VARIABLE _sentry_revision OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" diff --quiet --ignore-submodules
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.." RESULT_VARIABLE _sentry_dirty)
    if(_sentry_dirty EQUAL 1)
        string(APPEND _sentry_revision "-dirty")
    endif()
endif()
if(NOT _sentry_revision)
    set(_sentry_revision unknown)
endif()
target_compile_definitions(narutorise_reporting PRIVATE NARUTORISE_BUILD_REVISION="${_sentry_revision}")

# Keep source/line information for the reporting code and the application entry
# point. Generated guest code already uses -gline-tables-only.
set_property(SOURCE src/sentry_reporting.cpp src/main.cpp APPEND PROPERTY COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-g>")
if(WIN32)
    target_link_options(narutorise PRIVATE "LINKER:/DEBUG" "LINKER:/OPT:REF" "LINKER:/OPT:ICF")
else()
    target_link_options(narutorise PRIVATE "LINKER:--build-id")
endif()

add_custom_target(narutorise_sentry_runtime
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_sentry_handler}"
            $<TARGET_FILE_DIR:narutorise>
    DEPENDS ${_sentry_runtime_dependency} VERBATIM)
if(WIN32)
    if(NOT _sentry_external_windows)
        add_dependencies(narutorise_sentry_runtime crashpad::wer)
    endif()
    add_custom_command(TARGET narutorise_sentry_runtime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_sentry_wer}"
                $<TARGET_FILE_DIR:narutorise> VERBATIM)
    if(_sentry_external_windows)
        add_custom_command(TARGET narutorise_sentry_runtime POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_sentry_install}/bin/sentry.dll" "${_sentry_install}/bin/sentry.pdb"
                "${_sentry_install}/bin/crashpad_handler.pdb" "${_sentry_install}/bin/crashpad_wer.pdb"
                $<TARGET_FILE_DIR:narutorise> VERBATIM)
    endif()
endif()

# Preserve the upstream redistribution notices beside the built application.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sentry-notices.txt" "Sentry Native 0.17.4 and bundled dependencies\n\n")
foreach(_license IN ITEMS LICENSE external/crashpad/LICENSE
        external/crashpad/third_party/mini_chromium/mini_chromium/LICENSE
        external/crashpad/third_party/zlib/zlib/LICENSE
        external/crashpad/third_party/mpack/LICENSE
        external/crashpad/third_party/getopt/LICENSE
        vendor/libunwind/COPYING)
    file(READ "${sentry_native_SOURCE_DIR}/${_license}" _license_text)
    file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/sentry-notices.txt" "\n--- ${_license} ---\n${_license_text}\n")
endforeach()
add_dependencies(narutorise narutorise_sentry_runtime)
