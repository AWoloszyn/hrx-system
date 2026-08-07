// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "iree/base/api.h"
#include "iree/base/threading/call_once.h"
#include "iree/base/threading/mutex.h"
#include "libhrx/src/binding/hip/api.h"

// Resource partitioning and descriptor management are host-side control-plane
// operations. Execution-context streams require backend queues that enforce a
// CU mask; ordinary streaming-layer streams share queues and cannot provide
// that isolation. This file therefore owns only the resource and context
// contracts that can be implemented independently of queue creation.

typedef struct hrx_hip_sm_resource_metadata_t {
  // Device ordinal whose SM range is described by the resource.
  int32_t device;
  // First SM in the contiguous resource range.
  uint32_t start_sm;
} hrx_hip_sm_resource_metadata_t;

_Static_assert(
    sizeof(hrx_hip_sm_resource_metadata_t) <=
        sizeof(((hipDevResource*)0)->_internal_padding),
    "SM resource metadata must fit in the runtime-owned ABI storage");

struct ihipDevResourceDesc_t {
  // Next descriptor awaiting consumption by hipGreenCtxCreate.
  struct ihipDevResourceDesc_t* next_live_descriptor;
  // Device shared by every resource in the descriptor.
  hipDevice_t device;
  // Number of entries in resources.
  unsigned int resource_count;
  // Owned resource records.
  hipDevResource* resources;
};

struct ihipExecutionCtx_t {
  // References held by the registry and active API calls.
  iree_atomic_ref_count_t ref_count;
  // Next live execution context in the process registry.
  struct ihipExecutionCtx_t* next_live_context;
  // Device ordinal targeted by the context.
  hipDevice_t device;
  // Stable process-local context identifier.
  unsigned long long context_id;
  // Resource descriptor consumed when the context was created.
  hipDevResourceDesc_t descriptor;
};

static iree_once_flag hrx_hip_descriptor_registry_once = IREE_ONCE_FLAG_INIT;
static iree_slim_mutex_t hrx_hip_descriptor_registry_mutex;
static hipDevResourceDesc_t hrx_hip_descriptor_registry_head = NULL;

static iree_once_flag hrx_hip_execution_context_registry_once =
    IREE_ONCE_FLAG_INIT;
static iree_slim_mutex_t hrx_hip_execution_context_registry_mutex;
static hipExecutionCtx_t hrx_hip_execution_context_registry_head = NULL;
static unsigned long long hrx_hip_next_execution_context_id = 1;

static void hrx_hip_descriptor_registry_initialize(void) {
  iree_slim_mutex_initialize(&hrx_hip_descriptor_registry_mutex);
}

static void hrx_hip_descriptor_registry_lock(void) {
  iree_call_once(&hrx_hip_descriptor_registry_once,
                 hrx_hip_descriptor_registry_initialize);
  iree_slim_mutex_lock(&hrx_hip_descriptor_registry_mutex);
}

static void hrx_hip_execution_context_registry_initialize(void) {
  iree_slim_mutex_initialize(&hrx_hip_execution_context_registry_mutex);
}

static void hrx_hip_execution_context_registry_lock(void) {
  iree_call_once(&hrx_hip_execution_context_registry_once,
                 hrx_hip_execution_context_registry_initialize);
  iree_slim_mutex_lock(&hrx_hip_execution_context_registry_mutex);
}

static hipError_t hrx_hip_validate_device(hipDevice_t device) {
  int device_count = 0;
  hipError_t result = hipGetDeviceCount(&device_count);
  if (result != hipSuccess) return result;
  return device >= 0 && device < device_count ? hipSuccess
                                              : hipErrorInvalidDevice;
}

static hipError_t hrx_hip_device_sm_count(hipDevice_t device,
                                          unsigned int* out_sm_count) {
  if (!out_sm_count) return hipErrorInvalidValue;
  hipError_t result = hrx_hip_validate_device(device);
  if (result != hipSuccess) return result;
  int sm_count = 0;
  result = hipDeviceGetAttribute(&sm_count,
                                 hipDeviceAttributeMultiprocessorCount, device);
  if (result != hipSuccess) return result;
  if (sm_count <= 0) return hipErrorInvalidResourceConfiguration;
  *out_sm_count = (unsigned int)sm_count;
  return hipSuccess;
}

static void hrx_hip_set_sm_resource_metadata(hipDevResource* resource,
                                             hipDevice_t device,
                                             unsigned int start_sm) {
  hrx_hip_sm_resource_metadata_t metadata = {
      .device = (int32_t)device,
      .start_sm = start_sm,
  };
  memset(resource->_internal_padding, 0, sizeof(resource->_internal_padding));
  memcpy(resource->_internal_padding, &metadata, sizeof(metadata));
}

static hrx_hip_sm_resource_metadata_t hrx_hip_get_sm_resource_metadata(
    const hipDevResource* resource) {
  hrx_hip_sm_resource_metadata_t metadata = {0};
  memcpy(&metadata, resource->_internal_padding, sizeof(metadata));
  return metadata;
}

static void hrx_hip_fill_sm_resource(hipDevResource* resource,
                                     unsigned int sm_count,
                                     unsigned int alignment, unsigned int flags,
                                     hipDevice_t device,
                                     unsigned int start_sm) {
  memset(resource, 0, sizeof(*resource));
  resource->type = hipDevResourceTypeSm;
  resource->sm.smCount = sm_count;
  resource->sm.minSmPartitionSize = alignment;
  resource->sm.smCoscheduledAlignment = alignment;
  resource->sm.flags = flags;
  hrx_hip_set_sm_resource_metadata(resource, device, start_sm);
}

static void hrx_hip_fill_sm_remainder(hipDevResource* remainder,
                                      unsigned int sm_count,
                                      unsigned int alignment,
                                      hipDevice_t device,
                                      unsigned int start_sm) {
  if (!remainder) return;
  memset(remainder, 0, sizeof(*remainder));
  if (sm_count == 0) {
    remainder->type = hipDevResourceTypeInvalid;
    return;
  }
  hrx_hip_fill_sm_resource(remainder, sm_count, alignment,
                           hipDevSmResourceGroupDefault, device, start_sm);
}

static hipError_t hrx_hip_validate_sm_resource(
    const hipDevResource* resource,
    hrx_hip_sm_resource_metadata_t* out_metadata) {
  if (!resource) return hipErrorInvalidValue;
  if (resource->type != hipDevResourceTypeSm) {
    return hipErrorInvalidResourceType;
  }
  if (resource->sm.smCount == 0 || resource->sm.minSmPartitionSize == 0 ||
      resource->sm.smCoscheduledAlignment == 0) {
    return hipErrorInvalidResourceConfiguration;
  }

  hrx_hip_sm_resource_metadata_t metadata =
      hrx_hip_get_sm_resource_metadata(resource);
  unsigned int device_sm_count = 0;
  hipError_t result =
      hrx_hip_device_sm_count(metadata.device, &device_sm_count);
  if (result != hipSuccess) return result;
  if (metadata.start_sm > device_sm_count ||
      resource->sm.smCount > device_sm_count - metadata.start_sm) {
    return hipErrorInvalidResourceConfiguration;
  }
  if (out_metadata) *out_metadata = metadata;
  return hipSuccess;
}

static unsigned int hrx_hip_round_up_to_multiple(unsigned int value,
                                                 unsigned int alignment) {
  uint64_t rounded = ((uint64_t)value + alignment - 1u) / alignment * alignment;
  return rounded <= UINT32_MAX ? (unsigned int)rounded : 0;
}

static void hrx_hip_descriptor_destroy(hipDevResourceDesc_t descriptor) {
  if (!descriptor) return;
  free(descriptor->resources);
  free(descriptor);
}

static void hrx_hip_descriptor_registry_insert(
    hipDevResourceDesc_t descriptor) {
  hrx_hip_descriptor_registry_lock();
  descriptor->next_live_descriptor = hrx_hip_descriptor_registry_head;
  hrx_hip_descriptor_registry_head = descriptor;
  iree_slim_mutex_unlock(&hrx_hip_descriptor_registry_mutex);
}

static hipError_t hrx_hip_descriptor_registry_consume(
    hipDevResourceDesc_t descriptor, hipDevice_t device,
    hipDevResourceDesc_t* out_descriptor) {
  *out_descriptor = NULL;
  if (!descriptor) return hipErrorInvalidValue;

  hrx_hip_descriptor_registry_lock();
  hipDevResourceDesc_t* current = &hrx_hip_descriptor_registry_head;
  while (*current && *current != descriptor) {
    current = &(*current)->next_live_descriptor;
  }
  if (!*current) {
    iree_slim_mutex_unlock(&hrx_hip_descriptor_registry_mutex);
    return hipErrorInvalidValue;
  }
  if (descriptor->device != device) {
    iree_slim_mutex_unlock(&hrx_hip_descriptor_registry_mutex);
    return hipErrorInvalidDevice;
  }
  *current = descriptor->next_live_descriptor;
  descriptor->next_live_descriptor = NULL;
  iree_slim_mutex_unlock(&hrx_hip_descriptor_registry_mutex);

  *out_descriptor = descriptor;
  return hipSuccess;
}

static void hrx_hip_execution_context_destroy(hipExecutionCtx_t context) {
  hrx_hip_descriptor_destroy(context->descriptor);
  free(context);
}

static void hrx_hip_execution_context_release(hipExecutionCtx_t context) {
  if (iree_atomic_ref_count_dec(&context->ref_count) == 1) {
    hrx_hip_execution_context_destroy(context);
  }
}

static void hrx_hip_execution_context_registry_insert(
    hipExecutionCtx_t context) {
  hrx_hip_execution_context_registry_lock();
  context->context_id = hrx_hip_next_execution_context_id++;
  context->next_live_context = hrx_hip_execution_context_registry_head;
  hrx_hip_execution_context_registry_head = context;
  iree_slim_mutex_unlock(&hrx_hip_execution_context_registry_mutex);
}

static hipExecutionCtx_t hrx_hip_execution_context_registry_lookup(
    hipExecutionCtx_t context) {
  if (!context) return NULL;
  hipExecutionCtx_t retained_context = NULL;
  hrx_hip_execution_context_registry_lock();
  for (hipExecutionCtx_t current = hrx_hip_execution_context_registry_head;
       current; current = current->next_live_context) {
    if (current == context) {
      iree_atomic_ref_count_inc(&current->ref_count);
      retained_context = current;
      break;
    }
  }
  iree_slim_mutex_unlock(&hrx_hip_execution_context_registry_mutex);
  return retained_context;
}

static bool hrx_hip_execution_context_registry_remove(
    hipExecutionCtx_t context) {
  if (!context) return false;
  hrx_hip_execution_context_registry_lock();
  hipExecutionCtx_t* current = &hrx_hip_execution_context_registry_head;
  while (*current && *current != context) {
    current = &(*current)->next_live_context;
  }
  if (!*current) {
    iree_slim_mutex_unlock(&hrx_hip_execution_context_registry_mutex);
    return false;
  }
  *current = context->next_live_context;
  context->next_live_context = NULL;
  iree_slim_mutex_unlock(&hrx_hip_execution_context_registry_mutex);
  return true;
}

HIPAPI hipError_t hipDeviceGetDevResource(hipDevice_t device,
                                          hipDevResource* resource,
                                          hipDevResourceType type) {
  if (!resource) return hipErrorInvalidValue;
  if (type != hipDevResourceTypeSm) return hipErrorInvalidResourceType;
  unsigned int sm_count = 0;
  hipError_t result = hrx_hip_device_sm_count(device, &sm_count);
  if (result != hipSuccess) return result;
  hrx_hip_fill_sm_resource(resource, sm_count, /*alignment=*/2,
                           hipDevSmResourceGroupDefault, device,
                           /*start_sm=*/0);
  return hipSuccess;
}

HIPAPI hipError_t hipDevSmResourceSplit(
    hipDevResource* result, unsigned int nbGroups, const hipDevResource* input,
    hipDevResource* remainder, unsigned int flags,
    hipDevSmResourceGroupParams* groupParams) {
  if (!input || !groupParams || nbGroups == 0) return hipErrorInvalidValue;
  if (flags != 0) return hipErrorInvalidValue;

  hrx_hip_sm_resource_metadata_t metadata;
  hipError_t validation_result = hrx_hip_validate_sm_resource(input, &metadata);
  if (validation_result != hipSuccess) return validation_result;

  const unsigned int total_sm_count = input->sm.smCount;
  const unsigned int default_alignment = input->sm.smCoscheduledAlignment;
  const unsigned int minimum_partition_size = input->sm.minSmPartitionSize;
  uint64_t assigned_sm_count = 0;
  for (unsigned int i = 0; i < nbGroups; ++i) {
    if (groupParams[i].flags & ~hipDevSmResourceGroupBackfill) {
      return hipErrorInvalidValue;
    }
    if (groupParams[i].coscheduledSmCount == 0) {
      groupParams[i].coscheduledSmCount = default_alignment;
    }
    if (groupParams[i].preferredCoscheduledSmCount == 0) {
      groupParams[i].preferredCoscheduledSmCount =
          groupParams[i].coscheduledSmCount;
    }
    const unsigned int coscheduled_sm_count = groupParams[i].coscheduledSmCount;
    if (coscheduled_sm_count == 0 || assigned_sm_count > total_sm_count) {
      return hipErrorInvalidResourceConfiguration;
    }
    if (groupParams[i].smCount == 0) {
      const unsigned int available_sm_count =
          total_sm_count - (unsigned int)assigned_sm_count;
      groupParams[i].smCount =
          (groupParams[i].flags & hipDevSmResourceGroupBackfill)
              ? available_sm_count
              : (available_sm_count / coscheduled_sm_count) *
                    coscheduled_sm_count;
    }
    const unsigned int sm_count = groupParams[i].smCount;
    if (sm_count < minimum_partition_size || sm_count > total_sm_count ||
        (sm_count < coscheduled_sm_count) ||
        ((sm_count % coscheduled_sm_count) != 0 &&
         !(groupParams[i].flags & hipDevSmResourceGroupBackfill))) {
      return hipErrorInvalidResourceConfiguration;
    }
    assigned_sm_count += sm_count;
    if (assigned_sm_count > total_sm_count) {
      return hipErrorInvalidResourceConfiguration;
    }
  }

  unsigned int output_start_sm = metadata.start_sm;
  if (result) {
    for (unsigned int i = 0; i < nbGroups; ++i) {
      hrx_hip_fill_sm_resource(
          &result[i], groupParams[i].smCount, groupParams[i].coscheduledSmCount,
          groupParams[i].flags, metadata.device, output_start_sm);
      output_start_sm += groupParams[i].smCount;
    }
  }
  hrx_hip_fill_sm_remainder(
      remainder, total_sm_count - (unsigned int)assigned_sm_count,
      default_alignment, metadata.device,
      metadata.start_sm + (unsigned int)assigned_sm_count);
  return hipSuccess;
}

HIPAPI hipError_t hipDevSmResourceSplitByCount(
    hipDevResource* result, unsigned int* nbGroups, const hipDevResource* input,
    hipDevResource* remainder, unsigned int flags, unsigned int minCount) {
  if (!nbGroups || !input) return hipErrorInvalidValue;
  const unsigned int supported_flags =
      hipDevSmResourceSplitIgnoreSmCoscheduling |
      hipDevSmResourceSplitMaxPotentialClusterSize;
  if (flags & ~supported_flags) return hipErrorInvalidValue;
  if (flags & hipDevSmResourceSplitMaxPotentialClusterSize) {
    return hipErrorNotSupported;
  }

  hrx_hip_sm_resource_metadata_t metadata;
  hipError_t validation_result = hrx_hip_validate_sm_resource(input, &metadata);
  if (validation_result != hipSuccess) return validation_result;

  const unsigned int alignment =
      (flags & hipDevSmResourceSplitIgnoreSmCoscheduling)
          ? 1
          : input->sm.smCoscheduledAlignment;
  unsigned int aligned_minimum =
      hrx_hip_round_up_to_multiple(minCount, alignment);
  if (aligned_minimum == 0 && minCount == 0) aligned_minimum = alignment;
  if (aligned_minimum == 0) {
    return hipErrorInvalidResourceConfiguration;
  }

  const unsigned int possible_group_count = input->sm.smCount / aligned_minimum;
  if (!result) {
    *nbGroups = possible_group_count;
    return hipSuccess;
  }

  const unsigned int actual_group_count =
      *nbGroups < possible_group_count ? *nbGroups : possible_group_count;
  *nbGroups = actual_group_count;
  unsigned int assigned_sm_count = 0;
  for (unsigned int i = 0; i < actual_group_count; ++i) {
    hrx_hip_fill_sm_resource(&result[i], aligned_minimum, alignment,
                             hipDevSmResourceGroupDefault, metadata.device,
                             metadata.start_sm + assigned_sm_count);
    assigned_sm_count += aligned_minimum;
  }
  hrx_hip_fill_sm_remainder(remainder, input->sm.smCount - assigned_sm_count,
                            alignment, metadata.device,
                            metadata.start_sm + assigned_sm_count);
  return hipSuccess;
}

HIPAPI hipError_t hipDevResourceGenerateDesc(hipDevResourceDesc_t* desc,
                                             hipDevResource* resources,
                                             unsigned int resourceCount) {
  if (!desc || !resources || resourceCount == 0) return hipErrorInvalidValue;
  *desc = NULL;
  iree_host_size_t resource_bytes = 0;
  if (!iree_host_size_checked_mul(resourceCount, sizeof(*resources),
                                  &resource_bytes)) {
    return hipErrorOutOfMemory;
  }

  hrx_hip_sm_resource_metadata_t first_metadata;
  hipError_t validation_result =
      hrx_hip_validate_sm_resource(&resources[0], &first_metadata);
  if (validation_result != hipSuccess) return validation_result;
  for (unsigned int i = 1; i < resourceCount; ++i) {
    hrx_hip_sm_resource_metadata_t metadata;
    validation_result = hrx_hip_validate_sm_resource(&resources[i], &metadata);
    if (validation_result != hipSuccess) return validation_result;
    if (metadata.device != first_metadata.device ||
        resources[i].sm.smCoscheduledAlignment !=
            resources[0].sm.smCoscheduledAlignment) {
      return hipErrorInvalidResourceConfiguration;
    }
    const uint64_t range_start = metadata.start_sm;
    const uint64_t range_end = range_start + resources[i].sm.smCount;
    for (unsigned int j = 0; j < i; ++j) {
      const hrx_hip_sm_resource_metadata_t other_metadata =
          hrx_hip_get_sm_resource_metadata(&resources[j]);
      const uint64_t other_start = other_metadata.start_sm;
      const uint64_t other_end = other_start + resources[j].sm.smCount;
      if (range_start < other_end && other_start < range_end) {
        return hipErrorInvalidResourceConfiguration;
      }
    }
  }

  hipDevResourceDesc_t descriptor = calloc(1, sizeof(*descriptor));
  if (!descriptor) return hipErrorOutOfMemory;
  descriptor->resources = calloc(1, resource_bytes);
  if (!descriptor->resources) {
    free(descriptor);
    return hipErrorOutOfMemory;
  }
  memcpy(descriptor->resources, resources, resource_bytes);
  descriptor->device = first_metadata.device;
  descriptor->resource_count = resourceCount;
  hrx_hip_descriptor_registry_insert(descriptor);
  *desc = descriptor;
  return hipSuccess;
}

HIPAPI hipError_t hipGreenCtxCreate(hipExecutionCtx_t* context,
                                    hipDevResourceDesc_t desc, int device,
                                    unsigned int flags) {
  if (!context || !desc || flags != 0) return hipErrorInvalidValue;
  *context = NULL;
  hipError_t result = hrx_hip_validate_device(device);
  if (result != hipSuccess) return result;

  hipExecutionCtx_t new_context = calloc(1, sizeof(*new_context));
  if (!new_context) return hipErrorOutOfMemory;
  hipDevResourceDesc_t owned_descriptor = NULL;
  result = hrx_hip_descriptor_registry_consume(desc, device, &owned_descriptor);
  if (result != hipSuccess) {
    free(new_context);
    return result;
  }

  iree_atomic_ref_count_init(&new_context->ref_count);
  new_context->device = device;
  new_context->descriptor = owned_descriptor;
  hrx_hip_execution_context_registry_insert(new_context);
  *context = new_context;
  return hipSuccess;
}

HIPAPI hipError_t hipExecutionCtxDestroy(hipExecutionCtx_t context) {
  if (!context) return hipErrorInvalidValue;
  if (!hrx_hip_execution_context_registry_remove(context)) {
    return hipErrorInvalidValue;
  }
  hrx_hip_execution_context_release(context);
  return hipSuccess;
}

HIPAPI hipError_t hipExecutionCtxGetDevResource(hipExecutionCtx_t context,
                                                hipDevResource* resource,
                                                hipDevResourceType type) {
  if (!resource) return hipErrorInvalidValue;
  if (type != hipDevResourceTypeSm) return hipErrorInvalidResourceType;
  hipExecutionCtx_t retained_context =
      hrx_hip_execution_context_registry_lookup(context);
  if (!retained_context) return hipErrorInvalidValue;

  unsigned int output_count = 0;
  for (unsigned int i = 0; i < retained_context->descriptor->resource_count;
       ++i) {
    const hipDevResource* source = &retained_context->descriptor->resources[i];
    if (source->type != type) continue;
    resource[output_count] = *source;
    resource[output_count].nextResource = NULL;
    if (output_count > 0) {
      resource[output_count - 1].nextResource = &resource[output_count];
    }
    ++output_count;
  }
  hrx_hip_execution_context_release(retained_context);
  return output_count > 0 ? hipSuccess : hipErrorInvalidResourceType;
}

HIPAPI hipError_t hipExecutionCtxGetDevice(hipDevice_t* device,
                                           hipExecutionCtx_t context) {
  if (!device) return hipErrorInvalidValue;
  hipExecutionCtx_t retained_context =
      hrx_hip_execution_context_registry_lookup(context);
  if (!retained_context) return hipErrorInvalidValue;
  *device = retained_context->device;
  hrx_hip_execution_context_release(retained_context);
  return hipSuccess;
}

HIPAPI hipError_t hipExecutionCtxGetId(hipExecutionCtx_t context,
                                       unsigned long long* contextId) {
  if (!contextId) return hipErrorInvalidValue;
  hipExecutionCtx_t retained_context =
      hrx_hip_execution_context_registry_lookup(context);
  if (!retained_context) return hipErrorInvalidValue;
  *contextId = retained_context->context_id;
  hrx_hip_execution_context_release(retained_context);
  return hipSuccess;
}
