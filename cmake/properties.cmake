## Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

# This file is loaded after the customization. It contains logic to set complex variables, combined
# from other customizable variables. Also it contains defaults for the rarely overridden variables.
# Finally, we can declare here some variables that are not overridden, but are semantically related
# to the customizable product options.

# Localization.  See https://developer.crowdin.com/language-codes/ for the list of codes.
set(translations
    en_US
    en_GB
    fr_FR
    cs_CZ
    da_DK
    de_DE
    el_GR
    fi_FI
    ru_RU
    es_ES
    ca_ES
    gl_ES
    id_ID
    it_IT
    ja_JP
    ko_KR
    lt_LT
    tr_TR
    zh_CN
    zh_TW
    he_IL
    hu_HU
    nl_BE
    nl_NL
    no_NO
    pl_PL
    pt_BR
    pt_PT
    uk_UA
    vi_VN
    th_TH
    sv_SE
)


# Packages (.deb, .zip and .tar.gz) compression levels.
if(developerBuild)
    set(deb_compression_level 0)
    set(zip_compression_level 1)
    set(gzip_compression_level 1)
else()
    set(deb_compression_level 6)
    set(zip_compression_level 6)
    set(gzip_compression_level 6)
endif()

set(appleTeamId "" CACHE STRING "Custom team id for signing a developer build.")

if(MACOSX)
    # If appleTeamId is not passed via command line:

    # 1. Figure out sign identity from customization.
    if(NOT appleTeamId)
        set(identityFromCustomization "${customization.desktop.macos.signIdentity}")

        if(identityFromCustomization)
            string(REGEX REPLACE "^.*\\(([^\\)]+)\\)$" "\\1" appleTeamId "${identityFromCustomization}")
        endif()
    endif()

    # 2. For a developer build try autoselect the identity from Xcode settings - select the first
    # non-free team id. If no non-free team id is found, select the first encountered free team id.
    # This is similar to how Qt does the auto selection.
    if(NOT appleTeamId)
        if(developerBuild)
            nx_execute_process_or_fail(
                COMMAND ${PYTHON_EXECUTABLE} ${open_source_root}/build_utils/macos/team.py
                    select
                OUTPUT_VARIABLE appleTeamId
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_MESSAGE
                    "Cannot determine the developer team id automatically. "
                    "Please specify it manually via -DappleTeamId"
            )
        else()
            # For non-developer builds avoid autoselection and just fail with an informative
            # message about invalid customization file.
            set(missingProperty "customization.desktop.macos.signIdentity")
            message(FATAL_ERROR "Customization file is missing ${missingProperty} property")
        endif()
    endif()
endif()

if(appleTeamIdIsFreeString STREQUAL "True")
    set(appleTeamIdIsFree TRUE)
    message(WARNING "Push notifications capability is disabled because free team id ${appleTeamId}"
        " is used for signing")
else()
    set(appleTeamIdIsFree FALSE)
endif()
