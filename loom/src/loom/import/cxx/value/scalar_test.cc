// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/value/scalar.h"

#include <limits>

#include "loom/import/cxx/value/builder_test.h"
#include "loom/ops/scalar/ops.h"

namespace loom::cxx_import {
namespace {
using ScalarsTest = ValueBuilderTest;

TEST_F(ScalarsTest, WideningAndComparisonsUseSourceSignedness) {
  Locations locations(source_.unit(), source_.diagnostics(), module_);
  Scalars scalars(source_.unit(), source_.diagnostics(), types_, locations,
                  builder_);
  auto* control = source_.unit().control();
  auto* owner = source_.unit().ast();
  auto value = scalars.integer(-1, LOOM_SCALAR_TYPE_I32);
  auto signed_wide = scalars.convert(value, control->getIntType(),
                                     control->getLongLongIntType(), owner);
  auto unsigned_wide =
      scalars.convert(value, control->getUnsignedIntType(),
                      control->getUnsignedLongLongIntType(), owner);
  EXPECT_TRUE(loom_scalar_extsi_isa(producer(signed_wide)));
  EXPECT_TRUE(loom_scalar_extui_isa(producer(unsigned_wide)));
  auto zero = scalars.integer(0, LOOM_SCALAR_TYPE_I32);
  auto signed_less =
      scalars.binary(cxx::TokenKind::T_LESS, value, zero, control->getIntType(),
                     control->getBoolType(), owner);
  auto unsigned_less = scalars.binary(cxx::TokenKind::T_LESS, value, zero,
                                      control->getUnsignedIntType(),
                                      control->getBoolType(), owner);
  EXPECT_EQ(loom_scalar_cmpi_predicate(producer(signed_less)),
            LOOM_SCALAR_CMPI_PREDICATE_SLT);
  EXPECT_EQ(loom_scalar_cmpi_predicate(producer(unsigned_less)),
            LOOM_SCALAR_CMPI_PREDICATE_ULT);
}

TEST_F(ScalarsTest, BooleanConversionPreservesNaNTruthAndZeroExtension) {
  Locations locations(source_.unit(), source_.diagnostics(), module_);
  Scalars scalars(source_.unit(), source_.diagnostics(), types_, locations,
                  builder_);
  auto* control = source_.unit().control();
  auto* owner = source_.unit().ast();
  auto nan = scalars.constant(std::numeric_limits<double>::quiet_NaN(),
                              control->getDoubleType(), owner);
  auto truth = scalars.convert(nan, control->getDoubleType(),
                               control->getBoolType(), owner);
  EXPECT_EQ(loom_scalar_cmpf_predicate(producer(truth)),
            LOOM_SCALAR_CMPF_PREDICATE_UNE);
  auto widened = scalars.convert(truth, control->getBoolType(),
                                 control->getIntType(), owner);
  EXPECT_TRUE(loom_scalar_extui_isa(producer(widened)));
}

TEST_F(ScalarsTest, ConstantsPreserveUnsignedStorageBits) {
  Locations locations(source_.unit(), source_.diagnostics(), module_);
  Scalars scalars(source_.unit(), source_.diagnostics(), types_, locations,
                  builder_);
  auto* control = source_.unit().control();
  auto* owner = source_.unit().ast();
  auto value = scalars.constant(int64_t{UINT32_MAX},
                                control->getUnsignedIntType(), owner);
  EXPECT_EQ(loom_scalar_constant_value(producer(value)).i64, -1);
  EXPECT_EQ(scalars.convert(value, control->getUnsignedIntType(),
                            control->getIntType(), owner),
            value);
}

}  // namespace
}  // namespace loom::cxx_import
