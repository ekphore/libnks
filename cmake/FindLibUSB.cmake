# SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# One hardware backend on every platform. Works with system packages and CMake
# toolchains (including vcpkg); pkg-config provides optional search hints only.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_LIBUSB QUIET libusb-1.0)
endif()
find_path(LibUSB_INCLUDE_DIR NAMES libusb.h
    HINTS ${PC_LIBUSB_INCLUDE_DIRS} PATH_SUFFIXES libusb-1.0)
find_library(LibUSB_LIBRARY NAMES usb-1.0 libusb-1.0
    HINTS ${PC_LIBUSB_LIBRARY_DIRS})
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibUSB
    REQUIRED_VARS LibUSB_LIBRARY LibUSB_INCLUDE_DIR)
if(LibUSB_FOUND AND NOT TARGET LibUSB::LibUSB)
    add_library(LibUSB::LibUSB UNKNOWN IMPORTED)
    set_target_properties(LibUSB::LibUSB PROPERTIES
        IMPORTED_LOCATION "${LibUSB_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LibUSB_INCLUDE_DIR}")
endif()
mark_as_advanced(LibUSB_INCLUDE_DIR LibUSB_LIBRARY)
