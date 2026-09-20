// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TOOLING_TESTBENCH_SOURCE_REPORT_H_
#define LOOM_TOOLING_TESTBENCH_SOURCE_REPORT_H_

#include "loom/ir/ir.h"
#include "loom/util/json.h"

#ifdef __cplusplus
extern "C" {
#endif

// Appends a source_location field for an authored file range. Tagged locations
// retain their child range; unknown or non-file locations omit the field. The
// module owns the location and source names and remains live during the write.
iree_status_t loom_testbench_write_source_location_json(
    const loom_module_t* module, loom_location_id_t location_id,
    loom_json_object_writer_t* object);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_TESTBENCH_SOURCE_REPORT_H_
