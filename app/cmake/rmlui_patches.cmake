# Local fixes of RmlUi: each patch of rmlui-patches/ applies to a copy of a core
# source in the build folder, and the copy replaces that source in rmlui_core, so
# the submodule stays clean. Source, patch and result are checked by the SHA-256
# of their text with LF line endings: an RmlUi update stops here until its patch
# is redone, instead of compiling an unreviewed mix.
find_package(Git REQUIRED)
get_filename_component(_rmlui_core_dir "${CMAKE_CURRENT_LIST_DIR}/../../third_party/rmlui/Source/Core" ABSOLUTE)

function(rmlui_text_sha256 path out)
    file(READ "${path}" text)
    string(REPLACE "\r\n" "\n" text "${text}")
    string(SHA256 sha "${text}")
    set(${out} "${sha}" PARENT_SCOPE)
endfunction()

# Compiles `file` (relative to Source/Core) from a copy with `patch` applied.
function(rmlui_patch_core_source file patch source_sha256 patch_sha256 patched_sha256)
    set(source "${_rmlui_core_dir}/${file}")
    set(dir "${CMAKE_BINARY_DIR}/rmlui-patched")
    set(copy "${dir}/${file}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}" "${patch}")
    rmlui_text_sha256("${source}" source_now)
    rmlui_text_sha256("${patch}" patch_now)
    if(NOT source_now STREQUAL source_sha256 OR NOT patch_now STREQUAL patch_sha256)
        message(FATAL_ERROR "RmlUi ${file} or ${patch} differs from the accepted identity; redo the patch")
    endif()
    set(patched_now "")
    if(EXISTS "${copy}")
        rmlui_text_sha256("${copy}" patched_now)
    endif()
    # An accepted copy is kept as it is, so a reconfigure does not rebuild it.
    if(NOT patched_now STREQUAL patched_sha256)
        get_filename_component(copy_dir "${copy}" DIRECTORY)
        file(REMOVE "${copy}")
        file(MAKE_DIRECTORY "${copy_dir}")
        foreach(pair IN ITEMS "${source}|${copy}" "${patch}|${copy}.diff")
            string(REPLACE "|" ";" pair "${pair}")
            list(GET pair 0 pair_from)
            list(GET pair 1 pair_to)
            file(READ "${pair_from}" text)
            string(REPLACE "\r\n" "\n" text "${text}")
            file(WRITE "${pair_to}" "${text}")
        endforeach()
        # The build folder can sit inside a Git work tree: the ceiling keeps git apply
        # from finding it, so the patch applies to the copy as a plain patch.
        get_filename_component(copy_name "${copy}" NAME)
        execute_process(COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${CMAKE_BINARY_DIR}"
                                "${GIT_EXECUTABLE}" -c core.autocrlf=false apply --whitespace=nowarn "${copy_name}.diff"
                        WORKING_DIRECTORY "${copy_dir}" RESULT_VARIABLE applied)
        rmlui_text_sha256("${copy}" patched_now)
        if(NOT applied EQUAL 0 OR NOT patched_now STREQUAL patched_sha256)
            message(FATAL_ERROR "The RmlUi patch ${patch} did not give the accepted source")
        endif()
    endif()
    get_target_property(sources rmlui_core SOURCES)
    list(FIND sources "${file}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "rmlui_core does not compile ${file}")
    endif()
    list(REMOVE_AT sources ${at})
    set_property(TARGET rmlui_core PROPERTY SOURCES ${sources} "${copy}")
    # The copy keeps the relative includes of the original: its own folder first.
    get_filename_component(source_dir "${source}" DIRECTORY)
    set_source_files_properties("${copy}" TARGET_DIRECTORY rmlui_core PROPERTIES INCLUDE_DIRECTORIES "${source_dir}")
endfunction()

# text-transform maps two-byte UTF-8 letters too (Latin-1, Latin Extended-A and
# basic Cyrillic), not only a-z: accented menu titles and Russian were left as
# typed.
rmlui_patch_core_source(ElementText.cpp "${CMAKE_CURRENT_LIST_DIR}/rmlui-patches/element-text-case.patch"
    40f2c958c52d5030f140fbe8004dc2a6e2f03c50dadd16866d0eb1e8cd673657
    54294bd43d07f1c8705c9f10806a20230cc389b220f0b70ab1e48e53923d2614
    0c2205e1f8cfe306b4208df959cf16b7390e367f0f542ef85b7c8fc121ed8954)
