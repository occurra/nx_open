## Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

include(${open_source_root}/cmake/distribution/common.cmake)
include(${open_source_root}/cmake/distribution/wix.cmake)
include(${open_source_root}/cmake/wix_utils.cmake)

set(common_wixobjects_path ${CMAKE_CURRENT_BINARY_DIR}/common CACHE INTERNAL "" FORCE)
set(common_modules_output_path ${CMAKE_CURRENT_BINARY_DIR}/msm CACHE INTERNAL "" FORCE)

add_subdirectory(common)
