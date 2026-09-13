// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/kfd/topology.h"

#include <fcntl.h>
#include <stdlib.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"
#include "libamdf/src/platform/linux/endpoint.h"
#include "libamdf/src/platform/linux/file.h"

namespace {

// The filesystem is the native dependency. The production query reads it
// through the instance's sysfs root without a render node or native device.
class KfdTopologyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string directory = ::testing::TempDir() + "/amdf-topology-XXXXXX";
    ASSERT_NE(mkdtemp(directory.data()), nullptr);
    directory_ = directory;
    const auto topology = directory_ / "class/kfd/kfd/topology";
    const auto node = topology / "nodes/6";
    const auto device = directory_ / "dev/char/226:160/device";
    const auto sdma = device / "ip_discovery/die/0/42/0";
    std::filesystem::create_directories(node);
    std::filesystem::create_directories(sdma);
    WriteAttribute(topology / "generation_id", "1\n");
    WriteAttribute(node / "gpu_id", "53458\n");
    WriteAttribute(device / "mem_info_vram_total", "206141652992\n");
    WriteAttribute(device / "mem_info_vis_vram_total", "206141652992\n");
    WriteAttribute(sdma / "major", "4\n");
    WriteAttribute(sdma / "minor", "4\n");
    WriteAttribute(sdma / "revision", "2\n");
    instance_.sysfs_descriptor =
        open(directory_.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    ASSERT_GE(instance_.sysfs_descriptor, 0);
    endpoint_.instance = &instance_;
    endpoint_.info.id.words[0] = (UINT64_C(226) << 32) | 160;
    endpoint_.info.pci.vendor_id = 0x1002;
    endpoint_.info.pci.device_id = 0x74a1;
  }

  void TearDown() override {
    EXPECT_EQ(amdf_linux_file_close(&instance_.sysfs_descriptor),
              AMDF_STATUS_OK);
    if (!directory_.empty()) std::filesystem::remove_all(directory_);
  }

  void WriteAttribute(const std::filesystem::path& path,
                      const std::string& value) {
    std::ofstream stream(path);
    stream << value;
    stream.close();
    ASSERT_TRUE(stream.good()) << path;
  }

  void WriteProperties(const char* optional_properties) {
    WriteAttribute(directory_ / "class/kfd/kfd/topology/nodes/6/properties",
                   std::string("drm_render_minor 160\n"
                               "vendor_id 4098\n"
                               "device_id 29857\n"
                               "gfx_target_version 90402\n"
                               "capability 2893521536\n"
                               "simd_count 1216\n"
                               "simd_per_cu 4\n"
                               "max_waves_per_simd 8\n"
                               "max_slots_scratch_cu 32\n"
                               "wave_front_size 64\n"
                               "lds_size_in_kb 64\n"
                               "array_count 32\n"
                               "simd_arrays_per_engine 1\n"
                               "num_xcc 8\n"
                               "num_cp_queues 24\n") +
                       optional_properties);
  }

  // Temporary native metadata directory owned by this case.
  std::filesystem::path directory_;
  // Instance whose discovery root names only the test-owned directory.
  amdf_platform_instance_t instance_ = {.sysfs_descriptor = -1};
  // Identity-checked endpoint supplied to the native topology consumer.
  amdf_platform_endpoint_t endpoint_ = {};
};

TEST_F(KfdTopologyTest, DiscoversMemoryAndSdmaWithoutComputeStorageMetadata) {
  WriteProperties(
      "num_sdma_engines 2\n"
      "num_sdma_xgmi_engines 14\n"
      "num_sdma_queues_per_engine 8\n");
  amdf_gpu_kfd_topology_t topology = {};
  ASSERT_EQ(amdf_gpu_kfd_topology_query(&endpoint_, &topology), AMDF_STATUS_OK);
  EXPECT_EQ(topology.gpu_id, 53458u);
  EXPECT_EQ(topology.properties.gfx_ip.major, 9u);
  EXPECT_EQ(topology.properties.gfx_ip.minor, 4u);
  EXPECT_EQ(topology.properties.gfx_ip.stepping, 2u);
  EXPECT_EQ(topology.properties.compute.compute_unit_count, 304u);
  EXPECT_EQ(topology.properties.compute.wavefront_size, 64u);
  EXPECT_EQ(topology.properties.topology.xcc_count, 8u);
  EXPECT_EQ(topology.vram.total_byte_length, UINT64_C(206141652992));
  EXPECT_EQ(topology.vram.visible_byte_length, UINT64_C(206141652992));
  EXPECT_EQ(topology.sdma.engine_count, 2u);
  EXPECT_EQ(topology.sdma.xgmi_engine_count, 14u);
  EXPECT_EQ(topology.sdma.queue_count_per_engine, 8u);
  EXPECT_TRUE(topology.sdma.ip.exact);
  EXPECT_EQ(topology.sdma.ip.major, 4u);
  EXPECT_EQ(topology.sdma.ip.minor, 4u);
  EXPECT_EQ(topology.sdma.ip.revision, 2u);
  EXPECT_EQ(topology.context_save_restore_byte_length, 0u);
  EXPECT_EQ(topology.control_stack_byte_length, 0u);
}

TEST_F(KfdTopologyTest, PreservesComputeStorageWithoutSdmaMetadata) {
  WriteProperties("cwsr_size 19185664\nctl_stack_size 16384\n");
  amdf_gpu_kfd_topology_t topology = {};
  ASSERT_EQ(amdf_gpu_kfd_topology_query(&endpoint_, &topology), AMDF_STATUS_OK);
  EXPECT_EQ(topology.context_save_restore_byte_length, 19185664u);
  EXPECT_EQ(topology.control_stack_byte_length, 16384u);
  EXPECT_EQ(topology.sdma.engine_count, 0u);
  EXPECT_EQ(topology.sdma.xgmi_engine_count, 0u);
  EXPECT_EQ(topology.sdma.queue_count_per_engine, 0u);
  EXPECT_FALSE(topology.sdma.ip.exact);
  EXPECT_EQ(topology.vram.total_byte_length, UINT64_C(206141652992));
}

TEST_F(KfdTopologyTest, RejectsPartialOptionalGroupsWithoutPublication) {
  for (const char* properties : {
           "cwsr_size 19185664\n",
           "ctl_stack_size 16384\n",
           "num_sdma_engines 2\n",
           "num_sdma_engines 2\nnum_sdma_xgmi_engines 14\n",
           "cwsr_size 19185664\nctl_stack_size 16384\n"
           "num_sdma_engines 2\n",
       }) {
    SCOPED_TRACE(properties);
    WriteProperties(properties);
    amdf_gpu_kfd_topology_t topology;
    std::memset(&topology, 0xA5, sizeof(topology));
    const amdf_gpu_kfd_topology_t original = topology;
    EXPECT_EQ(amdf_gpu_kfd_topology_query(&endpoint_, &topology),
              amdf_linux_error(EPROTO));
    EXPECT_EQ(std::memcmp(&topology, &original, sizeof(topology)), 0);
  }
}

}  // namespace
