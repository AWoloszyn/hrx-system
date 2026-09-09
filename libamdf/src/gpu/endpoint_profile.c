// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/endpoint_profile.h"

#include <stddef.h>

static bool amdf_gpu_queue_family_properties_validate(
    const amdf_gpu_queue_family_properties_t* properties) {
  const amdf_queue_publication_modes_t known_publication_modes =
      AMDF_QUEUE_PUBLICATION_MODE_USER | AMDF_QUEUE_PUBLICATION_MODE_KERNEL;
  const amdf_queue_roles_t known_roles =
      AMDF_QUEUE_ROLE_COMPUTE | AMDF_QUEUE_ROLE_TRANSFER |
      AMDF_QUEUE_ROLE_ATOMIC | AMDF_QUEUE_ROLE_CACHE_CONTROL |
      AMDF_QUEUE_ROLE_VIRTUAL_MEMORY;
  const amdf_queue_producer_modes_t known_producer_modes =
      AMDF_QUEUE_PRODUCER_MODE_BIT_SINGLE | AMDF_QUEUE_PRODUCER_MODE_BIT_MULTI;
  const amdf_queue_priority_capabilities_t known_priorities =
      AMDF_QUEUE_PRIORITY_CAPABILITY_LOW |
      AMDF_QUEUE_PRIORITY_CAPABILITY_NORMAL |
      AMDF_QUEUE_PRIORITY_CAPABILITY_HIGH;
  const amdf_user_queue_capabilities_t known_user_capabilities =
      AMDF_USER_QUEUE_CAPABILITY_HOST_PRODUCER |
      AMDF_USER_QUEUE_CAPABILITY_DEVICE_PRODUCER;
  const amdf_kernel_queue_capabilities_t known_kernel_capabilities =
      AMDF_KERNEL_QUEUE_CAPABILITY_VECTOR_SUBMIT |
      AMDF_KERNEL_QUEUE_CAPABILITY_VIRTUAL_MEMORY |
      AMDF_KERNEL_QUEUE_CAPABILITY_EXTERNAL_SYNCHRONIZATION;
  if ((properties->command_type != AMDF_QUEUE_COMMAND_TYPE_GPU_PM4 &&
       properties->command_type != AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA &&
       properties->command_type != AMDF_QUEUE_COMMAND_TYPE_GPU_AQL) ||
      properties->format_version == 0 || properties->publication_modes == 0 ||
      (properties->publication_modes & ~known_publication_modes) != 0 ||
      properties->roles == 0 || (properties->roles & ~known_roles) != 0 ||
      (properties->user_queue_capabilities & ~known_user_capabilities) != 0 ||
      (properties->kernel_queue_capabilities & ~known_kernel_capabilities) !=
          0 ||
      (properties->producer_modes & ~known_producer_modes) != 0 ||
      (properties->priority_capabilities & ~known_priorities) != 0 ||
      properties->metadata.reserved != 0) {
    return false;
  }

  const bool supports_user =
      (properties->publication_modes & AMDF_QUEUE_PUBLICATION_MODE_USER) != 0;
  const bool supports_kernel =
      (properties->publication_modes & AMDF_QUEUE_PUBLICATION_MODE_KERNEL) != 0;
  const bool has_metadata =
      properties->metadata.command_type != AMDF_QUEUE_COMMAND_TYPE_UNKNOWN ||
      properties->metadata.dispatch_version != 0 ||
      properties->metadata.barrier_version != 0;
  if (!supports_kernel && properties->kernel_queue_capabilities != 0) {
    return false;
  }
  if (!supports_user) {
    return properties->user_queue_capabilities == 0 &&
           properties->producer_modes == 0 &&
           properties->priority_capabilities == 0 &&
           properties->metadata.command_type ==
               AMDF_QUEUE_COMMAND_TYPE_UNKNOWN &&
           properties->metadata.dispatch_version == 0 &&
           properties->metadata.barrier_version == 0 &&
           properties->minimum_ring_byte_length == 0 &&
           properties->maximum_ring_byte_length == 0 &&
           properties->ring_byte_length_alignment == 0;
  }

  const amdf_user_queue_capabilities_t producer_capabilities =
      AMDF_USER_QUEUE_CAPABILITY_HOST_PRODUCER |
      AMDF_USER_QUEUE_CAPABILITY_DEVICE_PRODUCER;
  if ((properties->user_queue_capabilities & producer_capabilities) == 0 ||
      properties->producer_modes == 0 ||
      properties->priority_capabilities == 0 ||
      properties->minimum_ring_byte_length == 0 ||
      (properties->minimum_ring_byte_length &
       (properties->minimum_ring_byte_length - 1)) != 0 ||
      properties->maximum_ring_byte_length <
          properties->minimum_ring_byte_length ||
      (properties->maximum_ring_byte_length &
       (properties->maximum_ring_byte_length - 1)) != 0 ||
      properties->ring_byte_length_alignment == 0 ||
      (properties->ring_byte_length_alignment &
       (properties->ring_byte_length_alignment - 1)) != 0 ||
      properties->minimum_ring_byte_length %
              properties->ring_byte_length_alignment !=
          0 ||
      properties->maximum_ring_byte_length %
              properties->ring_byte_length_alignment !=
          0) {
    return false;
  }
  if (has_metadata) {
    return properties->command_type == AMDF_QUEUE_COMMAND_TYPE_GPU_AQL &&
           properties->metadata.command_type ==
               AMDF_QUEUE_COMMAND_TYPE_GPU_AQL_METADATA &&
           properties->metadata.dispatch_version != 0 &&
           properties->metadata.barrier_version != 0;
  }
  return properties->metadata.command_type == AMDF_QUEUE_COMMAND_TYPE_UNKNOWN &&
         properties->metadata.dispatch_version == 0 &&
         properties->metadata.barrier_version == 0;
}

bool amdf_gpu_endpoint_profile_initialize(
    const amdf_gpu_endpoint_properties_t* properties,
    amdf_gpu_endpoint_profile_t* out_profile) {
  if (properties == NULL || out_profile == NULL ||
      properties->gfx_ip.major == 0 ||
      (properties->compute.wavefront_size != 32 &&
       properties->compute.wavefront_size != 64) ||
      properties->compute.compute_unit_count == 0 ||
      properties->compute.maximum_wave_count_per_compute_unit == 0 ||
      properties->compute.maximum_scratch_wave_count_per_compute_unit == 0 ||
      properties->compute.local_data_share_byte_length == 0 ||
      properties->topology.xcc_count == 0 ||
      properties->topology.shader_engine_count_per_xcc == 0 ||
      properties->queue_family_count > AMDF_GPU_QUEUE_FAMILY_CAPACITY) {
    return false;
  }

  if (properties->compute.maximum_scratch_wave_count_per_compute_unit >
      properties->compute.maximum_wave_count_per_compute_unit) {
    return false;
  }
  for (uint32_t i = 0; i < properties->queue_family_count; ++i) {
    if (!amdf_gpu_queue_family_properties_validate(
            &properties->queue_families[i])) {
      return false;
    }
  }

  amdf_gpu_endpoint_profile_t profile = {0};
  profile.info.type = AMDF_STRUCTURE_TYPE_GPU_ENDPOINT_INFO;
  profile.info.structure_size = sizeof(profile.info);
  profile.info.gfx_ip.major = properties->gfx_ip.major;
  profile.info.gfx_ip.minor = properties->gfx_ip.minor;
  profile.info.gfx_ip.stepping = properties->gfx_ip.stepping;
  profile.info.asic_revision = properties->asic_revision;
  profile.info.compute.wavefront_size = properties->compute.wavefront_size;
  profile.info.compute.compute_unit_count =
      properties->compute.compute_unit_count;
  profile.info.compute.maximum_wave_count_per_compute_unit =
      properties->compute.maximum_wave_count_per_compute_unit;
  profile.info.compute.maximum_scratch_wave_count_per_compute_unit =
      properties->compute.maximum_scratch_wave_count_per_compute_unit;
  profile.info.compute.local_data_share_byte_length =
      properties->compute.local_data_share_byte_length;
  profile.info.topology.xcc_count = properties->topology.xcc_count;
  profile.info.topology.shader_engine_count_per_xcc =
      properties->topology.shader_engine_count_per_xcc;
  profile.queue_family_count = properties->queue_family_count;
  for (uint32_t i = 0; i < properties->queue_family_count; ++i) {
    const amdf_gpu_queue_family_properties_t* source =
        &properties->queue_families[i];
    profile.queue_families[i] = (amdf_queue_family_info_t){
        .type = AMDF_STRUCTURE_TYPE_QUEUE_FAMILY_INFO,
        .structure_size = sizeof(amdf_queue_family_info_t),
        .ordinal = i,
        .command_type = source->command_type,
        .publication_modes = source->publication_modes,
        .format_version = source->format_version,
        .roles = source->roles,
        .user_queue_capabilities = source->user_queue_capabilities,
        .kernel_queue_capabilities = source->kernel_queue_capabilities,
        .producer_modes = source->producer_modes,
        .priority_capabilities = source->priority_capabilities,
        .metadata = source->metadata,
        .minimum_ring_byte_length = source->minimum_ring_byte_length,
        .maximum_ring_byte_length = source->maximum_ring_byte_length,
        .ring_byte_length_alignment = source->ring_byte_length_alignment,
    };
  }
  for (uint32_t i = 0; i < 2; ++i) {
    profile.device_modes[i] = properties->device_modes[i];
  }
  *out_profile = profile;
  return true;
}

const amdf_gpu_endpoint_info_t* amdf_gpu_endpoint_profile_get_info(
    const amdf_gpu_endpoint_profile_t* profile) {
  return &profile->info;
}

amdf_status_t amdf_gpu_endpoint_profile_query_device_features(
    const amdf_gpu_endpoint_profile_t* profile, amdf_gpu_device_mode_t mode,
    amdf_gpu_device_features_t* out_features) {
  if (mode > AMDF_GPU_DEVICE_MODE_PROCESS) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  if (!profile->device_modes[mode].supported) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  *out_features = profile->device_modes[mode].features;
  return AMDF_STATUS_OK;
}
