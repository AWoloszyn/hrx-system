// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/shm/storage.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "iree/async/notification.h"
#include "iree/async/operations/scheduling.h"
#include "iree/async/proactor_platform.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

class ShmStorageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!iree_async_notification_native_is_supported()) {
      GTEST_SKIP();
    }
  }

  void TearDown() override {
    iree_net_shm_storage_release(client_);
    iree_net_shm_storage_release(server_);
  }

  iree_status_t Create(uint32_t slot_count) {
    iree_net_shm_region_layout_t layout;
    IREE_RETURN_IF_ERROR(
        iree_net_shm_region_calculate_layout({2, slot_count, 4097}, &layout));
    IREE_RETURN_IF_ERROR(iree_net_shm_storage_create(
        &layout, iree_allocator_system(), &server_));
    iree_async_primitive_t resources[IREE_NET_SHM_STORAGE_HANDLE_COUNT] = {};
    IREE_RETURN_IF_ERROR(DuplicateResources(resources));
    return iree_net_shm_storage_import(&layout, resources,
                                       iree_allocator_system(), &client_);
  }

  iree_status_t DuplicateResources(iree_async_primitive_t* out_resources) {
    iree_async_primitive_t borrowed[IREE_NET_SHM_STORAGE_HANDLE_COUNT];
    iree_net_shm_storage_export(server_, borrowed);
    iree_status_t status = iree_ok_status();
    for (uint32_t i = 0;
         i < IREE_NET_SHM_STORAGE_HANDLE_COUNT && iree_status_is_ok(status);
         ++i) {
      status = iree_async_primitive_dup(borrowed[i], &out_resources[i]);
    }
    if (!iree_status_is_ok(status)) {
      for (uint32_t i = 0; i < IREE_NET_SHM_STORAGE_HANDLE_COUNT; ++i) {
        iree_async_primitive_close(&out_resources[i]);
      }
    }
    return status;
  }

  // Independently owned server and client mappings/resources.
  iree_net_shm_storage_t* server_ = nullptr;
  // Client leases borrow from this owner, not the server or a proactor.
  iree_net_shm_storage_t* client_ = nullptr;
};

TEST_F(ShmStorageTest, RetentionReservesOneProgressSlotPerEndpoint) {
  IREE_ASSERT_OK(Create(4));
  iree_async_buffer_lease_t leases[2][4] = {};
  uint16_t slots[2][4];
  for (uint32_t endpoint = 0; endpoint < 2; ++endpoint) {
    auto& outgoing = server_->directions[endpoint * 2];
    for (uint32_t i = 0; i < 4; ++i) {
      ASSERT_TRUE(iree_atomic_freelist_try_pop(
          outgoing.free_slots, outgoing.links, &slots[endpoint][i]));
      EXPECT_EQ(iree_net_shm_storage_try_lease(&client_->endpoints[endpoint],
                                               slots[endpoint][i], 17,
                                               &leases[endpoint][i]),
                i < 3);
    }
    EXPECT_EQ(iree_atomic_load(&client_->endpoints[endpoint].retained_count,
                               iree_memory_order_acquire),
              3);
    EXPECT_EQ(leases[endpoint][3].release.fn, nullptr);
    iree_atomic_freelist_push(outgoing.free_slots, outgoing.links,
                              slots[endpoint][3]);
  }
  // Return leases in reverse order from a consumer thread. Each direction
  // regains all capacity independently of any proactor or connection.
  std::thread returner([&] {
    for (int i = 2; i >= 0; --i) {
      for (uint32_t endpoint = 0; endpoint < 2; ++endpoint) {
        iree_async_buffer_lease_release(&leases[endpoint][i]);
      }
    }
  });
  returner.join();
  for (uint32_t endpoint = 0; endpoint < 2; ++endpoint) {
    EXPECT_EQ(iree_atomic_freelist_count(
                  server_->directions[endpoint * 2].free_slots),
              4);
    EXPECT_EQ(iree_atomic_load(&client_->endpoints[endpoint].retained_count,
                               iree_memory_order_acquire),
              0);
  }
}

TEST_F(ShmStorageTest, OneSlotGeometryAlwaysDeliversBorrowedStorage) {
  IREE_ASSERT_OK(Create(1));
  auto& outgoing = server_->directions[0];
  uint16_t slot = 0;
  ASSERT_TRUE(
      iree_atomic_freelist_try_pop(outgoing.free_slots, outgoing.links, &slot));
  iree_async_buffer_lease_t lease;
  EXPECT_FALSE(
      iree_net_shm_storage_try_lease(&client_->endpoints[0], slot, 1, &lease));
  EXPECT_EQ(lease.release.fn, nullptr);
  iree_atomic_freelist_push(outgoing.free_slots, outgoing.links, slot);
}

TEST_F(ShmStorageTest, LateLeasesKeepBytesAndNativeWakeAliveWithoutPeer) {
  IREE_ASSERT_OK(Create(4));
  iree_async_buffer_lease_t leases[3] = {};
  auto& outgoing = server_->directions[0];
  for (uint32_t i = 0; i < 3; ++i) {
    uint16_t slot = 0;
    ASSERT_TRUE(iree_atomic_freelist_try_pop(outgoing.free_slots,
                                             outgoing.links, &slot));
    std::memset(outgoing.payload + slot * server_->layout.slot_stride, 0x30 + i,
                4097);
    ASSERT_TRUE(iree_net_shm_storage_try_lease(&client_->endpoints[0], slot,
                                               4097, &leases[i]));
  }
  iree_net_shm_storage_release(server_);
  server_ = nullptr;
  iree_net_shm_storage_release(client_);
  client_ = nullptr;
  std::thread returners[3];
  for (uint32_t i = 0; i < 3; ++i) {
    returners[i] = std::thread([&, i] {
      auto* bytes = iree_async_span_ptr(leases[i].span);
      for (size_t j = 0; j < 4097; ++j) {
        EXPECT_EQ(bytes[j], 0x30 + i);
      }
      iree_async_buffer_lease_release(&leases[i]);
      EXPECT_EQ(leases[i].release.fn, nullptr);
    });
  }
  for (auto& returner : returners) {
    returner.join();
  }
}

TEST_F(ShmStorageTest, LeaseReturnWakesRealNotificationBeforeDetachedReturn) {
  IREE_ASSERT_OK(Create(4));
  iree_async_proactor_t* proactor = nullptr;
  IREE_ASSERT_OK(
      iree_async_proactor_create_platform(iree_async_proactor_options_default(),
                                          iree_allocator_system(), &proactor));
  iree_async_notification_t* notification = nullptr;
  IREE_ASSERT_OK(iree_async_notification_create_shared(
      proactor, &server_->wakes[0], &notification));
  iree_async_buffer_lease_t leases[2] = {};
  auto& outgoing = server_->directions[0];
  for (uint32_t i = 0; i < 2; ++i) {
    uint16_t slot = 0;
    ASSERT_TRUE(iree_atomic_freelist_try_pop(outgoing.free_slots,
                                             outgoing.links, &slot));
    ASSERT_TRUE(iree_net_shm_storage_try_lease(&client_->endpoints[0], slot, 1,
                                               &leases[i]));
  }
  bool completed = false;
  iree_async_notification_wait_operation_t wait = {};
  iree_async_operation_initialize(
      &wait.base, IREE_ASYNC_OPERATION_TYPE_NOTIFICATION_WAIT,
      IREE_ASYNC_OPERATION_FLAG_NONE,
      [](void* user_data, iree_async_operation_t*, iree_status_t status,
         iree_async_completion_flags_t) {
        IREE_EXPECT_OK(status);
        *static_cast<bool*>(user_data) = true;
      },
      &completed);
  wait.notification = notification;
  wait.wait_flags = IREE_ASYNC_NOTIFICATION_WAIT_FLAG_USE_WAIT_TOKEN;
  wait.wait_token = iree_async_notification_begin_observe(notification);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor, &wait.base));
  iree_async_notification_end_observe(notification);
  std::thread returner([&] { iree_async_buffer_lease_release(&leases[0]); });
  while (!completed) {
    IREE_ASSERT_OK(
        iree_async_proactor_poll(proactor, iree_infinite_timeout(), nullptr));
  }
  returner.join();
  iree_async_notification_release(notification);
  iree_async_proactor_end_polling(proactor);
  iree_async_proactor_release(proactor);
  iree_net_shm_storage_release(server_);
  server_ = nullptr;
  iree_async_buffer_lease_release(&leases[1]);
  IREE_EXPECT_OK(iree_net_shm_storage_clone_failure(client_));
}

TEST_F(ShmStorageTest,
       ConcurrentFailuresScheduleOnceAndRemainOwnedAfterDetach) {
  IREE_ASSERT_OK(Create(2));
  std::atomic<uint32_t> scheduled{0};
  iree_net_shm_storage_set_failure_callback(
      client_, {[](void* user_data) {
                  ++*static_cast<std::atomic<uint32_t>*>(user_data);
                },
                &scheduled});
  std::vector<std::thread> threads;
  for (uint32_t i = 0; i < 16; ++i) {
    threads.emplace_back([&] {
      iree_net_shm_storage_fail(
          client_, iree_make_status(IREE_STATUS_UNAVAILABLE, "peer departed"));
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(scheduled.load(), 1);
  iree_net_shm_storage_set_failure_callback(client_, {});
  iree_net_shm_storage_fail(
      client_,
      iree_make_status(IREE_STATUS_ABORTED, "detached terminal error"));
  EXPECT_EQ(scheduled.load(), 1);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_UNAVAILABLE,
                        iree_net_shm_storage_clone_failure(client_));
}

TEST_F(ShmStorageTest, FailedImportClosesEntireReceivedBundle) {
  IREE_ASSERT_OK(Create(2));
  iree_async_primitive_t resources[IREE_NET_SHM_STORAGE_HANDLE_COUNT] = {};
  IREE_ASSERT_OK(DuplicateResources(resources));
  iree_net_shm_storage_t* imported = nullptr;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_shm_storage_import(&server_->layout, resources,
                                  iree_allocator_null(), &imported));
  EXPECT_EQ(imported, nullptr);
  for (const auto& resource : resources) {
    EXPECT_TRUE(iree_async_primitive_is_none(resource));
  }
}

}  // namespace
