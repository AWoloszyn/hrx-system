// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/status_wire.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "iree/base/alignment.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

struct WireEntry {
  iree_net_status_wire_entry_type_t type;
  uint32_t aux;
  std::string text;
};

static iree_host_size_t AlignWire(iree_host_size_t value) {
  return (value + IREE_NET_STATUS_WIRE_ALIGNMENT - 1) &
         ~(IREE_NET_STATUS_WIRE_ALIGNMENT - 1);
}

static std::vector<uint8_t> BuildWire(iree_status_code_t status_code,
                                      const std::vector<WireEntry>& entries) {
  iree_host_size_t total_size = IREE_NET_STATUS_WIRE_HEADER_SIZE;
  for (const WireEntry& entry : entries) {
    total_size += IREE_NET_STATUS_WIRE_ENTRY_HEADER_SIZE +
                  AlignWire(entry.text.size() + 1);
  }
  std::vector<uint8_t> wire(total_size, 0);
  wire[0] = IREE_NET_STATUS_WIRE_VERSION;
  wire[1] = static_cast<uint8_t>(status_code);
  iree_unaligned_store_le_u16(wire.data() + 2,
                              static_cast<uint16_t>(entries.size()));
  iree_unaligned_store_le_u32(wire.data() + 4,
                              static_cast<uint32_t>(wire.size()));

  iree_host_size_t offset = IREE_NET_STATUS_WIRE_HEADER_SIZE;
  for (const WireEntry& entry : entries) {
    wire[offset] = static_cast<uint8_t>(entry.type);
    iree_unaligned_store_le_u32(wire.data() + offset + 4,
                                static_cast<uint32_t>(entry.text.size()));
    iree_unaligned_store_le_u32(wire.data() + offset + 8, entry.aux);
    std::memcpy(wire.data() + offset + IREE_NET_STATUS_WIRE_ENTRY_HEADER_SIZE,
                entry.text.data(), entry.text.size());
    offset += IREE_NET_STATUS_WIRE_ENTRY_HEADER_SIZE +
              AlignWire(entry.text.size() + 1);
  }
  return wire;
}

static std::vector<uint8_t> Serialize(const iree_status_t status) {
  iree_host_size_t size = 0;
  IREE_CHECK_OK(iree_net_status_wire_calculate_size(status, &size));
  std::vector<uint8_t> wire(size);
  IREE_CHECK_OK(iree_net_status_wire_serialize(
      status, iree_make_byte_span(wire.data(), wire.size())));
  return wire;
}

static iree_status_t Deserialize(const std::vector<uint8_t>& wire) {
  iree_status_t result = iree_ok_status();
  IREE_CHECK_OK(iree_net_status_wire_deserialize(
      iree_make_const_byte_span(wire.data(), wire.size()), &result));
  return result;
}

static void ExpectInvalid(const std::vector<uint8_t>& wire) {
  iree_status_t result = iree_ok_status();
  iree_status_t operation_status = iree_net_status_wire_deserialize(
      iree_make_const_byte_span(wire.data(), wire.size()), &result);
  EXPECT_EQ(iree_status_code(operation_status), IREE_STATUS_INVALID_ARGUMENT);
  iree_status_free(operation_status);
  IREE_EXPECT_OK(result);
}

static std::string FormatStatus(const iree_status_t status) {
  iree_host_size_t length = 0;
  if (!iree_status_format(status, 0, nullptr, &length)) return {};
  std::vector<char> buffer(length + 1);
  if (!iree_status_format(status, buffer.size(), buffer.data(), &length)) {
    return {};
  }
  return std::string(buffer.data(), length);
}

TEST(StatusWireTest, RoundTripsOkStatus) {
  std::vector<uint8_t> wire = Serialize(iree_ok_status());
  ASSERT_EQ(wire.size(), IREE_NET_STATUS_WIRE_HEADER_SIZE);
  EXPECT_EQ(wire[0], IREE_NET_STATUS_WIRE_VERSION);
  EXPECT_EQ(wire[1], IREE_STATUS_OK);
  EXPECT_EQ(iree_unaligned_load_le_u16(wire.data() + 2), 0u);
  EXPECT_EQ(iree_unaligned_load_le_u32(wire.data() + 4), wire.size());
  iree_status_t result = Deserialize(wire);
  IREE_EXPECT_OK(result);
}

TEST(StatusWireTest, RoundTripsEveryStatusCode) {
  for (uint32_t code = 0; code <= IREE_STATUS_CODE_MASK; ++code) {
    iree_status_t result = Deserialize(Serialize(
        iree_status_from_code(static_cast<iree_status_code_t>(code))));
    EXPECT_EQ(iree_status_code(result), code) << "code=" << code;
    iree_status_free(result);
  }
}

TEST(StatusWireTest, RoundTripsStructuredDiagnostics) {
  iree_status_t status =
      iree_make_status(IREE_STATUS_INTERNAL, "primary message");
  status = iree_status_annotate_f(status, "annotation %d", 42);
  std::vector<uint8_t> wire = Serialize(status);
  iree_status_free(status);

  iree_status_t result = Deserialize(wire);
  EXPECT_EQ(iree_status_code(result), IREE_STATUS_INTERNAL);
#if IREE_STATUS_FEATURES & IREE_STATUS_FEATURE_ANNOTATIONS
  iree_string_view_t message = iree_status_message(result);
  EXPECT_EQ(std::string(message.data, message.size), "primary message");
  EXPECT_NE(FormatStatus(result).find("annotation 42"), std::string::npos);
#endif  // IREE_STATUS_FEATURE_ANNOTATIONS
  iree_status_free(result);
}

TEST(StatusWireTest, ResultOwnsSourceMessageAndAnnotations) {
  std::vector<uint8_t> wire = BuildWire(
      IREE_STATUS_DATA_LOSS,
      {{IREE_NET_STATUS_WIRE_ENTRY_TYPE_SOURCE_LOCATION, 123, "remote.cc"},
       {IREE_NET_STATUS_WIRE_ENTRY_TYPE_MESSAGE, 0, "primary"},
       {IREE_NET_STATUS_WIRE_ENTRY_TYPE_ANNOTATION, 0, "detail"}});
  iree_status_t result = Deserialize(wire);
  std::memset(wire.data(), 0xCD, wire.size());

  EXPECT_EQ(iree_status_code(result), IREE_STATUS_DATA_LOSS);
#if IREE_STATUS_FEATURES & IREE_STATUS_FEATURE_SOURCE_LOCATION
  iree_status_source_location_t source = iree_status_source_location(result);
  ASSERT_NE(source.file, nullptr);
  EXPECT_STREQ(source.file, "remote.cc");
  EXPECT_EQ(source.line, 123u);
#endif  // IREE_STATUS_FEATURE_SOURCE_LOCATION
#if IREE_STATUS_FEATURES & IREE_STATUS_FEATURE_ANNOTATIONS
  iree_string_view_t message = iree_status_message(result);
  EXPECT_EQ(std::string(message.data, message.size), "primary");
  EXPECT_NE(FormatStatus(result).find("detail"), std::string::npos);
#endif  // IREE_STATUS_FEATURE_ANNOTATIONS
  iree_status_free(result);
}

TEST(StatusWireTest, LeavesExcessOutputCapacityUnmodified) {
  iree_host_size_t required_size = 0;
  IREE_ASSERT_OK(
      iree_net_status_wire_calculate_size(iree_ok_status(), &required_size));
  std::vector<uint8_t> buffer(required_size + 4, 0xA5);
  IREE_ASSERT_OK(iree_net_status_wire_serialize(
      iree_ok_status(), iree_make_byte_span(buffer.data(), buffer.size())));
  for (iree_host_size_t i = required_size; i < buffer.size(); ++i) {
    EXPECT_EQ(buffer[i], 0xA5);
  }
}

TEST(StatusWireTest, RejectsMissingOutputBuffer) {
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_status_wire_serialize(iree_ok_status(), iree_byte_span_empty()));
}

TEST(StatusWireTest, RejectsSmallOutputBuffer) {
  uint8_t buffer[IREE_NET_STATUS_WIRE_HEADER_SIZE - 1] = {0};
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      iree_net_status_wire_serialize(
          iree_ok_status(), iree_make_byte_span(buffer, sizeof(buffer))));
}

TEST(StatusWireTest, RejectsTruncatedHeader) {
  std::vector<uint8_t> wire(IREE_NET_STATUS_WIRE_HEADER_SIZE - 1, 0);
  ExpectInvalid(wire);
}

TEST(StatusWireTest, RejectsUnsupportedVersion) {
  std::vector<uint8_t> wire = Serialize(iree_ok_status());
  wire[0] = IREE_NET_STATUS_WIRE_VERSION + 1;
  ExpectInvalid(wire);
}

TEST(StatusWireTest, RejectsInvalidStatusCode) {
  std::vector<uint8_t> wire = Serialize(iree_ok_status());
  wire[1] = IREE_STATUS_CODE_MASK + 1;
  ExpectInvalid(wire);
}

TEST(StatusWireTest, RejectsTrailingBytes) {
  std::vector<uint8_t> wire = Serialize(iree_ok_status());
  wire.push_back(0);
  ExpectInvalid(wire);
}

TEST(StatusWireTest, RejectsMisalignedTotalSize) {
  std::vector<uint8_t> wire = Serialize(iree_ok_status());
  wire.push_back(0);
  iree_unaligned_store_le_u32(wire.data() + 4,
                              static_cast<uint32_t>(wire.size()));
  ExpectInvalid(wire);
}

TEST(StatusWireTest, RejectsOkStatusWithEntries) {
  std::vector<uint8_t> wire =
      BuildWire(IREE_STATUS_OK,
                {{IREE_NET_STATUS_WIRE_ENTRY_TYPE_MESSAGE, 0, "not okay"}});
  ExpectInvalid(wire);
}

TEST(StatusWireTest, RejectsTruncatedEntryHeader) {
  std::vector<uint8_t> wire = Serialize(iree_ok_status());
  wire[1] = IREE_STATUS_INTERNAL;
  iree_unaligned_store_le_u16(wire.data() + 2, 1);
  ExpectInvalid(wire);
}

TEST(StatusWireTest, RejectsTruncatedEntryData) {
  std::vector<uint8_t> wire =
      BuildWire(IREE_STATUS_INTERNAL,
                {{IREE_NET_STATUS_WIRE_ENTRY_TYPE_MESSAGE, 0, "message"}});
  iree_unaligned_store_le_u32(
      wire.data() + IREE_NET_STATUS_WIRE_HEADER_SIZE + 4, UINT32_MAX);
  ExpectInvalid(wire);
}

TEST(StatusWireTest, RejectsUnknownEntryType) {
  std::vector<uint8_t> wire =
      BuildWire(IREE_STATUS_INTERNAL,
                {{IREE_NET_STATUS_WIRE_ENTRY_TYPE_MESSAGE, 0, "message"}});
  wire[IREE_NET_STATUS_WIRE_HEADER_SIZE] = 0xFF;
  ExpectInvalid(wire);
}

TEST(StatusWireTest, RejectsReservedEntryData) {
  std::vector<uint8_t> wire =
      BuildWire(IREE_STATUS_INTERNAL,
                {{IREE_NET_STATUS_WIRE_ENTRY_TYPE_MESSAGE, 0, "message"}});
  wire[IREE_NET_STATUS_WIRE_HEADER_SIZE + 1] = 1;
  ExpectInvalid(wire);
  wire[IREE_NET_STATUS_WIRE_HEADER_SIZE + 1] = 0;
  iree_unaligned_store_le_u32(
      wire.data() + IREE_NET_STATUS_WIRE_HEADER_SIZE + 12, 1);
  ExpectInvalid(wire);
}

TEST(StatusWireTest, RejectsEmptyEntry) {
  std::vector<uint8_t> wire =
      BuildWire(IREE_STATUS_INTERNAL,
                {{IREE_NET_STATUS_WIRE_ENTRY_TYPE_MESSAGE, 0, "message"}});
  iree_unaligned_store_le_u32(
      wire.data() + IREE_NET_STATUS_WIRE_HEADER_SIZE + 4, 0);
  ExpectInvalid(wire);
}

TEST(StatusWireTest, RejectsEmbeddedNul) {
  std::vector<uint8_t> wire =
      BuildWire(IREE_STATUS_INTERNAL,
                {{IREE_NET_STATUS_WIRE_ENTRY_TYPE_MESSAGE, 0, "message"}});
  wire[IREE_NET_STATUS_WIRE_HEADER_SIZE +
       IREE_NET_STATUS_WIRE_ENTRY_HEADER_SIZE + 1] = 0;
  ExpectInvalid(wire);
}

TEST(StatusWireTest, RejectsNonzeroTerminatorOrPadding) {
  std::vector<uint8_t> wire =
      BuildWire(IREE_STATUS_INTERNAL,
                {{IREE_NET_STATUS_WIRE_ENTRY_TYPE_MESSAGE, 0, "message"}});
  const iree_host_size_t text_offset =
      IREE_NET_STATUS_WIRE_HEADER_SIZE + IREE_NET_STATUS_WIRE_ENTRY_HEADER_SIZE;
  wire[text_offset + 7] = 1;
  ExpectInvalid(wire);
}

TEST(StatusWireTest, RejectsAuxiliaryDataOnMessage) {
  std::vector<uint8_t> wire =
      BuildWire(IREE_STATUS_INTERNAL,
                {{IREE_NET_STATUS_WIRE_ENTRY_TYPE_MESSAGE, 1, "message"}});
  ExpectInvalid(wire);
}

TEST(StatusWireTest, RejectsDuplicateSource) {
  std::vector<uint8_t> wire =
      BuildWire(IREE_STATUS_INTERNAL,
                {{IREE_NET_STATUS_WIRE_ENTRY_TYPE_SOURCE_LOCATION, 1, "a.cc"},
                 {IREE_NET_STATUS_WIRE_ENTRY_TYPE_SOURCE_LOCATION, 2, "b.cc"}});
  ExpectInvalid(wire);
}

TEST(StatusWireTest, RejectsDuplicateMessage) {
  std::vector<uint8_t> wire =
      BuildWire(IREE_STATUS_INTERNAL,
                {{IREE_NET_STATUS_WIRE_ENTRY_TYPE_MESSAGE, 0, "one"},
                 {IREE_NET_STATUS_WIRE_ENTRY_TYPE_MESSAGE, 0, "two"}});
  ExpectInvalid(wire);
}

TEST(StatusWireTest, RejectsSourceAfterMessage) {
  std::vector<uint8_t> wire = BuildWire(
      IREE_STATUS_INTERNAL,
      {{IREE_NET_STATUS_WIRE_ENTRY_TYPE_MESSAGE, 0, "message"},
       {IREE_NET_STATUS_WIRE_ENTRY_TYPE_SOURCE_LOCATION, 1, "file.cc"}});
  ExpectInvalid(wire);
}

TEST(StatusWireTest, RejectsMessageAfterAnnotation) {
  std::vector<uint8_t> wire =
      BuildWire(IREE_STATUS_INTERNAL,
                {{IREE_NET_STATUS_WIRE_ENTRY_TYPE_ANNOTATION, 0, "detail"},
                 {IREE_NET_STATUS_WIRE_ENTRY_TYPE_MESSAGE, 0, "message"}});
  ExpectInvalid(wire);
}

TEST(StatusWireTest, RejectsUnclaimedData) {
  std::vector<uint8_t> wire =
      BuildWire(IREE_STATUS_INTERNAL,
                {{IREE_NET_STATUS_WIRE_ENTRY_TYPE_MESSAGE, 0, "message"}});
  iree_unaligned_store_le_u16(wire.data() + 2, 0);
  ExpectInvalid(wire);
}

}  // namespace
