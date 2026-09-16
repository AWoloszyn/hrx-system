// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/bootstrap.h"

#include <string.h>

// Unsigned Versal partial-PDI framing: SMAP identification, image-header table,
// one image header, one partition header, and a 16-byte-aligned CDO partition.
// Header offsets and partition lengths are encoded in four-byte words.
enum {
  AMDF_XDNA_PDI_TABLE_OFFSET = 16,
  AMDF_XDNA_PDI_TABLE_LENGTH = 128,
  AMDF_XDNA_PDI_IMAGE_OFFSET = 144,
  AMDF_XDNA_PDI_IMAGE_LENGTH = 64,
  AMDF_XDNA_PDI_PARTITION_OFFSET = 208,
  AMDF_XDNA_PDI_PARTITION_LENGTH = 128,
  AMDF_XDNA_PDI_CDO_OFFSET = 336,
  AMDF_XDNA_PDI_CDO_LENGTH = 24,
  AMDF_XDNA_PDI_CDO_STORAGE_LENGTH = 32,
};

_Static_assert(AMDF_XDNA_PDI_CDO_OFFSET + AMDF_XDNA_PDI_CDO_STORAGE_LENGTH ==
                   AMDF_XDNA_BOOTSTRAP_PDI_BYTE_LENGTH,
               "bootstrap output must cover the complete aligned partition");

static void amdf_xdna_bootstrap_write_u32(uint8_t* bytes, uint32_t value) {
  for (uint32_t i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (i * 8));
}

// Each header ends with the complement of the sum of its preceding LE words.
static void amdf_xdna_bootstrap_write_checksum(uint8_t* bytes,
                                               uint32_t byte_length) {
  uint32_t sum = 0;
  for (uint32_t offset = 0; offset < byte_length - 4; offset += 4) {
    sum += (uint32_t)bytes[offset] | ((uint32_t)bytes[offset + 1] << 8) |
           ((uint32_t)bytes[offset + 2] << 16) |
           ((uint32_t)bytes[offset + 3] << 24);
  }
  amdf_xdna_bootstrap_write_u32(bytes + byte_length - 4, ~sum);
}

void amdf_xdna_bootstrap_write_pdi(void* target) {
  uint8_t* bytes = target;
  memset(bytes, 0, AMDF_XDNA_BOOTSTRAP_PDI_BYTE_LENGTH);

  // Partial-PDI identification for the 32-bit SMAP bus format.
  amdf_xdna_bootstrap_write_u32(bytes + 0, UINT32_C(0xDD));
  amdf_xdna_bootstrap_write_u32(bytes + 4, UINT32_C(0x11223344));
  amdf_xdna_bootstrap_write_u32(bytes + 8, UINT32_C(0x55667788));
  amdf_xdna_bootstrap_write_u32(bytes + 12, UINT32_C(0x99AABBCC));

  uint8_t* table = bytes + AMDF_XDNA_PDI_TABLE_OFFSET;
  amdf_xdna_bootstrap_write_u32(table + 0, UINT32_C(0x00040000));
  amdf_xdna_bootstrap_write_u32(table + 4, 1);  // One image.
  amdf_xdna_bootstrap_write_u32(table + 8, AMDF_XDNA_PDI_IMAGE_OFFSET / 4);
  amdf_xdna_bootstrap_write_u32(table + 12, 1);  // One partition.
  amdf_xdna_bootstrap_write_u32(table + 16, AMDF_XDNA_PDI_PARTITION_OFFSET / 4);
  // Bootgen's partial-PDI ID code and extended ID, not PCI device identity.
  amdf_xdna_bootstrap_write_u32(table + 24, UINT32_C(0x14CA8093));
  amdf_xdna_bootstrap_write_u32(table + 68, 1);
  amdf_xdna_bootstrap_write_u32(table + 40, UINT32_C(0x50504449));
  amdf_xdna_bootstrap_write_u32(
      table + 44, (AMDF_XDNA_PDI_TABLE_LENGTH / 4) |
                      ((AMDF_XDNA_PDI_IMAGE_LENGTH / 4) << 8) |
                      ((AMDF_XDNA_PDI_PARTITION_LENGTH / 4) << 16));
  amdf_xdna_bootstrap_write_u32(
      table + 48,
      (AMDF_XDNA_PDI_IMAGE_LENGTH + AMDF_XDNA_PDI_PARTITION_LENGTH) / 4);
  amdf_xdna_bootstrap_write_checksum(table, AMDF_XDNA_PDI_TABLE_LENGTH);

  uint8_t* image = bytes + AMDF_XDNA_PDI_IMAGE_OFFSET;
  amdf_xdna_bootstrap_write_u32(image + 0, AMDF_XDNA_PDI_PARTITION_OFFSET / 4);
  amdf_xdna_bootstrap_write_u32(image + 4, 1);  // One data section.
  memcpy(image + 16, "aie_image", 9);
  // Default subsystem image ID; unique, parent, and function IDs remain zero.
  amdf_xdna_bootstrap_write_u32(image + 32, UINT32_C(0x1C000000));
  amdf_xdna_bootstrap_write_checksum(image, AMDF_XDNA_PDI_IMAGE_LENGTH);

  uint8_t* partition = bytes + AMDF_XDNA_PDI_PARTITION_OFFSET;
  amdf_xdna_bootstrap_write_u32(partition + 0,
                                AMDF_XDNA_PDI_CDO_STORAGE_LENGTH / 4);
  amdf_xdna_bootstrap_write_u32(partition + 4, AMDF_XDNA_PDI_CDO_LENGTH / 4);
  amdf_xdna_bootstrap_write_u32(partition + 8,
                                AMDF_XDNA_PDI_CDO_STORAGE_LENGTH / 4);
  // CDO commands address the array themselves; the load address is unused.
  amdf_xdna_bootstrap_write_u32(partition + 24, UINT32_MAX);
  amdf_xdna_bootstrap_write_u32(partition + 28, UINT32_MAX);
  amdf_xdna_bootstrap_write_u32(partition + 32, AMDF_XDNA_PDI_CDO_OFFSET / 4);
  // CDO partition type (2 << 24) and EL3 attributes (3 << 1).
  amdf_xdna_bootstrap_write_u32(partition + 36, UINT32_C(0x02000006));
  amdf_xdna_bootstrap_write_u32(partition + 40, 1);  // One section.
  amdf_xdna_bootstrap_write_checksum(partition, AMDF_XDNA_PDI_PARTITION_LENGTH);

  uint8_t* cdo = bytes + AMDF_XDNA_PDI_CDO_OFFSET;
  amdf_xdna_bootstrap_write_u32(cdo + 0, 4);  // Header words before checksum.
  amdf_xdna_bootstrap_write_u32(cdo + 4, UINT32_C(0x004F4443));
  amdf_xdna_bootstrap_write_u32(cdo + 8, UINT32_C(0x200));  // CDO version 2.0.
  amdf_xdna_bootstrap_write_u32(cdo + 12, 1);               // One command word.
  amdf_xdna_bootstrap_write_checksum(cdo, 20);
  amdf_xdna_bootstrap_write_u32(cdo + 20,
                                UINT32_C(0x111));  // NOP, no arguments.
}
