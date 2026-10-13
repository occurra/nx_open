## Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

nx_add_target(nx_media_core LIBRARY
    PUBLIC_LIBS
        Qt6::Core
        nx_utils
        nx_codec
        nx_metric
        nx_reflect
        ffmpeg::ffmpeg
    PRIVATE_LIBS
        Qt6::Gui
        Qt6::CorePrivate
    FOLDER common/libs
)

if(LINUX AND NOT ANDROID)
    target_link_libraries(nx_media_core PRIVATE rt)
endif()

target_compile_definitions(nx_media_core
    PRIVATE NX_MEDIA_CORE_API=${API_EXPORT_MACRO}
    INTERFACE NX_MEDIA_CORE_API=${API_IMPORT_MACRO}
)

if("${arch}" STREQUAL "arm" OR "${arch}" STREQUAL "arm64")
    find_package(sse2neon)
    target_link_libraries(nx_media_core PUBLIC sse2neon::sse2neon)
endif()

include(CheckCXXCompilerFlag)

check_cxx_compiler_flag("-march=x86-64-v3" COMPILER_SUPPORTS_X86_64_V3)

if(COMPILER_SUPPORTS_X86_64_V3)
    target_compile_options(nx_media_core PRIVATE -march=x86-64-v3)
endif()

if(compilerMsvc)
    # Qt's QT_COMPILER_SUPPORTS_HERE(), which guards the SIMD dispatch in yuvconvert.cpp, expands
    # to an expression containing `defined`. That is undefined behavior the conforming
    # preprocessor reports as C5105, at our use site rather than in the Qt header, so /external:W0
    # does not cover it.
    target_compile_options(nx_media_core PRIVATE /wd5105)
endif()

if(withTests)
    add_subdirectory(unit_tests)
endif()
