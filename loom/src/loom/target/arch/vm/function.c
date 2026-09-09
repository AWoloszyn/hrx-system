// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/vm/function.h"

#include <string.h>

#include "iree/vm/bytecode/wire/core.h"
#include "loom/codegen/low/allocation/move_sequence.h"
#include "loom/codegen/low/frame.h"
#include "loom/ops/low/ops.h"
#include "loom/target/arch/vm/descriptors/descriptors.h"

// Branch displacement fields are patched after the single emission walk. Dense
// target indices come directly from the shared CFG, in scheduled block order.
typedef struct loom_vm_branch_fixup_t {
  // Absolute stream offset of the signed word-displacement field.
  iree_io_stream_pos_t offset;
  // Dense target block index in the shared schedule.
  uint16_t target_block;
} loom_vm_branch_fixup_t;

static iree_status_t loom_vm_function_moves(const loom_low_move_t* moves,
                                            loom_low_move_range_t range,
                                            iree_io_stream_t* stream) {
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0; i < range.count && iree_status_is_ok(status);
       ++i) {
    const loom_low_move_t* move = &moves[range.start + i];
    const iree_vm_bytecode_value_copy_t instruction = {
        .opcode = IREE_VM_BYTECODE_OPCODE_VALUE_COPY,
        .destination_v8 = (uint8_t)move->destination.location,
        .source_v8 = (uint8_t)move->source.location,
    };
    status = iree_io_stream_write(stream, sizeof(instruction), &instruction);
  }
  return status;
}

static iree_status_t loom_vm_function_branch(
    iree_io_stream_t* stream, uint8_t opcode, uint8_t condition,
    uint16_t target_block, loom_vm_branch_fixup_t* out_fixup) {
  // The wide branch family shares one encoding. An unconditional branch has
  // zero in the condition byte, which is reserved padding in that form.
  const iree_vm_bytecode_control_branch_if_s32_t instruction = {
      .opcode = opcode,
      .condition_v8 = condition,
  };
  *out_fixup = (loom_vm_branch_fixup_t){
      .offset = iree_io_stream_offset(stream) +
                offsetof(iree_vm_bytecode_control_branch_if_s32_t,
                         target_word_offset_s32),
      .target_block = target_block,
  };
  return iree_io_stream_write(stream, sizeof(instruction), &instruction);
}

// Only returned values remain live at a return boundary. A cycle temporary
// can use any value register absent from that parallel move group; it does
// not reserve a register throughout the function or enlarge unrelated frames.
static iree_status_t loom_vm_return_temporary(
    void* user_data, const loom_low_move_location_t* storage_class,
    const loom_low_move_t* moves, iree_host_size_t move_count,
    loom_low_move_location_t* out_temporary, bool* out_resolved) {
  uint16_t* register_count = user_data;
  bool occupied[256] = {false};
  for (iree_host_size_t i = 0; i < move_count; ++i) {
    occupied[moves[i].source.location] = true;
    occupied[moves[i].destination.location] = true;
  }
  uint32_t location = 0;
  while (occupied[location]) ++location;
  *out_temporary = *storage_class;
  out_temporary->location = location;
  *register_count = iree_max(*register_count, location + 1);
  *out_resolved = true;
  return iree_ok_status();
}

static iree_status_t loom_vm_function_return(
    const loom_low_emission_frame_t* frame,
    const loom_low_schedule_node_t* node,
    loom_low_move_sequence_scratch_t* scratch, iree_io_stream_t* stream,
    uint16_t* register_count) {
  const loom_value_ordinal_t* ordinals =
      loom_low_schedule_node_const_operand_ordinals(node);
  for (uint16_t i = 0; i < node->operand_count; ++i) {
    const loom_low_allocation_assignment_t* assignment =
        loom_low_allocation_assignment_for_value_ordinal(&frame->allocation,
                                                         ordinals[i], NULL);
    const loom_low_move_location_t source = {
        .location_kind = LOOM_LOW_ALLOCATION_LOCATION_PHYSICAL_REGISTER,
        .value_class = assignment->value_class,
        .descriptor_reg_class_id = VM_CORE_REG_CLASS_ID_VALUE,
        .location = assignment->location_base,
    };
    scratch->moves[i] =
        (loom_low_move_t){.source = source, .destination = source};
    scratch->moves[i].destination.location = i;
  }
  const loom_low_move_sequence_options_t options = {
      .descriptor_set = frame->target.descriptor_set,
      .resolve_temporary = {.fn = loom_vm_return_temporary,
                            .user_data = register_count},
  };
  // The direct value ABI has at most 16 results. Each cycle adds at most one
  // save for two original moves, so this also covers every cyclic permutation.
  loom_low_move_t moves[24];
  iree_host_size_t move_count = 0;
  bool complete = false;
  IREE_RETURN_IF_ERROR(loom_low_move_sequence_resolve(
      scratch, node->operand_count, &options, IREE_ARRAYSIZE(moves), moves,
      &move_count, &complete));
  // Capacity covers the worst permutation and the temporary always resolves.
  IREE_ASSERT(complete);
  IREE_RETURN_IF_ERROR(loom_vm_function_moves(
      moves, (loom_low_move_range_t){.count = move_count}, stream));
  const iree_vm_bytecode_control_return_t instruction = {
      .opcode = IREE_VM_BYTECODE_OPCODE_CONTROL_RETURN,
  };
  return iree_io_stream_write(stream, sizeof(instruction), &instruction);
}

static uint64_t loom_vm_function_immediate(
    const loom_low_emission_frame_t* frame,
    const loom_low_immediate_t* immediate, loom_attribute_t value) {
  if (value.kind != LOOM_ATTR_STRING) return (uint64_t)value.i64;
  // Verified Low permits either an integer ordinal or a named enum token.
  // Compiler lowering emits ordinals; authored assembly can use either form.
  const loom_low_descriptor_set_t* descriptors = frame->target.descriptor_set;
  const loom_low_enum_domain_t* domain =
      &descriptors->enum_domains[immediate->enum_domain_id];
  const iree_string_view_t token =
      frame->module->strings.entries[value.string_id];
  for (uint16_t i = 0; i < domain->value_count; ++i) {
    const loom_low_enum_value_t* entry =
        &descriptors->enum_values[domain->value_start + i];
    if (iree_string_view_equal(token,
                               loom_low_descriptor_set_string(
                                   descriptors, entry->token_string_offset))) {
      return (uint64_t)entry->value;
    }
  }
  IREE_BUILTIN_UNREACHABLE();
}

static iree_status_t loom_vm_function_packet(
    const loom_low_emission_frame_t* frame,
    const loom_low_schedule_node_t* node, iree_io_stream_t* stream) {
  const loom_low_descriptor_t* descriptor = node->descriptor;
  const loom_low_operand_t* operands =
      frame->target.descriptor_set->operands + descriptor->operand_start;
  const loom_value_ordinal_t* results =
      loom_low_schedule_node_const_result_ordinals(node);
  const loom_value_ordinal_t* inputs =
      loom_low_schedule_node_const_operand_ordinals(node);
  uint8_t packet[sizeof(iree_vm_bytecode_constant_i64_t)] = {
      (uint8_t)descriptor->encoding_id,
  };
  for (uint16_t i = 0; i < descriptor->operand_count; ++i) {
    const loom_low_operand_t* operand = &operands[i];
    const loom_value_ordinal_t ordinal =
        (operand->role == LOOM_LOW_OPERAND_ROLE_RESULT
             ? results
             : inputs)[operand->source_value_index];
    const loom_low_allocation_assignment_t* assignment =
        loom_low_allocation_assignment_for_value_ordinal(&frame->allocation,
                                                         ordinal, NULL);
    packet[operand->encoding_field_id] = (uint8_t)assignment->location_base;
  }
  if (descriptor->immediate_count) {
    // The projection and Low verifier establish one required immediate, so its
    // dictionary position is unambiguous and needs no attribute-name lookup.
    const loom_named_attr_slice_t attributes =
        loom_low_const_isa(node->op) ? loom_low_const_attrs(node->op)
                                     : loom_low_op_attrs(node->op);
    const loom_low_immediate_t* immediate =
        &frame->target.descriptor_set->immediates[descriptor->immediate_start];
    const uint64_t bits = loom_vm_function_immediate(
        frame, immediate, attributes.entries[0].value);
    memcpy(packet + immediate->encoding_field_id, &bits,
           immediate->bit_width / 8);
  }
  return iree_io_stream_write(stream, descriptor->encoding_format_id, packet);
}

iree_status_t loom_vm_function_emit(
    const loom_target_emit_request_t* request, loom_func_like_t function,
    const loom_target_facts_t* target_facts, iree_io_stream_t* stream,
    iree_vm_bytecode_v0_function_row_t* out_row) {
  uint16_t argument_count = 0;
  const loom_value_id_t* arguments =
      loom_func_like_arg_ids(function, &argument_count);
  const loom_value_slice_t results = loom_low_func_def_results(function.op);
  if (argument_count > 16 || results.count > 16) {
    return iree_make_status(
        IREE_STATUS_UNIMPLEMENTED,
        "VM overflow arguments and results require stack ABI lowering");
  }
  loom_low_allocation_fixed_value_t fixed_values[16];
  for (uint16_t i = 0; i < argument_count; ++i) {
    fixed_values[i] = (loom_low_allocation_fixed_value_t){
        .value_id = arguments[i],
        .location_kind = LOOM_LOW_ALLOCATION_LOCATION_PHYSICAL_REGISTER,
        .location_base = i,
        .location_count = 1,
    };
  }
  const loom_low_emission_frame_options_t options = {
      .descriptor_registry = request->low_descriptor_registry,
      .function_target_facts = target_facts,
      .schedule_strategy = LOOM_LOW_SCHEDULE_STRATEGY_SOURCE_PRIORITY,
      .allocation_fixed_values = fixed_values,
      .allocation_fixed_value_count = argument_count,
      .emitter = request->diagnostic_emitter,
  };
  loom_low_emission_frame_t frame = {0};
  IREE_RETURN_IF_ERROR(loom_low_emission_frame_build(
      request->module, function.op, &options, request->scratch_arena, &frame));
  if (frame.schedule.error_count || frame.allocation.error_count) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "VM scheduling or allocation failed");
  }
  if (frame.allocation.spill_count) {
    return iree_make_status(
        IREE_STATUS_UNIMPLEMENTED,
        "VM register spills require stack instruction lowering");
  }
  out_row->value_register_count_u16 =
      (uint16_t)iree_max(iree_max(argument_count, results.count),
                         frame.allocation.physical_extents
                             .ends_by_reg_class[VM_CORE_REG_CLASS_ID_VALUE]);
  out_row->block_count_u32 = (uint32_t)frame.schedule.block_count;
  loom_low_move_sequence_scratch_t return_scratch = {0};
  IREE_RETURN_IF_ERROR(loom_low_move_sequence_scratch_initialize(
      request->scratch_arena, results.count, &return_scratch));
  const loom_cfg_graph_t* graph = &frame.schedule.cfg_graph;
  iree_io_stream_pos_t* block_offsets = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      request->scratch_arena, graph->block_count, sizeof(*block_offsets),
      (void**)&block_offsets));
  loom_vm_branch_fixup_t* fixups = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(request->scratch_arena, graph->edge_count,
                                sizeof(*fixups), (void**)&fixups));
  iree_host_size_t fixup_count = 0;
  iree_host_size_t edge_copy_index = 0;

  const iree_io_stream_pos_t start = iree_io_stream_offset(stream);
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t b = 0;
       b < frame.schedule.block_count && iree_status_is_ok(status); ++b) {
    const loom_low_schedule_block_t* block = &frame.schedule.blocks[b];
    block_offsets[b] = iree_io_stream_offset(stream);
    const iree_vm_bytecode_control_block_t instruction = {
        .opcode = IREE_VM_BYTECODE_OPCODE_CONTROL_BLOCK,
    };
    status = iree_io_stream_write(stream, sizeof(instruction), &instruction);
    for (uint32_t i = 0;
         i < block->scheduled_node_count && iree_status_is_ok(status); ++i) {
      const uint32_t node_index =
          frame.schedule
              .scheduled_node_indices[block->scheduled_node_start + i];
      const loom_low_schedule_node_t* node = &frame.schedule.nodes[node_index];
      if (node->descriptor) {
        status = loom_vm_function_packet(&frame, node, stream);
      } else if (loom_low_return_isa(node->op)) {
        status = loom_vm_function_return(&frame, node, &return_scratch, stream,
                                         &out_row->value_register_count_u16);
      } else if (loom_low_br_isa(node->op)) {
        // Allocation records one group for each payload-bearing branch, in
        // source block order, including groups whose moves all coalesced.
        if (node->operand_count) {
          const loom_low_move_range_t range =
              frame.allocation.edge_copy_groups[edge_copy_index++]
                  .move_group.moves;
          status =
              loom_vm_function_moves(frame.allocation.moves, range, stream);
        }
        const uint16_t target =
            graph->successor_indices[graph->blocks[b].successor_start];
        if (iree_status_is_ok(status) && target != b + 1) {
          status = loom_vm_function_branch(
              stream, IREE_VM_BYTECODE_OPCODE_CONTROL_BRANCH_S32, 0, target,
              &fixups[fixup_count++]);
        }
      } else if (loom_low_cond_br_isa(node->op)) {
        const uint16_t* targets =
            graph->successor_indices + graph->blocks[b].successor_start;
        const loom_value_ordinal_t condition_ordinal =
            loom_low_schedule_node_const_operand_ordinals(node)[0];
        const uint8_t condition =
            (uint8_t)loom_low_allocation_assignment_for_value_ordinal(
                &frame.allocation, condition_ordinal, NULL)
                ->location_base;
        // Invert the condition when the true edge can fall through. Otherwise
        // branch on true and emit the false jump only if it cannot fall
        // through.
        if (targets[0] == b + 1) {
          status = loom_vm_function_branch(
              stream, IREE_VM_BYTECODE_OPCODE_CONTROL_BRANCH_UNLESS_S32,
              condition, targets[1], &fixups[fixup_count++]);
        } else {
          status = loom_vm_function_branch(
              stream, IREE_VM_BYTECODE_OPCODE_CONTROL_BRANCH_IF_S32, condition,
              targets[0], &fixups[fixup_count++]);
          if (iree_status_is_ok(status) && targets[1] != b + 1) {
            status = loom_vm_function_branch(
                stream, IREE_VM_BYTECODE_OPCODE_CONTROL_BRANCH_S32, 0,
                targets[1], &fixups[fixup_count++]);
          }
        }
      } else if (loom_low_copy_isa(node->op) || loom_low_move_isa(node->op) ||
                 loom_low_slice_isa(node->op) ||
                 loom_low_concat_isa(node->op)) {
        const loom_low_allocation_packet_move_group_t* group =
            loom_low_allocation_find_packet_move_group_by_source_ordinal(
                &frame.allocation, node->source_ordinal);
        if (group) {
          const loom_low_move_range_t range = group->move_group.moves;
          status =
              loom_vm_function_moves(frame.allocation.moves, range, stream);
        }
      } else {
        status = iree_make_status(
            IREE_STATUS_UNIMPLEMENTED,
            "VM instruction emission for '%.*s' is unavailable",
            (int)loom_op_name(request->module, node->op).size,
            loom_op_name(request->module, node->op).data);
      }
    }
  }
  if (iree_status_is_ok(status)) {
    const iree_io_stream_pos_t end = iree_io_stream_offset(stream);
    const iree_io_stream_pos_t length = end - start;
    if (length > INT32_MAX) {
      status =
          iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                           "VM function bytecode exceeds signed 32-bit extent");
    } else {
      out_row->bytecode_length_u32 = (uint32_t)length;
    }
    for (iree_host_size_t i = 0; i < fixup_count && iree_status_is_ok(status);
         ++i) {
      const loom_vm_branch_fixup_t fixup = fixups[i];
      const int32_t displacement =
          (int32_t)((block_offsets[fixup.target_block] - fixup.offset -
                     (iree_io_stream_pos_t)sizeof(int32_t)) /
                    4);
      status =
          iree_io_stream_seek(stream, IREE_IO_STREAM_SEEK_SET, fixup.offset);
      if (iree_status_is_ok(status)) {
        status =
            iree_io_stream_write(stream, sizeof(displacement), &displacement);
      }
    }
    if (fixup_count && iree_status_is_ok(status)) {
      status = iree_io_stream_seek(stream, IREE_IO_STREAM_SEEK_SET, end);
    }
  }
  return status;
}
