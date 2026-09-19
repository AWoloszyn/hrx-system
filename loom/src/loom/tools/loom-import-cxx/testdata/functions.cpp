// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

static int twice(int value) { return value * 2; }

int first(int value) { return twice(value) + 1; }

int second(int value) { return twice(value) + 2; }
