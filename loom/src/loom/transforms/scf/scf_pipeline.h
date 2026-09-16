// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Consumes explicit scf.for pipeline(%depth) requests before unrolling.
// Depth counts original iterations, independently of the unroll factor.
// Ordinary read producers run ahead of ordered consumers through a finite SSA
// queue. Guarded startup, a steady loop, an ordered drain and a serial short
// path execute exactly the original iteration domain. No policy means no
// transformation; unsupported requested schedules produce source diagnostics.

#ifndef LOOM_TRANSFORMS_SCF_SCF_PIPELINE_H_
#define LOOM_TRANSFORMS_SCF_SCF_PIPELINE_H_

#include "loom/pass/types.h"

#ifdef __cplusplus
extern "C" {
#endif

const loom_pass_info_t* loom_scf_pipeline_pass_info(void);

iree_status_t loom_scf_pipeline_run(loom_pass_t* pass, loom_module_t* module,
                                    loom_func_like_t function);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TRANSFORMS_SCF_SCF_PIPELINE_H_
