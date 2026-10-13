## Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

if(MACOSX)
    set(mac_sources
        src/nx/network/ssl/certificate_mac.mm
    )
endif()

nx_add_target(nx_network LIBRARY
    NO_MOC
    ADDITIONAL_SOURCES
        ${mac_sources}
    PUBLIC_LIBS
        Qt6::Network
        OpenSSL::OpenSSL
        nx_kit
        nx_utils
        nx_reflect
    FOLDER common/libs
)

target_compile_definitions(nx_network
    PRIVATE NX_NETWORK_API=${API_EXPORT_MACRO}
    INTERFACE NX_NETWORK_API=${API_IMPORT_MACRO})

if(WINDOWS)
    target_link_libraries(nx_network
        PRIVATE
            Iphlpapi
        PUBLIC
            ws2_32
    )
endif()

if(MACOSX)
    target_link_libraries(nx_network
        PRIVATE
            "-framework Security"
    )
endif()

if(withTests)
    add_subdirectory(unit_tests)
endif()
