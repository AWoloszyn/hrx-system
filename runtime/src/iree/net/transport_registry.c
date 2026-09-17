// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/transport_registry.h"

#include "iree/base/threading/mutex.h"

//===----------------------------------------------------------------------===//
// Registry entry
//===----------------------------------------------------------------------===//

// A single registry entry holding a scheme and its factory.
// The scheme string is stored inline after the entry using FAM.
typedef struct iree_net_transport_registry_entry_t {
  // Retained factory registered for this scheme.
  iree_net_transport_factory_t* factory;
  // Number of bytes in the trailing scheme string.
  iree_host_size_t scheme_length;
  // Scheme string follows (scheme_length bytes, NOT null-terminated).
  char scheme[];
} iree_net_transport_registry_entry_t;

static iree_string_view_t iree_net_transport_registry_entry_scheme(
    const iree_net_transport_registry_entry_t* entry) {
  return iree_make_string_view(entry->scheme, entry->scheme_length);
}

//===----------------------------------------------------------------------===//
// Transport registry
//===----------------------------------------------------------------------===//

// Initial capacity for the entries array.
#define IREE_NET_TRANSPORT_REGISTRY_INITIAL_CAPACITY 8

struct iree_net_transport_registry_t {
  // Allocator owning the registry, entries, and pointer array.
  iree_allocator_t host_allocator;
  // Serializes registration, lookup, enumeration, and count queries.
  iree_slim_mutex_t mutex;

  // Number of initialized pointers in |entries|.
  iree_host_size_t entry_count;
  // Number of pointers allocated in |entries|.
  iree_host_size_t entry_capacity;
  // Registration-ordered array of owned entry pointers.
  iree_net_transport_registry_entry_t** entries;
};

IREE_API_EXPORT iree_status_t iree_net_transport_registry_allocate(
    iree_allocator_t host_allocator,
    iree_net_transport_registry_t** out_registry) {
  IREE_ASSERT_ARGUMENT(out_registry);
  *out_registry = NULL;
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_net_transport_registry_t* registry = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(host_allocator, sizeof(*registry),
                                (void**)&registry));
  memset(registry, 0, sizeof(*registry));
  registry->host_allocator = host_allocator;
  iree_slim_mutex_initialize(&registry->mutex);

  // Allocate initial entries array.
  iree_status_t status = iree_allocator_malloc(
      host_allocator,
      IREE_NET_TRANSPORT_REGISTRY_INITIAL_CAPACITY * sizeof(*registry->entries),
      (void**)&registry->entries);
  if (iree_status_is_ok(status)) {
    registry->entry_capacity = IREE_NET_TRANSPORT_REGISTRY_INITIAL_CAPACITY;
    registry->entry_count = 0;
  }

  if (!iree_status_is_ok(status)) {
    iree_net_transport_registry_free(registry);
    IREE_TRACE_ZONE_END(z0);
    return status;
  }

  *out_registry = registry;
  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

IREE_API_EXPORT void iree_net_transport_registry_free(
    iree_net_transport_registry_t* registry) {
  if (!registry) {
    return;
  }
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_allocator_t host_allocator = registry->host_allocator;

  // Release all entries and their factories.
  for (iree_host_size_t i = 0; i < registry->entry_count; ++i) {
    iree_net_transport_registry_entry_t* entry = registry->entries[i];
    iree_net_transport_factory_release(entry->factory);
    iree_allocator_free(host_allocator, entry);
  }

  // Free entries array.
  iree_allocator_free(host_allocator, registry->entries);

  iree_slim_mutex_deinitialize(&registry->mutex);
  iree_allocator_free(host_allocator, registry);

  IREE_TRACE_ZONE_END(z0);
}

// Finds an entry by scheme. Returns NULL if not found.
// Caller must hold the mutex.
static iree_net_transport_registry_entry_t*
iree_net_transport_registry_find_entry_unsafe(
    iree_net_transport_registry_t* registry, iree_string_view_t scheme) {
  for (iree_host_size_t i = 0; i < registry->entry_count; ++i) {
    iree_net_transport_registry_entry_t* entry = registry->entries[i];
    if (iree_string_view_equal(iree_net_transport_registry_entry_scheme(entry),
                               scheme)) {
      return entry;
    }
  }
  return NULL;
}

// Grows the entries array if needed.
// Caller must hold the mutex.
static iree_status_t iree_net_transport_registry_grow_if_needed_unsafe(
    iree_net_transport_registry_t* registry) {
  if (registry->entry_count < registry->entry_capacity) {
    return iree_ok_status();
  }

  iree_host_size_t new_capacity = 0;
  if (!iree_host_size_checked_mul(registry->entry_capacity, 2, &new_capacity)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "transport registry capacity overflow");
  }
  iree_host_size_t allocation_size = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      0, &allocation_size,
      IREE_STRUCT_FIELD(new_capacity, iree_net_transport_registry_entry_t*,
                        NULL)));
  IREE_RETURN_IF_ERROR(iree_allocator_realloc(
      registry->host_allocator, allocation_size, (void**)&registry->entries));
  registry->entry_capacity = new_capacity;
  return iree_ok_status();
}

IREE_API_EXPORT iree_status_t iree_net_transport_registry_register(
    iree_net_transport_registry_t* registry, iree_string_view_t scheme,
    iree_net_transport_factory_t* factory) {
  IREE_ASSERT_ARGUMENT(registry);
  IREE_ASSERT_ARGUMENT(factory);
  IREE_TRACE_ZONE_BEGIN(z0);

  // Validate scheme before taking lock.
  if (scheme.size == 0) {
    IREE_TRACE_ZONE_END(z0);
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT, "scheme is empty");
  }

  iree_slim_mutex_lock(&registry->mutex);

  // Check for duplicate.
  if (iree_net_transport_registry_find_entry_unsafe(registry, scheme)) {
    iree_slim_mutex_unlock(&registry->mutex);
    IREE_TRACE_ZONE_END(z0);
    return iree_make_status(IREE_STATUS_ALREADY_EXISTS,
                            "transport scheme '%.*s' already registered",
                            (int)scheme.size, scheme.data);
  }

  // Grow array if needed.
  iree_status_t status =
      iree_net_transport_registry_grow_if_needed_unsafe(registry);

  // Allocate entry with inline scheme storage.
  iree_net_transport_registry_entry_t* entry = NULL;
  if (iree_status_is_ok(status)) {
    iree_host_size_t entry_size = 0;
    status = IREE_STRUCT_LAYOUT(sizeof(*entry), &entry_size,
                                IREE_STRUCT_FIELD_FAM(scheme.size, char));
    if (iree_status_is_ok(status)) {
      status = iree_allocator_malloc(registry->host_allocator, entry_size,
                                     (void**)&entry);
    }
  }

  // Initialize entry.
  if (iree_status_is_ok(status)) {
    iree_net_transport_factory_retain(factory);
    entry->factory = factory;
    entry->scheme_length = scheme.size;
    memcpy(entry->scheme, scheme.data, scheme.size);
    registry->entries[registry->entry_count++] = entry;
  }

  iree_slim_mutex_unlock(&registry->mutex);

  if (!iree_status_is_ok(status)) {
    // On failure, caller still owns the factory - they must release it.
    IREE_TRACE_ZONE_END(z0);
    return status;
  }

  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

IREE_API_EXPORT iree_net_transport_factory_t*
iree_net_transport_registry_lookup(iree_net_transport_registry_t* registry,
                                   iree_string_view_t scheme) {
  IREE_ASSERT_ARGUMENT(registry);

  iree_slim_mutex_lock(&registry->mutex);
  iree_net_transport_registry_entry_t* entry =
      iree_net_transport_registry_find_entry_unsafe(registry, scheme);
  iree_net_transport_factory_t* factory = entry ? entry->factory : NULL;
  iree_slim_mutex_unlock(&registry->mutex);

  return factory;
}

IREE_API_EXPORT iree_status_t iree_net_transport_registry_enumerate(
    iree_net_transport_registry_t* registry,
    iree_net_transport_registry_enumerate_fn_t callback, void* user_data) {
  IREE_ASSERT_ARGUMENT(registry);
  IREE_ASSERT_ARGUMENT(callback);

  // Snapshot stable entry pointers so callbacks can query or register without
  // running under the registry mutex. Entries remain alive until registry
  // destruction, which is externally synchronized with enumeration.
  iree_net_transport_registry_entry_t** entries = NULL;
  iree_host_size_t entry_count = 0;
  iree_slim_mutex_lock(&registry->mutex);
  entry_count = registry->entry_count;
  iree_status_t status = iree_ok_status();
  if (entry_count > 0) {
    iree_host_size_t allocation_size = 0;
    status = IREE_STRUCT_LAYOUT(
        0, &allocation_size,
        IREE_STRUCT_FIELD(entry_count, iree_net_transport_registry_entry_t*,
                          NULL));
    if (iree_status_is_ok(status)) {
      status = iree_allocator_malloc(registry->host_allocator, allocation_size,
                                     (void**)&entries);
    }
    if (iree_status_is_ok(status)) {
      memcpy(entries, registry->entries, allocation_size);
    }
  }
  iree_slim_mutex_unlock(&registry->mutex);

  for (iree_host_size_t i = 0; i < entry_count && iree_status_is_ok(status);
       ++i) {
    iree_net_transport_registry_entry_t* entry = entries[i];
    status =
        callback(user_data, iree_net_transport_registry_entry_scheme(entry),
                 entry->factory);
  }

  iree_allocator_free(registry->host_allocator, entries);
  return status;
}

IREE_API_EXPORT iree_host_size_t
iree_net_transport_registry_count(iree_net_transport_registry_t* registry) {
  IREE_ASSERT_ARGUMENT(registry);

  iree_slim_mutex_lock(&registry->mutex);
  iree_host_size_t count = registry->entry_count;
  iree_slim_mutex_unlock(&registry->mutex);

  return count;
}
