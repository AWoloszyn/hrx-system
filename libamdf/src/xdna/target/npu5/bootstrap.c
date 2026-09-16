// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/target/npu5/bootstrap.h"

const amdf_xdna_bootstrap_t amdf_xdna_npu5_bootstrap = {
    .context =
        {
            .uuid = {0x21, 0x98, 0xCF, 0x66, 0xEB, 0x67, 0xF2, 0xBE, 0x5E, 0x50,
                     0x31, 0xDD, 0xC7, 0x7D, 0x5D, 0x1C},
            .operations_per_cycle = 2048,
        },
};
