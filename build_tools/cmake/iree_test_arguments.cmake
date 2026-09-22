# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

include_guard(GLOBAL)

# Resolves explicit {{file}} spans without interpreting ordinary argument text
# or rescanning substituted paths. Relative files use DATA's project-source
# namespace; generated files use absolute paths or target-file expressions.
# RESOLVER(FILE, OUT_PATH) maps each file to its build or installed location.
# OUT_DATA receives those same paths for dependency/availability tracking.
function(iree_resolve_test_arguments OUT_ARGS OUT_DATA RESOLVER)
  set(_ARGS)
  set(_DATA)
  foreach(_ARG IN LISTS ARGN)
    set(_MAPPED_ARG "")
    set(_REMAINING "${_ARG}")
    string(FIND "${_REMAINING}" "{{" _START)
    while(_START GREATER_EQUAL 0)
      string(SUBSTRING "${_REMAINING}" 0 ${_START} _PREFIX)
      math(EXPR _FILE_START "${_START} + 2")
      string(SUBSTRING "${_REMAINING}" ${_FILE_START} -1 _REMAINING)
      string(FIND "${_REMAINING}" "}}" _END)
      if(_END LESS_EQUAL 0)
        message(FATAL_ERROR "Invalid file locator in test argument '${_ARG}'")
      endif()
      string(SUBSTRING "${_REMAINING}" 0 ${_END} _FILE)
      if(NOT IS_ABSOLUTE "${_FILE}" AND NOT _FILE MATCHES "^\\$<")
        get_filename_component(_FILE "${_FILE}" ABSOLUTE
          BASE_DIR "${PROJECT_SOURCE_DIR}")
      endif()
      cmake_language(CALL ${RESOLVER}
        "${_FILE}" _PATH)
      string(APPEND _MAPPED_ARG "${_PREFIX}${_PATH}")
      list(APPEND _DATA "${_PATH}")
      math(EXPR _NEXT "${_END} + 2")
      string(SUBSTRING "${_REMAINING}" ${_NEXT} -1 _REMAINING)
      string(FIND "${_REMAINING}" "{{" _START)
    endwhile()
    string(APPEND _MAPPED_ARG "${_REMAINING}")
    list(APPEND _ARGS "${_MAPPED_ARG}")
  endforeach()
  set(${OUT_ARGS} "${_ARGS}" PARENT_SCOPE)
  set(${OUT_DATA} "${_DATA}" PARENT_SCOPE)
endfunction()

function(iree_build_test_file_argument FILE OUT_PATH)
  set(${OUT_PATH} "${FILE}" PARENT_SCOPE)
endfunction()
