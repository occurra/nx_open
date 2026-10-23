## Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

find_package(libsrtp REQUIRED)
find_package(usrsctp REQUIRED)

nx_add_target(nx_webrtc LIBRARY
    PUBLIC_LIBS
        nx_vms_common
        libsrtp::libsrtp
        usrsctp::usrsctp
    FOLDER common/libs
)

target_compile_definitions(nx_webrtc
    PRIVATE NX_WEBRTC_API=${API_EXPORT_MACRO}
    INTERFACE NX_WEBRTC_API=${API_IMPORT_MACRO})

# usrsctp keeps its whole state in process globals, so a process with a second SCTP stack, like
# ai_agent_toolbox_ut with libdatachannel, needs a usrsctp of its own. The archive's symbols stay
# inside this library, as a Windows DLL keeps them by default, so the executable links its own
# copy instead of binding to this one.
if(LINUX)
    target_link_options(nx_webrtc PRIVATE "-Wl,--exclude-libs,libusrsctp.a")
elseif(MACOSX)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type)
    target_link_options(nx_webrtc PRIVATE
        "-L${usrsctp_LIB_DIRS_${build_type}}" "-Wl,-hidden-lusrsctp")
endif()

if(withTests)
    add_subdirectory(unit_tests)
endif()
