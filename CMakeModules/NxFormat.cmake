# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# nxformat (https://github.com/fab918/nxformat) reads and writes .nx game
# archives. The core uses its header-only C++ reader; the frontends call its
# C library to convert games to .nx and to edit them. The pinned release is
# downloaded and checked here. LICHII_NXFORMAT_ROOT may instead point at an
# unpacked release, such as a local build packed by nxformat's
# tools/package_release.py.
#
# Defines nxformat::reader, nxformat::nxformat and LICHII_NXFORMAT_RUNTIME,
# the shared library to ship next to the executable. LICHII_ENABLE_NX stays ON
# only where nxformat is available; without it, .nx archives are not supported.
cmake_dependent_option(LICHII_ENABLE_NX "Support .nx game archives (nxformat)" ON "NOT YUZU_STATIC_ROOM" OFF)
set(LICHII_NXFORMAT_ROOT "" CACHE PATH "Unpacked nxformat release (empty: download the pinned one)")
if (NOT LICHII_ENABLE_NX)
    return()
endif()

set(_nxformat_version 1.0.0)
set(_nxformat_tag V1.0.0)
set(_nxformat_root "${LICHII_NXFORMAT_ROOT}")
if (NOT _nxformat_root)
    if (ANDROID)
        set(_nxformat_platform android-arm64)
        set(_nxformat_sha256 cdd0584f153549b873767543e2eb2460c6d87ebe6bd9db97414e92b23a4edb95)
    elseif (WIN32 AND ARCHITECTURE_x86_64)
        set(_nxformat_platform windows-x64)
        set(_nxformat_sha256 04da098da72d302816c1d8a39da926f1c5310505f9eda17fb557be44042cb173)
    elseif (APPLE)
        set(_nxformat_platform macos-universal)
        set(_nxformat_sha256 efe81f6a28b285af11be4ea35b7097d94134f9a81c604c3fc7764876575aa458)
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ARCHITECTURE_x86_64)
        set(_nxformat_platform linux-x64)
        set(_nxformat_sha256 201481663a54f1d38f4a2dc4b1bf1084d995a899ff4237a78506646b1bd144e1)
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ARCHITECTURE_arm64)
        set(_nxformat_platform linux-arm64)
        set(_nxformat_sha256 c48952e4f3bcd92fd43e0317c3a534085c4be8bb32a8e27ed7880649549dd66c)
    else()
        message(WARNING "No nxformat build for this platform: .nx support disabled "
                        "(set LICHII_NXFORMAT_ROOT to enable it)")
        # Only for this configure, so setting LICHII_NXFORMAT_ROOT later still works.
        set(LICHII_ENABLE_NX OFF)
        return()
    endif()
    if (WIN32)
        set(_nxformat_ext zip)
    else()
        set(_nxformat_ext tar.gz)
    endif()
    set(_nxformat_name "nxformat-${_nxformat_version}-${_nxformat_platform}")
    set(_nxformat_archive "${CMAKE_BINARY_DIR}/externals/${_nxformat_name}.${_nxformat_ext}")
    set(_nxformat_root "${CMAKE_BINARY_DIR}/externals/${_nxformat_name}")
    if (NOT EXISTS "${_nxformat_root}/lib/cmake/nxformat/nxformatConfig.cmake")
        file(DOWNLOAD
            "https://github.com/fab918/nxformat/releases/download/${_nxformat_tag}/${_nxformat_name}.${_nxformat_ext}"
            "${_nxformat_archive}"
            EXPECTED_HASH SHA256=${_nxformat_sha256}
            TLS_VERIFY ON
            STATUS _nxformat_status)
        list(GET _nxformat_status 0 _nxformat_code)
        if (NOT _nxformat_code EQUAL 0)
            list(GET _nxformat_status 1 _nxformat_message)
            message(FATAL_ERROR "Could not download ${_nxformat_name}: ${_nxformat_message}")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${_nxformat_archive}" DESTINATION "${_nxformat_root}")
    endif()
endif()

find_package(nxformat ${_nxformat_version} REQUIRED CONFIG
    PATHS "${_nxformat_root}/lib/cmake/nxformat" NO_DEFAULT_PATH)
foreach (_nxformat_target nxformat::nxformat nxformat::reader)
    if (TARGET ${_nxformat_target})
        set_target_properties(${_nxformat_target} PROPERTIES IMPORTED_GLOBAL TRUE)
    endif()
endforeach()
set(LICHII_NXFORMAT_RUNTIME "${NXFORMAT_RUNTIME}" CACHE INTERNAL "")
message(STATUS "nxformat ${nxformat_VERSION}: ${_nxformat_root}")

foreach (_nxformat_var version tag root platform sha256 ext name archive status code message target)
    unset(_nxformat_${_nxformat_var})
endforeach()
