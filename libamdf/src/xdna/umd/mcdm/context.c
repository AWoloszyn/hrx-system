// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/context.h"

#include "libamdf/src/allocator.h"
#include "libamdf/src/xdna/umd/mcdm/context.h"
#include "libamdf/src/xdna/umd/mcdm/kernel_execution.h"

// Fixed direct-width native context record. Its kernel buffer is native
// context storage, not an application program or a per-submission BO list.
typedef struct amdf_windows_xdna_direct_context_t {
  // Optional image identity; zero for a program-independent partition.
  uint8_t uuid[16];
  // Zero selects the native default quality-of-service policy.
  uint8_t quality_of_service[0x30];
  // Reserved context configuration, zero for the ordinary native path.
  uint32_t reserved_0040;
  // Context ID returned by native creation, including zero.
  uint32_t command_aperture_cookie;
  // Size of the native instruction aperture in bytes.
  uint64_t command_aperture_byte_length;
  // Process creating the native context.
  uint32_t process_id;
  // Requested logical partition width in columns.
  uint32_t column_count;
  // Reserved native placement and proxy configuration; all zero.
  uint32_t reserved_0058[4];
  // Native allocation handle retained by the context owner.
  uint64_t kernel_buffer_allocation;
  // Byte offset within the kernel buffer allocation.
  uint32_t kernel_buffer_byte_offset;
  // Byte length of the kernel buffer.
  uint32_t kernel_buffer_byte_length;
  // Locked CPU pointer retained through native context destruction.
  uint64_t kernel_buffer_host_address;
  // Optional native configuration, zero on the direct-width path.
  uint8_t reserved_0080[0x20];
} amdf_windows_xdna_direct_context_t;

_Static_assert(sizeof(amdf_windows_xdna_direct_context_t) == 0xA0,
               "direct context must match the native wire record");
_Static_assert(offsetof(amdf_windows_xdna_direct_context_t,
                        kernel_buffer_allocation) == 0x68,
               "direct context buffer must match the native wire offset");

amdf_status_t amdf_xdna_umd_context_destroy(amdf_xdna_umd_context_t* context) {
  if (context->kernel_execution != NULL) {
    const amdf_status_t status =
        amdf_windows_xdna_kernel_execution_prepare_context_destroy(
            context->kernel_execution);
    if (!amdf_status_is_ok(status)) {
      return status;
    }
  }
  if (context->handle != 0) {
    D3DKMT_DESTROYCONTEXT destroy = {0};
    destroy.hContext = context->handle;
    const amdf_status_t status =
        amdf_kmt_make_status(context->device->kmt->destroy_context(&destroy));
    if (!amdf_status_is_ok(status)) {
      return status;
    }
    context->handle = 0;
  }
  if (context->kernel_execution != NULL) {
    const amdf_status_t status =
        amdf_windows_xdna_kernel_execution_destroy(context->kernel_execution);
    if (!amdf_status_is_ok(status)) {
      return status;
    }
    context->kernel_execution = NULL;
  }
  const amdf_status_t buffer_status =
      amdf_windows_xdna_private_allocation_destroy(&context->kernel_buffer);
  if (!amdf_status_is_ok(buffer_status)) return buffer_status;
  const amdf_allocator_t host_allocator = context->device->host_allocator;
  amdf_free(host_allocator, context);
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_xdna_umd_context_create(
    amdf_xdna_umd_device_t* device,
    const amdf_xdna_context_create_info_t* create_info,
    amdf_xdna_umd_context_t** out_context,
    amdf_xdna_umd_context_result_t* out_result) {
  const amdf_xdna_device_profile_t* profile = device->profile;
  // Device/paging resources also serve ordinary memory and do not establish
  // support for this context's interpreter bootstrap or private wire ABI.
  if ((profile->execution_capabilities &
       AMDF_XDNA_EXECUTION_CAPABILITY_TRANSACTION_INTERPRETER_V1) == 0 ||
      device->kmt->create_context_virtual == NULL ||
      device->kmt->destroy_context == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  if ((create_info->acceptable_scheduling_modes &
       AMDF_XDNA_SCHEDULING_MODE_TIME_SLICED) == 0 ||
      create_info->physical_column_origin !=
          AMDF_XDNA_PHYSICAL_COLUMN_ORIGIN_ANY) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }

  amdf_windows_xdna_adapter_info_t adapter_info = {0};
  amdf_status_t status = amdf_windows_xdna_adapter_info_query(
      device->kmt, device->adapter, &adapter_info);
  if (!amdf_status_is_ok(status)) return status;
  if (!amdf_kmt_api_supports_memory(device->kmt) || device->kmt->lock == NULL ||
      device->kmt->unlock == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  amdf_xdna_umd_context_t* context = NULL;
  status = amdf_calloc(device->host_allocator, sizeof(*context),
                       amdf_alignof(amdf_xdna_umd_context_t), (void**)&context);
  if (!amdf_status_is_ok(status)) return status;
  context->device = device;
  context->adapter_info = adapter_info;

  const amdf_windows_xdna_private_allocation_descriptor_t descriptor = {
      .requested_byte_length = 4096,
      .allocation_byte_length = 4096,
      .type = 0x332C,
      .policy = 2,
      .xcl_flags = 0x02000000,
      .flags = AMDF_WINDOWS_XDNA_PRIVATE_ALLOCATION_FLAG_DEVICE_ADDRESS |
               (adapter_info.shared_kernel_buffers
                    ? AMDF_WINDOWS_XDNA_PRIVATE_ALLOCATION_FLAG_SHARED_RESOURCE
                    : 0),
  };
  amdf_windows_xdna_private_allocation_initialize(device, &descriptor,
                                                  &context->kernel_buffer);
  status =
      amdf_windows_xdna_private_allocation_realize(&context->kernel_buffer);
  if (amdf_status_is_ok(status)) {
    status = amdf_windows_xdna_private_allocation_lock(&context->kernel_buffer);
  }
  amdf_windows_xdna_direct_context_t context_data = {
      .command_aperture_byte_length = AMDF_WINDOWS_XDNA_PRIVATE_APERTURE_SIZE,
      .process_id = GetCurrentProcessId(),
      .column_count = create_info->logical_column_count,
      .kernel_buffer_allocation = context->kernel_buffer.allocation,
      .kernel_buffer_byte_length =
          (uint32_t)context->kernel_buffer.descriptor.allocation_byte_length,
      .kernel_buffer_host_address =
          (uintptr_t)context->kernel_buffer.host_pointer,
  };

  D3DKMT_CREATECONTEXTVIRTUAL create = {0};
  create.hDevice = device->device;
  create.NodeOrdinal = 0;
  create.EngineAffinity = 1;
  create.Flags.HwQueueSupported = 1;
  create.pPrivateDriverData = &context_data;
  create.PrivateDriverDataSize = sizeof(context_data);
  create.ClientHint = (D3DKMT_CLIENTHINT)25;
  if (amdf_status_is_ok(status)) {
    status = amdf_kmt_make_status(device->kmt->create_context_virtual(&create));
  }
  if (amdf_status_is_ok(status)) {
    context->handle = create.hContext;
    if (context->handle == 0) {
      status = amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
    } else {
      context->command_aperture_cookie = context_data.command_aperture_cookie;
      if (context->command_aperture_cookie > UINT8_MAX) {
        status = amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
      }
    }
  }

  if (amdf_status_is_ok(status)) {
    status = amdf_windows_xdna_kernel_execution_create(
        context, &context->kernel_execution);
  }
  if (amdf_status_is_ok(status)) {
    amdf_xdna_umd_context_result_t result = {0};
    result.id.words[0] = (uintptr_t)context;
    result.id.words[1] = ((uint64_t)context->handle << 32) | device->device;
    result.scheduling_mode = AMDF_XDNA_SCHEDULING_MODE_TIME_SLICED;
    *out_result = result;
    *out_context = context;
  } else {
    const amdf_status_t release_status = amdf_xdna_umd_context_destroy(context);
    if (!amdf_status_is_ok(release_status)) {
      // No execution work was accepted. A failed native release leaks its
      // backing; retaining unreachable host bookkeeping cannot recover it.
      amdf_free(device->host_allocator, context);
      status = release_status;
    }
  }
  return status;
}
