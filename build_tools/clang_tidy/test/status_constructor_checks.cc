// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

typedef struct iree_status_handle_t* iree_status_t;

iree_status_t iree_clang_tidy_status_cpp_source();
bool iree_status_is_ok(iree_status_t status);
void iree_status_free(iree_status_t status);

class StatusError {
 public:
  explicit StatusError(iree_status_t status, unsigned context = 0);
};

class StatusView {
 public:
  explicit StatusView(const iree_status_t status);
};

class StatusReferenceView {
 public:
  explicit StatusReferenceView(const iree_status_t& status);
};

class StatusWithContext {
 public:
  StatusWithContext(iree_status_t status, const iree_status_t context);
};

void throws_error(iree_status_t thrown_status) {
  if (!iree_status_is_ok(thrown_status)) {
    throw StatusError(thrown_status);
  }
}

void observes_value(const iree_status_t value_borrow_status) {
  (void)StatusView(value_borrow_status);
}

void observes_reference(const iree_status_t reference_borrow_status) {
  (void)StatusReferenceView(reference_borrow_status);
}

void invalid_borrowed_transfer(
    const iree_status_t borrowed_constructor_status) {
  (void)StatusError(borrowed_constructor_status);
}

void invalid_observer(iree_status_t observed_constructor_status) {
  (void)StatusView(observed_constructor_status);
}

void constructs_owner() {
  iree_status_t local_constructor_status = iree_clang_tidy_status_cpp_source();
  (void)StatusError(local_constructor_status);
}

void constructs_borrower() {
  iree_status_t local_borrowed_status = iree_clang_tidy_status_cpp_source();
  (void)StatusView(local_borrowed_status);
  (void)StatusReferenceView(local_borrowed_status);
  iree_status_free(local_borrowed_status);
}

void invalid_reuse() {
  iree_status_t constructor_reused_status = iree_clang_tidy_status_cpp_source();
  (void)StatusError(constructor_reused_status);
  iree_status_free(constructor_reused_status);
}

void invalid_argument_order() {
  iree_status_t constructor_order_status = iree_clang_tidy_status_cpp_source();
  (void)StatusWithContext(constructor_order_status, constructor_order_status);
}
