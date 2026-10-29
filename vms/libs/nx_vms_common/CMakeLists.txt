## Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

find_package(libsrtp REQUIRED)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/resources")

if(APPLE)
    set(apple_sources src/utils/apple_utils.mm)
endif()

nx_add_target(nx_vms_common LIBRARY
    ADDITIONAL_SOURCES
        ${mac_sources}
        ${apple_sources}
    ADDITIONAL_RESOURCES
        "${CMAKE_CURRENT_BINARY_DIR}/resources"
    PUBLIC_LIBS
        Qt6::Gui
        Qt6::Concurrent
        ffmpeg::ffmpeg
        nx_vms_api
        nx_network
        nx_network_rest
        nx_utils
        nx_rtp
        nx_metric
        nx_media_core
        nx_codec
        libsrtp::libsrtp
    FOLDER common/libs
)
nx_make_target_translatable(nx_vms_common COMPONENTS "server")

if(compilerMsvc)
    # ignore PDB was not found
    target_link_options(nx_vms_common PRIVATE "/IGNORE:4099")
endif()

target_compile_definitions(nx_vms_common
    PRIVATE
        NX_VMS_COMMON_API=${API_EXPORT_MACRO}
    INTERFACE
        NX_VMS_COMMON_API=${API_IMPORT_MACRO}
)

target_include_directories(nx_vms_common
    PRIVATE
        ${device_vendor_names_root}/src
    PUBLIC
        ${date_time_formatter_root}/src
)

if(WINDOWS)
    target_link_libraries(nx_vms_common PUBLIC Winmm)
endif()

if(MACOSX)
    find_library(CoreFoundation_LIBRARY CoreFoundation)
    mark_as_advanced(CoreFoundation_LIBRARY)

    find_library(IOKit_LIBRARY IOKit)
    mark_as_advanced(IOKit_LIBRARY)

    find_library(AppKit_LIBRARY AppKit)
    mark_as_advanced(AppKit_LIBRARY)

    target_link_libraries(nx_vms_common
        PUBLIC ${CoreFoundation_LIBRARY} ${IOKit_LIBRARY} ${AppKit_LIBRARY})
endif()

find_package(Freetype)
target_link_libraries(nx_vms_common PRIVATE Freetype::Freetype)

if(withTests)
    add_subdirectory(unit_tests)
endif()
