## Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

find_package(Qt6 COMPONENTS Test)

nx_add_test(nx_webrtc_ut
    PRIVATE_LIBS
        nx_webrtc
    FOLDER common/tests
)
