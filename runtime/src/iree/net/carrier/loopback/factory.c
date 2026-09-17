// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/loopback/factory.h"

#include <string.h>

#include "iree/async/operations/scheduling.h"
#include "iree/base/threading/mutex.h"
#include "iree/net/carrier/loopback/connection.h"

typedef struct iree_net_loopback_factory_t iree_net_loopback_factory_t;
typedef struct iree_net_loopback_listener_t iree_net_loopback_listener_t;

typedef enum iree_net_loopback_listener_state_e {
  IREE_NET_LOOPBACK_LISTENER_STATE_LISTENING = 0,
  IREE_NET_LOOPBACK_LISTENER_STATE_STOPPING = 1,
  IREE_NET_LOOPBACK_LISTENER_STATE_STOPPED = 2,
} iree_net_loopback_listener_state_t;

struct iree_net_loopback_factory_t {
  // Public transport factory base; must be first.
  iree_net_transport_factory_t base;

  // Serializes the named listener registry and listener lifecycle state.
  iree_slim_mutex_t mutex;

  // Active named listeners. Entries are borrowed and protected by |mutex|.
  iree_net_loopback_listener_t** listeners;

  // Number of active entries in |listeners|.
  iree_host_size_t listener_count;

  // Allocated capacity of |listeners|.
  iree_host_size_t listener_capacity;

  // Connection and carrier admission limits.
  iree_net_loopback_factory_options_t options;

  // Allocator used for factory-owned and connection-establishment state.
  iree_allocator_t host_allocator;
};

struct iree_net_loopback_listener_t {
  // Public listener base; must be first.
  iree_net_listener_t base;

  // Factory containing the named listener registry. Retained.
  iree_net_loopback_factory_t* factory;

  // Proactor dispatching accepts and final stop notification. Retained.
  iree_async_proactor_t* proactor;

  // Notification signaled after the final claimed accept retires.
  iree_async_notification_t* stop_notification;

  // Preallocated wait dispatching the stopped callback.
  iree_async_notification_wait_operation_t stop_wait_operation;

  // Current listener lifecycle state protected by the factory mutex.
  iree_net_loopback_listener_state_t state;

  // Accepted connects not yet retired, protected by the factory mutex.
  uint32_t pending_accept_count;

  // Callback receiving accepted connections and terminal listener errors.
  iree_net_listener_accept_callback_t accept_callback;

  // Callback awaiting the final listener stop notification.
  iree_net_listener_stopped_callback_t stopped_callback;

  // Allocator used for this listener allocation.
  iree_allocator_t host_allocator;

  // Byte length of the listener name stored in |name|.
  iree_host_size_t name_length;

  // Listener name bytes without a trailing NUL.
  char name[];
};

typedef struct iree_net_loopback_accept_dispatch_t {
  // Operation dispatching the accept callback on the listener proactor.
  iree_async_nop_operation_t operation;

  // Claimed listener kept alive by its pending accept count.
  iree_net_loopback_listener_t* listener;

  // Unpublished server connection transferred by a successful callback.
  iree_net_connection_t* server_connection;
} iree_net_loopback_accept_dispatch_t;

typedef struct iree_net_loopback_connect_dispatch_t {
  // Operation dispatching the connect callback on the client proactor.
  iree_async_nop_operation_t operation;

  // Factory retained until the connect callback retires.
  iree_net_loopback_factory_t* factory;

  // Client proactor retained until the connect callback retires.
  iree_async_proactor_t* proactor;

  // Callback receiving the client connection or terminal failure.
  iree_net_transport_connect_callback_t callback;

  // Asynchronous connection result joined with operation completion status.
  iree_status_t result_status;

  // Unpublished client connection transferred by a successful callback.
  iree_net_connection_t* client_connection;

  // Server-side accept dispatch owned until submitted successfully.
  iree_net_loopback_accept_dispatch_t* accept_dispatch;
} iree_net_loopback_connect_dispatch_t;

// Finds an active listener by name. The caller must hold |factory->mutex|.
static iree_net_loopback_listener_t*
iree_net_loopback_factory_find_listener_locked(
    iree_net_loopback_factory_t* factory, iree_string_view_t name) {
  for (iree_host_size_t i = 0; i < factory->listener_count; ++i) {
    iree_net_loopback_listener_t* listener = factory->listeners[i];
    if (iree_string_view_equal(
            name,
            iree_make_string_view(listener->name, listener->name_length))) {
      return listener;
    }
  }
  return NULL;
}

// Removes an active listener. The caller must hold |factory->mutex|.
static bool iree_net_loopback_factory_remove_listener_locked(
    iree_net_loopback_factory_t* factory,
    iree_net_loopback_listener_t* listener) {
  for (iree_host_size_t i = 0; i < factory->listener_count; ++i) {
    if (factory->listeners[i] == listener) {
      factory->listeners[i] = factory->listeners[--factory->listener_count];
      return true;
    }
  }
  return false;
}

// Retires a claimed accept and signals a stopping listener when it was last.
static void iree_net_loopback_listener_retire_accept(
    iree_net_loopback_listener_t* listener) {
  bool signal_stop = false;
  iree_net_loopback_factory_t* factory = listener->factory;
  iree_slim_mutex_lock(&factory->mutex);
  IREE_ASSERT(listener->pending_accept_count > 0,
              "loopback listener retired an unclaimed accept");
  --listener->pending_accept_count;
  signal_stop = listener->state == IREE_NET_LOOPBACK_LISTENER_STATE_STOPPING &&
                listener->pending_accept_count == 0;
  iree_slim_mutex_unlock(&factory->mutex);
  if (signal_stop) {
    iree_async_notification_signal(listener->stop_notification, 1);
  }
}

//===----------------------------------------------------------------------===//
// Listener
//===----------------------------------------------------------------------===//

static void iree_net_loopback_listener_free(
    iree_net_listener_t* base_listener) {
  iree_net_loopback_listener_t* listener =
      (iree_net_loopback_listener_t*)base_listener;
  IREE_ASSERT(listener->state == IREE_NET_LOOPBACK_LISTENER_STATE_STOPPED,
              "loopback listener freed before it stopped");
  IREE_ASSERT(listener->pending_accept_count == 0,
              "loopback listener freed with pending accepts");

  iree_net_loopback_factory_t* factory = listener->factory;
  iree_allocator_t host_allocator = listener->host_allocator;
  iree_async_notification_release(listener->stop_notification);
  iree_async_proactor_release(listener->proactor);
  iree_allocator_free(host_allocator, listener);
  iree_net_transport_factory_release(&factory->base);
}

static void iree_net_loopback_listener_stop_complete(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)operation;
  IREE_ASSERT(!iree_any_bit_set(flags, IREE_ASYNC_COMPLETION_FLAG_MORE),
              "listener stop wait produced a nonterminal completion");
  iree_net_loopback_listener_t* listener =
      (iree_net_loopback_listener_t*)user_data;

  iree_slim_mutex_lock(&listener->factory->mutex);
  IREE_ASSERT(listener->state == IREE_NET_LOOPBACK_LISTENER_STATE_STOPPING,
              "loopback listener stop completed from state %d",
              (int)listener->state);
  IREE_ASSERT(listener->pending_accept_count == 0,
              "loopback listener stopped with pending accepts");
  listener->state = IREE_NET_LOOPBACK_LISTENER_STATE_STOPPED;
  iree_net_listener_accept_callback_t accept_callback =
      listener->accept_callback;
  iree_net_listener_stopped_callback_t stopped_callback =
      listener->stopped_callback;
  iree_slim_mutex_unlock(&listener->factory->mutex);

  if (!iree_status_is_ok(status)) {
    accept_callback.fn(accept_callback.user_data, status, NULL);
  }
  stopped_callback.fn(stopped_callback.user_data);
}

static iree_status_t iree_net_loopback_listener_stop(
    iree_net_listener_t* base_listener,
    iree_net_listener_stopped_callback_t callback) {
  iree_net_loopback_listener_t* listener =
      (iree_net_loopback_listener_t*)base_listener;
  bool signal_stop = false;
  iree_slim_mutex_lock(&listener->factory->mutex);
  iree_status_t status = iree_ok_status();
  if (listener->state != IREE_NET_LOOPBACK_LISTENER_STATE_LISTENING) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "loopback listener is not listening");
  } else {
    const bool removed = iree_net_loopback_factory_remove_listener_locked(
        listener->factory, listener);
    IREE_ASSERT(removed, "listening loopback listener was not registered");
    listener->state = IREE_NET_LOOPBACK_LISTENER_STATE_STOPPING;
    listener->stopped_callback = callback;
    signal_stop = listener->pending_accept_count == 0;
  }
  iree_slim_mutex_unlock(&listener->factory->mutex);
  if (signal_stop) {
    iree_async_notification_signal(listener->stop_notification, 1);
  }
  return status;
}

static iree_status_t iree_net_loopback_listener_query_bound_address(
    iree_net_listener_t* base_listener, iree_host_size_t buffer_capacity,
    char* buffer, iree_string_view_t* out_address) {
  iree_net_loopback_listener_t* listener =
      (iree_net_loopback_listener_t*)base_listener;
  if (!out_address) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bound address output is required");
  }
  *out_address = iree_string_view_empty();
  if (!buffer) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bound address storage is required");
  }
  if (buffer_capacity < listener->name_length) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "bound address requires %zu bytes",
                            listener->name_length);
  }
  memcpy(buffer, listener->name, listener->name_length);
  *out_address = iree_make_string_view(buffer, listener->name_length);
  return iree_ok_status();
}

static const iree_net_listener_vtable_t iree_net_loopback_listener_vtable = {
    .free = iree_net_loopback_listener_free,
    .stop = iree_net_loopback_listener_stop,
    .query_bound_address = iree_net_loopback_listener_query_bound_address,
};

//===----------------------------------------------------------------------===//
// Connection establishment
//===----------------------------------------------------------------------===//

static void iree_net_loopback_accept_dispatch_discard(
    iree_net_loopback_accept_dispatch_t* dispatch) {
  if (!dispatch) return;
  iree_net_loopback_listener_t* listener = dispatch->listener;
  iree_allocator_t host_allocator = listener->factory->host_allocator;
  iree_net_connection_release(dispatch->server_connection);
  iree_allocator_free(host_allocator, dispatch);
  iree_net_loopback_listener_retire_accept(listener);
}

static void iree_net_loopback_connect_dispatch_destroy(
    iree_net_loopback_connect_dispatch_t* dispatch) {
  iree_net_loopback_factory_t* factory = dispatch->factory;
  iree_async_proactor_t* proactor = dispatch->proactor;
  iree_allocator_t host_allocator = factory->host_allocator;
  iree_allocator_free(host_allocator, dispatch);
  iree_async_proactor_release(proactor);
  iree_net_transport_factory_release(&factory->base);
}

static void iree_net_loopback_accept_dispatch_complete(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)operation;
  IREE_ASSERT(!iree_any_bit_set(flags, IREE_ASYNC_COMPLETION_FLAG_MORE),
              "loopback accept NOP produced a nonterminal completion");
  iree_net_loopback_accept_dispatch_t* dispatch =
      (iree_net_loopback_accept_dispatch_t*)user_data;
  iree_net_loopback_listener_t* listener = dispatch->listener;
  iree_allocator_t host_allocator = listener->factory->host_allocator;

  iree_net_connection_t* server_connection = NULL;
  if (iree_status_is_ok(status)) {
    server_connection = dispatch->server_connection;
    dispatch->server_connection = NULL;
    iree_net_loopback_connection_publish(server_connection);
  } else {
    iree_net_connection_release(dispatch->server_connection);
    dispatch->server_connection = NULL;
  }
  listener->accept_callback.fn(listener->accept_callback.user_data, status,
                               server_connection);

  iree_allocator_free(host_allocator, dispatch);
  iree_net_loopback_listener_retire_accept(listener);
}

static void iree_net_loopback_connect_dispatch_complete(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)operation;
  IREE_ASSERT(!iree_any_bit_set(flags, IREE_ASYNC_COMPLETION_FLAG_MORE),
              "loopback connect NOP produced a nonterminal completion");
  iree_net_loopback_connect_dispatch_t* dispatch =
      (iree_net_loopback_connect_dispatch_t*)user_data;
  iree_net_transport_connect_callback_t callback = dispatch->callback;

  status = iree_status_join(status, dispatch->result_status);
  dispatch->result_status = iree_ok_status();
  iree_net_connection_t* client_connection = NULL;
  if (iree_status_is_ok(status) && dispatch->accept_dispatch) {
    iree_net_loopback_accept_dispatch_t* accept_dispatch =
        dispatch->accept_dispatch;
    iree_async_operation_initialize(
        &accept_dispatch->operation.base, IREE_ASYNC_OPERATION_TYPE_NOP,
        IREE_ASYNC_OPERATION_FLAG_NONE,
        iree_net_loopback_accept_dispatch_complete, accept_dispatch);
    status = iree_async_proactor_submit_one(accept_dispatch->listener->proactor,
                                            &accept_dispatch->operation.base);
    if (iree_status_is_ok(status)) {
      dispatch->accept_dispatch = NULL;
      client_connection = dispatch->client_connection;
      dispatch->client_connection = NULL;
      iree_net_loopback_connection_publish(client_connection);
    }
  }

  if (!iree_status_is_ok(status)) {
    iree_net_connection_release(dispatch->client_connection);
    dispatch->client_connection = NULL;
    iree_net_loopback_accept_dispatch_discard(dispatch->accept_dispatch);
    dispatch->accept_dispatch = NULL;
  }

  callback.fn(callback.user_data, status, client_connection);
  iree_net_loopback_connect_dispatch_destroy(dispatch);
}

//===----------------------------------------------------------------------===//
// Factory
//===----------------------------------------------------------------------===//

static void iree_net_loopback_factory_destroy(
    iree_net_transport_factory_t* base_factory) {
  iree_net_loopback_factory_t* factory =
      (iree_net_loopback_factory_t*)base_factory;
  IREE_ASSERT(factory->listener_count == 0,
              "loopback factory destroyed with active listeners");
  iree_allocator_t host_allocator = factory->host_allocator;
  iree_allocator_free(host_allocator, factory->listeners);
  iree_slim_mutex_deinitialize(&factory->mutex);
  iree_allocator_free(host_allocator, factory);
}

static iree_net_transport_capabilities_t
iree_net_loopback_factory_query_capabilities(
    iree_net_transport_factory_t* base_factory) {
  (void)base_factory;
  return IREE_NET_TRANSPORT_CAPABILITY_RELIABLE |
         IREE_NET_TRANSPORT_CAPABILITY_ORDERED;
}

static iree_status_t iree_net_loopback_factory_connect(
    iree_net_transport_factory_t* base_factory, iree_string_view_t address,
    iree_async_proactor_t* proactor, iree_async_buffer_pool_t* receive_pool,
    iree_net_transport_connect_callback_t callback) {
  (void)receive_pool;
  iree_net_loopback_factory_t* factory =
      (iree_net_loopback_factory_t*)base_factory;
  if (iree_string_view_is_empty(address) || !address.data) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "loopback listener name is required");
  }
  if (!proactor) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "loopback client proactor is required");
  }

  iree_net_loopback_connect_dispatch_t* dispatch = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(
      factory->host_allocator, sizeof(*dispatch), (void**)&dispatch));
  memset(dispatch, 0, sizeof(*dispatch));
  dispatch->factory = factory;
  iree_net_transport_factory_retain(base_factory);
  dispatch->proactor = proactor;
  iree_async_proactor_retain(proactor);
  dispatch->callback = callback;

  iree_slim_mutex_lock(&factory->mutex);
  iree_net_loopback_listener_t* listener =
      iree_net_loopback_factory_find_listener_locked(factory, address);
  if (listener) ++listener->pending_accept_count;
  iree_slim_mutex_unlock(&factory->mutex);

  iree_status_t status = iree_ok_status();
  if (!listener) {
    dispatch->result_status = iree_make_status(
        IREE_STATUS_UNAVAILABLE, "no loopback listener named '%.*s'",
        (int)address.size, address.data);
  } else {
    status = iree_allocator_malloc(factory->host_allocator,
                                   sizeof(*dispatch->accept_dispatch),
                                   (void**)&dispatch->accept_dispatch);
    if (iree_status_is_ok(status)) {
      memset(dispatch->accept_dispatch, 0, sizeof(*dispatch->accept_dispatch));
      dispatch->accept_dispatch->listener = listener;
      status = iree_net_loopback_connection_create_pair(
          proactor, listener->proactor, factory->options.max_endpoint_count,
          &factory->options.carrier_options, factory->host_allocator,
          &dispatch->client_connection,
          &dispatch->accept_dispatch->server_connection);
    }
  }

  if (iree_status_is_ok(status)) {
    iree_async_operation_initialize(
        &dispatch->operation.base, IREE_ASYNC_OPERATION_TYPE_NOP,
        IREE_ASYNC_OPERATION_FLAG_NONE,
        iree_net_loopback_connect_dispatch_complete, dispatch);
    status =
        iree_async_proactor_submit_one(proactor, &dispatch->operation.base);
  }
  if (!iree_status_is_ok(status)) {
    status = iree_status_join(status, dispatch->result_status);
    dispatch->result_status = iree_ok_status();
    iree_net_connection_release(dispatch->client_connection);
    dispatch->client_connection = NULL;
    if (dispatch->accept_dispatch) {
      iree_net_loopback_accept_dispatch_discard(dispatch->accept_dispatch);
      dispatch->accept_dispatch = NULL;
    } else if (listener) {
      iree_net_loopback_listener_retire_accept(listener);
    }
    iree_net_loopback_connect_dispatch_destroy(dispatch);
  }
  return status;
}

static iree_status_t iree_net_loopback_factory_create_listener(
    iree_net_transport_factory_t* base_factory, iree_string_view_t bind_address,
    iree_async_proactor_t* proactor, iree_async_buffer_pool_t* receive_pool,
    iree_net_listener_accept_callback_t accept_callback,
    iree_allocator_t host_allocator, iree_net_listener_t** out_listener) {
  (void)receive_pool;
  iree_net_loopback_factory_t* factory =
      (iree_net_loopback_factory_t*)base_factory;
  *out_listener = NULL;
  if (iree_string_view_is_empty(bind_address) || !bind_address.data) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "loopback listener name is required");
  }
  if (!proactor) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "loopback listener proactor is required");
  }

  iree_net_loopback_listener_t* listener = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc_with_trailing(
      host_allocator, sizeof(*listener), bind_address.size, (void**)&listener));
  memset(listener, 0, sizeof(*listener));
  listener->base.vtable = &iree_net_loopback_listener_vtable;
  listener->factory = factory;
  iree_net_transport_factory_retain(base_factory);
  listener->proactor = proactor;
  iree_async_proactor_retain(proactor);
  listener->state = IREE_NET_LOOPBACK_LISTENER_STATE_LISTENING;
  listener->accept_callback = accept_callback;
  listener->host_allocator = host_allocator;
  listener->name_length = bind_address.size;
  memcpy(listener->name, bind_address.data, bind_address.size);

  iree_status_t status = iree_async_notification_create(
      proactor, IREE_ASYNC_NOTIFICATION_FLAG_NONE,
      &listener->stop_notification);
  uint32_t stop_wait_token = 0;
  if (iree_status_is_ok(status)) {
    stop_wait_token =
        iree_async_notification_begin_observe(listener->stop_notification);
    iree_async_operation_initialize(&listener->stop_wait_operation.base,
                                    IREE_ASYNC_OPERATION_TYPE_NOTIFICATION_WAIT,
                                    IREE_ASYNC_OPERATION_FLAG_NONE,
                                    iree_net_loopback_listener_stop_complete,
                                    listener);
    listener->stop_wait_operation.notification = listener->stop_notification;
    listener->stop_wait_operation.wait_flags =
        IREE_ASYNC_NOTIFICATION_WAIT_FLAG_USE_WAIT_TOKEN;
    listener->stop_wait_operation.wait_token = stop_wait_token;
  }

  if (iree_status_is_ok(status)) {
    iree_slim_mutex_lock(&factory->mutex);
    if (iree_net_loopback_factory_find_listener_locked(factory, bind_address)) {
      status = iree_make_status(IREE_STATUS_ALREADY_EXISTS,
                                "loopback listener '%.*s' already exists",
                                (int)bind_address.size, bind_address.data);
    }
    if (iree_status_is_ok(status) &&
        factory->listener_count == factory->listener_capacity) {
      status = iree_allocator_grow_array(
          factory->host_allocator, factory->listener_count + 1,
          sizeof(factory->listeners[0]), &factory->listener_capacity,
          (void**)&factory->listeners);
    }
    if (iree_status_is_ok(status)) {
      status = iree_async_proactor_submit_one(
          proactor, &listener->stop_wait_operation.base);
    }
    if (iree_status_is_ok(status)) {
      factory->listeners[factory->listener_count++] = listener;
    }
    iree_slim_mutex_unlock(&factory->mutex);
    iree_async_notification_end_observe(listener->stop_notification);
  }

  if (iree_status_is_ok(status)) {
    *out_listener = &listener->base;
  } else {
    iree_async_notification_release(listener->stop_notification);
    iree_async_proactor_release(listener->proactor);
    iree_net_transport_factory_release(base_factory);
    iree_allocator_free(host_allocator, listener);
  }
  return status;
}

static const iree_net_transport_factory_vtable_t
    iree_net_loopback_factory_vtable = {
        .destroy = iree_net_loopback_factory_destroy,
        .query_capabilities = iree_net_loopback_factory_query_capabilities,
        .connect = iree_net_loopback_factory_connect,
        .create_listener = iree_net_loopback_factory_create_listener,
};

iree_status_t iree_net_loopback_factory_create(
    const iree_net_loopback_factory_options_t* options,
    iree_allocator_t host_allocator,
    iree_net_transport_factory_t** out_factory) {
  IREE_ASSERT_ARGUMENT(out_factory);
  *out_factory = NULL;
  iree_net_loopback_factory_options_t default_options =
      iree_net_loopback_factory_options_default();
  if (!options) options = &default_options;
  if (options->max_endpoint_count == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "loopback connections require endpoint slots");
  }
  if (options->carrier_options.max_send_operations == 0 ||
      options->carrier_options.max_send_spans == 0) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "loopback send operation and span limits must be nonzero");
  }

  iree_net_loopback_factory_t* factory = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(host_allocator, sizeof(*factory),
                                             (void**)&factory));
  memset(factory, 0, sizeof(*factory));
  iree_net_transport_factory_initialize(&iree_net_loopback_factory_vtable,
                                        &factory->base);
  iree_slim_mutex_initialize(&factory->mutex);
  factory->options = *options;
  factory->host_allocator = host_allocator;
  *out_factory = &factory->base;
  return iree_ok_status();
}
