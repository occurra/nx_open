## Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

include_guard(GLOBAL)

enable_testing()

set(testTempDirectory "${CMAKE_BINARY_DIR}" CACHE STRING "Temp directory for running tests.")

add_custom_target(unit_tests)

set_target_properties(unit_tests PROPERTIES FOLDER utils)

function(nx_add_test target) # [NO_GTEST] [NO_QT] [NO_NX_UTILS] ...
    set(options NO_GTEST NO_QT NO_NX_UTILS)
    set(oneValueArgs FOLDER)
    cmake_parse_arguments(NX_ADD_TEST "${options}"
        "${oneValueArgs}" #[[multiValueArgs]] "" ${ARGN})

    set_output_directories(RUNTIME "bin" LIBRARY "lib"
        AFFECTED_VARIABLES_RESULT affected_variables)
    nx_expose_variables_to_parent_scope(${affected_variables})

    if(NOT NX_ADD_TEST_NO_GTEST)
        set(additional_private_libs
            PRIVATE_LIBS
                GTest
                GMock
        )
    endif()

    if(NX_ADD_TEST_FOLDER)
        set(folder "${NX_ADD_TEST_FOLDER}")
    else()
        set(folder "tests")
    endif()

    if(NOT NX_ADD_TEST_NO_GTEST AND NOT NX_ADD_TEST_NO_NX_UTILS AND NOT NX_ADD_TEST_NO_QT)
        set(additional_test_support_private_libs
            PRIVATE_LIBS
                nx_utils
                Qt6::Core)
        list(APPEND CMAKE_INSTALL_RPATH ${QT_DIR}/lib)
        list(REMOVE_DUPLICATES CMAKE_INSTALL_RPATH)
    endif()

    nx_add_target(${target}
        EXECUTABLE
        NO_RC_FILE
        NO_MOC
        ${additional_private_libs}
        ${additional_test_support_private_libs}
        FOLDER ${folder}
        ${NX_ADD_TEST_UNPARSED_ARGUMENTS}
    )

    add_test(NAME ${target} COMMAND ${target})

    if(WINDOWS)
        # Windows has no RPATH, so a test finds the DLLs it links against through PATH. The
        # activate_run script Conan generates in the build directory is what puts them there, and
        # it stays authoritative: setting an ENVIRONMENT property here would replace PATH with a
        # configure-time snapshot and discard the activated one.
        set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE OFF) #< Build a console app.
    endif()

    add_dependencies(unit_tests ${target})
endfunction()
