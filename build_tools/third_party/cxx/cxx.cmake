# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

include(iree_third_party_helpers)

function(iree_configure_cxx)
  if(TARGET iree::third_party::cxx_parser)
    return()
  endif()
  # The importer uses the pinned semantic API and its include ownership/path
  # contracts.
  # An arbitrary installed parser package cannot establish that contract.
  iree_dependency_require_pinned_source_allowed("cxx")
  iree_populate_locked_fetch_content(cxx _cxx_source_dir)
  # The upstream root also configures drivers and unrelated SDK downloads.
  # Build only the independently consumable semantic parser component.
  file(GLOB _cxx_sources CONFIGURE_DEPENDS
    "${_cxx_source_dir}/src/parser/cxx/*.cc")
  add_library(iree_cxx_parser STATIC ${_cxx_sources})
  target_include_directories(iree_cxx_parser SYSTEM PUBLIC
    "${_cxx_source_dir}/src/parser")
  target_compile_features(iree_cxx_parser PRIVATE cxx_std_23)
  target_compile_definitions(iree_cxx_parser PUBLIC CXX_VERSION="1.2.0")
  set_target_properties(iree_cxx_parser PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(MSVC)
    target_compile_options(iree_cxx_parser PRIVATE /EHsc /GR)
  else()
    target_compile_options(iree_cxx_parser PRIVATE -fexceptions -frtti)
  endif()
  iree_add_alias_interface(iree::third_party::cxx_parser iree_cxx_parser)
endfunction()
