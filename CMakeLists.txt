# SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20)
project(libnks VERSION 0.1.0 LANGUAGES CXX)

# Headers are co-located with implementation; export the same namespaced SDK
# layout for build-tree and installed consumers.
set(LIBNKS_PUBLIC_HEADERS
    src/nks.h
    src/libnks.h
    src/daemon.h
    src/session.h
    src/wire.h
    src/transport.h
    src/events.h
    src/daw.h
    src/assets.h
    src/client.h
    src/midi.h
)
foreach(header IN LISTS LIBNKS_PUBLIC_HEADERS)
    get_filename_component(name "${header}" NAME)
    configure_file("${header}" "${CMAKE_CURRENT_BINARY_DIR}/public/libnks/${name}" COPYONLY)
endforeach()

include(GNUInstallDirs)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/libnks)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(LibUSB REQUIRED)
option(LIBNKS_BUILD_EXAMPLES "Build libnks integration examples" ON)
option(LIBNKS_BUILD_DAEMON "Build the independent nksd hardware daemon" ON)

add_library(libnks src/nks.cpp src/wire.cpp src/events.cpp src/transport.cpp
    src/usb.cpp src/session.cpp src/assets.cpp src/daw.cpp src/midi.cpp
    src/daemon.cpp src/daemon_bridge.cpp src/client.cpp)
add_library(libnks::libnks ALIAS libnks)
target_compile_features(libnks PUBLIC cxx_std_20)
target_link_libraries(libnks PUBLIC Threads::Threads)
target_link_libraries(libnks PRIVATE LibUSB::LibUSB)
if(WIN32)
    target_link_libraries(libnks PRIVATE ws2_32 winmm)
elseif(APPLE)
    target_link_libraries(libnks PRIVATE "-framework CoreMIDI" "-framework CoreFoundation" "-framework IOKit")
    target_sources(libnks PRIVATE src/usb_darwin.cpp src/service_darwin.cpp)
endif()
target_include_directories(libnks PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/public>
    $<INSTALL_INTERFACE:include>)
if(MSVC)
    target_compile_options(libnks PRIVATE /W4 /permissive-)
else()
    target_compile_options(libnks PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(LIBNKS_BUILD_DAEMON)
    add_executable(nksd src/nksd_main.cpp)
    target_link_libraries(nksd PRIVATE libnks::libnks)
    install(TARGETS nksd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(LIBNKS_BUILD_EXAMPLES)
    add_executable(libnks-example-sampler examples/sampler.cpp)
    target_link_libraries(libnks-example-sampler PRIVATE libnks::libnks)
    add_executable(libnks-example-plugin examples/plugin.cpp)
    target_link_libraries(libnks-example-plugin PRIVATE libnks::libnks)
    add_executable(libnks-example-daw examples/daw.cpp)
    target_link_libraries(libnks-example-daw PRIVATE libnks::libnks)
endif()

install(TARGETS libnks EXPORT libnksTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${LIBNKS_PUBLIC_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libnks)
install(EXPORT libnksTargets FILE libnksTargets.cmake NAMESPACE libnks::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libnks)
configure_package_config_file(
    cmake/libnksConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/libnksConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libnks)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/libnksConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    cmake/FindLibUSB.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/libnksConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/libnksConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libnks)
