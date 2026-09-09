// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/memory.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "libamdf/src/allocator.h"
#include "libamdf/src/device.h"
#include "libamdf/src/structure.h"

static amdf_memory_flags_t amdf_memory_known_flags(void) {
  return AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_DEVICE_LOCAL |
         AMDF_MEMORY_FLAG_SHAREABLE | AMDF_MEMORY_FLAG_EXECUTABLE |
         AMDF_MEMORY_FLAG_QUEUE_STORAGE | AMDF_MEMORY_FLAG_HOST_COHERENT |
         AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
}

static bool amdf_external_memory_type_is_valid(
    amdf_external_memory_type_t type) {
  return type >= AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD &&
         type <= AMDF_EXTERNAL_MEMORY_TYPE_DEVICE_ADDRESS;
}

static amdf_status_t amdf_external_memory_validate(
    const amdf_external_memory_t* value) {
  if (value == NULL || !amdf_external_memory_type_is_valid(value->type) ||
      value->reserved != 0 || value->byte_length == 0 ||
      value->source_byte_offset > UINT64_MAX - value->byte_length) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  switch (value->type) {
    case AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD:
      if (value->payload.file_descriptor < 0) {
        return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
      }
      break;
    case AMDF_EXTERNAL_MEMORY_TYPE_OPAQUE_FD:
      if (value->payload.file_descriptor < 0 ||
          !amdf_external_memory_provenance_is_valid(&value->provenance)) {
        return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
      }
      break;
    case AMDF_EXTERNAL_MEMORY_TYPE_NT_HANDLE:
      if (value->payload.native_handle == NULL) {
        return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
      }
      break;
    case AMDF_EXTERNAL_MEMORY_TYPE_HOST_POINTER:
      if (value->payload.host_pointer == NULL) {
        return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
      }
      break;
    case AMDF_EXTERNAL_MEMORY_TYPE_DEVICE_ADDRESS:
      if (value->payload.device_address == 0 ||
          !amdf_external_memory_provenance_is_valid(&value->provenance)) {
        return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
      }
      break;
    default:
      return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  return AMDF_STATUS_OK;
}

static amdf_status_t amdf_memory_validate_create_info(
    const amdf_memory_create_info_t* create_info) {
  const amdf_status_t status = amdf_structure_validate_input(
      create_info, AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO,
      (uint32_t)sizeof(amdf_memory_create_info_t));
  if (!amdf_status_is_ok(status)) {
    return status;
  }
  if (create_info->memory_class < AMDF_MEMORY_CLASS_SYSTEM ||
      create_info->memory_class > AMDF_MEMORY_CLASS_REGISTERED_HOST) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  if ((create_info->required_flags & ~amdf_memory_known_flags()) != 0 ||
      create_info->byte_length == 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  if (create_info->minimum_alignment != 0 &&
      (create_info->minimum_alignment & (create_info->minimum_alignment - 1)) !=
          0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  if ((create_info->memory_class == AMDF_MEMORY_CLASS_REGISTERED_HOST) !=
      (create_info->registered_host_pointer != NULL)) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  return AMDF_STATUS_OK;
}

static amdf_status_t amdf_memory_validate_import_info(
    const amdf_memory_import_info_t* import_info) {
  const amdf_status_t status = amdf_structure_validate_input(
      import_info, AMDF_STRUCTURE_TYPE_MEMORY_IMPORT_INFO,
      (uint32_t)sizeof(amdf_memory_import_info_t));
  if (!amdf_status_is_ok(status)) return status;
  if (import_info->reserved != 0 ||
      (import_info->required_flags & ~amdf_memory_known_flags()) != 0 ||
      (import_info->minimum_alignment != 0 &&
       (import_info->minimum_alignment &
        (import_info->minimum_alignment - 1)) != 0)) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  return AMDF_STATUS_OK;
}

static amdf_status_t amdf_memory_validate_export_info(
    const amdf_memory_t* memory, const amdf_memory_export_info_t* export_info) {
  const amdf_status_t status = amdf_structure_validate_input(
      export_info, AMDF_STRUCTURE_TYPE_MEMORY_EXPORT_INFO,
      (uint32_t)sizeof(amdf_memory_export_info_t));
  if (!amdf_status_is_ok(status)) return status;
  if (!amdf_external_memory_type_is_valid(export_info->external_memory_type) ||
      export_info->reserved != 0 || export_info->byte_length == 0 ||
      export_info->byte_offset > memory->info.byte_length ||
      export_info->byte_length >
          memory->info.byte_length - export_info->byte_offset) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  return AMDF_STATUS_OK;
}

static amdf_status_t amdf_memory_validate_site(const amdf_memory_site_t* site) {
  const amdf_status_t status =
      amdf_structure_validate_input(site, AMDF_STRUCTURE_TYPE_MEMORY_SITE,
                                    (uint32_t)sizeof(amdf_memory_site_t));
  if (!amdf_status_is_ok(status)) return status;
  if (site->memory == NULL || site->reserved != 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_memory_initialize(amdf_memory_t* memory,
                                     const amdf_memory_vtable_t* vtable,
                                     amdf_device_t* device) {
  const amdf_status_t status = amdf_device_register_child(device);
  if (!amdf_status_is_ok(status)) {
    return status;
  }
  memory->host_allocator = amdf_device_host_allocator(device);
  memory->vtable = vtable;
  memory->device = device;
  amdf_child_tracker_initialize(&memory->children);
  return AMDF_STATUS_OK;
}

void amdf_memory_deinitialize(amdf_memory_t* memory) {
  amdf_device_unregister_child(memory->device);
  memory->device = NULL;
}

amdf_status_t amdf_memory_register_child(amdf_memory_t* memory) {
  return amdf_child_tracker_register(&memory->children);
}

void amdf_memory_unregister_child(amdf_memory_t* memory) {
  amdf_child_tracker_unregister(&memory->children);
}

amdf_allocator_t amdf_memory_host_allocator(const amdf_memory_t* memory) {
  return memory->host_allocator;
}

amdf_status_t AMDF_CALL amdf_device_query_memory_profile(
    amdf_device_t* device, uint32_t memory_profile_ordinal,
    amdf_memory_profile_t* out_profile) {
  if (device == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  const amdf_status_t validation_status = amdf_structure_validate_output(
      out_profile, AMDF_STRUCTURE_TYPE_MEMORY_PROFILE,
      (uint32_t)sizeof(amdf_memory_profile_t));
  if (!amdf_status_is_ok(validation_status)) return validation_status;

  amdf_memory_profile_t profile = {
      .type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE,
      .structure_size = out_profile->structure_size,
      .next = out_profile->next,
  };
  const amdf_status_t status = device->vtable->query_memory_profile(
      device, memory_profile_ordinal, &profile);
  if (amdf_status_is_ok(status)) {
    profile.type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE;
    profile.structure_size = out_profile->structure_size;
    profile.next = out_profile->next;
    *out_profile = profile;
  }
  return status;
}

amdf_status_t AMDF_CALL amdf_memory_create(
    amdf_device_t* device, const amdf_memory_create_info_t* create_info,
    amdf_memory_t** out_memory) {
  if (out_memory == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  if (device == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  const amdf_status_t status = amdf_memory_validate_create_info(create_info);
  if (!amdf_status_is_ok(status)) {
    return status;
  }

  amdf_memory_t* memory = NULL;
  const amdf_status_t create_status =
      device->vtable->memory_create(device, create_info, &memory);
  if (amdf_status_is_ok(create_status)) {
    assert(memory != NULL && "successful memory creation must return memory");
    *out_memory = memory;
  }
  return create_status;
}

amdf_status_t AMDF_CALL amdf_memory_import(
    amdf_device_t* device, const amdf_memory_import_info_t* import_info,
    amdf_external_memory_t* inout_external_memory, amdf_memory_t** out_memory) {
  if (out_memory == NULL || device == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  amdf_status_t status = amdf_memory_validate_import_info(import_info);
  if (!amdf_status_is_ok(status)) return status;
  status = amdf_external_memory_validate(inout_external_memory);
  if (!amdf_status_is_ok(status)) return status;

  amdf_memory_t* memory = NULL;
  status = device->vtable->memory_import(device, import_info,
                                         inout_external_memory, &memory);
  if (amdf_status_is_ok(status)) {
    assert(memory != NULL && "successful memory import must return memory");
    memset(inout_external_memory, 0, sizeof(*inout_external_memory));
    *out_memory = memory;
  }
  return status;
}

amdf_status_t AMDF_CALL amdf_memory_query_info(amdf_memory_t* memory,
                                               amdf_memory_info_t* out_info) {
  if (memory == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  const amdf_status_t status =
      amdf_structure_validate_output(out_info, AMDF_STRUCTURE_TYPE_MEMORY_INFO,
                                     (uint32_t)sizeof(amdf_memory_info_t));
  if (!amdf_status_is_ok(status)) {
    return status;
  }
  const uint32_t structure_size = out_info->structure_size;
  void* const next = out_info->next;
  *out_info = memory->info;
  out_info->structure_size = structure_size;
  out_info->next = next;
  return AMDF_STATUS_OK;
}

amdf_status_t AMDF_CALL amdf_memory_export(
    amdf_memory_t* memory, const amdf_memory_export_info_t* export_info,
    amdf_external_memory_t* out_value) {
  if (out_value == NULL || memory == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  const amdf_status_t validation_status =
      amdf_memory_validate_export_info(memory, export_info);
  if (!amdf_status_is_ok(validation_status)) return validation_status;
  if ((memory->info.flags & AMDF_MEMORY_FLAG_SHAREABLE) == 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }

  amdf_external_memory_t value = {0};
  const amdf_status_t status =
      memory->vtable->export_external(memory, export_info, &value);
  if (amdf_status_is_ok(status)) {
    assert(value.release != NULL &&
           "successful memory export must own its payload lifetime");
    value.type = export_info->external_memory_type;
    value.reserved = 0;
    value.source_byte_offset =
        memory->info.source_byte_offset + export_info->byte_offset;
    value.byte_length = export_info->byte_length;
    value.physical_backing_id = memory->info.physical_backing_id;
    *out_value = value;
  }
  return status;
}

void AMDF_CALL amdf_external_memory_release(amdf_external_memory_t* value) {
  if (value == NULL) return;
  if (value->release != NULL) {
    value->release(value->release_user_data, value->type, value->payload);
  }
  memset(value, 0, sizeof(*value));
}

amdf_status_t AMDF_CALL
amdf_memory_query_pair_info(const amdf_memory_site_t* producer_site,
                            const amdf_memory_site_t* consumer_site,
                            amdf_memory_pair_info_t* out_info) {
  amdf_status_t status = amdf_memory_validate_site(producer_site);
  if (!amdf_status_is_ok(status)) return status;
  status = amdf_memory_validate_site(consumer_site);
  if (!amdf_status_is_ok(status)) return status;
  status = amdf_structure_validate_output(
      out_info, AMDF_STRUCTURE_TYPE_MEMORY_PAIR_INFO,
      (uint32_t)sizeof(amdf_memory_pair_info_t));
  if (!amdf_status_is_ok(status)) return status;

  const amdf_physical_memory_id_t* producer_id =
      &producer_site->memory->info.physical_backing_id;
  const amdf_physical_memory_id_t* consumer_id =
      &consumer_site->memory->info.physical_backing_id;
  if (amdf_physical_memory_id_is_valid(producer_id) &&
      amdf_physical_memory_id_is_valid(consumer_id) &&
      !amdf_physical_memory_id_is_equal(producer_id, consumer_id)) {
    return amdf_make_api_status(AMDF_STATUS_CODE_FAILED_PRECONDITION);
  }

  amdf_memory_pair_info_t info = {
      .type = AMDF_STRUCTURE_TYPE_MEMORY_PAIR_INFO,
      .structure_size = out_info->structure_size,
      .next = out_info->next,
  };
  status = producer_site->memory->vtable->query_pair_info(producer_site,
                                                          consumer_site, &info);
  if (amdf_status_is_ok(status)) {
    info.type = AMDF_STRUCTURE_TYPE_MEMORY_PAIR_INFO;
    info.structure_size = out_info->structure_size;
    info.next = out_info->next;
    *out_info = info;
  }
  return status;
}

amdf_status_t AMDF_CALL amdf_memory_map(amdf_memory_t* memory,
                                        const amdf_memory_map_info_t* map_info,
                                        amdf_host_mapping_t** out_mapping) {
  if (out_mapping == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  if (memory == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  const amdf_status_t status = amdf_structure_validate_input(
      map_info, AMDF_STRUCTURE_TYPE_MEMORY_MAP_INFO,
      (uint32_t)sizeof(amdf_memory_map_info_t));
  if (!amdf_status_is_ok(status)) {
    return status;
  }
  const amdf_memory_map_flags_t known_flags =
      AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE;
  if (map_info->flags == 0 || (map_info->flags & ~known_flags) != 0 ||
      map_info->byte_length == 0 ||
      map_info->byte_offset > memory->info.byte_length ||
      map_info->byte_length >
          memory->info.byte_length - map_info->byte_offset) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  if ((memory->info.flags & AMDF_MEMORY_FLAG_HOST_VISIBLE) == 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }

  amdf_host_mapping_t* mapping = NULL;
  const amdf_status_t map_status =
      memory->vtable->map(memory, map_info, &mapping);
  if (amdf_status_is_ok(map_status)) {
    assert(mapping != NULL && "successful memory map must return a mapping");
    *out_mapping = mapping;
  }
  return map_status;
}

amdf_status_t AMDF_CALL amdf_memory_destroy(amdf_memory_t* memory) {
  if (memory == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  if (amdf_child_tracker_count(&memory->children) != 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_BUSY);
  }
  const amdf_status_t status = memory->vtable->destroy_native(memory);
  if (amdf_status_is_ok(status)) {
    const amdf_allocator_t host_allocator = memory->host_allocator;
    amdf_memory_deinitialize(memory);
    amdf_free(host_allocator, memory);
  }
  return status;
}
