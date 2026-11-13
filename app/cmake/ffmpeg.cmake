# FFmpeg for the native audio's XMA (stage 2b): only the XMA frames decoder of
# the WMA Pro family and what it needs of libavcodec and libavutil, as one
# static library embedded in the executable.
#
# The source is third_party/ffmpeg: the wmarti/FFmpeg fork (FFmpeg 4.4) at the
# commit the SDK uses (0604b464c7), so our decoding can be compared bit for bit
# with the SDK's. Its platform configuration headers (config.h dispatches by
# platform) come with the fork; the lists below register only xmaframes.
#
# The SDK links its own FFmpeg into its runtime library without exporting it;
# this copy has hidden visibility, so the two never meet.

set(NARUTORISE_FFMPEG_DIR "${CMAKE_CURRENT_LIST_DIR}/../../third_party/ffmpeg" CACHE PATH
    "FFmpeg fork for the native audio's XMA (wmarti/FFmpeg 0604b464c7)")
set(_ff "${NARUTORISE_FFMPEG_DIR}")
if(NOT EXISTS "${_ff}/libavcodec/wmaprodec.c")
    message(FATAL_ERROR "FFmpeg not found at ${_ff}: run git submodule update --init third_party/ffmpeg")
endif()

# Only xmaframes is registered; no parser; the null filter that decoding needs.
set(_ff_lists "${CMAKE_CURRENT_BINARY_DIR}/narutorise-ffmpeg-lists")
foreach(_list IN ITEMS codec_list parser_list bsf_list)
    configure_file("${CMAKE_CURRENT_LIST_DIR}/ffmpeg/${_list}.c" "${_ff_lists}/libavcodec/${_list}.c" COPYONLY)
endforeach()

set(_ff_avutil
    avstring.c bprint.c buffer.c channel_layout.c cpu.c dict.c error.c eval.c fifo.c file_open.c fixed_dsp.c float_dsp.c
    frame.c hwcontext.c imgutils.c intmath.c log.c log2_tab.c mathematics.c mem.c opt.c parseutils.c
    pixdesc.c random_seed.c rational.c reverse.c samplefmt.c sha.c slicethread.c time.c utils.c)
set(_ff_avcodec
    allcodecs.c avcodec.c avpacket.c bitstream.c bitstream_filters.c bsf.c codec_desc.c codec_par.c
    decode.c encode.c fft_float.c fft_init_table.c mathtables.c mdct_float.c null_bsf.c options.c
    parser.c parsers.c profiles.c pthread.c pthread_frame.c pthread_slice.c raw.c sinewin.c utils.c
    wma.c wma_common.c wma_freqs.c wmaprodec.c)
list(TRANSFORM _ff_avutil PREPEND "${_ff}/libavutil/")
list(TRANSFORM _ff_avcodec PREPEND "${_ff}/libavcodec/")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    list(APPEND _ff_avutil "${_ff}/libavutil/x86/cpu.c" "${_ff}/libavutil/x86/fixed_dsp_init.c"
         "${_ff}/libavutil/x86/float_dsp_init.c" "${_ff}/libavutil/x86/imgutils_init.c")
    list(APPEND _ff_avcodec "${_ff}/libavcodec/x86/fft_init.c")
endif()

add_library(narutorise_ffmpeg STATIC ${_ff_avutil} ${_ff_avcodec})
target_include_directories(narutorise_ffmpeg BEFORE PRIVATE "${_ff_lists}")
target_include_directories(narutorise_ffmpeg SYSTEM PUBLIC "${_ff}")
if(WIN32)
    target_include_directories(narutorise_ffmpeg PRIVATE "${_ff}/compat/atomics/win32")
    target_link_libraries(narutorise_ffmpeg PRIVATE bcrypt)
else()
    target_include_directories(narutorise_ffmpeg PRIVATE "${_ff}/compat/atomics/gcc")
    target_link_libraries(narutorise_ffmpeg PRIVATE m pthread)
endif()
target_compile_definitions(narutorise_ffmpeg PRIVATE HAVE_AV_CONFIG_H _USE_MATH_DEFINES)
set_target_properties(narutorise_ffmpeg PROPERTIES LINKER_LANGUAGE C C_VISIBILITY_PRESET hidden
                      VISIBILITY_INLINES_HIDDEN ON POSITION_INDEPENDENT_CODE ON)
target_compile_options(narutorise_ffmpeg PRIVATE -w)
