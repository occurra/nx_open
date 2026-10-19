## Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

include_guard(GLOBAL)

if(NOT WINDOWS)
    return()
endif()

function(nx_get_windows_sign_command variable)
    if(NOT windows_sign_tool)
        message(FATAL_ERROR
            "windows_sign_tool is not set; the project must provide a signing tool.")
    endif()

    set(signing_parameters
        ${PYTHON_EXECUTABLE} ${windows_sign_tool}
        ${additional_windows_sign_tool_parameters}
        --file)
    set(${variable} ${signing_parameters} PARENT_SCOPE)
endfunction()

function(nx_sign_windows_executable target)
    set(signCommand "")
    nx_get_windows_sign_command(signCommand)

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${signCommand} $<TARGET_FILE:${target}>
            --output $<TARGET_FILE:${target}>
    )
endfunction()
