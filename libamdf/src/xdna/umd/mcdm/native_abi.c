// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/mcdm/native_abi.h"

static const amdf_windows_xdna_native_abi_t amdf_windows_xdna_xclbin_abi = {
    .context_encoding = AMDF_WINDOWS_XDNA_CONTEXT_ENCODING_XCLBIN,
    .context_cookie_byte_offset = 0x40,
    .submission_header_byte_length = 104,
};

static const amdf_windows_xdna_native_abi_t amdf_windows_xdna_metadata_abi = {
    .context_encoding = AMDF_WINDOWS_XDNA_CONTEXT_ENCODING_METADATA,
    .context_cookie_byte_offset = 0x30,
    .submission_header_byte_length = 88,
};

amdf_status_t amdf_windows_xdna_native_abi_query(
    const amdf_kmt_api_t* kmt, D3DKMT_HANDLE adapter,
    amdf_windows_xdna_native_abi_t* out_abi) {
  D3DKMT_KMD_DRIVER_VERSION version = {0};
  amdf_status_t status = amdf_kmt_query_adapter_info(
      kmt, adapter, KMTQAITYPE_KMD_DRIVER_VERSION, &version, sizeof(version));
  if (!amdf_status_is_ok(status)) return status;
  // This miniport's private query returns success without writing any bytes.
  // Its standard build identity admits the inspected schema, not a version
  // range or an inferred relationship between hardware and driver revisions.
  if ((uint64_t)version.DriverVersion.QuadPart ==
      UINT64_C(0x0020000000CB00F0)) {
    *out_abi = amdf_windows_xdna_metadata_abi;
    return AMDF_STATUS_OK;
  }
  uint32_t private_info[2] = {UINT32_MAX, UINT32_MAX};
  status = amdf_kmt_query_adapter_info(kmt, adapter, KMTQAITYPE_UMDRIVERPRIVATE,
                                       private_info, sizeof(private_info));
  if (status == amdf_make_status(AMDF_STATUS_DOMAIN_NTSTATUS, 0xC0000023u)) {
    // The current miniport extends the private reply with a kernel-buffer
    // allocation policy byte. It rejects the legacy reply size before writing
    // anything. Query the complete current record, including native padding.
    struct {
      // Reserved word written as zero by the native provider.
      uint32_t reserved;
      // Hardware kind, distinct from private wire-format compatibility.
      uint32_t hardware_kind;
      // One permits kernel buffers without a shared KMT resource.
      uint8_t unshared_kernel_buffers;
      // Native alignment padding, not part of the returned information.
      uint8_t padding[3];
    } current_info = {UINT32_MAX, UINT32_MAX, UINT8_MAX, {0}};
    status =
        amdf_kmt_query_adapter_info(kmt, adapter, KMTQAITYPE_UMDRIVERPRIVATE,
                                    &current_info, sizeof(current_info));
    if (!amdf_status_is_ok(status)) return status;
    if (current_info.reserved != 0 ||
        current_info.hardware_kind == UINT32_MAX ||
        current_info.unshared_kernel_buffers > 1) {
      return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
    }
    *out_abi = (amdf_windows_xdna_native_abi_t){
        .context_encoding = AMDF_WINDOWS_XDNA_CONTEXT_ENCODING_DIRECT,
        .context_cookie_byte_offset = 0x44,
        .submission_header_byte_length = 120,
        .shared_kernel_buffers = current_info.unshared_kernel_buffers == 0,
    };
    return AMDF_STATUS_OK;
  }
  if (!amdf_status_is_ok(status)) return status;
  // The second word is hardware identity, not a private ABI version.
  if (private_info[0] != 0 || private_info[1] == UINT32_MAX) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  *out_abi = amdf_windows_xdna_xclbin_abi;
  return AMDF_STATUS_OK;
}
