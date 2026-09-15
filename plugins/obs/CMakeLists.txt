# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(avsync_obs_adapter LANGUAGES CXX)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "The experimental OBS adapter currently supports Linux only")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBOBS REQUIRED IMPORTED_TARGET libobs>=32.0)
# Some OBS installations export their C dialect in pkg-config, even to C++.
get_target_property(_obs_options PkgConfig::LIBOBS INTERFACE_COMPILE_OPTIONS)
if(_obs_options)
  list(FILTER _obs_options EXCLUDE REGEX "^-std=")
  set_property(TARGET PkgConfig::LIBOBS PROPERTY INTERFACE_COMPILE_OPTIONS "${_obs_options}")
endif()
find_package(Threads REQUIRED)

if(NOT TARGET avsync_ipc)
  message(FATAL_ERROR "Build from the repository root with its Linux avsync_ipc target enabled")
endif()

add_library(avsync-obs MODULE adapter.cpp)
target_compile_features(avsync-obs PRIVATE cxx_std_20)
target_link_libraries(avsync-obs PRIVATE avsync_ipc PkgConfig::LIBOBS Threads::Threads)
target_compile_options(avsync-obs PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(avsync-obs PROPERTIES PREFIX "" CXX_EXTENSIONS OFF)
# A staging install is intentional: production profile installation is a separate step.
install(TARGETS avsync-obs LIBRARY DESTINATION lib/obs-plugins)
