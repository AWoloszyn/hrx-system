// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Typed adaptive in-place sorting for compiler hot paths.
//
// Many Loom compiler arrays are already close to program order. A direct
// insertion sort keeps that common path tiny and cache-local, while an in-place
// heap sort bounds heavily disordered generated-kernel inputs without libc
// qsort callback dispatch or temporary arena scratch. Large arrays attempt at
// most one insertion movement per element before switching to heap sort, so
// adversarial order adds only linear work to the O(n log n) fallback.

#ifndef LOOM_UTIL_ADAPTIVE_SORT_H_
#define LOOM_UTIL_ADAPTIVE_SORT_H_

#include "iree/base/api.h"

#define LOOM_ADAPTIVE_SORT_INSERTION_COUNT_THRESHOLD 64u

// Defines an in-place sort over indexed storage, with a typed comparator
// context. The accessor returns the mutable element at an existing index;
// access must preserve all element addresses for the duration of the sort.
// Neither accessor nor comparator may change the storage layout.
//
//   element_type* at_fn(storage_type values, iree_host_size_t index)
//   bool less_fn(context_type context, const element_type* lhs,
//                const element_type* rhs)
//
// The generated entry point is:
//
//   void function_name(context_type context, storage_type values,
//                      iree_host_size_t count)
//
// Contiguous and segmented owners use the same bounded algorithm without
// allocating a flat projection. Access and comparison are statically bound.
#define LOOM_DEFINE_ADAPTIVE_SORT_WITH_ACCESSOR(                             \
    function_name, element_type, storage_type, at_fn, context_type, less_fn) \
  LOOM_DEFINE_ADAPTIVE_SORT_WITH_ACCESSOR_IMPL(function_name, element_type,  \
                                               storage_type, at_fn,          \
                                               context_type, less_fn, )

#define LOOM_DEFINE_ADAPTIVE_SORT_WITH_ACCESSOR_IMPL(                        \
    function_name, element_type, storage_type, at_fn, context_type, less_fn, \
    heap_attributes)                                                         \
  static void function_name##_swap(element_type* lhs, element_type* rhs) {   \
    element_type temporary = *lhs;                                           \
    *lhs = *rhs;                                                             \
    *rhs = temporary;                                                        \
  }                                                                          \
                                                                             \
  static bool function_name##_try_insertion_sort(                            \
      context_type context, storage_type values, iree_host_size_t count,     \
      iree_host_size_t start_index, iree_host_size_t move_budget) {          \
    for (iree_host_size_t i = start_index; i < count; ++i) {                 \
      if (i != start_index &&                                                \
          !less_fn(context, at_fn(values, i), at_fn(values, i - 1))) {       \
        continue;                                                            \
      }                                                                      \
      element_type value = *at_fn(values, i);                                \
      iree_host_size_t j = i;                                                \
      while (true) {                                                         \
        if (move_budget == 0) {                                              \
          *at_fn(values, j) = value;                                         \
          return false;                                                      \
        }                                                                    \
        --move_budget;                                                       \
        *at_fn(values, j) = *at_fn(values, j - 1);                           \
        --j;                                                                 \
        if (j == 0 || !less_fn(context, &value, at_fn(values, j - 1))) {     \
          break;                                                             \
        }                                                                    \
      }                                                                      \
      *at_fn(values, j) = value;                                             \
    }                                                                        \
    return true;                                                             \
  }                                                                          \
                                                                             \
  static void function_name##_heap_sift_down(                                \
      context_type context, storage_type values, iree_host_size_t root,      \
      iree_host_size_t count) {                                              \
    while (true) {                                                           \
      if (root >= count / 2u) {                                              \
        return;                                                              \
      }                                                                      \
      const iree_host_size_t left_child = root * 2u + 1u;                    \
      iree_host_size_t child = left_child;                                   \
      const iree_host_size_t right_child = left_child + 1u;                  \
      if (right_child < count && less_fn(context, at_fn(values, child),      \
                                         at_fn(values, right_child))) {      \
        child = right_child;                                                 \
      }                                                                      \
      if (!less_fn(context, at_fn(values, root), at_fn(values, child))) {    \
        return;                                                              \
      }                                                                      \
      function_name##_swap(at_fn(values, root), at_fn(values, child));       \
      root = child;                                                          \
    }                                                                        \
  }                                                                          \
                                                                             \
  static heap_attributes void function_name##_heap_sort(                     \
      context_type context, storage_type values, iree_host_size_t count) {   \
    iree_host_size_t root = count / 2u;                                      \
    while (root > 0) {                                                       \
      --root;                                                                \
      function_name##_heap_sift_down(context, values, root, count);          \
    }                                                                        \
                                                                             \
    iree_host_size_t end = count;                                            \
    while (end > 1) {                                                        \
      --end;                                                                 \
      function_name##_swap(at_fn(values, 0), at_fn(values, end));            \
      function_name##_heap_sift_down(context, values, 0, end);               \
    }                                                                        \
  }                                                                          \
                                                                             \
  static void function_name(context_type context, storage_type values,       \
                            iree_host_size_t count) {                        \
    if (count < 2) {                                                         \
      return;                                                                \
    }                                                                        \
                                                                             \
    iree_host_size_t first_inversion = 1;                                    \
    while (first_inversion < count &&                                        \
           !less_fn(context, at_fn(values, first_inversion),                 \
                    at_fn(values, first_inversion - 1))) {                   \
      ++first_inversion;                                                     \
    }                                                                        \
    if (first_inversion == count) {                                          \
      return;                                                                \
    }                                                                        \
                                                                             \
    const iree_host_size_t move_budget =                                     \
        count <= LOOM_ADAPTIVE_SORT_INSERTION_COUNT_THRESHOLD                \
            ? IREE_HOST_SIZE_MAX                                             \
            : count;                                                         \
    if (function_name##_try_insertion_sort(context, values, count,           \
                                           first_inversion, move_budget)) {  \
      return;                                                                \
    }                                                                        \
    function_name##_heap_sort(context, values, count);                       \
  }

// Contiguous array form with a typed comparator context. Its entry point is:
//
//   void function_name(context_type context, element_type* values,
//                      iree_host_size_t count)
#define LOOM_DEFINE_ADAPTIVE_SORT_WITH_CONTEXT(function_name, element_type, \
                                               context_type, less_fn)       \
  LOOM_DEFINE_ADAPTIVE_SORT_WITH_CONTEXT_IMPL(function_name, element_type,  \
                                              context_type, less_fn, )

// The same algorithm with the heap fallback kept out of line. This keeps the
// ordered/insertion path small enough to inline into shared sorting helpers;
// callers select it when measurements justify that code-layout tradeoff.
#define LOOM_DEFINE_ADAPTIVE_SORT_WITH_CONTEXT_OUTLINED_HEAP(              \
    function_name, element_type, context_type, less_fn)                    \
  LOOM_DEFINE_ADAPTIVE_SORT_WITH_CONTEXT_IMPL(function_name, element_type, \
                                              context_type, less_fn,       \
                                              IREE_ATTRIBUTE_NOINLINE)

#define LOOM_DEFINE_ADAPTIVE_SORT_WITH_CONTEXT_IMPL(                        \
    function_name, element_type, context_type, less_fn, heap_attributes)    \
  typedef element_type function_name##_element_t;                           \
  static function_name##_element_t* function_name##_at(                     \
      function_name##_element_t* values, iree_host_size_t index) {          \
    return &values[index];                                                  \
  }                                                                         \
  LOOM_DEFINE_ADAPTIVE_SORT_WITH_ACCESSOR_IMPL(                             \
      function_name, function_name##_element_t, function_name##_element_t*, \
      function_name##_at, context_type, less_fn, heap_attributes)

// Context-free form for comparisons using only the array elements. Both forms
// inline the comparator into the same sorting algorithm without dynamic calls.
// |less_fn| must have this shape:
//
//   bool less_fn(const element_type* lhs, const element_type* rhs)
//
// The generated entry point is:
//
//   void function_name(element_type* values, iree_host_size_t count)
#define LOOM_DEFINE_ADAPTIVE_SORT(function_name, element_type, less_fn)     \
  typedef element_type function_name##_element_t;                           \
  static bool function_name##_less_with_context(                            \
      void* context, const function_name##_element_t* lhs,                  \
      const function_name##_element_t* rhs) {                               \
    (void)context;                                                          \
    return less_fn(lhs, rhs);                                               \
  }                                                                         \
  LOOM_DEFINE_ADAPTIVE_SORT_WITH_CONTEXT(function_name##_with_context,      \
                                         element_type, void*,               \
                                         function_name##_less_with_context) \
  static void function_name(element_type* values, iree_host_size_t count) { \
    function_name##_with_context(NULL, values, count);                      \
  }

#endif  // LOOM_UTIL_ADAPTIVE_SORT_H_
