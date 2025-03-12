# Copyright (c) Microsoft Corporation.
#
# MIT License
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED *AS IS*, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

# .rst FindDotnet
# ----------
#
# Find DotNet executable, and initialize functions for adding dotnet projects.
#
# Results are reported in the following variables::
#
# DOTNET_FOUND          - True if dotnet executable is found DOTNET_EXE - Dotnet
# executable DOTNET_VERSION        - Dotnet version as reported by dotnet
# executable NUGET_EXE             - Nuget executable (WIN32 only)
# NUGET_CACHE_PATH      - Nuget package cache path
#
# The following functions are defined to add dotnet/msbuild projects:
#
# ADD_DOTNET -- add a project to be built by dotnet.
#
# ~~~
# ADD_DOTNET(<project_file> [RELEASE|DEBUG] [X86|X64|ANYCPU] [NETCOREAPP]
#            [CONFIG configuration]
#            [PLATFORM platform]
#            [PACKAGE output_nuget_packages... ]
#            [VERSION nuget_package_version]
#            [DEPENDS depend_nuget_packages... ]
#            [OUTPUT_PATH output_path relative to cmake binary output dir]
#            [CUSTOM_BUILDPROPS <CustomProp>value</CustomProp>....]
#            [SOURCES additional_file_dependencies... ]
#            [ARGUMENTS additional_build_args...]
#            [PACK_ARGUMENTS additional_pack_args...])
# ~~~
#
# RUN_DOTNET -- Run a project with `dotnet run`. The `OUTPUT` argument
# represents artifacts produced by running the .NET program, and can be consumed
# from other build steps.
#
# ~~~
# RUN_DOTNET(<project_file> [RELEASE|DEBUG] [X86|X64|ANYCPU] [NETCOREAPP]
#            [ARGUMENTS program_args...]
#            [OUTPUT outputs...]
#            [CONFIG configuration]
#            [PLATFORM platform]
#            [DEPENDS depend_nuget_packages... ]
#            [OUTPUT_PATH output_path relative to cmake binary output dir]
#            [CUSTOM_BUILDPROPS <CustomProp>value</CustomProp>....]
#            [SOURCES additional_file_dependencies... ])
# ~~~
#
# ADD_MSBUILD -- add a project to be built by msbuild. Windows-only. When
# building in Unix systems, msbuild targets are skipped.
#
# ~~~
# ADD_MSBUILD(<project_file> [RELEASE|DEBUG] [X86|X64|ANYCPU] [NETCOREAPP]
#            [CONFIG configuration]
#            [PLATFORM platform]
#            [PACKAGE output_nuget_packages... ]
#            [DEPENDS depend_nuget_packages... ]
#            [CUSTOM_BUILDPROPS <CustomProp>value</CustomProp>....]
#            [SOURCES additional_file_dependencies... ]
#            [ARGUMENTS additional_build_args...]
#            [PACK_ARGUMENTS additional_pack_args...])
# ~~~
#
# SMOKETEST_DOTNET -- add a dotnet smoke test project to the build. The project
# will be run during a build, and if the program fails to build or run, the
# build fails. Currently only .NET Core App framework is supported. Multiple
# smoke tests will be run one-by-one to avoid global resource conflicts.
#
# SMOKETEST_DOTNET(<project_file> [RELEASE|DEBUG] [X86|X64|ANYCPU] [NETCOREAPP]
# [ARGUMENTS program_args...] [CONFIG configuration] [PLATFORM platform]
# [DEPENDS depend_nuget_packages... ] [OUTPUT_PATH output_path relative to cmake
# binary output dir] [CUSTOM_BUILDPROPS <CustomProp>value</CustomProp>....]
# [SOURCES additional_file_dependencies... ])
#
# For all the above functions, `RELEASE|DEBUG` overrides `CONFIG`,
# `X86|X64|ANYCPU` overrides PLATFORM. For Unix systems, the target framework
# defaults to `netstandard2.0`, unless `NETCOREAPP` is specified. For Windows,
# the project is built as-is, allowing multi-targeting.
#
# DOTNET_REGISTER_LOCAL_REPOSITORY -- register a local NuGet package repository.
#
# ~~~
# DOTNET_REGISTER_LOCAL_REPOSITORY(repo_name repo_path)
# ~~~
#
# TEST_DOTNET -- add a dotnet test project to ctest. The project will be run
# with `dotnet test`, and trx test reports will be generated in the build
# directory. For Windows, all target frameworks are tested against. For other
# platforms, only .NET Core App is tested against. Test failures will not fail
# the build. Tests are only run with `ctest -C <config>`, not with `cmake
# --build ...`
#
# ~~~
# TEST_DOTNET(<project_file>
#             [ARGUMENTS additional_dotnet_test_args...]
#             [OUTPUT_PATH output_path relative to cmake binary output dir])
# ~~~
#
# GEN_DOTNET_PROPS -- Generates a Directory.Build.props file. The created file
# is populated with MSBuild properties: - DOTNET_PACKAGE_VERSION: a version
# string that can be referenced in the actual project file as
# $(DOTNET_PACKAGE_VERSION). The version string value can be set with
# PACKAGE_VERSION argument, and defaults to '1.0.0'. - XPLAT_LIB_DIR: points to
# the cmake build root directory. - OutputPath: Points to the cmake binary
# directory (overridden by OUTPUT_PATH, relatively). Therefore, projects built
# without cmake will consistently output to the cmake build directory. - Custom
# properties can be injected with XML_INJECT argument, which injects an
# arbitrary string into the project XML file.
#
# ~~~
# GEN_DOTNET_PROPS(<target_props_file>
#                  [PACKAGE_VERSION version]
#                  [XML_INJECT xml_injection])
# ~~~
#
# Require 3.5 for batch copy multiple files

cmake_minimum_required(VERSION 3.10.0)

if(DOTNET_FOUND)
  return()
endif()

set(NUGET_CACHE_PATH "~/.nuget/packages")
find_program(DOTNET_EXE dotnet)
set(DOTNET_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR} PARENT_SCOPE)

if(NOT DOTNET_EXE)
  set(DOTNET_FOUND FALSE)
  if(Dotnet_FIND_REQUIRED)
    message(SEND_ERROR "Command 'dotnet' is not found.")
  endif()
  return()
endif()

execute_process(
  COMMAND ${DOTNET_EXE} --version
  OUTPUT_VARIABLE DOTNET_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE)

if(WIN32)
  find_program(NUGET_EXE nuget PATHS ${CMAKE_BINARY_DIR}/tools)
  if(NUGET_EXE)
    message("-- Found nuget: ${NUGET_EXE}")
  else()
    set(NUGET_EXE ${CMAKE_BINARY_DIR}/tools/nuget.exe)
    message("-- Downloading nuget...")
    file(DOWNLOAD https://dist.nuget.org/win-x86-commandline/latest/nuget.exe
         ${NUGET_EXE})
    message("nuget.exe downloaded and saved to ${NUGET_EXE}")
  endif()
endif()

function(DOTNET_REGISTER_LOCAL_REPOSITORY repo_name repo_path)
  message(
    "-- Registering NuGet local repository '${repo_name}' at '${repo_path}'.")
  get_filename_component(repo_path ${repo_path} ABSOLUTE)
  if(WIN32)
    string(REPLACE "/" "\\" repo_path ${repo_path})
    execute_process(COMMAND ${NUGET_EXE} sources list OUTPUT_QUIET)
    execute_process(COMMAND ${NUGET_EXE} sources Remove -Name "${repo_name}"
                    OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND ${NUGET_EXE} sources Add -Name "${repo_name}"
                            -Source "${repo_path}")
  else()
    get_filename_component(nuget_config ~/.nuget/NuGet/NuGet.Config ABSOLUTE)
    execute_process(COMMAND ${DOTNET_EXE} nuget locals all --list OUTPUT_QUIET)
    execute_process(COMMAND sed -i "/${repo_name}/d" "${nuget_config}")
    execute_process(
      COMMAND
        sed -i
        "s#</packageSources>#  <add key=\\\"${repo_name}\\\" value=\\\"${repo_path}\\\" />\\n  </packageSources>#g"
        "${nuget_config}")
  endif()
endfunction()

function(DOTNET_GET_DEPS _DN_PROJECT arguments)
  cmake_parse_arguments(
    # prefix
    _DN
    # options (flags)
    "RELEASE;DEBUG;X86;X64;ANYCPU;NETCOREAPP"
    # oneValueArgs
    "CONFIG;PLATFORM;VERSION;OUTPUT_PATH"
    # multiValueArgs
    "PACKAGE;DEPENDS;ARGUMENTS;PACK_ARGUMENTS;OUTPUT;SOURCES;CUSTOM_BUILDPROPS"
    # the input arguments
    ${arguments})

  get_filename_component(_DN_abs_proj "${_DN_PROJECT}" ABSOLUTE)
  get_filename_component(_DN_proj_dir "${_DN_abs_proj}" DIRECTORY)
  get_filename_component(_DN_projname "${_DN_PROJECT}" NAME)
  string(REGEX REPLACE "\\.[^.]*$" "" _DN_projname_noext ${_DN_projname})

  file(
    GLOB_RECURSE
    DOTNET_deps
    ${_DN_proj_dir}/*.cs
    ${_DN_proj_dir}/*.fs
    ${_DN_proj_dir}/*.vb
    ${_DN_proj_dir}/*.xaml
    ${_DN_proj_dir}/*.resx
    ${_DN_proj_dir}/*.xml
    ${_DN_proj_dir}/*.*proj
    ${_DN_proj_dir}/*.cs
    ${_DN_proj_dir}/*.config)
  list(APPEND DOTNET_deps ${_DN_SOURCES})
  set(_DN_deps "")
  foreach(dep ${DOTNET_deps})
    if(NOT dep MATCHES /obj/ AND NOT dep MATCHES /bin/)
      list(APPEND _DN_deps ${dep})
    endif()
  endforeach()

  if(_DN_RELEASE)
    set(_DN_CONFIG Release)
  elseif(_DN_DEBUG)
    set(_DN_CONFIG Debug)
  endif()

  if(NOT _DN_CONFIG)
    set(_DN_CONFIG "$<$<CONFIG:Debug>:Debug>$<$<NOT:$<CONFIG:Debug>>:Release>")
  endif()

  # If platform is not specified, do not pass the Platform property. dotnet will
  # pick the default Platform.

  if(_DN_X86)
    set(_DN_PLATFORM x86)
  elseif(_DN_X64)
    set(_DN_PLATFORM x64)
  elseif(_DN_ANYCPU)
    set(_DN_PLATFORM "AnyCPU")
  endif()

  # If package version is not set, first fallback to DOTNET_PACKAGE_VERSION If
  # again not set, defaults to 1.0.0
  if(NOT _DN_VERSION)
    set(_DN_VERSION ${DOTNET_PACKAGE_VERSION})
  endif()
  if(NOT _DN_VERSION)
    set(_DN_VERSION "1.0.0")
  endif()

  # Set the output path to the binary directory. Build outputs in separated
  # output directories prevent overwriting. Later we then copy the outputs to
  # the destination.

  if(NOT _DN_OUTPUT_PATH)
    set(_DN_OUTPUT_PATH ${_DN_projname_noext})
  endif()

  get_filename_component(_DN_OUTPUT_PATH ${CMAKE_BINARY_DIR}/${_DN_OUTPUT_PATH}
                         ABSOLUTE)

  # In a cmake build, the XPLAT libraries are always copied over. Set the proper
  # directory for .NET projects.
  set(_DN_XPLAT_LIB_DIR ${CMAKE_BINARY_DIR})

  set(DOTNET_PACKAGES
      ${_DN_PACKAGE}
      PARENT_SCOPE)
  set(DOTNET_CONFIG
      ${_DN_CONFIG}
      PARENT_SCOPE)
  set(DOTNET_PLATFORM
      ${_DN_PLATFORM}
      PARENT_SCOPE)
  set(DOTNET_DEPENDS
      ${_DN_DEPENDS}
      PARENT_SCOPE)
  set(DOTNET_PROJNAME
      ${_DN_projname_noext}
      PARENT_SCOPE)
  set(DOTNET_PROJPATH
      ${_DN_abs_proj}
      PARENT_SCOPE)
  set(DOTNET_PROJDIR
      ${_DN_proj_dir}
      PARENT_SCOPE)
  set(DOTNET_ARGUMENTS
      ${_DN_ARGUMENTS}
      PARENT_SCOPE)
  set(DOTNET_RUN_OUTPUT
      ${_DN_OUTPUT}
      PARENT_SCOPE)
  set(DOTNET_PACKAGE_VERSION
      ${_DN_VERSION}
      PARENT_SCOPE)
  set(DOTNET_OUTPUT_PATH
      ${_DN_OUTPUT_PATH}
      PARENT_SCOPE)
  set(DOTNET_deps
      ${_DN_deps}
      PARENT_SCOPE)

  if(_DN_PLATFORM)
    set(_DN_PLATFORM_PROP "/p:Platform=${_DN_PLATFORM}")
  endif()

  if(_DN_NETCOREAPP)
    set(_DN_BUILD_OPTIONS -f net8.0)
    set(_DN_PACK_OPTIONS /p:TargetFrameworks=net8.0)
  elseif(UNIX)
    # Unix builds default to netstandard2.0
    set(_DN_BUILD_OPTIONS -f netstandard2.0)
    set(_DN_PACK_OPTIONS /p:TargetFrameworks=netstandard2.0)
  endif()

  set(_DN_IMPORT_PROP ${CMAKE_CURRENT_BINARY_DIR}/${_DN_projname}.imports.props)
  configure_file(${DOTNET_MODULE_DIR}/DotnetImports.props.in ${_DN_IMPORT_PROP})
  set(_DN_IMPORT_ARGS "/p:DirectoryBuildPropsPath=${_DN_IMPORT_PROP}")

  set(DOTNET_IMPORT_PROPERTIES
      ${_DN_IMPORT_ARGS}
      PARENT_SCOPE)
  set(DOTNET_BUILD_PROPERTIES
      ${_DN_PLATFORM_PROP} ${_DN_IMPORT_ARGS}
      PARENT_SCOPE)
  set(DOTNET_BUILD_OPTIONS
      ${_DN_BUILD_OPTIONS}
      PARENT_SCOPE)
  set(DOTNET_PACK_OPTIONS
      --include-symbols ${_DN_PACK_OPTIONS} ${_DN_PACK_ARGUMENTS}
      PARENT_SCOPE)

endfunction()

macro(ADD_DOTNET_DEPENDENCY_TARGETS tgt)
  foreach(pkg_dep ${DOTNET_DEPENDS})
    add_dependencies(${tgt}_${DOTNET_PROJNAME} PKG_${pkg_dep})
    message("     ${DOTNET_PROJNAME} <- ${pkg_dep}")
  endforeach()

  foreach(pkg ${DOTNET_PACKAGES})
    string(TOLOWER ${pkg} pkg_lowercase)
    get_filename_component(cache_path ${NUGET_CACHE_PATH}/${pkg_lowercase}
                           ABSOLUTE)
    if(WIN32)
      set(rm_command
          powershell -NoLogo -NoProfile -NonInteractive -Command
          "Remove-Item -Recurse -Force -ErrorAction Ignore '${cache_path}'\; exit 0"
      )
    else()
      set(rm_command rm -rf ${cache_path})
    endif()
    add_custom_target(
      DOTNET_PURGE_${pkg}
      COMMAND ${CMAKE_COMMAND} -E echo
              "======= [x] Purging nuget package cache for ${pkg}"
      COMMAND ${rm_command}
      DEPENDS ${DOTNET_deps})
    add_dependencies(${tgt}_${DOTNET_PROJNAME} DOTNET_PURGE_${pkg})
    # Add a target for the built package -- this can be referenced in another
    # project.
    add_custom_target(PKG_${pkg})
    add_dependencies(PKG_${pkg} ${tgt}_${DOTNET_PROJNAME})
    message("==== ${DOTNET_PROJNAME} -> ${pkg}")
  endforeach()
endmacro()

macro(DOTNET_BUILD_COMMANDS)
  if(${DOTNET_IS_MSBUILD})
    set(build_dotnet_cmds
        COMMAND
        ${CMAKE_COMMAND}
        -E
        echo
        "======= Building msbuild project ${DOTNET_PROJNAME} [${DOTNET_CONFIG} ${DOTNET_PLATFORM}]"
        COMMAND
        ${NUGET_EXE}
        restore
        -Force
        ${DOTNET_PROJPATH}
        COMMAND
        ${DOTNET_EXE}
        msbuild
        ${DOTNET_PROJPATH}
        /t:Clean
        ${DOTNET_BUILD_PROPERTIES}
        /p:Configuration="${DOTNET_CONFIG}"
        COMMAND
        ${DOTNET_EXE}
        msbuild
        ${DOTNET_PROJPATH}
        /t:Build
        ${DOTNET_BUILD_PROPERTIES}
        /p:Configuration="${DOTNET_CONFIG}"
        ${DOTNET_ARGUMENTS})
    set(build_dotnet_type "msbuild")
  else()
    set(build_dotnet_cmds
        COMMAND
        ${CMAKE_COMMAND}
        -E
        echo
        "======= Building .NET project ${DOTNET_PROJNAME} [${DOTNET_CONFIG} ${DOTNET_PLATFORM}]"
        COMMAND
        ${DOTNET_EXE}
        restore
        ${DOTNET_PROJPATH}
        ${DOTNET_IMPORT_PROPERTIES}
        COMMAND
        ${DOTNET_EXE}
        clean
        ${DOTNET_PROJPATH}
        ${DOTNET_BUILD_PROPERTIES}
        COMMAND
        ${DOTNET_EXE}
        build
        --no-restore
        ${DOTNET_PROJPATH}
        -c
        ${DOTNET_CONFIG}
        ${DOTNET_BUILD_PROPERTIES}
        ${DOTNET_BUILD_OPTIONS}
        ${DOTNET_ARGUMENTS})
    set(build_dotnet_type "dotnet")
  endif()

  # DOTNET_OUTPUTS refer to artifacts produced, that the BUILD_proj_name target
  # depends on.
  set(DOTNET_OUTPUTS "")
  if(NOT "${DOTNET_PACKAGES}" STREQUAL "")
    message(
      "-- Adding ${build_dotnet_type} project ${DOTNET_PROJPATH} (version ${DOTNET_PACKAGE_VERSION})"
    )
    foreach(pkg ${DOTNET_PACKAGES})
      list(APPEND DOTNET_OUTPUTS
           ${DOTNET_OUTPUT_PATH}/${pkg}.${DOTNET_PACKAGE_VERSION}.nupkg)
      list(APPEND DOTNET_OUTPUTS
           ${DOTNET_OUTPUT_PATH}/${pkg}.${DOTNET_PACKAGE_VERSION}.symbols.nupkg)
      list(
        APPEND
        build_dotnet_cmds
        COMMAND
        ${CMAKE_COMMAND}
        -E
        remove
        ${DOTNET_OUTPUT_PATH}/${pkg}.${DOTNET_PACKAGE_VERSION}.nupkg)
      list(
        APPEND
        build_dotnet_cmds
        COMMAND
        ${CMAKE_COMMAND}
        -E
        remove
        ${DOTNET_OUTPUT_PATH}/${pkg}.${DOTNET_PACKAGE_VERSION}.symbols.nupkg)
    endforeach()
    list(
      APPEND
      build_dotnet_cmds
      COMMAND
      ${DOTNET_EXE}
      pack
      --no-build
      --no-restore
      ${DOTNET_PROJPATH}
      -c
      ${DOTNET_CONFIG}
      ${DOTNET_BUILD_PROPERTIES}
      ${DOTNET_PACK_OPTIONS})
    list(
      APPEND
      build_dotnet_cmds
      COMMAND
      ${CMAKE_COMMAND}
      -E
      copy
      ${DOTNET_OUTPUTS}
      ${CMAKE_BINARY_DIR})
  else()
    message(
      "-- Adding ${build_dotnet_type} project ${DOTNET_PROJPATH} (no nupkg)")
  endif()
  list(APPEND DOTNET_OUTPUTS
       ${CMAKE_CURRENT_BINARY_DIR}/${DOTNET_PROJNAME}.buildtimestamp)
  list(
    APPEND
    build_dotnet_cmds
    COMMAND
    ${CMAKE_COMMAND}
    -E
    touch
    ${CMAKE_CURRENT_BINARY_DIR}/${DOTNET_PROJNAME}.buildtimestamp)

  add_custom_command(OUTPUT ${DOTNET_OUTPUTS} DEPENDS ${DOTNET_deps}
                                                      ${build_dotnet_cmds})
  add_custom_target(BUILD_${DOTNET_PROJNAME} ALL DEPENDS ${DOTNET_OUTPUTS})

endmacro()

function(ADD_DOTNET DOTNET_PROJECT)
  dotnet_get_deps(${DOTNET_PROJECT} "${ARGN}")
  set(DOTNET_IS_MSBUILD FALSE)
  dotnet_build_commands()
  add_dotnet_dependency_targets(BUILD)
endfunction()

function(ADD_MSBUILD DOTNET_PROJECT)
  if(NOT WIN32)
    message("-- Building non-Win32, skipping ${DOTNET_PROJECT}")
    return()
  endif()

  dotnet_get_deps(${DOTNET_PROJECT} "${ARGN}")
  set(DOTNET_IS_MSBUILD TRUE)
  dotnet_build_commands()
  add_dotnet_dependency_targets(BUILD)
endfunction()

function(RUN_DOTNET DOTNET_PROJECT)
  dotnet_get_deps(${DOTNET_PROJECT} "${ARGN};NETCOREAPP")
  message("-- Adding dotnet run project ${DOTNET_PROJECT}")
  file(MAKE_DIRECTORY ${DOTNET_OUTPUT_PATH})
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${DOTNET_PROJNAME}.runtimestamp
           ${DOTNET_RUN_OUTPUT}
    DEPENDS ${DOTNET_deps}
    COMMAND ${DOTNET_EXE} restore ${DOTNET_PROJPATH} ${DOTNET_IMPORT_PROPERTIES}
    COMMAND ${DOTNET_EXE} clean ${DOTNET_PROJPATH} ${DOTNET_BUILD_PROPERTIES}
    COMMAND ${DOTNET_EXE} build --no-restore ${DOTNET_PROJPATH} -c
            ${DOTNET_CONFIG} ${DOTNET_BUILD_PROPERTIES} ${DOTNET_BUILD_OPTIONS}
    # XXX tfm
    COMMAND ${DOTNET_EXE} ${DOTNET_OUTPUT_PATH}/net8.0/${DOTNET_PROJNAME}.dll
            ${DOTNET_ARGUMENTS}
    COMMAND ${CMAKE_COMMAND} -E touch
            ${CMAKE_CURRENT_BINARY_DIR}/${DOTNET_PROJNAME}.runtimestamp
    WORKING_DIRECTORY ${DOTNET_OUTPUT_PATH})
  add_custom_target(
    RUN_${DOTNET_PROJNAME}
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${DOTNET_PROJNAME}.runtimestamp
            ${DOTNET_RUN_OUTPUT})
  add_dotnet_dependency_targets(RUN)
endfunction()

function(TEST_DOTNET DOTNET_PROJECT)
  dotnet_get_deps(${DOTNET_PROJECT} "${ARGN}")
  message("-- Adding dotnet test project ${DOTNET_PROJECT}")
  if(WIN32)
    set(test_framework_args "")
  else()
    set(test_framework_args -f net8.0)
  endif()

  add_test(
    NAME ${DOTNET_PROJNAME}
    COMMAND ${DOTNET_EXE} test ${test_framework_args} --results-directory
            "${CMAKE_BINARY_DIR}" --logger trx ${DOTNET_ARGUMENTS}
    WORKING_DIRECTORY ${DOTNET_OUTPUT_PATH})

endfunction()

set_property(GLOBAL PROPERTY DOTNET_LAST_SMOKETEST "")

function(SMOKETEST_DOTNET DOTNET_PROJECT)
  message("-- Adding dotnet smoke test project ${DOTNET_PROJECT}")
  if(WIN32)
    run_dotnet(${DOTNET_PROJECT} "${ARGN}")
  else()
    run_dotnet(${DOTNET_PROJECT} "${ARGN}")
  endif()

  dotnet_get_deps(${DOTNET_PROJECT} "${ARGN}")
  add_custom_target(
    SMOKETEST_${DOTNET_PROJNAME} ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${DOTNET_PROJNAME}.runtimestamp)
  add_dotnet_dependency_targets(SMOKETEST)
  get_property(_dn_last_smoketest GLOBAL PROPERTY DOTNET_LAST_SMOKETEST)
  if(_dn_last_smoketest)
    message("${_dn_last_smoketest} -> SMOKETEST_${DOTNET_PROJNAME}")
    add_dependencies(SMOKETEST_${DOTNET_PROJNAME} ${_dn_last_smoketest})
  endif()
  # Chain the smoke tests together so they are executed sequentially
  set_property(GLOBAL PROPERTY DOTNET_LAST_SMOKETEST
                               SMOKETEST_${DOTNET_PROJNAME})

endfunction()

set(DOTNET_IMPORTS_TEMPLATE ${CMAKE_CURRENT_LIST_DIR}/DotnetImports.props.in)

function(GEN_DOTNET_PROPS target_props_file)
  cmake_parse_arguments(
    # prefix
    _DNP
    # options (flags)
    ""
    # oneValueArgs
    "PACKAGE_VERSION;XML_INJECT"
    # multiValueArgs
    ""
    # the input arguments
    ${ARGN})

  if(NOT _DNP_PACKAGE_VERSION)
    set(_DNP_PACKAGE_VERSION 1.0.0)
  endif()

  if(_DNP_XML_INJECT)
    set(_DN_CUSTOM_BUILDPROPS ${_DNP_XML_INJECT})
  endif()

  set(_DN_OUTPUT_PATH ${CMAKE_BINARY_DIR})
  set(_DN_XPLAT_LIB_DIR ${CMAKE_BINARY_DIR})
  set(_DN_VERSION ${_DNP_PACKAGE_VERSION})
  configure_file(${DOTNET_IMPORTS_TEMPLATE} ${target_props_file})
  unset(_DN_OUTPUT_PATH)
  unset(_DN_XPLAT_LIB_DIR)
  unset(_DN_VERSION)
endfunction()

message("-- Found .NET toolchain: ${DOTNET_EXE} (version ${DOTNET_VERSION})")
set(DOTNET_FOUND TRUE)
