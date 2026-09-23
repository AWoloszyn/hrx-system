// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "iree/async/buffer_pool.h"
#include "iree/async/operations/net.h"
#include "iree/async/platform/io_uring/api.h"
#include "iree/async/slab.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

class SlabRegistrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(getrlimit(RLIMIT_MEMLOCK, &original_limit_), 0);
    page_size_ = static_cast<iree_host_size_t>(sysconf(_SC_PAGESIZE));
    iree_status_t status = iree_async_proactor_create_io_uring(
        iree_async_proactor_options_default(), iree_allocator_system(),
        &proactor_);
    if (iree_status_is_unavailable(status)) {
      iree_status_free(status);
      GTEST_SKIP() << "io_uring is unavailable";
    }
    IREE_ASSERT_OK(status);
  }

  void TearDown() override {
    if (limit_changed_) {
      EXPECT_EQ(setrlimit(RLIMIT_MEMLOCK, &original_limit_), 0);
    }
    ResetSlab();
    iree_async_proactor_release(proactor_);
  }

  void ResetSlab() {
    iree_async_region_release(region_);
    region_ = nullptr;
    iree_async_slab_release(slab_);
    slab_ = nullptr;
    if (mapping_ != MAP_FAILED) {
      EXPECT_EQ(munmap(mapping_, mapping_length_), 0);
      mapping_ = MAP_FAILED;
    }
  }

  void CreateSlab(iree_host_size_t buffer_size, iree_host_size_t buffer_count) {
    ResetSlab();
    mapping_length_ = buffer_size * buffer_count;
    mapping_ = mmap(nullptr, mapping_length_, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(mapping_, MAP_FAILED) << strerror(errno);
    ASSERT_EQ(madvise(mapping_, mapping_length_, MADV_NOHUGEPAGE), 0);
    IREE_ASSERT_OK(iree_async_slab_wrap(mapping_, buffer_size, buffer_count,
                                        iree_allocator_system(), &slab_));
  }

  void RegisterAtFirstSlot() {
    IREE_ASSERT_OK(iree_async_proactor_register_slab(
        proactor_, slab_, IREE_ASYNC_BUFFER_ACCESS_FLAG_READ, &region_));
    ASSERT_NE(region_, nullptr);
    EXPECT_EQ(region_->handles.iouring.base_buffer_index, 0);
  }

  // Proactor owning the kernel and userspace fixed-buffer tables.
  iree_async_proactor_t* proactor_ = nullptr;

  // Slab borrowing the mapped pages.
  iree_async_slab_t* slab_ = nullptr;

  // Region retained across assertions until fixture teardown.
  iree_async_region_t* region_ = nullptr;

  // Mapping owned until after all registrations and slab references expire.
  void* mapping_ = MAP_FAILED;

  // Byte length of the mapped slab storage.
  iree_host_size_t mapping_length_ = 0;

  // Native page size used to control the registration geometry.
  iree_host_size_t page_size_ = 0;

  // Original process pinning limit restored before teardown.
  struct rlimit original_limit_ = {};

  // True after the test lowers the process pinning limit.
  bool limit_changed_ = false;
};

class SlabRegistrationFaultTest : public SlabRegistrationTest,
                                  public ::testing::WithParamInterface<int> {};

TEST_P(SlabRegistrationFaultTest, ReportsFaultAndReleasesEveryReservedSlot) {
  ASSERT_NO_FATAL_FAILURE(CreateSlab(page_size_, 4));
  auto* fault_page = static_cast<uint8_t*>(mapping_) + GetParam() * page_size_;
  ASSERT_EQ(mprotect(fault_page, page_size_, PROT_NONE), 0);

  // Later faults follow a successfully registered prefix. They must preserve
  // the mapping error instead of reporting an unexpected short registration.
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_async_proactor_register_slab(
          proactor_, slab_, IREE_ASYNC_BUFFER_ACCESS_FLAG_READ, &region_));
  EXPECT_EQ(region_, nullptr);
  iree_async_region_release(region_);
  region_ = nullptr;

  ASSERT_EQ(mprotect(fault_page, page_size_, PROT_READ | PROT_WRITE), 0);
  ASSERT_NO_FATAL_FAILURE(RegisterAtFirstSlot());
}

INSTANTIATE_TEST_SUITE_P(FaultPosition, SlabRegistrationFaultTest,
                         ::testing::Values(0, 1, 3));

TEST_F(SlabRegistrationTest, PinLimitReleasesPartialRegistration) {
  constexpr rlim_t kMaxPinLimit = 8 * 1024 * 1024;
  struct rlimit limit = original_limit_;
  if (limit.rlim_cur > kMaxPinLimit) {
    limit.rlim_cur = kMaxPinLimit;
  }
  const iree_host_size_t buffer_count = limit.rlim_cur / page_size_ + 1;
  ASSERT_NO_FATAL_FAILURE(CreateSlab(page_size_, buffer_count));
  ASSERT_EQ(setrlimit(RLIMIT_MEMLOCK, &limit), 0);
  limit_changed_ = true;

  // The slab exceeds the entire pin budget by one page. Registration may
  // complete a prefix or fail immediately if other rings already consume
  // that budget. Both cases use the existing unpinned region representation.
  IREE_ASSERT_OK(iree_async_proactor_register_slab(
      proactor_, slab_, IREE_ASYNC_BUFFER_ACCESS_FLAG_READ, &region_));
  ASSERT_NE(region_, nullptr);
  EXPECT_EQ(region_->handles.iouring.base_buffer_index, -1);
  iree_async_region_release(region_);
  region_ = nullptr;

  ASSERT_EQ(setrlimit(RLIMIT_MEMLOCK, &original_limit_), 0);
  limit_changed_ = false;
  ASSERT_NO_FATAL_FAILURE(CreateSlab(page_size_, 4));
  ASSERT_NO_FATAL_FAILURE(RegisterAtFirstSlot());
}

class ReceiveSlabRegistrationTest : public SlabRegistrationTest {
 protected:
  void TearDown() override {
    iree_async_socket_release(socket_);
    for (int fd : socket_fds_) {
      if (fd >= 0) {
        EXPECT_EQ(close(fd), 0);
      }
    }
    iree_async_buffer_pool_release(pool_);
    SlabRegistrationTest::TearDown();
  }

  // Owned descriptors not yet transferred to the proactor.
  int socket_fds_[2] = {-1, -1};
  // Receive socket owned until all one-shot receives complete.
  iree_async_socket_t* socket_ = nullptr;
  // Pool retaining the receive region through every returned lease.
  iree_async_buffer_pool_t* pool_ = nullptr;
};

TEST_F(ReceiveSlabRegistrationTest, OneShotPoolSurvivesDisabledMultishot) {
  if (!iree_any_bit_set(iree_async_proactor_query_capabilities(proactor_),
                        IREE_ASYNC_PROACTOR_CAPABILITY_MULTISHOT)) {
    GTEST_SKIP() << "kernel predates provided-buffer rings";
  }
  iree_async_proactor_release(proactor_);
  proactor_ = nullptr;
  auto options = iree_async_proactor_options_default();
  options.allowed_capabilities &= ~IREE_ASYNC_PROACTOR_CAPABILITY_MULTISHOT;
  IREE_ASSERT_OK(iree_async_proactor_create_io_uring(
      options, iree_allocator_system(), &proactor_));
  EXPECT_FALSE(
      iree_any_bit_set(iree_async_proactor_query_capabilities(proactor_),
                       IREE_ASYNC_PROACTOR_CAPABILITY_MULTISHOT));
  ASSERT_NO_FATAL_FAILURE(CreateSlab(page_size_, 2));
  IREE_ASSERT_OK(iree_async_proactor_register_slab(
      proactor_, slab_, IREE_ASYNC_BUFFER_ACCESS_FLAG_WRITE, &region_));
  IREE_ASSERT_OK(
      iree_async_buffer_pool_create(region_, iree_allocator_system(), &pool_));
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                       socket_fds_),
            0);
  IREE_ASSERT_OK(iree_async_socket_import(
      proactor_, iree_async_primitive_from_fd(socket_fds_[0]),
      IREE_ASYNC_SOCKET_TYPE_UNIX_STREAM, IREE_ASYNC_SOCKET_FLAG_NONE,
      &socket_));
  socket_fds_[0] = -1;

  // More receives than pool slots require returned leases to replenish the
  // kernel ring. Multishot stays disabled throughout these real byte transfers.
  for (uint8_t expected = 1; expected <= 9; ++expected) {
    ASSERT_EQ(send(socket_fds_[1], &expected, sizeof(expected), MSG_NOSIGNAL),
              1);
    struct Completion {
      // Set by the poll-owner callback when operation storage is reusable.
      bool done = false;
      // Terminal receive result consumed after the callback.
      iree::Status status;
    } completion;
    iree_async_socket_recv_pool_operation_t operation = {};
    iree_async_operation_initialize(
        &operation.base, IREE_ASYNC_OPERATION_TYPE_SOCKET_RECV_POOL,
        IREE_ASYNC_OPERATION_FLAG_NONE,
        +[](void* user_data, iree_async_operation_t*, iree_status_t status,
            iree_async_completion_flags_t flags) {
          auto& completion = *static_cast<Completion*>(user_data);
          EXPECT_EQ(flags & IREE_ASYNC_COMPLETION_FLAG_MORE, 0u);
          completion.status = std::move(status);
          completion.done = true;
        },
        &completion);
    operation.socket = socket_;
    operation.pool = pool_;
    IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
    while (!completion.done) {
      IREE_EXPECT_OK(iree_async_proactor_poll(
          proactor_, iree_infinite_timeout(), /*out_completed_count=*/nullptr));
    }
    IREE_EXPECT_OK(completion.status.release());
    EXPECT_EQ(operation.bytes_received, sizeof(expected));
    if (operation.bytes_received == sizeof(expected)) {
      EXPECT_EQ(*iree_async_span_ptr(operation.lease.span), expected);
    }
    iree_async_buffer_lease_release(&operation.lease);
  }
}

}  // namespace
