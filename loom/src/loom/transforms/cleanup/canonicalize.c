// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/cleanup/canonicalize.h"

#include <string.h>

#include "loom/pass/pipeline.h"
#include "loom/pass/registry.h"
#include "loom/target/math_policy.h"
#include "loom/target/pass_environment.h"
#include "loom/transforms/cleanup/canonicalizer.h"

static const loom_pass_option_def_t kCanonicalizeOptions[] = {
    {IREE_SVL("max-iterations"),
     IREE_SVL("Maximum number of worklist iterations.")},
    {IREE_SVL("view-loads"),
     IREE_SVL(
         "Preserve scalar loads (default) or coalesce before legalization.")},
    {IREE_SVL("table-lookups"),
     IREE_SVL("Preserve scalar extracts (default) or combine register table "
              "lookups before legalization.")},
};

#define LOOM_CANONICALIZE_STATISTICS(V, statistics_type)                       \
  V(statistics_type, ops_modified, "ops-modified",                             \
    "Number of ops simplified by canonicalization.")                           \
  V(statistics_type, type_propagation_conflicts, "type-propagation-conflicts", \
    "Number of type propagation candidate closures rejected as "               \
    "inconsistent.")                                                           \
  V(statistics_type, type_propagation_rejection_cache_hits,                    \
    "type-propagation-rejection-cache-hits",                                   \
    "Number of repeated rejected type candidates skipped within an "           \
    "iteration.")

LOOM_PASS_STATISTICS_DEFINE(loom_canonicalize_statistics,
                            loom_canonicalize_statistics_t,
                            LOOM_CANONICALIZE_STATISTICS)

static const loom_pass_info_t loom_canonicalize_pass_info_storage = {
    .name = IREE_SVL("canonicalize"),
    .description = IREE_SVL("Apply op-specific canonicalization patterns."),
    .kind = LOOM_PASS_FUNCTION,
    .option_defs = kCanonicalizeOptions,
    .option_count = IREE_ARRAYSIZE(kCanonicalizeOptions),
    .statistic_layout = &loom_canonicalize_statistics_layout,
};

const loom_pass_info_t* loom_canonicalize_pass_info(void) {
  return &loom_canonicalize_pass_info_storage;
}

static iree_status_t loom_canonicalize_parse_option(void* user_data,
                                                    iree_string_view_t name,
                                                    iree_string_view_t value) {
  loom_canonicalizer_options_t* options =
      (loom_canonicalizer_options_t*)user_data;
  if (iree_string_view_equal(name, IREE_SV("table-lookups"))) {
    if (iree_string_view_equal(value, IREE_SV("combine"))) {
      options->flags |= LOOM_CANONICALIZER_FLAG_COMBINE_TABLE_LOOKUPS;
    } else if (iree_string_view_equal(value, IREE_SV("preserve"))) {
      options->flags &= ~LOOM_CANONICALIZER_FLAG_COMBINE_TABLE_LOOKUPS;
    } else {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "pass 'canonicalize' option 'table-lookups' must "
                              "be 'combine' or 'preserve'");
    }
    return iree_ok_status();
  }
  if (iree_string_view_equal(name, IREE_SV("view-loads"))) {
    if (iree_string_view_equal(value, IREE_SV("coalesce"))) {
      options->flags |= LOOM_CANONICALIZER_FLAG_COALESCE_VIEW_LOADS;
    } else if (iree_string_view_equal(value, IREE_SV("preserve"))) {
      options->flags &= ~LOOM_CANONICALIZER_FLAG_COALESCE_VIEW_LOADS;
    } else {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "pass 'canonicalize' option 'view-loads' must be "
                              "'coalesce' or 'preserve'");
    }
    return iree_ok_status();
  }
  if (iree_string_view_equal(name, IREE_SV("max-iterations"))) {
    if (options->max_iterations != 0) {
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "duplicate option 'max-iterations' for pass 'canonicalize'");
    }
    IREE_RETURN_IF_ERROR(loom_pass_option_parse_uint32(
        IREE_SV("canonicalize"), name, value, &options->max_iterations));
    if (options->max_iterations == 0) {
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "pass 'canonicalize' option 'max-iterations' must be greater than 0");
    }
    return iree_ok_status();
  }
  return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                          "unknown option '%.*s' for pass 'canonicalize'",
                          (int)name.size, name.data);
}

iree_status_t loom_canonicalize_create(loom_pass_t* pass,
                                       iree_string_view_t options_string) {
  loom_canonicalizer_options_t* options = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate(pass->instance_arena,
                                           sizeof(*options), (void**)&options));
  memset(options, 0, sizeof(*options));
  if (pass->decoded_options) {
    for (uint16_t i = 0; i < pass->decoded_options->option_count; ++i) {
      const loom_pass_decoded_option_t* option =
          &pass->decoded_options->options[i];
      if (!option->present) {
        continue;
      }
      if (iree_string_view_equal(option->schema->name,
                                 IREE_SV("max-iterations"))) {
        options->max_iterations = option->uint32_value;
        continue;
      }
      if (iree_string_view_equal(option->schema->name, IREE_SV("view-loads"))) {
        if (option->enum_value_index == 0) {
          options->flags |= LOOM_CANONICALIZER_FLAG_COALESCE_VIEW_LOADS;
        }
        continue;
      }
      if (iree_string_view_equal(option->schema->name,
                                 IREE_SV("table-lookups"))) {
        if (option->enum_value_index == 0) {
          options->flags |= LOOM_CANONICALIZER_FLAG_COMBINE_TABLE_LOOKUPS;
        }
        continue;
      }
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "unknown decoded option '%.*s' for pass 'canonicalize'",
          (int)option->schema->name.size, option->schema->name.data);
    }
  } else {
    IREE_RETURN_IF_ERROR(
        loom_pass_options_parse(pass->info->name, options_string,
                                (loom_pass_option_parse_callback_t){
                                    .fn = loom_canonicalize_parse_option,
                                    .user_data = options,
                                }));
  }
  pass->state = options;
  return iree_ok_status();
}

iree_status_t loom_canonicalize_run(loom_pass_t* pass, loom_module_t* module,
                                    loom_func_like_t function) {
  loom_canonicalizer_options_t run_options = {0};
  if (pass->state) {
    run_options = *(const loom_canonicalizer_options_t*)pass->state;
  }
  bool target_resolved = false;
  IREE_RETURN_IF_ERROR(loom_target_pass_resolve_function_facts(
      pass, module, function, &target_resolved, &run_options.target_facts));
  if (!target_resolved) {
    run_options.target_facts = NULL;
  }
  const loom_target_math_pass_capability_t* math_capability =
      loom_target_math_pass_capability_from_pass(pass);
  run_options.math_policy = loom_target_math_policy_registry_lookup_for_bundle(
      loom_target_math_pass_capability_policy_registry(math_capability),
      loom_target_facts_bundle(run_options.target_facts));

  loom_canonicalizer_t canonicalizer;
  IREE_RETURN_IF_ERROR(loom_canonicalizer_initialize(
      module, pass->arena, pass->value_facts, &canonicalizer));

  loom_canonicalizer_result_t result;
  iree_status_t status = loom_canonicalizer_run_function(
      &canonicalizer, function, &run_options, &result);
  if (iree_status_is_ok(status)) {
    if (result.changed) {
      loom_pass_mark_changed(pass);
    }
    loom_canonicalize_statistics_t* statistics =
        loom_canonicalize_statistics(pass);
    statistics->ops_modified += result.ops_modified;
    statistics->type_propagation_conflicts += result.type_propagation_conflicts;
    statistics->type_propagation_rejection_cache_hits +=
        result.type_propagation_rejection_cache_hits;
  }
  loom_canonicalizer_deinitialize(&canonicalizer);
  return status;
}
