// Copyright 2026 The HRX Authors
// SPDX-License-Identifier: Apache-2.0
//
// HIP binding internal header. Includes the streaming types that are
// now compiled as part of this binding (no longer a separate library).

#ifndef HRX_BINDING_HIP_BINDING_INTERNAL_H_
#define HRX_BINDING_HIP_BINDING_INTERNAL_H_

#include "common/internal.h"

// Returns a dlopen handle scoped to THIS shared object (the HIP shim), or NULL
// if it could not be established. The handle is resolved once and cached for
// the process lifetime; callers must NOT dlclose() it.
//
// This is the single source of truth for the "resolve HIP symbols against our
// own library" behavior shared by hipGetProcAddress() and the _spt lookup
// (hipGetProcAddress_spt/hipGetDriverEntryPoint_spt). It exists because a
// consumer may dlopen the HIP runtime with RTLD_LOCAL (Triton's AMD backend
// does exactly this), which keeps our symbols out of the process-global scope;
// a dlsym(dlopen(NULL), ...) lookup would then spuriously fail. Callers should
// dlsym() against this handle and fall back to the global scope only if it is
// NULL. Defined in api.c (which enables the GNU extensions it needs).
void* iree_hip_self_dl_handle(void);

// Converts and consumes an IREE status using the binding-wide HIP mapping.
hipError_t iree_status_to_hip_result(iree_status_t status);

// Resolves a public stream handle and retains both the stream and its context
// for one binding operation. The caller releases both output references.
hipError_t iree_hip_resolve_stream_retain(
    hipStream_t stream, iree_hal_streaming_stream_t** out_stream,
    iree_hal_streaming_context_t** out_context);

// Publishes a newly-created stream as a public HIP handle. Consumes the
// stream's initial reference on failure.
hipError_t iree_hip_publish_stream(iree_hal_streaming_stream_t* stream,
                                   hipStream_t* out_stream);

// Resolves a live public event handle and returns a retained event.
hipError_t iree_hip_event_lookup_retain(
    hipEvent_t event, iree_hal_streaming_event_t** out_event);

// Clamps a requested stream priority to the supported device range.
int iree_hip_clamp_stream_priority(int priority);

#endif  // HRX_BINDING_HIP_BINDING_INTERNAL_H_
