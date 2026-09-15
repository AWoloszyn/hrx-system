// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "common/graph.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

using ::iree::Status;
using ::iree::StatusCode;
using ::iree::testing::status::StatusIs;

// Owns a dependency-free graph node using the same variable-sized allocation
// shape as production graph construction.
class GraphNodeStorage {
 public:
  GraphNodeStorage() {
    IREE_CHECK_OK(iree_allocator_malloc(iree_allocator_system(), sizeof(*node_),
                                        (void**)&node_));
    memset(node_, 0, sizeof(*node_));
  }

  ~GraphNodeStorage() { iree_allocator_free(iree_allocator_system(), node_); }

  GraphNodeStorage(const GraphNodeStorage&) = delete;
  GraphNodeStorage& operator=(const GraphNodeStorage&) = delete;

  iree_hal_streaming_graph_node_t* get() const { return node_; }

 private:
  // Allocated graph node header with no trailing dependency pointers.
  iree_hal_streaming_graph_node_t* node_ = nullptr;
};

TEST(GraphTest, KernelParameterUpdateIsFailureAtomic) {
  constexpr size_t kArgumentCount = 3;
  std::array<iree_hal_streaming_parameter_op_t, kArgumentCount> operations = {};
  for (uint16_t i = 0; i < kArgumentCount; ++i) {
    operations[i].copy = {
        /*.size=*/sizeof(uint32_t),
        /*.native_abi_destination_offset=*/
        static_cast<uint16_t>(i * sizeof(uint32_t)),
        /*.source_offset=*/static_cast<uint16_t>(i * sizeof(uint32_t)),
        /*.source_ordinal=*/static_cast<uint16_t>(i),
        /*.constant_destination_offset=*/
        static_cast<uint16_t>(i * sizeof(uint32_t)),
    };
  }

  iree_hal_streaming_symbol_t symbol = {};
  symbol.type = IREE_HAL_STREAMING_SYMBOL_TYPE_FUNCTION;
  symbol.parameters.buffer_size = kArgumentCount * sizeof(uint32_t);
  symbol.parameters.constant_bytes = kArgumentCount * sizeof(uint32_t);
  symbol.parameters.direct_arg_bytes = kArgumentCount * sizeof(uint32_t);
  symbol.parameters.copy_count = kArgumentCount;
  symbol.parameters.ops = operations.data();

  for (size_t missing_ordinal = 0; missing_ordinal < kArgumentCount;
       ++missing_ordinal) {
    iree_hal_streaming_graph_t graph = {};
    graph.host_allocator = iree_allocator_system();

    std::array<uint8_t, kArgumentCount * sizeof(uint32_t)> constants = {};
    memset(constants.data(), 0xA5, constants.size());
    const std::array<uint8_t, kArgumentCount * sizeof(uint32_t)>
        original_constants = constants;
    iree_hal_streaming_symbol_t previous_symbol = {};
    GraphNodeStorage node_storage;
    iree_hal_streaming_graph_node_t& node = *node_storage.get();
    node.graph = &graph;
    node.type = IREE_HAL_STREAMING_GRAPH_NODE_TYPE_KERNEL;
    node.attrs.kernel.symbol = &previous_symbol;
    node.attrs.kernel.grid_dim[0] = 7;
    node.attrs.kernel.grid_dim[1] = 5;
    node.attrs.kernel.grid_dim[2] = 3;
    node.attrs.kernel.block_dim[0] = 11;
    node.attrs.kernel.block_dim[1] = 13;
    node.attrs.kernel.block_dim[2] = 17;
    node.attrs.kernel.shared_memory_bytes = 19;
    node.attrs.kernel.constants =
        iree_make_const_byte_span(constants.data(), constants.size());
    node.attrs.kernel.constants_capacity = constants.size();
    std::array<iree_hal_buffer_ref_t, 1> binding_storage = {
        iree_hal_make_indirect_buffer_ref(/*buffer_slot=*/23, /*offset=*/29,
                                          /*length=*/31),
    };
    node.attrs.kernel.bindings = {
        /*.count=*/binding_storage.size(),
        /*.values=*/binding_storage.data(),
    };
    node.attrs.kernel.binding_capacity = binding_storage.size();

    std::array<uint32_t, kArgumentCount> values = {1, 2, 3};
    std::array<void*, kArgumentCount> arguments = {
        &values[0],
        &values[1],
        &values[2],
    };
    arguments[missing_ordinal] = nullptr;
    const iree_hal_streaming_dispatch_params_t params = {
        /*.grid_dim=*/{23, 29, 31},
        /*.block_dim=*/{37, 41, 43},
        /*.shared_memory_bytes=*/47,
        /*.buffer=*/arguments.data(),
        /*.buffer_size=*/0,
        /*.flags=*/IREE_HAL_STREAMING_DISPATCH_FLAG_ARGS_ARRAY,
    };

    EXPECT_THAT(Status(iree_hal_streaming_graph_set_kernel_node_params(
                    &node, &symbol, &params)),
                StatusIs(StatusCode::kInvalidArgument));
    EXPECT_EQ(original_constants, constants);
    EXPECT_EQ(&previous_symbol, node.attrs.kernel.symbol);
    EXPECT_EQ(7u, node.attrs.kernel.grid_dim[0]);
    EXPECT_EQ(5u, node.attrs.kernel.grid_dim[1]);
    EXPECT_EQ(3u, node.attrs.kernel.grid_dim[2]);
    EXPECT_EQ(11u, node.attrs.kernel.block_dim[0]);
    EXPECT_EQ(13u, node.attrs.kernel.block_dim[1]);
    EXPECT_EQ(17u, node.attrs.kernel.block_dim[2]);
    EXPECT_EQ(19u, node.attrs.kernel.shared_memory_bytes);
    EXPECT_EQ(constants.size(), node.attrs.kernel.constants.data_length);
    EXPECT_EQ(binding_storage.size(), node.attrs.kernel.bindings.count);
    EXPECT_EQ(binding_storage.data(), node.attrs.kernel.bindings.values);
    EXPECT_EQ(23u, binding_storage[0].buffer_slot);
    EXPECT_EQ(29u, binding_storage[0].offset);
    EXPECT_EQ(31u, binding_storage[0].length);
  }
}

TEST(GraphTest, KernelParameterUpdateRejectsShortPrepackedSpan) {
  iree_hal_streaming_graph_t graph = {};
  graph.host_allocator = iree_allocator_system();

  std::array<uint8_t, 16> constants = {};
  constants.fill(0x5A);
  const std::array<uint8_t, 16> original_constants = constants;
  GraphNodeStorage node_storage;
  iree_hal_streaming_graph_node_t& node = *node_storage.get();
  node.graph = &graph;
  node.type = IREE_HAL_STREAMING_GRAPH_NODE_TYPE_KERNEL;
  iree_hal_streaming_symbol_t previous_symbol = {};
  node.attrs.kernel.symbol = &previous_symbol;
  node.attrs.kernel.grid_dim[0] = 7;
  node.attrs.kernel.block_dim[0] = 11;
  node.attrs.kernel.shared_memory_bytes = 19;
  node.attrs.kernel.constants =
      iree_make_const_byte_span(constants.data(), constants.size());
  node.attrs.kernel.constants_capacity = constants.size();
  std::array<iree_hal_buffer_ref_t, 1> binding_storage = {};
  node.attrs.kernel.bindings = {
      /*.count=*/binding_storage.size(),
      /*.values=*/binding_storage.data(),
  };
  node.attrs.kernel.binding_capacity = binding_storage.size();

  iree_hal_streaming_symbol_t symbol = {};
  symbol.type = IREE_HAL_STREAMING_SYMBOL_TYPE_FUNCTION;
  symbol.parameters.constant_bytes = constants.size();
  symbol.parameters.direct_arg_bytes = constants.size();

  const iree_hal_streaming_dispatch_params_t params = {
      /*.grid_dim=*/{},
      /*.block_dim=*/{},
      /*.shared_memory_bytes=*/0,
      /*.buffer=*/reinterpret_cast<void*>(uintptr_t{1}),
      /*.buffer_size=*/0,
      /*.flags=*/IREE_HAL_STREAMING_DISPATCH_FLAG_PRE_PACKED,
  };
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_hal_streaming_graph_set_kernel_node_params(&node, &symbol, &params));
  EXPECT_EQ(original_constants, constants);
  EXPECT_EQ(&previous_symbol, node.attrs.kernel.symbol);
  EXPECT_EQ(7u, node.attrs.kernel.grid_dim[0]);
  EXPECT_EQ(11u, node.attrs.kernel.block_dim[0]);
  EXPECT_EQ(19u, node.attrs.kernel.shared_memory_bytes);
  EXPECT_EQ(constants.size(), node.attrs.kernel.constants.data_length);
  EXPECT_EQ(binding_storage.size(), node.attrs.kernel.bindings.count);
  EXPECT_EQ(binding_storage.data(), node.attrs.kernel.bindings.values);
}

TEST(GraphTest, KernelParameterUpdateCapturesPrepackedArgumentSpans) {
  iree_hal_streaming_graph_t graph = {};
  graph.host_allocator = iree_allocator_system();

  std::array<uint8_t, 24> constants = {};
  constants.fill(0xA5);
  GraphNodeStorage node_storage;
  iree_hal_streaming_graph_node_t& node = *node_storage.get();
  node.graph = &graph;
  node.type = IREE_HAL_STREAMING_GRAPH_NODE_TYPE_KERNEL;
  node.attrs.kernel.constants =
      iree_make_const_byte_span(constants.data(), constants.size());
  node.attrs.kernel.constants_capacity = constants.size();

  iree_hal_streaming_symbol_t symbol = {};
  symbol.type = IREE_HAL_STREAMING_SYMBOL_TYPE_FUNCTION;
  symbol.parameters.constant_bytes = 16;
  symbol.parameters.direct_arg_bytes = 16;

  std::array<uint8_t, 16> exact_arguments = {};
  for (uint8_t i = 0; i < exact_arguments.size(); ++i) {
    exact_arguments[i] = i;
  }
  const iree_hal_streaming_dispatch_params_t exact_params = {
      /*.grid_dim=*/{},
      /*.block_dim=*/{},
      /*.shared_memory_bytes=*/0,
      /*.buffer=*/exact_arguments.data(),
      /*.buffer_size=*/exact_arguments.size(),
      /*.flags=*/IREE_HAL_STREAMING_DISPATCH_FLAG_PRE_PACKED,
  };
  IREE_EXPECT_OK(iree_hal_streaming_graph_set_kernel_node_params(
      &node, &symbol, &exact_params));
  EXPECT_EQ(exact_arguments.size(), node.attrs.kernel.constants.data_length);
  EXPECT_EQ(0, memcmp(exact_arguments.data(), constants.data(),
                      exact_arguments.size()));
  for (size_t i = exact_arguments.size(); i < constants.size(); ++i) {
    EXPECT_EQ(0u, constants[i]);
  }
  exact_arguments.fill(0xFF);
  EXPECT_NE(0, memcmp(exact_arguments.data(), constants.data(),
                      exact_arguments.size()));

  std::array<uint8_t, 24> padded_arguments = {};
  for (uint8_t i = 0; i < padded_arguments.size(); ++i) {
    padded_arguments[i] = static_cast<uint8_t>(0x80u + i);
  }
  const iree_hal_streaming_dispatch_params_t padded_params = {
      /*.grid_dim=*/{},
      /*.block_dim=*/{},
      /*.shared_memory_bytes=*/0,
      /*.buffer=*/padded_arguments.data(),
      /*.buffer_size=*/padded_arguments.size(),
      /*.flags=*/IREE_HAL_STREAMING_DISPATCH_FLAG_PRE_PACKED,
  };
  IREE_EXPECT_OK(iree_hal_streaming_graph_set_kernel_node_params(
      &node, &symbol, &padded_params));
  EXPECT_EQ(padded_arguments.size(), node.attrs.kernel.constants.data_length);
  EXPECT_EQ(0, memcmp(padded_arguments.data(), constants.data(),
                      padded_arguments.size()));
  padded_arguments.fill(0xFF);
  EXPECT_NE(0, memcmp(padded_arguments.data(), constants.data(),
                      padded_arguments.size()));

  iree_hal_streaming_symbol_t empty_symbol = {};
  empty_symbol.type = IREE_HAL_STREAMING_SYMBOL_TYPE_FUNCTION;
  const iree_hal_streaming_dispatch_params_t empty_params = {
      /*.grid_dim=*/{},
      /*.block_dim=*/{},
      /*.shared_memory_bytes=*/0,
      /*.buffer=*/nullptr,
      /*.buffer_size=*/0,
      /*.flags=*/IREE_HAL_STREAMING_DISPATCH_FLAG_PRE_PACKED,
  };
  IREE_EXPECT_OK(iree_hal_streaming_graph_set_kernel_node_params(
      &node, &empty_symbol, &empty_params));
  EXPECT_EQ(0u, node.attrs.kernel.constants.data_length);
}

TEST(GraphTest, ArgsArrayPackingProducesCompleteNativeAbiImage) {
  iree_hal_streaming_graph_t graph = {};
  graph.host_allocator = iree_allocator_system();

  constexpr iree_host_size_t kNativeArgumentSize = 52;
  std::array<uint8_t, kNativeArgumentSize> constants;
  constants.fill(0xA5);
  std::array<iree_hal_buffer_ref_t, 2> binding_storage = {};
  GraphNodeStorage node_storage;
  iree_hal_streaming_graph_node_t& node = *node_storage.get();
  node.graph = &graph;
  node.type = IREE_HAL_STREAMING_GRAPH_NODE_TYPE_KERNEL;
  node.attrs.kernel.constants =
      iree_make_const_byte_span(constants.data(), constants.size());
  node.attrs.kernel.constants_capacity = constants.size();
  node.attrs.kernel.bindings = {
      /*.count=*/binding_storage.size(),
      /*.values=*/binding_storage.data(),
  };
  node.attrs.kernel.binding_capacity = binding_storage.size();

  std::array<iree_hal_streaming_parameter_op_t, 4> operations = {};
  operations[0].copy = {
      /*.size=*/sizeof(uint32_t),
      /*.native_abi_destination_offset=*/4,
      /*.source_offset=*/0,
      /*.source_ordinal=*/0,
      /*.constant_destination_offset=*/0,
  };
  operations[1].copy = {
      /*.size=*/sizeof(uint16_t),
      /*.native_abi_destination_offset=*/28,
      /*.source_offset=*/12,
      /*.source_ordinal=*/2,
      /*.constant_destination_offset=*/4,
  };
  operations[2].resolve = {
      /*.native_abi_destination_offset=*/16,
      /*.reserved=*/0,
      /*.source_offset=*/4,
      /*.source_ordinal=*/1,
      /*.destination_ordinal=*/1,
  };
  operations[3].resolve = {
      /*.native_abi_destination_offset=*/40,
      /*.reserved=*/0,
      /*.source_offset=*/14,
      /*.source_ordinal=*/3,
      /*.destination_ordinal=*/0,
  };
  iree_hal_streaming_symbol_t symbol = {};
  symbol.type = IREE_HAL_STREAMING_SYMBOL_TYPE_FUNCTION;
  symbol.parameters.buffer_size = 22;
  symbol.parameters.constant_bytes = 6;
  symbol.parameters.direct_arg_bytes = kNativeArgumentSize;
  symbol.parameters.binding_count = 2;
  symbol.parameters.copy_count = 2;
  symbol.parameters.ops = operations.data();

  uint32_t scalar0 = 0x11223344u;
  void* pointer1 = reinterpret_cast<void*>(uintptr_t{0x0102030405060708ull});
  uint16_t scalar2 = 0x5566u;
  void* pointer3 = reinterpret_cast<void*>(uintptr_t{0x1112131415161718ull});
  std::array<void*, 4> arguments = {
      &scalar0,
      &pointer1,
      &scalar2,
      &pointer3,
  };
  const iree_hal_streaming_dispatch_params_t params = {
      /*.grid_dim=*/{1, 1, 1},
      /*.block_dim=*/{1, 1, 1},
      /*.shared_memory_bytes=*/0,
      /*.buffer=*/arguments.data(),
      /*.buffer_size=*/0,
      /*.flags=*/IREE_HAL_STREAMING_DISPATCH_FLAG_ARGS_ARRAY,
  };

  IREE_ASSERT_OK(
      iree_hal_streaming_graph_set_kernel_node_params(&node, &symbol, &params));
  std::array<uint8_t, kNativeArgumentSize> expected = {};
  memcpy(expected.data() + 4, &scalar0, sizeof(scalar0));
  const iree_hal_streaming_deviceptr_t device_pointer1 =
      static_cast<iree_hal_streaming_deviceptr_t>(
          reinterpret_cast<uintptr_t>(pointer1));
  memcpy(expected.data() + 16, &device_pointer1, sizeof(device_pointer1));
  memcpy(expected.data() + 28, &scalar2, sizeof(scalar2));
  const iree_hal_streaming_deviceptr_t device_pointer3 =
      static_cast<iree_hal_streaming_deviceptr_t>(
          reinterpret_cast<uintptr_t>(pointer3));
  memcpy(expected.data() + 40, &device_pointer3, sizeof(device_pointer3));

  EXPECT_EQ(expected.size(), node.attrs.kernel.constants.data_length);
  EXPECT_EQ(expected, constants);
  EXPECT_EQ(0u, node.attrs.kernel.bindings.count);
}

TEST(GraphTest, ArgsArrayPackingRejectsDuplicateSourceWithoutMutation) {
  iree_hal_streaming_graph_t graph = {};
  graph.host_allocator = iree_allocator_system();

  std::array<uint8_t, 16> constants;
  constants.fill(0xA5);
  const std::array<uint8_t, 16> original_constants = constants;
  std::array<iree_hal_buffer_ref_t, 1> binding_storage = {
      iree_hal_make_indirect_buffer_ref(/*buffer_slot=*/23, /*offset=*/29,
                                        /*length=*/31),
  };
  GraphNodeStorage node_storage;
  iree_hal_streaming_graph_node_t& node = *node_storage.get();
  node.graph = &graph;
  node.type = IREE_HAL_STREAMING_GRAPH_NODE_TYPE_KERNEL;
  iree_hal_streaming_symbol_t previous_symbol = {};
  node.attrs.kernel.symbol = &previous_symbol;
  node.attrs.kernel.constants =
      iree_make_const_byte_span(constants.data(), constants.size());
  node.attrs.kernel.constants_capacity = constants.size();
  node.attrs.kernel.bindings = {
      /*.count=*/binding_storage.size(),
      /*.values=*/binding_storage.data(),
  };
  node.attrs.kernel.binding_capacity = binding_storage.size();

  std::array<iree_hal_streaming_parameter_op_t, 2> operations = {};
  operations[0].copy = {
      /*.size=*/sizeof(uint32_t),
      /*.native_abi_destination_offset=*/0,
      /*.source_offset=*/0,
      /*.source_ordinal=*/0,
      /*.constant_destination_offset=*/0,
  };
  operations[1].resolve = {
      /*.native_abi_destination_offset=*/8,
      /*.reserved=*/0,
      /*.source_offset=*/4,
      /*.source_ordinal=*/0,
      /*.destination_ordinal=*/0,
  };
  iree_hal_streaming_symbol_t symbol = {};
  symbol.type = IREE_HAL_STREAMING_SYMBOL_TYPE_FUNCTION;
  symbol.parameters.buffer_size = 12;
  symbol.parameters.constant_bytes = 4;
  symbol.parameters.direct_arg_bytes = constants.size();
  symbol.parameters.binding_count = 1;
  symbol.parameters.copy_count = 1;
  symbol.parameters.ops = operations.data();

  uint32_t value = 7;
  std::array<void*, 1> arguments = {&value};
  const iree_hal_streaming_dispatch_params_t params = {
      /*.grid_dim=*/{},
      /*.block_dim=*/{},
      /*.shared_memory_bytes=*/0,
      /*.buffer=*/arguments.data(),
      /*.buffer_size=*/0,
      /*.flags=*/IREE_HAL_STREAMING_DISPATCH_FLAG_ARGS_ARRAY,
  };

  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_hal_streaming_graph_set_kernel_node_params(&node, &symbol, &params));
  EXPECT_EQ(original_constants, constants);
  EXPECT_EQ(&previous_symbol, node.attrs.kernel.symbol);
  EXPECT_EQ(1u, node.attrs.kernel.bindings.count);
  EXPECT_EQ(23u, binding_storage[0].buffer_slot);
  EXPECT_EQ(29u, binding_storage[0].offset);
  EXPECT_EQ(31u, binding_storage[0].length);
}

struct ProbedHostAllocator {
  iree_allocator_t delegate = iree_allocator_system();
  bool fail_allocations = false;
  int allocation_attempt_count = 0;
  int successful_allocation_count = 0;
  int free_count = 0;

  static iree_status_t Control(void* self, iree_allocator_command_t command,
                               const void* params, void** inout_ptr) {
    auto* allocator = static_cast<ProbedHostAllocator*>(self);
    if (command == IREE_ALLOCATOR_COMMAND_MALLOC ||
        command == IREE_ALLOCATOR_COMMAND_CALLOC ||
        command == IREE_ALLOCATOR_COMMAND_REALLOC) {
      ++allocator->allocation_attempt_count;
      if (allocator->fail_allocations) {
        return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "injected allocation failure");
      }
      ++allocator->successful_allocation_count;
    } else if (command == IREE_ALLOCATOR_COMMAND_FREE) {
      ++allocator->free_count;
    }
    return allocator->delegate.ctl(allocator->delegate.self, command, params,
                                   inout_ptr);
  }

  iree_allocator_t AsAllocator() {
    return iree_allocator_t{this, &ProbedHostAllocator::Control};
  }
};

struct CaptureRecordGate {
  // Set once the recorder is executing under the stream mutex.
  std::atomic<bool> entered = false;
  // Set by the test after a concurrent capture end has started.
  std::atomic<bool> release = false;
  // Existing graph node published as the terminal capture frontier.
  iree_hal_streaming_graph_node_t* terminal_node = nullptr;
};

iree_status_t RecordGatedCaptureNode(
    iree_hal_streaming_graph_t* graph,
    iree_hal_streaming_graph_node_t** dependencies,
    iree_host_size_t dependency_count, void* user_data,
    iree_hal_streaming_graph_node_t** out_terminal_node) {
  (void)graph;
  (void)dependencies;
  (void)dependency_count;
  auto* gate = static_cast<CaptureRecordGate*>(user_data);
  gate->entered.store(true, std::memory_order_release);
  while (!gate->release.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  *out_terminal_node = gate->terminal_node;
  return iree_ok_status();
}

iree_status_t FailCaptureNodeRecording(
    iree_hal_streaming_graph_t* graph,
    iree_hal_streaming_graph_node_t** dependencies,
    iree_host_size_t dependency_count, void* user_data,
    iree_hal_streaming_graph_node_t** out_terminal_node) {
  (void)graph;
  (void)dependencies;
  (void)dependency_count;
  (void)user_data;
  (void)out_terminal_node;
  return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                          "injected capture construction failure");
}

// Initializes the minimum production-shaped capture state needed to exercise
// capture recording and termination without creating a device.
class CaptureTransactionTestState {
 public:
  CaptureTransactionTestState() {
    iree_slim_mutex_initialize(&context_.stream_list_mutex);
    iree_slim_mutex_initialize(&stream_.mutex);
    iree_atomic_store(&context_.capture_stream_count, 1,
                      iree_memory_order_release);
    context_.streams = streams_;
    context_.stream_count = 1;
    context_.host_allocator = iree_allocator_system();
    streams_[0] = &stream_;

    IREE_CHECK_OK(iree_allocator_malloc(iree_allocator_system(), sizeof(*node_),
                                        (void**)&node_));
    memset(node_, 0, sizeof(*node_));
    IREE_CHECK_OK(iree_allocator_malloc(
        iree_allocator_system(),
        sizeof(iree_hal_streaming_node_block_t) + sizeof(node_block_->nodes[0]),
        (void**)&node_block_));
    memset(node_block_, 0,
           sizeof(*node_block_) + sizeof(node_block_->nodes[0]));
    node_block_->capacity = 1;
    node_block_->count = 1;
    node_block_->nodes[0] = node_;
    node_->graph = &graph_;
    node_->type = IREE_HAL_STREAMING_GRAPH_NODE_TYPE_EMPTY;
    graph_.host_allocator = iree_allocator_system();
    graph_.node_blocks = node_block_;
    graph_.current_node_block = node_block_;
    graph_.node_count = 1;
    graph_.next_clone_source_node_index = 1;

    stream_.context = &context_;
    stream_.host_allocator = iree_allocator_system();
    stream_.capture_status = IREE_HAL_STREAMING_CAPTURE_STATUS_ACTIVE;
    stream_.capture_mode = IREE_HAL_STREAMING_CAPTURE_MODE_RELAXED;
    stream_.capture_graph = &graph_;
    stream_.capture_origin = true;
    stream_.capture_joined_to_origin = true;
  }

  ~CaptureTransactionTestState() {
    iree_allocator_free(stream_.host_allocator, stream_.capture_dependencies);
    iree_allocator_free(iree_allocator_system(), node_block_);
    iree_allocator_free(iree_allocator_system(), node_);
    iree_slim_mutex_deinitialize(&stream_.mutex);
    iree_slim_mutex_deinitialize(&context_.stream_list_mutex);
  }

  iree_hal_streaming_context_t* context() { return &context_; }
  iree_hal_streaming_stream_t* stream() { return &stream_; }
  iree_hal_streaming_graph_t* graph() { return &graph_; }
  iree_hal_streaming_graph_node_t* node() { return node_; }

 private:
  iree_hal_streaming_context_t context_ = {};
  iree_hal_streaming_stream_t stream_ = {};
  iree_hal_streaming_stream_t* streams_[1] = {};
  iree_hal_streaming_graph_t graph_ = {};
  iree_hal_streaming_graph_node_t* node_ = nullptr;
  iree_hal_streaming_node_block_t* node_block_ = nullptr;
};

TEST(GraphTest, CaptureRecordingSerializesTermination) {
  CaptureTransactionTestState state;
  CaptureRecordGate gate;
  gate.terminal_node = state.node();
  bool was_capturing = false;
  iree_status_t record_status = iree_ok_status();
  std::thread record_thread([&] {
    record_status = iree_hal_streaming_capture_try_record_node(
        state.stream(), RecordGatedCaptureNode, &gate, &was_capturing);
  });
  while (!gate.entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  std::atomic<bool> end_started = false;
  iree_hal_streaming_graph_t* captured_graph = nullptr;
  iree_status_t end_status = iree_ok_status();
  std::thread end_thread([&] {
    end_started.store(true, std::memory_order_release);
    end_status =
        iree_hal_streaming_end_capture(state.stream(), &captured_graph);
  });
  while (!end_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  gate.release.store(true, std::memory_order_release);
  record_thread.join();
  end_thread.join();

  IREE_EXPECT_OK(record_status);
  IREE_EXPECT_OK(end_status);
  EXPECT_TRUE(was_capturing);
  EXPECT_EQ(state.graph(), captured_graph);
}

TEST(GraphTest, CaptureRecordingFailureInvalidatesCapture) {
  CaptureTransactionTestState state;
  bool was_capturing = false;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      iree_hal_streaming_capture_try_record_node(
          state.stream(), FailCaptureNodeRecording, nullptr, &was_capturing));
  EXPECT_TRUE(was_capturing);
  EXPECT_EQ(IREE_HAL_STREAMING_CAPTURE_STATUS_INVALIDATED,
            state.stream()->capture_status);
}

TEST(GraphTest, CaptureNoOpPreservesFrontier) {
  CaptureTransactionTestState state;
  bool was_capturing = false;
  IREE_EXPECT_OK(iree_hal_streaming_capture_try_record_noop(state.stream(),
                                                            &was_capturing));
  EXPECT_TRUE(was_capturing);
  EXPECT_EQ(IREE_HAL_STREAMING_CAPTURE_STATUS_ACTIVE,
            state.stream()->capture_status);
  EXPECT_EQ(0u, state.stream()->capture_dependency_count);
  EXPECT_EQ(0u, state.stream()->capture_dependency_capacity);
  EXPECT_EQ(1u, state.graph()->node_count);
}

void InitializeSingleCopySymbol(uint16_t direct_arg_bytes,
                                uint16_t destination_offset,
                                iree_hal_streaming_parameter_op_t* operation,
                                iree_hal_streaming_symbol_t* out_symbol) {
  operation->copy = {
      /*.size=*/sizeof(uint32_t),
      /*.native_abi_destination_offset=*/destination_offset,
      /*.source_offset=*/0,
      /*.source_ordinal=*/0,
      /*.constant_destination_offset=*/0,
  };
  out_symbol->type = IREE_HAL_STREAMING_SYMBOL_TYPE_FUNCTION;
  out_symbol->parameters.buffer_size = sizeof(uint32_t);
  out_symbol->parameters.constant_bytes = sizeof(uint32_t);
  out_symbol->parameters.direct_arg_bytes = direct_arg_bytes;
  out_symbol->parameters.copy_count = 1;
  out_symbol->parameters.ops = operation;
}

TEST(GraphTest, LaunchUsesInlineArgumentStorageForSmallMetadata) {
  ProbedHostAllocator allocator;
  iree_hal_streaming_stream_t stream = {};
  stream.host_allocator = allocator.AsAllocator();
  iree_hal_streaming_parameter_op_t operation = {};
  iree_hal_streaming_symbol_t symbol = {};
  InitializeSingleCopySymbol(/*direct_arg_bytes=*/128,
                             /*destination_offset=*/64, &operation, &symbol);
  std::array<void*, 1> arguments = {nullptr};
  const iree_hal_streaming_dispatch_params_t params = {
      /*.grid_dim=*/{},
      /*.block_dim=*/{},
      /*.shared_memory_bytes=*/0,
      /*.buffer=*/arguments.data(),
      /*.buffer_size=*/0,
      /*.flags=*/IREE_HAL_STREAMING_DISPATCH_FLAG_ARGS_ARRAY,
  };

  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_hal_streaming_launch_kernel(&symbol, &params, &stream));
  EXPECT_EQ(0, allocator.allocation_attempt_count);
  EXPECT_EQ(0, allocator.free_count);
}

TEST(GraphTest, LaunchFreesHeapArgumentStorageAfterPackingFailure) {
  ProbedHostAllocator allocator;
  iree_hal_streaming_stream_t stream = {};
  stream.host_allocator = allocator.AsAllocator();
  iree_hal_streaming_parameter_op_t operation = {};
  iree_hal_streaming_symbol_t symbol = {};
  InitializeSingleCopySymbol(/*direct_arg_bytes=*/512,
                             /*destination_offset=*/256, &operation, &symbol);
  std::array<void*, 1> arguments = {nullptr};
  const iree_hal_streaming_dispatch_params_t params = {
      /*.grid_dim=*/{},
      /*.block_dim=*/{},
      /*.shared_memory_bytes=*/0,
      /*.buffer=*/arguments.data(),
      /*.buffer_size=*/0,
      /*.flags=*/IREE_HAL_STREAMING_DISPATCH_FLAG_ARGS_ARRAY,
  };

  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_hal_streaming_launch_kernel(&symbol, &params, &stream));
  EXPECT_EQ(1, allocator.allocation_attempt_count);
  EXPECT_EQ(1, allocator.successful_allocation_count);
  EXPECT_EQ(1, allocator.free_count);
}

TEST(GraphTest, LaunchReportsHeapArgumentStorageAllocationFailure) {
  ProbedHostAllocator allocator;
  allocator.fail_allocations = true;
  iree_hal_streaming_stream_t stream = {};
  stream.host_allocator = allocator.AsAllocator();
  iree_hal_streaming_parameter_op_t operation = {};
  iree_hal_streaming_symbol_t symbol = {};
  InitializeSingleCopySymbol(/*direct_arg_bytes=*/512,
                             /*destination_offset=*/256, &operation, &symbol);
  uint32_t value = 7;
  std::array<void*, 1> arguments = {&value};
  const iree_hal_streaming_dispatch_params_t params = {
      /*.grid_dim=*/{},
      /*.block_dim=*/{},
      /*.shared_memory_bytes=*/0,
      /*.buffer=*/arguments.data(),
      /*.buffer_size=*/0,
      /*.flags=*/IREE_HAL_STREAMING_DISPATCH_FLAG_ARGS_ARRAY,
  };

  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      iree_hal_streaming_launch_kernel(&symbol, &params, &stream));
  EXPECT_EQ(1, allocator.allocation_attempt_count);
  EXPECT_EQ(0, allocator.successful_allocation_count);
  EXPECT_EQ(0, allocator.free_count);
}

}  // namespace
