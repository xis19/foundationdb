# Distributed under the OSI-approved Apache 2.0. See the LICENSE file in
# FoundationDB source code

#[=======================================================================[.rst:
Findtoml11
-------

Find toml11, a feature-rich TOML language library for C++11/14/17/20.

toml11_ROOT variable can be used for HINTS for different version of toml11.

Result variables
^^^^^^^^^^^^^^^^

This module will set the following variables in your project:

``toml11_INCLUDE_DIRS``
  where to find toml11.h, etc.
``toml11_FOUND``
  If false, do not try to use toml11.
``toml11_VERSION``
  the version of the toml11 library found
#]=======================================================================]

include(FindPackageHandleStandardArgs)
include(FindPackageMessage)

macro(_finalize_find_package_toml11)
  find_package_handle_standard_args(
    toml11
    FOUND_VAR toml11_FOUND
    REQUIRED_VARS toml11_INCLUDE_DIRS)
  mark_as_advanced(toml11_INCLUDE_DIRS toml11_FOUND)
endmacro()

if(NOT toml11_ROOT)
  set(toml11_ROOT $ENV{toml11_ROOT})
endif()

find_path(
  toml11_INCLUDE_DIRS
  NAMES toml.hpp
  PATH_SUFFIXES include include/toml11
  HINTS ${toml11_ROOT})

_finalize_find_package_toml11()
