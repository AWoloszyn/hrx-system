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
#include "libhrx/src/binding/common/stream.h"
#include "libhrx/src/binding/hip/api.h"
#include "libhrx/src/binding/hip/binding_internal.h"

// Resource partitioning and descriptor management are host-side control-plane
// operations. Each execution context lazily reserves one streaming queue and
// applies its immutable execution-unit mask when the first stream is created.

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
  // Serializes scope creation and context detachment.
  iree_slim_mutex_t mutex;
  // True for the device-owned context returned by hipDeviceGetExecutionCtx.
  bool is_primary;
  // True after this context has been invalidated.
  bool is_destroyed;
  // Resource descriptor consumed when the context was created.
  hipDevResourceDesc_t descriptor;
  // Number of valid bits in |execution_unit_mask|.
  iree_host_size_t execution_unit_mask_bit_count;
  // Immutable mask synthesized from |descriptor|.
  uint32_t* execution_unit_mask;
  // Lazily-created exclusive queue scope owned by this context.
  iree_hal_streaming_queue_scope_t* queue_scope;
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

static hipError_t hrx_hip_execution_context_allocate(
    hipDevice_t device, hipDevResourceDesc_t descriptor, bool is_primary,
    hipExecutionCtx_t* out_context) {
  *out_context = NULL;
  unsigned int sm_count = 0;
  hipError_t result = hrx_hip_device_sm_count(device, &sm_count);
  if (result != hipSuccess) return result;

  const iree_host_size_t mask_word_count =
      ((iree_host_size_t)sm_count + 31) / 32;
  iree_host_size_t mask_size = 0;
  if (!iree_host_size_checked_mul(mask_word_count, sizeof(uint32_t),
                                  &mask_size)) {
    return hipErrorOutOfMemory;
  }

  hipExecutionCtx_t context = calloc(1, sizeof(*context));
  if (!context) return hipErrorOutOfMemory;
  context->execution_unit_mask = calloc(1, mask_size);
  if (!context->execution_unit_mask) {
    free(context);
    return hipErrorOutOfMemory;
  }

  for (unsigned int i = 0; i < descriptor->resource_count; ++i) {
    const hipDevResource* resource = &descriptor->resources[i];
    if (resource->type != hipDevResourceTypeSm) continue;
    const hrx_hip_sm_resource_metadata_t metadata =
        hrx_hip_get_sm_resource_metadata(resource);
    for (unsigned int sm = metadata.start_sm;
         sm < metadata.start_sm + resource->sm.smCount; ++sm) {
      context->execution_unit_mask[sm / 32] |= UINT32_C(1) << (sm % 32);
    }
  }

  iree_atomic_ref_count_init(&context->ref_count);
  iree_slim_mutex_initialize(&context->mutex);
  context->device = device;
  context->is_primary = is_primary;
  context->descriptor = descriptor;
  context->execution_unit_mask_bit_count = mask_word_count * 32;
  *out_context = context;
  return hipSuccess;
}

static void hrx_hip_execution_context_destroy(hipExecutionCtx_t context) {
  iree_hal_streaming_queue_scope_release(context->queue_scope);
  free(context->execution_unit_mask);
  hrx_hip_descriptor_destroy(context->descriptor);
  iree_slim_mutex_deinitialize(&context->mutex);
  free(context);
}

static void hrx_hip_execution_context_release(hipExecutionCtx_t context) {
  if (context && iree_atomic_ref_count_dec(&context->ref_count) == 1) {
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

static hipExecutionCtx_t
hrx_hip_execution_context_registry_lookup_primary_locked(hipDevice_t device) {
  for (hipExecutionCtx_t current = hrx_hip_execution_context_registry_head;
       current; current = current->next_live_context) {
    if (current->is_primary && current->device == device) return current;
  }
  return NULL;
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
  if (!*current || (*current)->is_primary) {
    iree_slim_mutex_unlock(&hrx_hip_execution_context_registry_mutex);
    return false;
  }
  *current = context->next_live_context;
  context->next_live_context = NULL;
  iree_slim_mutex_unlock(&hrx_hip_execution_context_registry_mutex);
  return true;
}

static hipExecutionCtx_t hrx_hip_execution_context_registry_lookup_scope(
    iree_hal_streaming_queue_scope_t* queue_scope) {
  hipExecutionCtx_t retained_context = NULL;
  hrx_hip_execution_context_registry_lock();
  for (hipExecutionCtx_t current = hrx_hip_execution_context_registry_head;
       current; current = current->next_live_context) {
    iree_slim_mutex_lock(&current->mutex);
    const bool matches =
        !current->is_destroyed && current->queue_scope == queue_scope;
    if (matches) iree_atomic_ref_count_inc(&current->ref_count);
    iree_slim_mutex_unlock(&current->mutex);
    if (matches) {
      retained_context = current;
      break;
    }
  }
  iree_slim_mutex_unlock(&hrx_hip_execution_context_registry_mutex);
  return retained_context;
}

static hipError_t hrx_hip_execution_context_retain_queue_scope(
    hipExecutionCtx_t context, bool create,
    iree_hal_streaming_queue_scope_t** out_queue_scope) {
  *out_queue_scope = NULL;
  iree_slim_mutex_lock(&context->mutex);
  hipError_t result = hipSuccess;
  if (context->is_destroyed) {
    result = hipErrorInvalidValue;
  } else if (!context->queue_scope && create) {
    iree_status_t status = iree_hal_streaming_queue_scope_create(
        (iree_host_size_t)context->device,
        context->execution_unit_mask_bit_count, context->execution_unit_mask,
        iree_allocator_system(), &context->queue_scope);
    result = iree_status_to_hip_result(status);
  }
  if (result == hipSuccess && context->queue_scope) {
    iree_hal_streaming_queue_scope_retain(context->queue_scope);
    *out_queue_scope = context->queue_scope;
  }
  iree_slim_mutex_unlock(&context->mutex);
  return result;
}

static hipError_t hrx_hip_execution_context_fill_resource(
    hipExecutionCtx_t context, hipDevResource* resource,
    hipDevResourceType type) {
  unsigned int output_count = 0;
  for (unsigned int i = 0; i < context->descriptor->resource_count; ++i) {
    const hipDevResource* source = &context->descriptor->resources[i];
    if (source->type != type) continue;
    resource[output_count] = *source;
    resource[output_count].nextResource = NULL;
    if (output_count > 0) {
      resource[output_count - 1].nextResource = &resource[output_count];
    }
    ++output_count;
  }
  return output_count > 0 ? hipSuccess : hipErrorInvalidResourceType;
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

  hipDevResourceDesc_t owned_descriptor = NULL;
  result = hrx_hip_descriptor_registry_consume(desc, device, &owned_descriptor);
  if (result != hipSuccess) return result;

  hipExecutionCtx_t new_context = NULL;
  result = hrx_hip_execution_context_allocate(
      device, owned_descriptor, /*is_primary=*/false, &new_context);
  if (result != hipSuccess) {
    hrx_hip_descriptor_destroy(owned_descriptor);
    return result;
  }
  hrx_hip_execution_context_registry_insert(new_context);
  *context = new_context;
  return hipSuccess;
}

HIPAPI hipError_t hipExecutionCtxDestroy(hipExecutionCtx_t context) {
  if (!context) return hipErrorInvalidValue;
  if (!hrx_hip_execution_context_registry_remove(context)) {
    return hipErrorInvalidValue;
  }
  iree_slim_mutex_lock(&context->mutex);
  context->is_destroyed = true;
  iree_hal_streaming_queue_scope_t* queue_scope = context->queue_scope;
  context->queue_scope = NULL;
  iree_hal_streaming_queue_scope_detach(queue_scope);
  iree_slim_mutex_unlock(&context->mutex);
  iree_hal_streaming_queue_scope_release(queue_scope);
  hrx_hip_execution_context_release(context);
  return hipSuccess;
}

HIPAPI hipError_t hipDeviceGetExecutionCtx(hipExecutionCtx_t* context,
                                           hipDevice_t device) {
  if (!context) return hipErrorInvalidValue;
  *context = NULL;
  unsigned int sm_count = 0;
  hipError_t result = hrx_hip_device_sm_count(device, &sm_count);
  if (result != hipSuccess) return result;

  hrx_hip_execution_context_registry_lock();
  hipExecutionCtx_t primary_context =
      hrx_hip_execution_context_registry_lookup_primary_locked(device);
  iree_slim_mutex_unlock(&hrx_hip_execution_context_registry_mutex);
  if (primary_context) {
    *context = primary_context;
    return hipSuccess;
  }

  hipDevResourceDesc_t descriptor = calloc(1, sizeof(*descriptor));
  if (!descriptor) return hipErrorOutOfMemory;
  descriptor->resources = calloc(1, sizeof(*descriptor->resources));
  if (!descriptor->resources) {
    free(descriptor);
    return hipErrorOutOfMemory;
  }
  descriptor->device = device;
  descriptor->resource_count = 1;
  hrx_hip_fill_sm_resource(descriptor->resources, sm_count, /*alignment=*/2,
                           hipDevSmResourceGroupDefault, device,
                           /*start_sm=*/0);

  hipExecutionCtx_t candidate = NULL;
  result = hrx_hip_execution_context_allocate(device, descriptor,
                                              /*is_primary=*/true, &candidate);
  if (result != hipSuccess) {
    hrx_hip_descriptor_destroy(descriptor);
    return result;
  }

  hrx_hip_execution_context_registry_lock();
  primary_context =
      hrx_hip_execution_context_registry_lookup_primary_locked(device);
  if (!primary_context) {
    candidate->context_id = hrx_hip_next_execution_context_id++;
    candidate->next_live_context = hrx_hip_execution_context_registry_head;
    hrx_hip_execution_context_registry_head = candidate;
    primary_context = candidate;
    candidate = NULL;
  }
  iree_slim_mutex_unlock(&hrx_hip_execution_context_registry_mutex);
  hrx_hip_execution_context_release(candidate);
  *context = primary_context;
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

  hipError_t result =
      hrx_hip_execution_context_fill_resource(retained_context, resource, type);
  hrx_hip_execution_context_release(retained_context);
  return result;
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

HIPAPI hipError_t hipExecutionCtxStreamCreate(hipStream_t* stream,
                                              hipExecutionCtx_t context,
                                              unsigned int flags,
                                              int priority) {
  if (!stream) return hipErrorInvalidValue;
  *stream = NULL;
  if (flags != hipStreamDefault && flags != hipStreamNonBlocking) {
    return hipErrorInvalidValue;
  }
  hipExecutionCtx_t retained_context =
      hrx_hip_execution_context_registry_lookup(context);
  if (!retained_context) return hipErrorInvalidValue;

  // Keep scope attachment and stream registration in the same critical
  // section as context destruction. A successfully returned stream owns its
  // scope independently; otherwise destruction wins before creation begins.
  iree_slim_mutex_lock(&retained_context->mutex);
  hipError_t result = hipSuccess;
  if (retained_context->is_destroyed) {
    result = hipErrorInvalidValue;
  } else if (!retained_context->queue_scope) {
    iree_status_t status = iree_hal_streaming_queue_scope_create(
        (iree_host_size_t)retained_context->device,
        retained_context->execution_unit_mask_bit_count,
        retained_context->execution_unit_mask, iree_allocator_system(),
        &retained_context->queue_scope);
    result = iree_status_to_hip_result(status);
  }
  if (result == hipSuccess) {
    const uint64_t internal_flags =
        flags == hipStreamNonBlocking
            ? IREE_HAL_STREAMING_STREAM_FLAG_NON_BLOCKING
            : IREE_HAL_STREAMING_STREAM_FLAG_NONE;
    iree_hal_streaming_stream_t* streaming_stream = NULL;
    iree_status_t status = iree_hal_streaming_stream_create_in_queue_scope(
        retained_context->queue_scope, internal_flags,
        iree_hip_clamp_stream_priority(priority), iree_allocator_system(),
        &streaming_stream);
    result = iree_status_to_hip_result(status);
    if (result == hipSuccess) {
      result = iree_hip_publish_stream(streaming_stream, stream);
    }
  }
  iree_slim_mutex_unlock(&retained_context->mutex);
  hrx_hip_execution_context_release(retained_context);
  return result;
}

HIPAPI hipError_t hipExecutionCtxRecordEvent(hipExecutionCtx_t context,
                                             hipEvent_t event) {
  hipExecutionCtx_t retained_context =
      hrx_hip_execution_context_registry_lookup(context);
  if (!retained_context) return hipErrorInvalidValue;
  iree_hal_streaming_event_t* streaming_event = NULL;
  hipError_t result = iree_hip_event_lookup_retain(event, &streaming_event);
  iree_hal_streaming_queue_scope_t* queue_scope = NULL;
  if (result == hipSuccess) {
    result = hrx_hip_execution_context_retain_queue_scope(
        retained_context, /*create=*/false, &queue_scope);
  }
  if (result == hipSuccess && queue_scope) {
    result =
        iree_status_to_hip_result(iree_hal_streaming_queue_scope_record_event(
            queue_scope, streaming_event));
  }
  iree_hal_streaming_queue_scope_release(queue_scope);
  iree_hal_streaming_event_release(streaming_event);
  hrx_hip_execution_context_release(retained_context);
  return result;
}

HIPAPI hipError_t hipExecutionCtxWaitEvent(hipExecutionCtx_t context,
                                           hipEvent_t event) {
  hipExecutionCtx_t retained_context =
      hrx_hip_execution_context_registry_lookup(context);
  if (!retained_context) return hipErrorInvalidValue;
  iree_hal_streaming_event_t* streaming_event = NULL;
  hipError_t result = iree_hip_event_lookup_retain(event, &streaming_event);
  iree_hal_streaming_queue_scope_t* queue_scope = NULL;
  if (result == hipSuccess) {
    result = hrx_hip_execution_context_retain_queue_scope(
        retained_context, /*create=*/false, &queue_scope);
  }
  if (result == hipSuccess && queue_scope) {
    result =
        iree_status_to_hip_result(iree_hal_streaming_queue_scope_wait_event(
            queue_scope, streaming_event));
  }
  iree_hal_streaming_queue_scope_release(queue_scope);
  iree_hal_streaming_event_release(streaming_event);
  hrx_hip_execution_context_release(retained_context);
  return result;
}

HIPAPI hipError_t hipExecutionCtxSynchronize(hipExecutionCtx_t context) {
  hipExecutionCtx_t retained_context =
      hrx_hip_execution_context_registry_lookup(context);
  if (!retained_context) return hipErrorInvalidValue;
  iree_hal_streaming_queue_scope_t* queue_scope = NULL;
  hipError_t result = hrx_hip_execution_context_retain_queue_scope(
      retained_context, /*create=*/false, &queue_scope);
  if (result == hipSuccess && queue_scope) {
    result = iree_status_to_hip_result(
        iree_hal_streaming_queue_scope_synchronize(queue_scope));
  }
  iree_hal_streaming_queue_scope_release(queue_scope);
  hrx_hip_execution_context_release(retained_context);
  return result;
}

HIPAPI hipError_t hipStreamGetDevResource(hipStream_t stream,
                                          hipDevResource* resource,
                                          hipDevResourceType type) {
  if (!resource) return hipErrorInvalidValue;
  if (type != hipDevResourceTypeSm) return hipErrorInvalidResourceType;
  iree_hal_streaming_stream_t* streaming_stream = NULL;
  iree_hal_streaming_context_t* streaming_context = NULL;
  hipError_t result = iree_hip_resolve_stream_retain(
      stream, &streaming_stream, &streaming_context);
  if (result != hipSuccess) return result;

  iree_hal_streaming_queue_scope_t* queue_scope =
      iree_hal_streaming_stream_queue_scope(streaming_stream);
  if (!queue_scope) {
    result = hipDeviceGetDevResource(
        (hipDevice_t)iree_hal_streaming_stream_device_ordinal(streaming_stream),
        resource, type);
  } else {
    hipExecutionCtx_t retained_context =
        hrx_hip_execution_context_registry_lookup_scope(queue_scope);
    if (!retained_context) {
      result = hipErrorStreamDetached;
    } else {
      result = hrx_hip_execution_context_fill_resource(retained_context,
                                                       resource, type);
      hrx_hip_execution_context_release(retained_context);
    }
  }
  iree_hal_streaming_context_release(streaming_context);
  iree_hal_streaming_stream_release(streaming_stream);
  return result;
}
