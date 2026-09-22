// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Descriptor-backed Low assembly parsing.

#ifndef LOOM_FORMAT_TEXT_PARSER_LOW_ASM_H_
#define LOOM_FORMAT_TEXT_PARSER_LOW_ASM_H_

#include "loom/format/text/parser/context.h"

#ifdef __cplusplus
extern "C" {
#endif

// Emits a low-asm parse error with a detail string.
iree_status_t loom_parser_emit_low_asm_error(loom_parser_t* parser,
                                             loom_token_t token,
                                             iree_string_view_t detail);

// Gives the active representation provider an opportunity to diagnose an
// unknown stable descriptor key or compact-assembly mnemonic. Sets
// |out_emitted| when the provider emitted a structured diagnostic.
iree_status_t loom_parser_try_emit_unknown_low_packet_diagnostic(
    loom_parser_t* parser,
    const loom_text_low_asm_descriptor_set_t* descriptor_set,
    loom_token_t name_token, bool* out_emitted);

// Parses `asm { ... }` using the active function representation contract.
iree_status_t loom_parse_low_asm_marked_region(
    loom_parser_t* parser, const loom_region_descriptor_t* region_descriptor,
    loom_region_t** out_region);

// Parses a braced low-asm region inheriting the active descriptor set.
iree_status_t loom_parse_low_asm_inherited_region(
    loom_parser_t* parser, const loom_region_descriptor_t* region_descriptor,
    loom_region_t** out_region);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_FORMAT_TEXT_PARSER_LOW_ASM_H_
