## Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

nx_add_target(nx_network_rest LIBRARY
    PUBLIC_LIBS nx_network
    FOLDER common/libs
)

target_compile_definitions(nx_network_rest
    PRIVATE NX_NETWORK_REST_API=${API_EXPORT_MACRO}
    INTERFACE NX_NETWORK_REST_API=${API_IMPORT_MACRO}
)
