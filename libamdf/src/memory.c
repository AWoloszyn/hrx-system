// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/memory.h"

#include <stddef.h>
#include <string.h>

#include "libamdf/src/allocator.h"
#include "libamdf/src/device.h"
#include "libamdf/src/structure.h"

static amdf_memory_flags_t amdf_memory_known_flags(void) {
  return AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_DEVICE_LOCAL |
         AMDF_MEMORY_FLAG_SHAREABLE | AMDF_MEMORY_FLAG_QUEUE_STORAGE |
         AMDF_MEMORY_FLAG_HOST_COHERENT | AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
}

static amdf_memory_access_t amdf_memory_known_device_access(void) {
  return AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE |
         AMDF_MEMORY_ACCESS_EXECUTE;
}

static amdf_atomic_operations_t amdf_memory_known_atomic_operations(void) {
  return AMDF_ATOMIC_OPERATION_WAIT | AMDF_ATOMIC_OPERATION_STORE |
         AMDF_ATOMIC_OPERATION_ADD | AMDF_ATOMIC_OPERATION_SUBTRACT |
         AMDF_ATOMIC_OPERATION_AND | AMDF_ATOMIC_OPERATION_OR |
         AMDF_ATOMIC_OPERATION_XOR;
}

static bool amdf_memory_is_power_of_two(uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

static bool amdf_external_memory_type_is_valid(
    amdf_external_memory_type_t type) {
  return type >= AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD &&
         type <= AMDF_EXTERNAL_MEMORY_TYPE_DEVICE_ADDRESS;
}

static bool amdf_memory_construction_capabilities_are_zero(
    const amdf_memory_construction_capabilities_t* capabilities) {
  return capabilities->maximum_byte_length == 0 &&
         capabilities->byte_length_granularity == 0 &&
         capabilities->registered_host_pointer_alignment == 0 &&
         capabilities->minimum_alignment == 0 &&
         capabilities->maximum_alignment == 0 &&
         capabilities->native_byte_length_granularity == 0;
}

static bool amdf_memory_construction_capabilities_are_valid(
    const amdf_memory_construction_capabilities_t* capabilities,
    bool requires_host_pointer) {
  return capabilities->maximum_byte_length != 0 &&
         capabilities->byte_length_granularity != 0 &&
         (!requires_host_pointer ||
          amdf_memory_is_power_of_two(
              capabilities->registered_host_pointer_alignment)) &&
         (requires_host_pointer ||
          capabilities->registered_host_pointer_alignment == 0) &&
         amdf_memory_is_power_of_two(capabilities->minimum_alignment) &&
         amdf_memory_is_power_of_two(capabilities->maximum_alignment) &&
         capabilities->minimum_alignment <= capabilities->maximum_alignment &&
         capabilities->native_byte_length_granularity != 0;
}

static void amdf_memory_profile_assert_valid(
    const amdf_memory_profile_t* profile) {
  const amdf_memory_profile_roles_t known_roles =
      AMDF_MEMORY_PROFILE_ROLE_CREATE | AMDF_MEMORY_PROFILE_ROLE_REGISTER |
      AMDF_MEMORY_PROFILE_ROLE_IMPORT | AMDF_MEMORY_PROFILE_ROLE_EXPORT |
      AMDF_MEMORY_PROFILE_ROLE_HOST_MAP |
      AMDF_MEMORY_PROFILE_ROLE_MAPPING_SOURCE |
      AMDF_MEMORY_PROFILE_ROLE_MAPPING_TARGET;
  (void)known_roles;
  amdf_assert(profile->memory_class >= AMDF_MEMORY_CLASS_SYSTEM &&
              profile->memory_class <= AMDF_MEMORY_CLASS_REGISTERED_HOST &&
              "memory profiles must report a concrete placement class");
  amdf_assert(profile->roles != 0 && (profile->roles & ~known_roles) == 0 &&
              "memory profiles must report only known roles");
  amdf_assert((profile->roles & AMDF_MEMORY_PROFILE_ROLE_CREATE) == 0 ||
              (profile->roles & AMDF_MEMORY_PROFILE_ROLE_REGISTER) == 0);
  amdf_assert((profile->guaranteed_flags & ~profile->supported_flags) == 0 &&
              (profile->supported_flags & ~amdf_memory_known_flags()) == 0 &&
              "guaranteed memory flags must be a subset of supported flags");
  amdf_assert((profile->guaranteed_device_access &
               ~profile->supported_device_access) == 0 &&
              (profile->supported_device_access &
               ~amdf_memory_known_device_access()) == 0 &&
              "guaranteed device access must be a subset of supported access");
  amdf_assert((profile->atomic_operations_32 &
               ~amdf_memory_known_atomic_operations()) == 0 &&
              (profile->atomic_operations_64 &
               ~amdf_memory_known_atomic_operations()) == 0 &&
              "memory profiles must report only known atomic operations");
  amdf_assert(profile->reserved == 0 &&
              "memory profiles must leave reserved fields zero");

  const bool allocation_valid = amdf_memory_construction_capabilities_are_valid(
      &profile->allocation, false);
  const bool allocation_zero =
      amdf_memory_construction_capabilities_are_zero(&profile->allocation);
  amdf_assert(((profile->roles & AMDF_MEMORY_PROFILE_ROLE_CREATE) != 0
                   ? allocation_valid
                   : allocation_zero) &&
              "allocation capabilities must exactly match the CREATE role");
  const bool registration_valid =
      amdf_memory_construction_capabilities_are_valid(&profile->registration,
                                                      true);
  const bool registration_zero =
      amdf_memory_construction_capabilities_are_zero(&profile->registration);
  amdf_assert(((profile->roles & AMDF_MEMORY_PROFILE_ROLE_REGISTER) != 0
                   ? registration_valid
                   : registration_zero) &&
              "registration capabilities must exactly match the REGISTER role");
  const bool import_valid =
      amdf_memory_construction_capabilities_are_valid(&profile->import, false);
  const bool import_zero =
      amdf_memory_construction_capabilities_are_zero(&profile->import);
  amdf_assert(((profile->roles & AMDF_MEMORY_PROFILE_ROLE_IMPORT) != 0
                   ? import_valid
                   : import_zero) &&
              "import capabilities must exactly match the IMPORT role");
  (void)allocation_valid;
  (void)allocation_zero;
  (void)registration_valid;
  (void)registration_zero;
  (void)import_valid;
  (void)import_zero;

  if ((profile->supported_flags & AMDF_MEMORY_FLAG_DEVICE_ADDRESS) != 0) {
    amdf_assert(profile->device_address.address_domain_ordinal !=
                    AMDF_ADDRESS_DOMAIN_ORDINAL_NONE &&
                profile->device_address.address_bit_count <= 64 &&
                amdf_memory_is_power_of_two(
                    profile->device_address.minimum_alignment) &&
                "addressable profiles must report a domain and alignment");
    if (profile->device_address.address_bit_count ==
        AMDF_MEMORY_ADDRESS_BIT_COUNT_UNKNOWN) {
      amdf_assert(profile->device_address.minimum_address == 0 &&
                  profile->device_address.maximum_address == 0 &&
                  "unknown address envelopes must leave numeric bounds zero");
    } else {
      const uint64_t address_width_maximum =
          profile->device_address.address_bit_count == 64
              ? UINT64_MAX
              : (UINT64_C(1) << profile->device_address.address_bit_count) - 1;
      (void)address_width_maximum;
      amdf_assert(profile->device_address.minimum_address <=
                      profile->device_address.maximum_address &&
                  profile->device_address.maximum_address <=
                      address_width_maximum &&
                  "known address envelopes must fit their native width");
    }
  } else {
    amdf_assert(
        profile->device_address.address_domain_ordinal ==
            AMDF_ADDRESS_DOMAIN_ORDINAL_NONE &&
        profile->device_address.address_bit_count == 0 &&
        profile->device_address.minimum_address == 0 &&
        profile->device_address.maximum_address == 0 &&
        profile->device_address.minimum_alignment == 0 &&
        "profiles without DEVICE_ADDRESS must leave address limits empty");
  }
  if ((profile->roles & AMDF_MEMORY_PROFILE_ROLE_HOST_MAP) != 0) {
    amdf_assert(
        profile->host_mapping.maximum_byte_length != 0 &&
        profile->host_mapping.byte_offset_granularity != 0 &&
        profile->host_mapping.byte_length_granularity != 0 &&
        profile->host_mapping.supported_access != 0 &&
        (profile->host_mapping.supported_access &
         ~(AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE)) == 0 &&
        profile->host_mapping.reserved == 0 &&
        "host-mappable profiles must report exact range and access limits");
  } else {
    amdf_assert(profile->host_mapping.maximum_byte_length == 0 &&
                profile->host_mapping.byte_offset_granularity == 0 &&
                profile->host_mapping.byte_length_granularity == 0 &&
                profile->host_mapping.supported_access == 0 &&
                profile->host_mapping.reserved == 0 &&
                "profiles without HOST_MAP must leave mapping limits empty");
  }
  amdf_assert(
      ((profile->roles & AMDF_MEMORY_PROFILE_ROLE_HOST_MAP) != 0) ==
          ((profile->supported_flags & AMDF_MEMORY_FLAG_HOST_VISIBLE) != 0) &&
      "HOST_VISIBLE and HOST_MAP must describe the same capability");
  amdf_assert(
      ((profile->roles & AMDF_MEMORY_PROFILE_ROLE_EXPORT) != 0) ==
          ((profile->supported_flags & AMDF_MEMORY_FLAG_SHAREABLE) != 0) &&
      "SHAREABLE and EXPORT must describe the same capability");
  amdf_assert(profile->external_memory_support_count <=
                  AMDF_MEMORY_PROFILE_EXTERNAL_SUPPORT_CAPACITY &&
              "external-memory support must fit the fixed profile storage");
  bool has_import_support = false;
  bool has_export_support = false;
  for (uint32_t i = 0; i < profile->external_memory_support_count; ++i) {
    const amdf_external_memory_support_t* support =
        &profile->external_memory_support[i];
    const amdf_external_memory_support_flags_t known_support_flags =
        AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_IMPORT |
        AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_EXPORT |
        AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_SOURCE_OFFSET |
        AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_CROSS_PROCESS |
        AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_FOREIGN_API;
    (void)known_support_flags;
    amdf_assert(
        amdf_external_memory_type_is_valid(support->type) &&
        (support->flags & (AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_IMPORT |
                           AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_EXPORT)) != 0 &&
        (support->flags & ~known_support_flags) == 0 &&
        support->byte_length_alignment != 0 &&
        (((support->flags & AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_SOURCE_OFFSET) !=
          0) == (support->source_offset_alignment != 0)) &&
        "external-memory support entries must be complete");
    for (uint32_t j = 0; j < i; ++j) {
      amdf_assert(profile->external_memory_support[j].type != support->type &&
                  "external-memory support types must be unique");
    }
    has_import_support |=
        (support->flags & AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_IMPORT) != 0;
    has_export_support |=
        (support->flags & AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_EXPORT) != 0;
  }
  amdf_assert(has_import_support ==
                  ((profile->roles & AMDF_MEMORY_PROFILE_ROLE_IMPORT) != 0) &&
              has_export_support ==
                  ((profile->roles & AMDF_MEMORY_PROFILE_ROLE_EXPORT) != 0) &&
              "transport roles and support entries must agree");
  (void)has_import_support;
  (void)has_export_support;
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
      if (value->payload.file_descriptor < 0 ||
          amdf_external_memory_provenance_is_valid(&value->provenance)) {
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
      if (value->payload.native_handle == NULL ||
          amdf_external_memory_provenance_is_valid(&value->provenance)) {
        return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
      }
      break;
    case AMDF_EXTERNAL_MEMORY_TYPE_HOST_POINTER:
      if (value->payload.host_pointer == NULL ||
          amdf_external_memory_provenance_is_valid(&value->provenance)) {
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

static const amdf_external_memory_support_t*
amdf_memory_profile_find_external_support(const amdf_memory_profile_t* profile,
                                          amdf_external_memory_type_t type) {
  amdf_assert(
      profile->external_memory_support_count <=
          AMDF_MEMORY_PROFILE_EXTERNAL_SUPPORT_CAPACITY &&
      "memory profile external support count must fit its fixed storage");
  for (uint32_t i = 0; i < profile->external_memory_support_count; ++i) {
    if (profile->external_memory_support[i].type == type) {
      return &profile->external_memory_support[i];
    }
  }
  return NULL;
}

static amdf_status_t amdf_memory_validate_external_range(
    const amdf_external_memory_support_t* support, uint64_t source_byte_offset,
    uint64_t byte_length) {
  if (source_byte_offset != 0 &&
      ((support->flags & AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_SOURCE_OFFSET) ==
           0 ||
       support->source_offset_alignment == 0 ||
       source_byte_offset % support->source_offset_alignment != 0)) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  amdf_assert(support->byte_length_alignment != 0 &&
              "external memory byte-length alignment must be nonzero");
  if (byte_length % support->byte_length_alignment != 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  if (support->maximum_byte_length != 0 &&
      byte_length > support->maximum_byte_length) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  return AMDF_STATUS_OK;
}

static amdf_status_t amdf_memory_query_profile(
    amdf_device_t* device, uint32_t memory_profile_ordinal,
    amdf_memory_profile_t* out_profile) {
  amdf_memory_profile_t profile = {
      .type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE,
      .structure_size = sizeof(profile),
  };
  const amdf_status_t status = amdf_device_query_memory_profile(
      device, memory_profile_ordinal, &profile);
  if (amdf_status_is_ok(status)) {
    *out_profile = profile;
  }
  return status;
}

static amdf_status_t amdf_memory_validate_profile_request(
    const amdf_memory_profile_t* profile,
    amdf_memory_profile_roles_t required_role,
    const amdf_memory_construction_capabilities_t* capabilities,
    amdf_memory_flags_t required_flags, amdf_memory_access_t device_access,
    uint64_t byte_length, uint64_t minimum_alignment) {
  if ((profile->roles & required_role) == 0 ||
      (required_flags & ~profile->supported_flags) != 0 ||
      (device_access & profile->guaranteed_device_access) !=
          profile->guaranteed_device_access ||
      (device_access & ~profile->supported_device_access) != 0 ||
      capabilities->maximum_byte_length == 0 ||
      byte_length > capabilities->maximum_byte_length ||
      capabilities->byte_length_granularity == 0 ||
      byte_length % capabilities->byte_length_granularity != 0 ||
      (minimum_alignment != 0 &&
       (capabilities->maximum_alignment == 0 ||
        minimum_alignment > capabilities->maximum_alignment))) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  return AMDF_STATUS_OK;
}

static amdf_status_t amdf_memory_query_external_support(
    const amdf_memory_profile_t* profile,
    amdf_external_memory_type_t external_memory_type,
    amdf_external_memory_support_flags_t required_support_flag,
    uint64_t source_byte_offset, uint64_t byte_length,
    amdf_external_memory_support_t* out_support) {
  const amdf_external_memory_support_t* support =
      amdf_memory_profile_find_external_support(profile, external_memory_type);
  if (support == NULL || (support->flags & required_support_flag) == 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  const amdf_status_t status = amdf_memory_validate_external_range(
      support, source_byte_offset, byte_length);
  if (amdf_status_is_ok(status)) {
    *out_support = *support;
  }
  return status;
}

static amdf_status_t amdf_memory_validate_create_info(
    const amdf_memory_create_info_t* create_info) {
  const amdf_status_t status = amdf_structure_validate_input(
      create_info, AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO,
      (uint32_t)sizeof(amdf_memory_create_info_t));
  if (!amdf_status_is_ok(status)) {
    return status;
  }
  if ((create_info->required_flags & ~amdf_memory_known_flags()) != 0 ||
      (create_info->device_access & ~amdf_memory_known_device_access()) != 0 ||
      create_info->byte_length == 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  if (create_info->minimum_alignment != 0 &&
      !amdf_memory_is_power_of_two(create_info->minimum_alignment)) {
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
  if ((import_info->required_flags & ~amdf_memory_known_flags()) != 0 ||
      (import_info->device_access & ~amdf_memory_known_device_access()) != 0 ||
      (import_info->minimum_alignment != 0 &&
       !amdf_memory_is_power_of_two(import_info->minimum_alignment))) {
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

static void amdf_memory_assert_result(
    const amdf_memory_t* memory, const amdf_memory_profile_t* profile,
    const amdf_memory_construction_capabilities_t* construction_capabilities,
    amdf_memory_flags_t required_flags, amdf_memory_access_t device_access,
    uint64_t minimum_byte_length, uint64_t minimum_alignment) {
  amdf_assert(memory != NULL && "successful construction must return memory");
  amdf_assert(memory->info.memory_profile_ordinal == profile->ordinal &&
              "memory must retain the selected profile");
  amdf_assert(memory->info.memory_class == profile->memory_class &&
              "memory class must come from the selected profile");
  amdf_assert(memory->info.device_access == device_access &&
              "memory must retain the exact requested device access");
  amdf_assert(memory->info.address_domain_ordinal ==
                  profile->device_address.address_domain_ordinal &&
              "memory must retain the selected profile address domain");
  amdf_assert((memory->info.flags & profile->guaranteed_flags) ==
                  profile->guaranteed_flags &&
              "memory must achieve every guaranteed profile property");
  amdf_assert((memory->info.flags & required_flags) == required_flags &&
              "memory must achieve every required property");
  amdf_assert((memory->info.flags & ~profile->supported_flags) == 0 &&
              "memory cannot achieve properties absent from its profile");
  amdf_assert((memory->info.atomic_operations_32 &
               ~profile->atomic_operations_32) == 0 &&
              (memory->info.atomic_operations_64 &
               ~profile->atomic_operations_64) == 0 &&
              "memory atomics must be a subset of the selected profile");
  amdf_assert(memory->info.byte_length >= minimum_byte_length &&
              "memory must achieve the requested logical length");
  amdf_assert(
      amdf_memory_is_power_of_two(memory->info.alignment) &&
      memory->info.alignment >= construction_capabilities->minimum_alignment &&
      (minimum_alignment == 0 || memory->info.alignment >= minimum_alignment) &&
      "memory must achieve the selected construction alignment");
  amdf_assert(memory->info.native_allocation_byte_length != 0 &&
              memory->info.native_allocation_granularity ==
                  construction_capabilities->native_byte_length_granularity &&
              memory->info.native_allocation_byte_length %
                      memory->info.native_allocation_granularity ==
                  0 &&
              "memory must report the selected native geometry");
  amdf_assert(memory->info.source_byte_offset <=
                  memory->info.native_allocation_byte_length &&
              memory->info.byte_length <=
                  memory->info.native_allocation_byte_length -
                      memory->info.source_byte_offset &&
              "logical memory must fit within its native allocation");
  amdf_assert(memory->info.address_kinds ==
                  ((memory->info.flags & AMDF_MEMORY_FLAG_DEVICE_ADDRESS) != 0
                       ? profile->address_kinds
                       : 0) &&
              "memory must establish the selected profile address kinds");
  for (amdf_memory_address_kind_t kind = AMDF_MEMORY_ADDRESS_GPU;
       kind <= AMDF_MEMORY_ADDRESS_XDNA_FIRMWARE; ++kind) {
    if ((memory->info.address_kinds & (UINT64_C(1) << kind)) == 0) continue;
    const uint64_t address = memory->addresses[kind];
    amdf_assert((address & (memory->info.alignment - 1)) == 0 &&
                "every memory address must satisfy the achieved alignment");
    if (profile->device_address.address_bit_count !=
        AMDF_MEMORY_ADDRESS_BIT_COUNT_UNKNOWN) {
      amdf_assert(address >= profile->device_address.minimum_address &&
                  address <= profile->device_address.maximum_address &&
                  memory->info.byte_length - 1 <=
                      profile->device_address.maximum_address - address &&
                  "every memory range must fit the selected numeric envelope");
    }
  }
}

void amdf_memory_initialize(amdf_memory_t* memory,
                            const amdf_memory_vtable_t* vtable,
                            amdf_device_t* device) {
  memory->host_allocator = amdf_device_host_allocator(device);
  memory->vtable = vtable;
  memory->device = device;
  amdf_child_tracker_initialize(&memory->children);
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
    profile.ordinal = memory_profile_ordinal;
    amdf_memory_profile_assert_valid(&profile);
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
  amdf_status_t status = amdf_memory_validate_create_info(create_info);
  if (!amdf_status_is_ok(status)) return status;

  amdf_memory_profile_t profile;
  status = amdf_memory_query_profile(
      device, create_info->memory_profile_ordinal, &profile);
  if (!amdf_status_is_ok(status)) return status;
  const bool profile_creates =
      (profile.roles & AMDF_MEMORY_PROFILE_ROLE_CREATE) != 0;
  const bool profile_registers =
      (profile.roles & AMDF_MEMORY_PROFILE_ROLE_REGISTER) != 0;
  if (profile_creates == profile_registers) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  const bool is_registration = profile_registers;
  if (is_registration != (create_info->registered_host_pointer != NULL)) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  const amdf_memory_profile_roles_t required_role =
      is_registration ? AMDF_MEMORY_PROFILE_ROLE_REGISTER
                      : AMDF_MEMORY_PROFILE_ROLE_CREATE;
  const amdf_memory_construction_capabilities_t* capabilities =
      is_registration ? &profile.registration : &profile.allocation;
  status = amdf_memory_validate_profile_request(
      &profile, required_role, capabilities, create_info->required_flags,
      create_info->device_access, create_info->byte_length,
      create_info->minimum_alignment);
  if (!amdf_status_is_ok(status)) return status;
  if (is_registration) {
    const uintptr_t pointer = (uintptr_t)create_info->registered_host_pointer;
    amdf_assert(capabilities->registered_host_pointer_alignment != 0 &&
                "registration profiles must report host pointer alignment");
    if (pointer % capabilities->registered_host_pointer_alignment != 0 ||
        (create_info->minimum_alignment != 0 &&
         (pointer & (create_info->minimum_alignment - 1)) != 0)) {
      return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
    }
    if (create_info->byte_length > UINTPTR_MAX - pointer) {
      return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
    }
  }

  amdf_memory_t* memory = NULL;
  status =
      device->vtable->memory_create(device, &profile, create_info, &memory);
  if (amdf_status_is_ok(status)) {
    amdf_memory_assert_result(
        memory, &profile, capabilities, create_info->required_flags,
        create_info->device_access, create_info->byte_length,
        create_info->minimum_alignment);
    *out_memory = memory;
  }
  return status;
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

  amdf_memory_profile_t profile;
  status = amdf_memory_query_profile(
      device, import_info->memory_profile_ordinal, &profile);
  if (!amdf_status_is_ok(status)) return status;
  status = amdf_memory_validate_profile_request(
      &profile, AMDF_MEMORY_PROFILE_ROLE_IMPORT, &profile.import,
      import_info->required_flags, import_info->device_access,
      inout_external_memory->byte_length, import_info->minimum_alignment);
  if (!amdf_status_is_ok(status)) return status;
  if (import_info->minimum_alignment != 0 &&
      (inout_external_memory->source_byte_offset &
       (import_info->minimum_alignment - 1)) != 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }

  amdf_external_memory_support_t support;
  status = amdf_memory_query_external_support(
      &profile, inout_external_memory->type,
      AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_IMPORT,
      inout_external_memory->source_byte_offset,
      inout_external_memory->byte_length, &support);
  if (!amdf_status_is_ok(status)) return status;
  if (!amdf_external_memory_provenance_is_equal(
          &support.provenance, &inout_external_memory->provenance)) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }

  amdf_memory_t* memory = NULL;
  status = device->vtable->memory_import(device, &profile, import_info,
                                         inout_external_memory, &memory);
  if (amdf_status_is_ok(status)) {
    amdf_memory_assert_result(
        memory, &profile, &profile.import, import_info->required_flags,
        import_info->device_access, inout_external_memory->byte_length,
        import_info->minimum_alignment);
    amdf_assert(memory->info.byte_length ==
                    inout_external_memory->byte_length &&
                "import must preserve the exact external logical range");
    memset(inout_external_memory, 0, sizeof(*inout_external_memory));
    *out_memory = memory;
  }
  return status;
}

amdf_status_t AMDF_CALL amdf_memory_query_address(
    amdf_memory_t* memory, amdf_memory_address_kind_t kind,
    uint64_t* out_address) {
  if (memory == NULL || out_address == NULL ||
      kind > AMDF_MEMORY_ADDRESS_XDNA_FIRMWARE) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  if ((memory->info.address_kinds & (UINT64_C(1) << kind)) == 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  *out_address = memory->addresses[kind];
  return AMDF_STATUS_OK;
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

  if (memory->info.memory_profile_ordinal ==
      AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  const uint64_t source_byte_offset =
      memory->info.source_byte_offset + export_info->byte_offset;
  amdf_memory_profile_t profile;
  amdf_status_t status = amdf_memory_query_profile(
      memory->device, memory->info.memory_profile_ordinal, &profile);
  if (!amdf_status_is_ok(status)) return status;
  if ((profile.roles & AMDF_MEMORY_PROFILE_ROLE_EXPORT) == 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  amdf_external_memory_support_t support;
  status = amdf_memory_query_external_support(
      &profile, export_info->external_memory_type,
      AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_EXPORT, source_byte_offset,
      export_info->byte_length, &support);
  if (!amdf_status_is_ok(status)) return status;

  amdf_external_memory_t value = {0};
  status = memory->vtable->export_external(memory, export_info, &value);
  if (amdf_status_is_ok(status)) {
    amdf_assert(value.release != NULL &&
                "successful memory export must own its payload lifetime");
    value.type = export_info->external_memory_type;
    value.reserved = 0;
    value.provenance = support.provenance;
    value.source_byte_offset = source_byte_offset;
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

  if (!amdf_device_shares_provider_instance(producer_site->memory->device,
                                            consumer_site->memory->device)) {
    return amdf_make_api_status(AMDF_STATUS_CODE_FAILED_PRECONDITION);
  }

  const amdf_physical_memory_id_t* producer_id =
      &producer_site->memory->info.physical_backing_id;
  const amdf_physical_memory_id_t* consumer_id =
      &consumer_site->memory->info.physical_backing_id;
  if (!amdf_physical_memory_id_is_valid(producer_id) ||
      !amdf_physical_memory_id_is_valid(consumer_id)) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  if (!amdf_physical_memory_id_is_equal(producer_id, consumer_id)) {
    return amdf_make_api_status(AMDF_STATUS_CODE_FAILED_PRECONDITION);
  }

  amdf_memory_site_description_t producer = {0};
  status = producer_site->memory->vtable->describe_site(
      producer_site->memory, producer_site->queue_family_ordinal, &producer);
  if (!amdf_status_is_ok(status)) return status;
  amdf_memory_site_description_t consumer = {0};
  status = consumer_site->memory->vtable->describe_site(
      consumer_site->memory, consumer_site->queue_family_ordinal, &consumer);
  if (!amdf_status_is_ok(status)) return status;

  if ((producer.capabilities & AMDF_MEMORY_SITE_CAPABILITY_WRITE) == 0 ||
      (consumer.capabilities & AMDF_MEMORY_SITE_CAPABILITY_READ) == 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }

  amdf_memory_pair_info_t info = {
      .type = AMDF_STRUCTURE_TYPE_MEMORY_PAIR_INFO,
      .structure_size = out_info->structure_size,
      .next = out_info->next,
      .flags = AMDF_MEMORY_PAIR_FLAG_SHARED_BACKING_REACHABLE,
      .release = producer.release,
      .acquire = consumer.acquire,
  };
  if ((producer.capabilities & AMDF_MEMORY_SITE_CAPABILITY_MAPPING_SOURCE) !=
          0 &&
      (consumer.capabilities & AMDF_MEMORY_SITE_CAPABILITY_MAPPING_TARGET) !=
          0 &&
      amdf_memory_compatibility_domain_is_valid(&producer.mapping_domain) &&
      amdf_memory_compatibility_domain_is_equal(&producer.mapping_domain,
                                                &consumer.mapping_domain)) {
    info.flags |= AMDF_MEMORY_PAIR_FLAG_MAPPING_SOURCE;
  }
  if (amdf_memory_compatibility_domain_is_valid(&producer.atomic_domain) &&
      amdf_memory_compatibility_domain_is_equal(&producer.atomic_domain,
                                                &consumer.atomic_domain)) {
    info.atomic_reach.scope_32 =
        producer.atomic_reach.scope_32 < consumer.atomic_reach.scope_32
            ? producer.atomic_reach.scope_32
            : consumer.atomic_reach.scope_32;
    info.atomic_reach.scope_64 =
        producer.atomic_reach.scope_64 < consumer.atomic_reach.scope_64
            ? producer.atomic_reach.scope_64
            : consumer.atomic_reach.scope_64;
  }
  if ((producer.capabilities &
       AMDF_MEMORY_SITE_CAPABILITY_RELEASE_COST_KNOWN) != 0 &&
      (consumer.capabilities &
       AMDF_MEMORY_SITE_CAPABILITY_ACQUIRE_COST_KNOWN) != 0) {
    if (producer.release_fixed_cost_nanoseconds >
        UINT64_MAX - consumer.acquire_fixed_cost_nanoseconds) {
      return amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
    }
    info.flags |= AMDF_MEMORY_PAIR_FLAG_FIXED_COST_KNOWN;
    info.estimated_fixed_cost_nanoseconds =
        producer.release_fixed_cost_nanoseconds +
        consumer.acquire_fixed_cost_nanoseconds;
  }
  *out_info = info;
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

  if (memory->info.memory_profile_ordinal ==
      AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  amdf_memory_profile_t profile;
  amdf_status_t map_status = amdf_memory_query_profile(
      memory->device, memory->info.memory_profile_ordinal, &profile);
  if (!amdf_status_is_ok(map_status)) return map_status;
  if ((profile.roles & AMDF_MEMORY_PROFILE_ROLE_HOST_MAP) == 0 ||
      (map_info->flags & ~profile.host_mapping.supported_access) != 0 ||
      profile.host_mapping.maximum_byte_length == 0 ||
      map_info->byte_length > profile.host_mapping.maximum_byte_length) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  amdf_assert(profile.host_mapping.byte_offset_granularity != 0 &&
              profile.host_mapping.byte_length_granularity != 0 &&
              "host-mappable profiles must report their range granularity");
  if (map_info->byte_offset % profile.host_mapping.byte_offset_granularity !=
          0 ||
      map_info->byte_length % profile.host_mapping.byte_length_granularity !=
          0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }

  amdf_host_mapping_t* mapping = NULL;
  map_status = memory->vtable->map(memory, &profile, map_info, &mapping);
  if (amdf_status_is_ok(map_status)) {
    amdf_assert(mapping != NULL &&
                "successful memory map must return a mapping");
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
    amdf_free(host_allocator, memory);
  }
  return status;
}
