// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/device_profile.h"

#include <string.h>

#include "libamdf/src/xdna/target/npu4/bootstrap.h"
#include "libamdf/src/xdna/target/npu5/bootstrap.h"

bool amdf_xdna_query_endpoint_info(const amdf_endpoint_info_t* endpoint_info,
                                   amdf_xdna_endpoint_info_t* out_info) {
  if (endpoint_info->engine_kind != AMDF_ENGINE_KIND_XDNA ||
      endpoint_info->pci.vendor_id != 0x1022u) {
    return false;
  }
  amdf_xdna_endpoint_info_t info = {
      .type = AMDF_STRUCTURE_TYPE_XDNA_ENDPOINT_INFO,
      .structure_size = sizeof(info),
  };
  const char* target_id = NULL;
  if (endpoint_info->pci.device_id == 0x1502u &&
      endpoint_info->pci.revision_id == 0x00u) {
    info.architecture = AMDF_XDNA_ARCHITECTURE_AIE2;
    target_id = "amd.xdna.phoenix.1502_00";
  } else if (endpoint_info->pci.device_id == 0x17F0u) {
    info.architecture = AMDF_XDNA_ARCHITECTURE_AIE2P;
    switch (endpoint_info->pci.revision_id) {
      case 0x10u:
        target_id = "amd.xdna.strix.17f0_10";
        break;
      case 0x11u:
        target_id = "amd.xdna.strix_halo.17f0_11";
        break;
      case 0x20u:
        target_id = "amd.xdna.krackan.17f0_20";
        break;
    }
  }
  if (target_id == NULL) return false;
  memcpy(info.target_id, target_id, strlen(target_id) + 1);
  *out_info = info;
  return true;
}

bool amdf_xdna_device_profile_initialize(
    const amdf_endpoint_info_t* endpoint_info,
    amdf_xdna_device_info_t* device_info,
    amdf_xdna_device_profile_t* out_profile) {
  amdf_xdna_endpoint_info_t identity;
  if (!amdf_xdna_query_endpoint_info(endpoint_info, &identity)) return false;

  amdf_xdna_device_info_t info = {
      .type = AMDF_STRUCTURE_TYPE_XDNA_DEVICE_INFO,
      .structure_size = sizeof(info),
  };
  amdf_xdna_device_profile_t profile = {.info = device_info};
  if (identity.architecture == AMDF_XDNA_ARCHITECTURE_AIE2P) {
    // AIE2P register and transaction encodings, independent of array geometry.
    // Native metadata supplies all row and column counts during activation.
    info.array.column_stride = UINT64_C(1) << 25;
    info.context.minimum_column_count = 1;
    info.context.column_count_granularity = 1;
    info.instruction.maximum_byte_length = UINT32_MAX & ~UINT64_C(3);
    info.instruction.address_alignment = 32u * 1024u;
    info.instruction.byte_length_granularity = 4;
    info.instruction.format = (amdf_xdna_binary_format_info_t){
        .format = AMDF_XDNA_BINARY_FORMAT_TRANSACTION,
        .version = AMDF_XDNA_TRANSACTION_FORMAT_VERSION_0_1,
    };
    profile.execution_capabilities =
        AMDF_XDNA_EXECUTION_CAPABILITY_TRANSACTION_INTERPRETER_V1 |
        AMDF_XDNA_EXECUTION_CAPABILITY_ELF_INSTRUCTIONS;
    // Both firmware families admit the generated no-effects PDI. Strix Halo
    // uses different native context accounting from Strix and Krackan.
    profile.bootstrap = endpoint_info->pci.revision_id == 0x11u
                            ? &amdf_xdna_npu5_bootstrap
                            : &amdf_xdna_npu4_bootstrap;
    profile.firmware_heap_byte_length = 64u * 1024u * 1024u;
    profile.dma.byte_offset = UINT32_C(0x80000000);
    profile.dma.address_bit_count = 48;
    profile.transaction.device_generation = 4;
  }
  *device_info = info;
  *out_profile = profile;
  return true;
}
