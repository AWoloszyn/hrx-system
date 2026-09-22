# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

iree_native_test(NAME relative_native SRC reader
  ARGS "{{fixture.txt}}" DATA fixture.txt)
iree_cc_test(NAME relative_compiled SRCS "${PROJECT_SOURCE_DIR}/reader.cc"
  ARGS "{{fixture.txt}}" DATA fixture.txt)
iree_py_test(NAME relative_python MAIN "${PROJECT_SOURCE_DIR}/reader.py"
  SRCS "${PROJECT_SOURCE_DIR}/reader.py" "${PROJECT_SOURCE_DIR}/reader_helper.py"
  PACKAGE_DIRS "${PROJECT_SOURCE_DIR}"
  ARGS "{{$<TARGET_FILE:late_reader>}}" "{{fixture.txt}}" DATA fixture.txt)
