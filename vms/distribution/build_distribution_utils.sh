#!/bin/bash

## Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

# Utilities helpful for creating distribution packages (.deb, .dmg or archives).
#
# All global symbols have prefix "distrib_".

#--------------------------------------------------------------------------------------------------
# private

# Checks whether the specified array is set and not empty. We can't use simple `[[ -n ]]`
# because with arrays it will produce an error with `set -u` if the variable is not set or empty.
# [in] variable_name
#
checkArrayNotEmpty()
{
    local -r variable_name="$1" && shift

    eval "[[ -n \${${variable_name}:+full} ]]"
}

# [in] PLATFORM
#
distrib_librarySuffix()
{
    if [[ "${PLATFORM}" =~ ^macos_.*$ ]]; then
        echo "dylib"
    else
        echo "so"
    fi
}

# [in] distrib_EXTRA_HELP
#
distrib_showHelp()
{
    echo "Options:"
    echo " -v, --verbose: Do not redirect output to a log file."
    echo " -k, --keep-work-dir: Do not delete work directory on success."
    if [[ ! -z $distrib_EXTRA_HELP ]]
    then
        echo "$distrib_EXTRA_HELP"
    fi
}

# [out] distrib_KEEP_WORK_DIR
# [out] distrib_VERBOSE
#
distrib_parseArgs() # "$@"
{
    distrib_KEEP_WORK_DIR=0
    distrib_VERBOSE=0

    local -i ARG_N=0
    local ARG
    for ARG in "$@"
    do
        if [ "$ARG" = "-h" -o "$ARG" = "--help" ]
        then
            distrib_showHelp
            exit 0
        elif [ "$ARG" = "-k" ] || [ "$ARG" = "--keep-work-dir" ]
        then
            distrib_KEEP_WORK_DIR=1
        elif [ "$ARG" = "-v" ] || [ "$ARG" = "--verbose" ]
        then
            distrib_VERBOSE=1
        elif [[ ! -z $distrib_PARSE_EXTRA_ARGS_FUNC ]]
        then
            $distrib_PARSE_EXTRA_ARGS_FUNC "$ARG" $ARG_N "$@"
        fi

        ((++ARG_N))
    done
}

# [out] distrib_STDERR_FILE
#
distrib_redirectOutputToLog() # log_file
{
    local -r LOG_FILE="$1" && shift

    echo "  See the log in $LOG_FILE"

    mkdir -p "$(dirname "$LOG_FILE")" #< In case log dir is not created yet.
    rm -rf "$LOG_FILE"

    # Redirect only stdout to the log file, leaving stderr as is, because there seems to be no
    # reliable way to duplicate error output to both log file and stderr, keeping the correct order
    # of regular and error lines in the log file.
    exec 1>"$LOG_FILE" #< Open log file for writing as stdout.

    distrib_STDERR_FILE=$(mktemp)
    exec 2> >(tee "$distrib_STDERR_FILE" >&2) #< Redirect stderr to a temporary file.
}

# Called by trap.
# [in] $?
# [in] distrib_VERBOSE
# [in] distrib_STDERR_FILE
# [in] distrib_KEEP_WORK_DIR
# [in] distrib_WORK_DIR
#
distrib_onExit()
{
    local -r -i RESULT=$?

    set +x #< Disable logging each command (in case it was enabled previously with "set -x").

    if [ -s "$distrib_STDERR_FILE" ] #< Whether the file exists and is not empty.
    then
        echo ""
        python3 -c 'print("-" * 99)' #< Print separator.
        echo "- stderr"
        echo ""
        cat "$distrib_STDERR_FILE" #< Copy accumulated stderr to the log file.
    fi
    rm -rf "$distrib_STDERR_FILE"

    echo "" #< Empty line to separate SUCCESS/FAILURE message from the above log.
    if [ $RESULT != 0 ]
    then #< Failure.
        local -r ERROR_MESSAGE="FAILURE (status $RESULT); see the error message(s) above."
        echo "$ERROR_MESSAGE" >&2 #< To stderr.
        if [ $distrib_VERBOSE = 0 ] #< Whether stdout is redirected to the log file.
        then
            echo "$ERROR_MESSAGE" #< To log file.
        fi
    else
        echo "SUCCESS"

        if [ $distrib_KEEP_WORK_DIR = 0 ]
        then
            rm -rf "$distrib_WORK_DIR"
        fi
    fi

    return $RESULT
}

# In the specified directory and all subdirectories recursively, strips from debug symbols all .so
# and .so.<version> files, and files from additional_file_names (must be specified without paths).
distrib_stripDirectory() # directory [additional_file_names...]
{
    local -r dir="$1" && shift;
    local -a additional_file_names=("$@")

    local -a files
    readarray -d "" files < <(find "${dir}"  -type f -name "*.so*" -print0)
    for file_name in "${additional_file_names[@]}"; do
        local -a additional_files
        readarray -d "" additional_files < <(find "${dir}" -type f -name "${file_name}" -print0)
        files+=("${additional_files[@]}")
    done

    local file
    for file in "${files[@]}"; do
        if [[ -z "${file}" ]]; then
            continue
        fi

        echo "  Stripping ${file}"
        local -i strip_status=0
        "${STRIP}" "${file}" || strip_status="$?"
        if [[ "${strip_status}" != 0 ]]; then
            echo "\"strip\" failed (status ${strip_status}) for ${file}" >&2
            exit "${strip_status}"
        fi
    done
}

# Global variables - need to be accessible in onExit().
declare distrib_WORK_DIR
declare -i distrib_VERBOSE
declare -i distrib_KEEP_WORK_DIR
declare distrib_STDERR_FILE

#--------------------------------------------------------------------------------------------------
# public

# Description of additional command-line options to show with the help.
declare distrib_EXTRA_HELP=""

# Callback which is called for each command-line argument:
#     parseExtraArgs arg_value arg_number "$@"
declare distrib_PARSE_EXTRA_ARGS_FUNC=""

# [in,out] CONFIG If defined, overrides config_file, otherwise is set to config_file.
#
distrib_loadConfig() # config_file
{
    : ${CONFIG="$1"} && shift #< CONFIG can be overridden externally.
    if [ ! -z "$CONFIG" ]
    then
        source "$CONFIG"
    fi
}

# Read installed size from a DEB package.
#
# dpkg-deb will print the required info like "Installed-Size: 12345" where 12345 is approximate
# size in kylobytes.
#
# [in] DEB_FILE
#
distrib_getInstalledSizeFromDeb() # deb_file
{
    local -r DEB_FILE="$1" && shift

    local FREE_SPACE_REQUIRED=$(dpkg-deb -I "$DEB_FILE" |grep "Installed-Size:" |awk '{print $2}')
    FREE_SPACE_REQUIRED=$(($FREE_SPACE_REQUIRED * 1024))
    echo $FREE_SPACE_REQUIRED
}

# Copy system libraries using copy_system_library.py.
#
# [in] OPEN_SOURCE_DIR
# [in] COMPILER
# [in] CFLAGS
# [in] LDFLAGS
#
distrib_copySystemLibs() # dest_dir libs_to_copy...
{
    local -r DEST_DIR="$1" && shift

    "$PYTHON" "$OPEN_SOURCE_DIR"/build_utils/linux/copy_system_library.py \
        --compiler="$COMPILER" \
        --flags="$CFLAGS" \
        --link-flags="$LDFLAGS" \
        --dest-dir="$DEST_DIR" \
        "$@"
}

# Copy common set of system libs
#
# [in] CPP_RUNTIME_LIBS
# [in] OPEN_SOURCE_DIR
distrib_copyDefaultSystemLibs() # destination_path
{
    local -r destination_path="$1" && shift

    echo ""
    echo "Copying system libs"

    echo "  Copying cpp runtime libs: ${CPP_RUNTIME_LIBS[@]}"
    local -r bundled_stdcpp_lib_path="${destination_path}/stdcpp"
    mkdir -p "${bundled_stdcpp_lib_path}"
    distrib_copySystemLibs "${bundled_stdcpp_lib_path}" "${CPP_RUNTIME_LIBS[@]}"
    install -m 755 \
        "${OPEN_SOURCE_DIR}/build_utils/linux/choose_newer_stdcpp.sh" "${bundled_stdcpp_lib_path}/"

    echo "  Copying other system libs"

    local -a libs_to_copy=(
        libpng16.so.16
        libfontconfig.so.1
        libfreetype.so.6
        libxkbcommon.so.0
    )
    distrib_copySystemLibs "${destination_path}" "${libs_to_copy[@]}"
}

# Copy libraries from the specified directory using copy_system_library.py.
#
# [in] OPEN_SOURCE_DIR
# [in] LIB_DIR
#
distrib_copyLibs() # lib_dir dest_dir libs_to_copy...
{
    local -r LIB_DIR="$1" && shift
    local -r DEST_DIR="$1" && shift

    "$PYTHON" "$OPEN_SOURCE_DIR"/build_utils/linux/copy_system_library.py \
        -L "$LIB_DIR" \
        --dest-dir="$DEST_DIR" \
        "$@"
}

# Copy the specified file to the proper location, creating the symlink if necessary: if
# ALT_LIB_INSTALL_PATH is unset, or the file is a symlink, just copy it to STAGE/LIB_INSTALL_PATH;
# otherwise, put it to STAGE/ALT_LIB_INSTALL_PATH, and create a symlink from
# STAGE/LIB_INSTALL_PATH to the file basename in INSTALL_ROOT/ALT_LIB_INSTALL_PATH. If an optional
# parameter library_subdirectory is passed to the function, it adds it as a suffix to both
# LIB_INSTALL_PATH and ALT_LIB_INSTALL_PATH.
#
# [in] STAGE
# [in] INSTALL_ROOT
# [in] LIB_INSTALL_PATH
# [in] ALT_LIB_INSTALL_PATH Path to the alternative lib directory in the distribution creation
#      directory.
distrib_copyLib() # file [library_subdirectory]
{
    local -r file="$1"
    local -r lib_subdir="${2-}" #< The default value for lib_subdir is an empty string.

    # Create a temporary variable which value is the same as the value of the
    # ALT_LIB_INSTAL_PATH variable if it is set, or empty otherwise.
    local -r alt_lib_instal_path_local="${ALT_LIB_INSTALL_PATH-}"

    if [[ -z "${lib_subdir}" ]]; then
        local -r lib_dir="${LIB_INSTALL_PATH}"
        local -r alt_lib_dir="${alt_lib_instal_path_local}"
    else
        local -r lib_dir="${LIB_INSTALL_PATH}/${lib_subdir}"
        local -r alt_lib_dir="${alt_lib_instal_path_local}/${lib_subdir}"
    fi

    local -r stage_lib="${STAGE}/${lib_dir}"

    if [[ ! -z "${alt_lib_instal_path_local}" && ! -L "${file}" ]]; then
        # FILE is not a symlink - put to the alt location, and create a symlink to it.
        cp -r "${file}" "${STAGE}/${alt_lib_dir}/"
        local -r file_name=$(basename "${file}")
        local -r install_root="${INSTALL_ROOT-}"
        ln -s "${install_root}/${alt_lib_dir}/${file_name}" "${stage_lib}/${file_name}"
    else
        # FILE is a symlink, or the alt location is not defined - put to the regular location.
        cp -r "${file}" "${stage_lib}/"
    fi
}

# Copy common libs to the distribution creation folder using distrib_copyLib() to ensure the
# correct directory structure (libraries and symlinks to them in different directories if
# necessary).
#
# [in] BUILD_DIR
# [in] PLATFORM
# [in] STAGE
# [in] LIB_INSTALL_PATH
# [in] ALT_LIB_INSTALL_PATH
distrib_copyCommonLibs() # additional_libs_to_copy...
{
    local -a additional_libs=("$@")

    echo ""
    echo "Copying libs"

    local -a libs_to_copy=(
        # occurra
        liboccurra_logging
        liboccurra_media
        liboccurra_rest
        liboccurra_relay_proto
        liboccurra_sdk

        # nx
        libnx_branding
        libnx_build_info
        libnx_codec
        libnx_kit
        libnx_media_core
        libnx_network
        libnx_network_rest
        libnx_reflect
        libnx_rtp
        libnx_sql
        libnx_utils
        libnx_vms_api
        libnx_vms_common
        libnx_webrtc

        # third-party
        libquazip

        # additional libraries
        "${additional_libs[@]}"
    )

    local -r optional_libs_to_copy=(
        libvpx
    )

    # OpenSSL.
    libs_to_copy+=(
        libssl
        libcrypto
    )

    # libatomic
    libs_to_copy+=(
        libatomic
    )

    # FFmpeg links VAAPI on linux_x64, so its libraries ship beside the FFmpeg ones.
    if [[ "${PLATFORM}" == "linux_x64" ]]; then
        libs_to_copy+=(
            libva
            libva-drm
        )
    fi

    mkdir -p "${STAGE}/${LIB_INSTALL_PATH}"
    # if ALT_LIB_INSTALL_PATH is not set, consider it to be an empty string.
    if [[ ! -z "${ALT_LIB_INSTALL_PATH-}" ]]; then
        mkdir -p "${STAGE}/${ALT_LIB_INSTALL_PATH}"
    fi

    local lib
    for lib in "${libs_to_copy[@]}"; do
        local file
        for file in "${BUILD_DIR}/lib/${lib}".so*; do
            if [[ "${file}" != *.debug ]]; then
                echo "  Copying $(basename "${file}")"
                distrib_copyLib "${file}"
            fi
        done
    done
    for lib in "${optional_libs_to_copy[@]}"; do
        local file
        for file in "${BUILD_DIR}/lib/${lib}".so*; do
            if [[ -f "${file}" ]]; then
                echo "  Copying (optional) $(basename "${file}")"
                distrib_copyLib "${file}"
            fi
        done
    done
}

# Copy ffmpeg libs to the distribution creation directory.
#
# [in] STAGE
# [in] LIB_INSTALL_PATH
# [in] ALT_LIB_INSTALL_PATH
distrib_copyFfmpegLibs() # source_directory [distribution_libs_ffmpeg_subdirectory]
{
    local -r source_directory="$1"
    local -r ffmpeg_subdirectory="${2-}" #< The default value for this argument is an empty string.

    local -r ffmpeg_libs=(
        libavcodec.so.61
        libavfilter.so.10
        libavformat.so.61
        libavutil.so.59
        libswscale.so.8
        libswresample.so.5
        libavdevice.so.61
    )

    echo ""
    echo "Copying FFmpeg libs"

    if [[ ! -z "${ffmpeg_subdirectory}" ]]; then
        mkdir -p "${STAGE}/${LIB_INSTALL_PATH}/${ffmpeg_subdirectory}"
        # if ALT_LIB_INSTALL_PATH is not set, consider it to be an empty string.
        if [[ ! -z "${ALT_LIB_INSTALL_PATH-}" ]]; then
            mkdir -p "${STAGE}/${ALT_LIB_INSTALL_PATH}/${ffmpeg_subdirectory}"
        fi
    fi

    for lib in "${ffmpeg_libs[@]}"; do
        local file
        for file in "${source_directory}/${lib}"*; do
            if [ -f "${file}" ]; then
                echo "  Copying $(basename "${file}")"
                distrib_copyLib "${file}" "${ffmpeg_subdirectory}"
            fi
        done
    done
}

# Copy Qt libs to the distribution creation directory.
#
# [in] QT_DIR
distrib_copyQtLibs() # additional_libs_to_copy...
{
    local -a additional_qt_libs=("$@")

    echo ""
    echo "Copying Qt libs"

    local qt_libs_to_copy=(
        Core
        Core5Compat
        Gui
        Concurrent
        Network
        Sql

        "${additional_qt_libs[@]}"
    )

    local qt_lib
    for qt_lib in "${qt_libs_to_copy[@]}"; do
        local lib_filename="libQt6${qt_lib}.so"
        echo "  Copying (Qt) ${lib_filename}"
        local file
        for file in "${QT_DIR}/lib/${lib_filename}"*; do
            distrib_copyLib "${file}"
        done
    done
}

# Copy Qt plugins to the distribution creation directory.
#
# [in] STAGE
# [in] MODULE_INSTALL_PATH
# [in] QT_DIR
distrib_copyQtPlugins() # additional_plugins_to_copy...
{
    local -a plugins=("$@")

    echo ""
    echo "Copying Qt plugins for media server"

    local -r qt_plugins_install_dir="${STAGE}/${MODULE_INSTALL_PATH}/plugins"
    mkdir -p "${qt_plugins_install_dir}"

    local plugin
    for plugin in "${plugins[@]}"; do
        echo "  Copying (Qt plugin) ${plugin}"

        mkdir -p "${qt_plugins_install_dir}/$(dirname "${plugin}")"
        cp -r "${QT_DIR}/plugins/${plugin}" "${qt_plugins_install_dir}/${plugin}"
    done
}

# Copy an executable and its siblings into the distribution bin directory.
#
# [in] STAGE
# [in] BIN_INSTALL_PATH
# [in] BUILD_DIR
# [in] CURRENT_BUILD_DIR
# [in] INSTALL_SCRIPTS_DIR
distrib_copyBins() # exe_name additional_bins_to_copy...
{
    local -r exe_name="$1"; shift
    local -a additional_binaries=("$@")

    echo ""
    echo "Copying ${exe_name} binaries"

    local -r stage_bin="${STAGE}/${BIN_INSTALL_PATH}"
    mkdir -p "${stage_bin}"

    install -m 755 "${BUILD_DIR}/bin/${exe_name}" "${stage_bin}/"
    for bin in "${additional_binaries[@]}"; do
        echo "  Installing ${bin}..."
        install -m 755 "${bin}" "${stage_bin}/"
    done

    echo "Copying qt.conf"
    install -m 644 "${CURRENT_BUILD_DIR}/qt.conf" "${stage_bin}/"

    # Copy the scripts if the variable INSTALL_SCRIPTS_DIR is defined and not empty.
    if [[ ! -z "${INSTALL_SCRIPTS_DIR-}" ]]; then
        echo "Copying scripts"
        install -m 755 -d "${STAGE}/${INSTALL_SCRIPTS_DIR}"
        install -m 755 "${CURRENT_BUILD_DIR}/scripts"/* "${STAGE}/${INSTALL_SCRIPTS_DIR}/"
    fi
}

# Copy the media server binary, plus the translations and log viewer that must reside next to it,
# to the distribution creation directory.
#
# [in] STAGE
# [in] BIN_INSTALL_PATH
# [in] BUILD_DIR
# [in] OPEN_SOURCE_DIR
# [in] SERVER_TRANSLATION_FILES
distrib_copyServerBins() # additional_bins_to_copy...
{
    distrib_copyBins media_server "$@"

    local -r stage_bin="${STAGE}/${BIN_INSTALL_PATH}"

    echo "Copying translations"
    install -m 755 -d "${stage_bin}/translations"
    for FILE in "${SERVER_TRANSLATION_FILES[@]}"
    do
        echo "  Copying bin/translations/$FILE"
        install -m 644 "${BUILD_DIR}/bin/translations/$FILE" "${stage_bin}/translations/"
    done

    echo "Copying nx_log_viewer.html"
    install -m 755 "${OPEN_SOURCE_DIR}/nx_log_viewer.html" "${stage_bin}/"
}

# [in] BIN_INSTALL_PATH
# [in] BUILD_DIR
# [in] STAGE
distrib_copyFonts()
{
    echo "Copying fonts"

    local targetDir="${STAGE}/${BIN_INSTALL_PATH}"

    set +u
    if [[ "${RESOURCE_INSTALL_PATH}" ]]; then
        targetDir="${STAGE}/${RESOURCE_INSTALL_PATH}"
    fi

    cp -r "${BUILD_DIR}/bin/fonts" "${targetDir}"

}

distrib_createArchive() # archive dir command...
{
    local -r archive="$1"; shift
    local -r dir="$1"; shift

    rm -rf "${archive}" #< Avoid updating an existing archive.
    echo "  Creating ${archive}"
    ( cd "${dir}" && "$@" "${archive}" * ) #< Subshell prevents "cd" from changing the current dir.
    echo "  Done"
}

# TODO: Check, if we really need this function - may be it would be better to use "install" instead
# of cp everywhere so there will be no need to setting permissions in the separate function.
#
# [in] STAGE
# [in] BIN_INSTALL_PATH
# [in] INSTALL_SCRIPTS_DIR
distrib_setPermissionsInStageDir()
{
    echo ""
    echo "Setting permissions"

    find "${STAGE}" -type d -print0 | xargs -0 chmod 755
    find "${STAGE}" -type f -print0 | xargs -0 chmod 644
    chmod -R 755 "${STAGE}/${BIN_INSTALL_PATH}"

    if [[ "${INSTALL_SCRIPTS_DIR-}" ]]; then
        chmod 755 "${STAGE}/${INSTALL_SCRIPTS_DIR}"/*
    fi

    if [[ -f "${STAGE}/${LIB_INSTALL_PATH}/fallback/manage_fallback_libs.sh" ]]; then
        chmod 755 "${STAGE}/${LIB_INSTALL_PATH}/fallback/manage_fallback_libs.sh"
    fi

    if [[ -f "${STAGE}/${LIB_INSTALL_PATH}/stdcpp/choose_newer_stdcpp.sh" ]]; then
        chmod 755 "${STAGE}/${LIB_INSTALL_PATH}/stdcpp/choose_newer_stdcpp.sh"
    fi
}

# [in] STAGE
# [in] BIN_INSTALL_PATH
# [in] LIB_INSTALL_PATH
distrib_stripDistributionIfNeeded() # executables...
{
    if [[ "${STRIP_BINARIES}" != "ON" ]]; then
        return
    fi

    echo ""
    echo "Stripping distribution"

    local -a executables=("$@")
    if (( ${#executables[@]} == 0 )); then
        executables=("media_server") #< Backward-compatible default.
    fi

    distrib_stripDirectory "${STAGE}/${BIN_INSTALL_PATH}" "${executables[@]}"

    local -r lib_path="${ALT_LIB_INSTALL_PATH-"${LIB_INSTALL_PATH}"}"
    distrib_stripDirectory "${STAGE}/${lib_path}"
}

# Prepare to build the distribution archives/packages.
#
# The specified work_dir will be deleted on exit (if not prohibited by the command-line args), but
# should be created after calling this function.
#
# Redirect the output to log_file, if the verbose mode is not requested. Stderr is duplicated
# to the actual stderr, and also accumulated in a file and finally appended to the log file.
#
# Parse common command-line args, in particular:
# - Provide help if needed.
# - Suppress output redirection (verbose mode).
# - Do not delete work_dir on exit (useful for debugging).
#
# [in] distrib_EXTRA_HELP
#
distrib_prepareToBuildDistribution() # work_dir log_file "$@"
{
    distrib_WORK_DIR="$1" && shift
    local -r LOG_FILE="$1" && shift

    distrib_parseArgs "$@"

    trap distrib_onExit EXIT

    rm -rf "$distrib_WORK_DIR"

    if [ $distrib_KEEP_WORK_DIR = 1 ]
    then
        local -r WORK_DIR_NOTE="(ATTENTION: will NOT be deleted)"
    else
        local -r WORK_DIR_NOTE="(will be deleted on success)"
    fi
    echo "Creating distribution in $distrib_WORK_DIR $WORK_DIR_NOTE."

    distrib_STDERR_FILE=""
    if [ $distrib_VERBOSE = 0 ]
    then
        distrib_redirectOutputToLog "$LOG_FILE"
    fi
}

# [in] STAGE_MODULE
# [in] DISTRIBUTION_OUTPUT_DIR
#
distrib_copyMetadata() {
    echo "Copying metadata"

    local -r metadataDir="${STAGE_MODULE}/metadata"

    mkdir -p "${metadataDir}"

    # Copy these files to two places for the compatibility reasons.
    cp "${DISTRIBUTION_OUTPUT_DIR}/build_info.txt" "${STAGE_MODULE}"
    cp "${DISTRIBUTION_OUTPUT_DIR}/build_info.json" "${STAGE_MODULE}"

    cp "${DISTRIBUTION_OUTPUT_DIR}/build_info.txt" "${metadataDir}"
    cp "${DISTRIBUTION_OUTPUT_DIR}/build_info.json" "${metadataDir}"
    cp "${DISTRIBUTION_OUTPUT_DIR}/conan_refs.txt" "${metadataDir}"
    cp "${DISTRIBUTION_OUTPUT_DIR}/conan.lock" "${metadataDir}"
}
