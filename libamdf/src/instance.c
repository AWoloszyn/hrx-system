// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/instance.h"

#include <stddef.h>

#include "libamdf/src/allocator.h"
#include "libamdf/src/child_tracker.h"
#include "libamdf/src/structure.h"

struct amdf_instance_t {
  // Immutable lifetime policy for native resources created by this instance.
  amdf_native_lifetime_t native_lifetime;
  // Host allocator copied for this instance and all of its children.
  amdf_allocator_t host_allocator;
  // Platform implementation owned by this instance.
  amdf_platform_instance_t* platform;
  // Number of open children borrowing this instance.
  amdf_child_tracker_t children;
};

amdf_status_t AMDF_CALL
amdf_instance_create(const amdf_instance_create_info_t* create_info,
                     amdf_instance_t** out_instance) {
  if (out_instance == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  amdf_status_t status = amdf_structure_validate_input(
      create_info, AMDF_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      (uint32_t)sizeof(amdf_instance_create_info_t));
  if (!amdf_status_is_ok(status)) {
    return status;
  }

  if (create_info->native_lifetime > AMDF_NATIVE_LIFETIME_INSTANCE ||
      create_info->reserved != 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }

  amdf_allocator_t host_allocator;
  status =
      amdf_allocator_resolve(&create_info->host_allocator, &host_allocator);
  if (!amdf_status_is_ok(status)) return status;

  amdf_instance_t* instance = NULL;
  status = amdf_calloc(host_allocator, sizeof(*instance),
                       amdf_alignof(amdf_instance_t), (void**)&instance);
  if (!amdf_status_is_ok(status)) return status;
  instance->host_allocator = host_allocator;
  instance->native_lifetime = create_info->native_lifetime;
  amdf_child_tracker_initialize(&instance->children);
  status = amdf_platform_instance_create(host_allocator, &instance->platform);
  if (amdf_status_is_ok(status)) {
    *out_instance = instance;
  } else {
    amdf_free(host_allocator, instance);
  }
  return status;
}

amdf_status_t AMDF_CALL amdf_instance_destroy(amdf_instance_t* instance) {
  if (instance == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  if (amdf_child_tracker_count(&instance->children) != 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_BUSY);
  }
  const amdf_status_t status =
      amdf_platform_instance_destroy(instance->platform);
  if (amdf_status_is_ok(status)) {
    const amdf_allocator_t host_allocator = instance->host_allocator;
    amdf_free(host_allocator, instance);
  }
  return status;
}

amdf_status_t AMDF_CALL amdf_endpoint_enumerate(
    amdf_instance_t* instance, uint32_t capacity,
    amdf_endpoint_summary_t* summaries, uint32_t* out_count) {
  if (out_count == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  if (instance == NULL || (capacity != 0 && summaries == NULL)) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  return amdf_platform_endpoint_enumerate(instance->platform, capacity,
                                          summaries, out_count);
}

amdf_platform_instance_t* amdf_instance_platform(amdf_instance_t* instance) {
  return instance->platform;
}

amdf_allocator_t amdf_instance_host_allocator(const amdf_instance_t* instance) {
  return instance->host_allocator;
}

amdf_native_lifetime_t amdf_instance_native_lifetime(
    const amdf_instance_t* instance) {
  return instance->native_lifetime;
}

amdf_status_t amdf_instance_register_endpoint(amdf_instance_t* instance) {
  return amdf_child_tracker_register(&instance->children);
}

void amdf_instance_unregister_endpoint(amdf_instance_t* instance) {
  amdf_child_tracker_unregister(&instance->children);
}
