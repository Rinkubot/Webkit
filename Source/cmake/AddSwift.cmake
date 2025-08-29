# This source file is part of the Swift open source project
#
# Copyright (c) 2023 Apple Inc. and the Swift project authors.
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
#
# This file originates from https://github.com/swiftlang/swift-cmake-examples

# Generate the bridging header from Swift to C++
#
# target: the name of the target to generate headers for.
#         This target must build swift source files.
# header: the name of the header file to generate.
#
# NOTE: This logic will eventually be unstreamed into CMake.
function(_swift_generate_cxx_header target header)
  if (NOT TARGET ${target})
    message(FATAL_ERROR "Target ${target} not defined.")
  endif ()

  if (NOT DEFINED CMAKE_Swift_COMPILER)
    message(WARNING "Swift not enabled in project. Cannot generate headers for Swift files.")
    return()
  endif ()

  cmake_parse_arguments(ARG "" "" "SEARCH_PATHS;MODULE_NAME" ${ARGN})

  if (NOT ARG_MODULE_NAME)
    set(target_module_name $<TARGET_PROPERTY:${target},Swift_MODULE_NAME>)
    set(ARG_MODULE_NAME $<IF:$<BOOL:${target_module_name}>,${target_module_name},${target}>)
  endif ()

  if (ARG_SEARCH_PATHS)
    list(TRANSFORM ARG_SEARCH_PATHS PREPEND "-I")
  endif ()

  if (APPLE AND CMAKE_OSX_SYSROOT)
    set(SDK_FLAGS "-sdk" "${CMAKE_OSX_SYSROOT}")
  elseif (WIN32)
    set(SDK_FLAGS "-sdk" "$ENV{SDKROOT}")
  elseif (CMAKE_SYSROOT)
    set(SDK_FLAGS "-sdk" "${CMAKE_SYSROOT}")
  endif ()

  cmake_path(APPEND CMAKE_CURRENT_BINARY_DIR include
    OUTPUT_VARIABLE base_path)

  cmake_path(APPEND base_path ${header}
    OUTPUT_VARIABLE header_path)

  # Work out the name of the .d file that swiftc will emit
  get_target_property(module_name ${target} Swift_MODULE_NAME)
  set(depfile_name "${module_name}.emit-module.d")
  cmake_path(APPEND CMAKE_CURRENT_BINARY_DIR ${depfile_name} OUTPUT_VARIABLE depfile_path)

  set(_AllSources $<TARGET_PROPERTY:${target},SOURCES>)
  set(_SwiftSources $<FILTER:${_AllSources},INCLUDE,\\.swift$>)
  # Ideally in this next line we'd use
  #  set(_AllCompileDefinitions "$<TARGET_PROPERTY:${target},COMPILE_DEFINITIONS>")
  #. set(_AllCompileDefinitionsWithoutD "$<FILTER:${_AllCompileDefinitions},EXCLUDE,-D.*>")
  #. set(_AllCompileDefinitionsWithoutValues "$<FILTER:${_AllCompileDefinitionsWithoutD},EXCLUDE,=.*>")
  #. set(_ModifiedCompileDefinitions "$<LIST:TRANSFORM,${_AllCompileDefinitionsWithoutValues},PREPEND,-D>")
  # but that doesn't work given that we're currently setting the COMPILE_DEFINITIONS only for Swift
  # invocations.
  # For now instead let's just refer to the list in a hard-coded fashion.
  GET_WEBKIT_COMPILE_DEFINITIONS(_AllCompileDefinitions)
  list(TRANSFORM _AllCompileDefinitions PREPEND "-D")
  add_custom_command(OUTPUT ${header_path}
    DEPENDS ${_SwiftSources}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMAND
      ${CMAKE_Swift_COMPILER} -typecheck
      ${ARG_SEARCH_PATHS}
      $<LIST:TRANSFORM,$<TARGET_PROPERTY:${target},INCLUDE_DIRECTORIES>,PREPEND,-I>
      ${_SwiftSources}
      ${_AllCompileDefinitions}
      ${SDK_FLAGS}
      -module-name "${ARG_MODULE_NAME}"
      -cxx-interoperability-mode=default
      -Xcc -std=c++2b
      -emit-clang-header-path ${header_path}
      -emit-dependencies
    DEPFILE ${depfile_path}
    COMMENT
      "Generating '${header_path}'"
    COMMAND_EXPAND_LISTS)

  # Added to public interface for dependees to find.
  target_include_directories(${target} PUBLIC ${base_path})
  # Added to the target to ensure target rebuilds if header changes and is used
  # by sources in the target.
  target_sources(${target} PRIVATE ${header_path})
endfunction()
