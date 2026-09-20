// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Immutable captured locations, independent of compiler IR and VM ownership.
//
// All integers are little-endian. Offsets are relative to the beginning of the
// supplied byte span, with no alignment requirement or embedded pointers.
// The 16-byte header contains "LLOC", version u32, total byte length u32, and
// node count u32. A nonempty postorder node table immediately follows; its
// final node is the root. Child indices always refer to preceding nodes.
// Each 16-byte node contains kind u8, source flags u8, zero u16, payload offset
// u32, payload length u32, and zero u32. Variable payloads follow the table.
//
// Unknown nodes have no payload. File payloads contain name offset/length,
// start line/column, exclusive end line/column, field offset/count, and source
// text offset/length (ten u32s). A field contains kind, index, and four range
// coordinates (six u32s). Coordinates are one-based UTF-8 byte positions; zero
// denotes unavailable coordinates. Source text contains the exact complete
// lines starting at start_line and spanning the range. A text offset of zero
// denotes unavailable text; present empty text has a nonzero offset.
// Fused payloads contain ordered u32 child indices. Opaque payloads contain
// namespace offset/length and uninterpreted data offset/length (four u32s).
// Tagged payloads contain tag, optional child (UINT32_MAX when absent), and
// uninterpreted data offset/length (four u32s). Tags retain their source tag
// identity and flags retain their source flag bits; neither borrows IR tables.

#ifndef LOOM_FORMAT_LOCATION_H_
#define LOOM_FORMAT_LOCATION_H_

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOOM_LOCATION_VALUE_VERSION 1u
#define LOOM_LOCATION_VALUE_HEADER_LENGTH 16u
#define LOOM_LOCATION_VALUE_NODE_LENGTH 16u
#define LOOM_LOCATION_VALUE_FILE_LENGTH 40u
#define LOOM_LOCATION_VALUE_FIELD_LENGTH 24u
#define LOOM_LOCATION_VALUE_OPAQUE_LENGTH 16u
#define LOOM_LOCATION_VALUE_TAGGED_LENGTH 16u

typedef enum loom_location_value_kind_e {
  LOOM_LOCATION_VALUE_UNKNOWN = 0,
  LOOM_LOCATION_VALUE_FILE = 1,
  LOOM_LOCATION_VALUE_FUSED = 2,
  LOOM_LOCATION_VALUE_OPAQUE = 3,
  LOOM_LOCATION_VALUE_TAGGED = 4,
} loom_location_value_kind_t;

enum loom_location_value_flag_bits_e {
  // The captured source marks this node as compiler-generated.
  LOOM_LOCATION_VALUE_FLAG_SYNTHETIC = 1u << 0,
};
typedef uint8_t loom_location_value_flags_t;

// Validated view borrowing the supplied immutable bytes. The caller retains
// their owner (for example a VM buffer) throughout use of this view.
typedef struct loom_location_value_t {
  // Complete bounded value, including the header and referenced payloads.
  iree_const_byte_span_t bytes;
  // Number of postorder nodes; the last node is the root.
  uint32_t node_count;
} loom_location_value_t;

typedef struct loom_location_value_range_t {
  // One-based line containing the first byte, or zero when unknown.
  uint32_t start_line;
  // One-based byte column of the first byte, or zero when unknown.
  uint32_t start_column;
  // One-based line containing the exclusive end, or zero when unknown.
  uint32_t end_line;
  // One-based byte column of the exclusive end, or zero when unknown.
  uint32_t end_column;
} loom_location_value_range_t;

typedef struct loom_location_value_file_t {
  // Borrowed original source name.
  iree_string_view_t source;
  // Authored range in the original source coordinate space.
  loom_location_value_range_t range;
  // Borrowed exact source lines, empty when text is absent or empty.
  iree_const_byte_span_t text;
  // Distinguishes absent source text from a present empty source snapshot.
  bool has_text;
  // Number of captured field ranges.
  uint32_t field_count;
} loom_location_value_file_t;

typedef struct loom_location_value_field_t {
  // Stable source field category: operand 0, result 1, attribute 2, region 3,
  // or successor 4.
  uint32_t kind;
  // Zero-based index within the field category.
  uint32_t index;
  // Authored field range in the containing file's coordinate space.
  loom_location_value_range_t range;
} loom_location_value_field_t;

typedef struct loom_location_value_opaque_t {
  // Borrowed external provenance namespace.
  iree_string_view_t source;
  // Borrowed uninterpreted external provenance bytes.
  iree_const_byte_span_t data;
} loom_location_value_opaque_t;

typedef struct loom_location_value_tagged_t {
  // Stable nonzero source tag identity.
  uint32_t tag;
  // Earlier child node index, or UINT32_MAX when no child is present.
  uint32_t child;
  // Borrowed uninterpreted tag payload.
  iree_const_byte_span_t data;
} loom_location_value_tagged_t;

// Validates external bytes once, without allocating or taking ownership.
// Accessors below consume a validated immutable value and the indicated kind.
iree_status_t loom_location_value_parse(iree_const_byte_span_t bytes,
                                        loom_location_value_t* out_value);

loom_location_value_kind_t loom_location_value_kind(loom_location_value_t value,
                                                    uint32_t node);
loom_location_value_flags_t loom_location_value_flags(
    loom_location_value_t value, uint32_t node);
loom_location_value_file_t loom_location_value_file(loom_location_value_t value,
                                                    uint32_t node);
loom_location_value_field_t loom_location_value_field(
    loom_location_value_t value, uint32_t node, uint32_t field);
uint32_t loom_location_value_child_count(loom_location_value_t value,
                                         uint32_t node);
uint32_t loom_location_value_child(loom_location_value_t value, uint32_t node,
                                   uint32_t child);
loom_location_value_opaque_t loom_location_value_opaque(
    loom_location_value_t value, uint32_t node);
loom_location_value_tagged_t loom_location_value_tagged(
    loom_location_value_t value, uint32_t node);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_FORMAT_LOCATION_H_
