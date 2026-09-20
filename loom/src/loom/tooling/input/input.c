// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/input/input.h"

#include <string.h>

#include "loom/ir/module.h"

static iree_status_t loom_input_text_load(const loom_input_request_t* request,
                                          loom_input_source_capture_t capture,
                                          loom_context_t* context,
                                          iree_arena_block_pool_t* block_pool,
                                          iree_allocator_t host_allocator,
                                          loom_module_t** out_module) {
  if (!iree_string_view_is_empty(request->options)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "Loom text input does not accept input options");
  }
  return loom_text_parse(request->source, request->path, context, block_pool,
                         &request->parse_options, out_module);
}

static const iree_string_view_t loom_input_text_suffixes[] = {
    IREE_SVL(".loom"),
    IREE_SVL(".loom-test"),
};

const loom_input_provider_t loom_input_text_provider = {
    .name = IREE_SVL("loom"),
    .suffixes = {IREE_ARRAYSIZE(loom_input_text_suffixes),
                 loom_input_text_suffixes},
    .load = loom_input_text_load,
};

iree_status_t loom_input_provider_select(
    loom_input_provider_list_t providers, iree_string_view_t format,
    iree_string_view_t path, const loom_input_provider_t** out_provider) {
  *out_provider = NULL;
  for (iree_host_size_t i = 0; i <= providers.count; ++i) {
    const loom_input_provider_t* provider =
        i == 0 ? &loom_input_text_provider : providers.values[i - 1];
    if (!iree_string_view_is_empty(format)) {
      if (iree_string_view_equal(format, provider->name)) {
        *out_provider = provider;
        return iree_ok_status();
      }
    } else {
      for (iree_host_size_t j = 0; j < provider->suffixes.count; ++j) {
        if (iree_string_view_ends_with(path, provider->suffixes.values[j])) {
          *out_provider = provider;
          return iree_ok_status();
        }
      }
    }
  }
  if (!iree_string_view_is_empty(format)) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "input format '%.*s' is not linked into this tool",
                            (int)format.size, format.data);
  }
  if (iree_string_view_ends_with(path, IREE_SV("-test"))) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "no linked input provider accepts '%.*s'",
                            (int)path.size, path.data);
  }
  *out_provider = &loom_input_text_provider;
  return iree_ok_status();
}

typedef struct loom_input_snapshot_t {
  // Next source admitted by this invocation.
  struct loom_input_snapshot_t* next;
  // Physical identity used by the frontend and include lookup.
  iree_string_view_t path;
  // Logical identity used by diagnostics and the output module.
  iree_string_view_t filename;
  // Owned copy of the exact admitted source bytes.
  iree_string_view_t source;
} loom_input_snapshot_t;

typedef struct loom_input_capture_t {
  // Output whose arena owns captured strings and records.
  loom_input_module_t* input;
  // Caller options borrowed during admission.
  const loom_input_request_t* request;
  // Sources already captured, including the main source.
  loom_input_snapshot_t* snapshots;
} loom_input_capture_t;

static iree_status_t loom_input_copy_string(iree_arena_allocator_t* arena,
                                            iree_string_view_t value,
                                            iree_string_view_t* out_value) {
  char* storage = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(arena, value.size + 1, (void**)&storage));
  if (value.size) {
    memcpy(storage, value.data, value.size);
  }
  storage[value.size] = 0;
  *out_value = iree_make_string_view(storage, value.size);
  return iree_ok_status();
}

static iree_status_t loom_input_remap_path(loom_input_capture_t* capture,
                                           iree_string_view_t path,
                                           iree_string_view_t* out_filename) {
  if (iree_string_view_is_empty(path)) {
    *out_filename = iree_string_view_empty();
    return iree_ok_status();
  }
  for (loom_input_snapshot_t* snapshot = capture->snapshots; snapshot;
       snapshot = snapshot->next) {
    if (iree_string_view_equal(snapshot->path, path)) {
      *out_filename = snapshot->filename;
      return iree_ok_status();
    }
  }
  char* storage = NULL;
  return loom_tooling_source_path_remap(
      path, &capture->request->source_path_options,
      iree_arena_allocator(&capture->input->source_arena), out_filename,
      &storage);
}

static iree_status_t loom_input_capture_source(void* user_data,
                                               iree_string_view_t path,
                                               iree_string_view_t source) {
  loom_input_capture_t* capture = (loom_input_capture_t*)user_data;
  for (loom_input_snapshot_t* snapshot = capture->snapshots; snapshot;
       snapshot = snapshot->next) {
    if (iree_string_view_equal(snapshot->path, path)) {
      return iree_ok_status();
    }
  }
  iree_string_view_t filename = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(loom_input_remap_path(capture, path, &filename));
  for (loom_input_snapshot_t* snapshot = capture->snapshots; snapshot;
       snapshot = snapshot->next) {
    if (iree_string_view_equal(snapshot->filename, filename)) {
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "source path remapping gives distinct sources '%.*s' and '%.*s' "
          "the same filename '%.*s'",
          (int)snapshot->path.size, snapshot->path.data, (int)path.size,
          path.data, (int)filename.size, filename.data);
    }
  }
  iree_arena_allocator_t* arena = &capture->input->source_arena;
  loom_input_snapshot_t* snapshot = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(arena, sizeof(*snapshot), (void**)&snapshot));
  IREE_RETURN_IF_ERROR(loom_input_copy_string(arena, path, &snapshot->path));
  IREE_RETURN_IF_ERROR(
      loom_input_copy_string(arena, filename, &snapshot->filename));
  IREE_RETURN_IF_ERROR(
      loom_input_copy_string(arena, source, &snapshot->source));
  snapshot->next = capture->snapshots;
  capture->snapshots = snapshot;
  return iree_ok_status();
}

static iree_status_t loom_input_capture_diagnostic(
    void* user_data, const loom_diagnostic_t* diagnostic) {
  loom_input_capture_t* capture = (loom_input_capture_t*)user_data;
  loom_diagnostic_t remapped = *diagnostic;
  IREE_RETURN_IF_ERROR(loom_input_remap_path(
      capture, diagnostic->origin.filename, &remapped.origin.filename));
  IREE_RETURN_IF_ERROR(
      loom_input_remap_path(capture, diagnostic->source_location.filename,
                            &remapped.source_location.filename));
  if (diagnostic->related_location_count) {
    loom_diagnostic_related_location_t* related = NULL;
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        &capture->input->source_arena, diagnostic->related_location_count,
        sizeof(*related), (void**)&related));
    for (iree_host_size_t i = 0; i < diagnostic->related_location_count; ++i) {
      related[i] = diagnostic->related_locations[i];
      IREE_RETURN_IF_ERROR(
          loom_input_remap_path(capture, related[i].source_location.filename,
                                &related[i].source_location.filename));
    }
    remapped.related_locations = related;
  }
  loom_diagnostic_sink_t sink = capture->request->parse_options.diagnostic_sink;
  return sink.fn(sink.user_data, &remapped);
}

static iree_status_t loom_input_bind_sources(loom_input_capture_t* capture) {
  loom_input_module_t* input = capture->input;
  loom_module_t* module = input->module;
  loom_source_entry_t* entries = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(&input->source_arena, module->sources.count,
                                sizeof(*entries), (void**)&entries));
  input->source_table.entries = entries;
  for (iree_host_size_t i = 0; i < module->sources.count; ++i) {
    iree_string_view_t path = module->sources.entries[i];
    iree_string_view_t filename = iree_string_view_empty();
    IREE_RETURN_IF_ERROR(loom_input_remap_path(capture, path, &filename));
    // Locations keep their IDs; only the source table's displayed names change.
    IREE_RETURN_IF_ERROR(loom_input_copy_string(&module->arena, filename,
                                                &module->sources.entries[i]));
    for (loom_input_snapshot_t* snapshot = capture->snapshots; snapshot;
         snapshot = snapshot->next) {
      if (iree_string_view_equal(snapshot->path, path)) {
        entries[input->source_table.count++] = (loom_source_entry_t){
            .source_id = (loom_source_id_t)i,
            .source = snapshot->source,
            .filename = snapshot->filename,
        };
        break;
      }
    }
  }
  return iree_ok_status();
}

iree_status_t loom_input_module_load(const loom_input_provider_t* provider,
                                     const loom_input_request_t* request,
                                     loom_context_t* context,
                                     iree_arena_block_pool_t* block_pool,
                                     iree_allocator_t host_allocator,
                                     loom_input_module_t* out_input) {
  *out_input = (loom_input_module_t){0};
  iree_arena_initialize(block_pool, &out_input->source_arena);
  loom_input_capture_t capture = {.input = out_input, .request = request};
  IREE_RETURN_IF_ERROR(
      loom_input_capture_source(&capture, request->path, request->source));
  out_input->filename = capture.snapshots->filename;
  loom_input_request_t frontend_request = *request;
  if (request->parse_options.diagnostic_sink.fn) {
    frontend_request.parse_options.diagnostic_sink = (loom_diagnostic_sink_t){
        .fn = loom_input_capture_diagnostic, .user_data = &capture};
  }
  IREE_RETURN_IF_ERROR(provider->load(
      &frontend_request,
      (loom_input_source_capture_t){.fn = loom_input_capture_source,
                                    .user_data = &capture},
      context, block_pool, host_allocator, &out_input->module));
  if (out_input->module) {
    IREE_RETURN_IF_ERROR(loom_input_bind_sources(&capture));
  }
  return iree_ok_status();
}

loom_source_resolver_t loom_input_module_source_resolver(
    loom_input_module_t* input) {
  return (loom_source_resolver_t){.fn = loom_source_table_resolve,
                                  .user_data = &input->source_table};
}

void loom_input_module_deinitialize(loom_input_module_t* input) {
  loom_module_free(input->module);
  iree_arena_deinitialize(&input->source_arena);
  memset(input, 0, sizeof(*input));
}
