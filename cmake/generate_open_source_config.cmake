## Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

include_guard(GLOBAL)

include(${CMAKE_CURRENT_LIST_DIR}/output_directories.cmake)

include(utils)

nx_configure_file("${open_source_root}/build_info.txt.in"
    "${distribution_output_dir}/build_info.txt")
nx_configure_file("${open_source_root}/build_info.json.in"
    "${distribution_output_dir}/build_info.json")
nx_configure_file("${open_source_root}/nx_log_viewer.html" ${CMAKE_BINARY_DIR} COPYONLY)
nx_configure_file("${CMAKE_BINARY_DIR}/conan_refs.txt" ${distribution_output_dir} COPYONLY)

# The installer ships the lockfile as build metadata. Conan 2 consumes it from the source
# tree instead of writing a resolved one into the build directory, so it is staged from
# there.
nx_configure_file("${CMAKE_SOURCE_DIR}/conan.lock" ${distribution_output_dir} COPYONLY)
