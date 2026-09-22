// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMCXX_LOW_H_
#define LOOMCXX_LOW_H_

namespace loom::low {

// Embeds a descriptor-backed Low function and inlines its invocation. Contract
// is a class tagged with loom::representation("contract.key"), independently of
// the caller's hardware target. Source is one narrow raw string literal with
// a register signature and body: `(%value: reg<...>) -> (reg<...>) { ... }`.
//
// Arguments bind positionally to the literal's formals and are evaluated once.
// Result and Arguments are ordinary scalar/vector C++ types; Result may also
// be void. Their selected physical representations must match the authored
// register signature. The body uses existing Loom assembly syntax and captures
// only its formal arguments. Register allocation and scheduling remain free.
template <class Contract, class Result, class... Arguments>
[[loom::op("low.assembly")]]
Result assembly(const char* source, Arguments... arguments);

}  // namespace loom::low

#endif  // LOOMCXX_LOW_H_
