// Copyright 2020 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/task/task.h"

#include <stdio.h>
#include <string.h>

#include "iree/base/tracing.h"
#include "iree/task/list.h"
#include "iree/task/pool.h"
#include "iree/task/post_batch.h"
#include "iree/task/scope.h"
#include "iree/task/submission.h"
#include "iree/task/task_impl.h"
#include "iree/task/tuning.h"

//==============================================================================
// Task bookkeeping
//==============================================================================

void iree_task_initialize(iree_task_type_t type, iree_task_scope_t* scope,
                          iree_task_t* out_task) {
  // NOTE: only clears the header, not the task body.
  memset(out_task, 0, sizeof(*out_task));
  out_task->scope = scope;
  out_task->affinity_set = iree_task_affinity_for_any_worker();
  out_task->type = type;
}

void iree_task_set_cleanup_fn(iree_task_t* task,
                              iree_task_cleanup_fn_t cleanup_fn) {
  task->cleanup_fn = cleanup_fn;
}

void iree_task_set_completion_task(iree_task_t* task,
                                   iree_task_t* completion_task) {
  IREE_ASSERT(!task->completion_task);
  task->completion_task = completion_task;
  iree_atomic_fetch_add_int32(&completion_task->pending_dependency_count, 1,
                              iree_memory_order_seq_cst);
}

bool iree_task_is_ready(iree_task_t* task) {
  if (iree_atomic_load_int32(&task->pending_dependency_count,
                             iree_memory_order_relaxed) > 0) {
    // At least one dependency is still pending.
    return false;
  }
  return true;
}

static void iree_task_try_set_status(iree_atomic_intptr_t* permanent_status,
                                     iree_status_t new_status) {
  if (IREE_UNLIKELY(iree_status_is_ok(new_status))) return;

  IREE_TRACE_ZONE_BEGIN(z0);
  IREE_TRACE_ZONE_APPEND_TEXT(z0, "failed: ");
  IREE_TRACE_ZONE_APPEND_TEXT(
      z0, iree_status_code_string(iree_status_code(new_status)));

  iree_status_t old_status = iree_ok_status();
  if (!iree_atomic_compare_exchange_strong_intptr(
          permanent_status, (intptr_t*)&old_status, (intptr_t)new_status,
          iree_memory_order_seq_cst, iree_memory_order_seq_cst)) {
    // Previous status was not OK; drop our new status.
    IREE_IGNORE_ERROR(new_status);
  }

  IREE_TRACE_ZONE_END(z0);
}

static void iree_task_cleanup(iree_task_t* task,
                              iree_status_code_t status_code) {
  // Call the (optional) cleanup function.
  // NOTE: this may free the memory of the task itself!
  iree_task_pool_t* pool = task->pool;
  if (task->cleanup_fn) {
    task->cleanup_fn(task, status_code);
  }

  // Return the task to the pool it was allocated from.
  // Some tasks are allocated as part of arenas/ringbuffers and won't have a
  // pool as they'll be cleaned up as part of a larger operation.
  if (pool) {
    iree_task_pool_release(pool, task);
  }
}

void iree_task_discard(iree_task_t* task, iree_task_list_t* discard_worklist) {
  IREE_TRACE_ZONE_BEGIN(z0);

  // NOTE: we always try adding to the head of the discard_worklist so that
  // we hopefully get some locality benefits. This models a DFS discard in
  // our non-recursive approach.

  // Almost all tasks will have a completion task; some may have additional
  // dependent tasks (like barriers) that will be handled below.
  if (task->completion_task) {
    iree_task_list_push_front(discard_worklist, task->completion_task);
  }

  switch (task->type) {
    default:
    case IREE_TASK_TYPE_NOP:
    case IREE_TASK_TYPE_CALL:
      break;
    case IREE_TASK_TYPE_BARRIER: {
      iree_task_barrier_t* barrier_task = (iree_task_barrier_t*)task;
      for (uint32_t i = 0; i < barrier_task->dependent_task_count; ++i) {
        iree_task_list_push_front(discard_worklist,
                                  barrier_task->dependent_tasks[i]);
      }
      break;
    }
    case IREE_TASK_TYPE_FENCE: {
      iree_task_scope_end(task->scope);
      break;
    }
    case IREE_TASK_TYPE_WAIT:
    case IREE_TASK_TYPE_DISPATCH:
    case IREE_TASK_TYPE_DISPATCH_SLICE:
      break;
  }

  iree_task_cleanup(task, IREE_STATUS_ABORTED);
  // NOTE: task is invalidated here and cannot be used!

  IREE_TRACE_ZONE_END(z0);
}

static void iree_task_retire(iree_task_t* task,
                             iree_task_submission_t* pending_submission,
                             iree_status_t status) {
  IREE_ASSERT_EQ(0, iree_atomic_load_int32(&task->pending_dependency_count,
                                           iree_memory_order_acquire));

  // Decrement the pending count on the completion task, if any.
  iree_task_t* completion_task = task->completion_task;
  task->completion_task = NULL;
  bool completion_task_ready =
      completion_task &&
      iree_atomic_fetch_sub_int32(&completion_task->pending_dependency_count, 1,
                                  iree_memory_order_acq_rel) == 1;

  if (iree_status_is_ok(status)) {
    // Task completed successfully.
    iree_task_cleanup(task, IREE_STATUS_OK);
    if (completion_task_ready) {
      // This was the last pending dependency and the completion task is ready
      // to run.
      iree_task_submission_enqueue(pending_submission, completion_task);
    }
  } else {
    // Task failed.
    iree_task_scope_fail(task->scope, task, status);
    status = iree_ok_status();  // consumed by the fail
    iree_task_cleanup(task, IREE_STATUS_ABORTED);
    if (completion_task_ready) {
      // This was the last pending dependency and we know that we can safely
      // abort the completion task by discarding.
      iree_task_list_t discard_worklist;
      iree_task_list_initialize(&discard_worklist);
      iree_task_discard(completion_task, &discard_worklist);
      iree_task_list_discard(&discard_worklist);
    } else if (completion_task) {
      // One or more pending dependencies are not yet satisfied and the
      // completion task must stay alive. We can mark it as aborted, though,
      // so that it knows not to execute when it is ready to run.
      // TODO(benvanik): make this atomic? we only ever add bits and it's safe
      // for it to run if we got this far.
      completion_task->flags |= IREE_TASK_FLAG_ABORTED;
    }
  }

  // NOTE: task is invalidated here and cannot be used!
  task = NULL;
}

//==============================================================================
// IREE_TASK_TYPE_NOP
//==============================================================================

void iree_task_nop_initialize(iree_task_scope_t* scope,
                              iree_task_nop_t* out_task) {
  iree_task_initialize(IREE_TASK_TYPE_NOP, scope, &out_task->header);
}

void iree_task_nop_retire(iree_task_nop_t* task,
                          iree_task_submission_t* pending_submission) {
  iree_task_retire(&task->header, pending_submission, iree_ok_status());
}

//==============================================================================
// IREE_TASK_TYPE_CALL
//==============================================================================

// Returns an XXBBGGRR color (red in the lowest bits).
// Must not be 0 (tracy will ignore).
static uint32_t iree_math_ptr_to_xrgb(const void* ptr) {
  // This is just a simple hack to give us a unique(ish) per-pointer color.
  // It's only to make it easier to distinguish which tiles are from the same
  // dispatch.
  uint64_t ptr64 = (uintptr_t)ptr;
  return (uint32_t)ptr64 ^ (uint32_t)(ptr64 >> 32);
}

void iree_task_call_initialize(iree_task_scope_t* scope,
                               iree_task_call_closure_t closure,
                               iree_task_call_t* out_task) {
  iree_task_initialize(IREE_TASK_TYPE_CALL, scope, &out_task->header);
  out_task->closure = closure;
  iree_atomic_store_intptr(&out_task->status, 0, iree_memory_order_release);
}

void iree_task_call_execute(iree_task_call_t* task,
                            iree_task_submission_t* pending_submission) {
  IREE_TRACE_ZONE_BEGIN(z0);
  IREE_TRACE_ZONE_SET_COLOR(z0,
                            iree_math_ptr_to_xrgb(task->closure.user_context));

  if (IREE_LIKELY(
          !iree_any_bit_set(task->header.flags, IREE_TASK_FLAG_ABORTED))) {
    // Execute the user callback.
    // Note that this may enqueue more nested tasks, including tasks that
    // prevent this task from retiring.
    iree_status_t status = task->closure.fn(task->closure.user_context,
                                            &task->header, pending_submission);
    if (!iree_status_is_ok(status)) {
      // Stash the failure status on the task.
      // If there's still pending dependencies we won't be able to discard
      // immediately and need to keep the status around until they all complete.
      iree_task_try_set_status(&task->status, status);
      status = iree_ok_status();  // consumed by try_set_status

      // TODO(benvanik): discard pending_submission? As we may have pending work
      // from multiple scopes it's dangerous to discard all. We could filter
      // based on scope, though, and if we did that we (probably) wouldn't need
      // to handle the permanent status on the task and could discard
      // immediately.
    }
  }

  // Check to see if there are no pending dependencies before retiring; the
  // dependency count can go up if new nested tasks were enqueued.
  if (iree_atomic_load_int32(&task->header.pending_dependency_count,
                             iree_memory_order_acquire) == 0) {
    iree_status_t status = (iree_status_t)iree_atomic_exchange_intptr(
        &task->status, 0, iree_memory_order_seq_cst);
    iree_task_retire(&task->header, pending_submission, status);
  }

  IREE_TRACE_ZONE_END(z0);
}

//==============================================================================
// IREE_TASK_TYPE_BARRIER
//==============================================================================

void iree_task_barrier_initialize(iree_task_scope_t* scope,
                                  iree_host_size_t dependent_task_count,
                                  iree_task_t* const* dependent_tasks,
                                  iree_task_barrier_t* out_task) {
  iree_task_initialize(IREE_TASK_TYPE_BARRIER, scope, &out_task->header);
  out_task->dependent_task_count = dependent_task_count;
  out_task->dependent_tasks = dependent_tasks;
  for (iree_host_size_t i = 0; i < out_task->dependent_task_count; ++i) {
    iree_task_t* dependent_task = out_task->dependent_tasks[i];
    iree_atomic_fetch_add_int32(&dependent_task->pending_dependency_count, 1,
                                iree_memory_order_relaxed);
  }
}

void iree_task_barrier_initialize_empty(iree_task_scope_t* scope,
                                        iree_task_barrier_t* out_task) {
  iree_task_initialize(IREE_TASK_TYPE_BARRIER, scope, &out_task->header);
  out_task->dependent_task_count = 0;
  out_task->dependent_tasks = NULL;
}

void iree_task_barrier_set_dependent_tasks(
    iree_task_barrier_t* task, iree_host_size_t dependent_task_count,
    iree_task_t* const* dependent_tasks) {
  task->dependent_task_count = dependent_task_count;
  task->dependent_tasks = dependent_tasks;
  for (iree_host_size_t i = 0; i < task->dependent_task_count; ++i) {
    iree_task_t* dependent_task = task->dependent_tasks[i];
    iree_atomic_fetch_add_int32(&dependent_task->pending_dependency_count, 1,
                                iree_memory_order_relaxed);
  }
}

void iree_task_barrier_retire(iree_task_barrier_t* task,
                              iree_task_submission_t* pending_submission) {
  IREE_TRACE_ZONE_BEGIN(z0);

  // NOTE: we walk in reverse so that we enqueue in LIFO order.
  for (iree_host_size_t i = 0; i < task->dependent_task_count; ++i) {
    iree_task_t* dependent_task =
        task->dependent_tasks[task->dependent_task_count - i - 1];
    if (iree_atomic_fetch_sub_int32(&dependent_task->pending_dependency_count,
                                    1, iree_memory_order_acq_rel) == 1) {
      // The dependent task has retired and can now be made ready.
      iree_task_submission_enqueue(pending_submission, dependent_task);
    }
  }

  iree_task_retire(&task->header, pending_submission, iree_ok_status());
  IREE_TRACE_ZONE_END(z0);
}

//==============================================================================
// IREE_TASK_TYPE_FENCE
//==============================================================================

void iree_task_fence_initialize(iree_task_scope_t* scope,
                                iree_task_fence_t* out_task) {
  iree_task_initialize(IREE_TASK_TYPE_FENCE, scope, &out_task->header);
  iree_task_scope_begin(scope);
}

void iree_task_fence_retire(iree_task_fence_t* task,
                            iree_task_submission_t* pending_submission) {
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_task_scope_end(task->header.scope);

  iree_task_retire(&task->header, pending_submission, iree_ok_status());
  IREE_TRACE_ZONE_END(z0);
}

//==============================================================================
// IREE_TASK_TYPE_WAIT
//==============================================================================

void iree_task_wait_initialize(iree_task_scope_t* scope,
                               iree_wait_handle_t wait_handle,
                               iree_task_wait_t* out_task) {
  iree_task_initialize(IREE_TASK_TYPE_WAIT, scope, &out_task->header);
  out_task->wait_handle = wait_handle;
}

bool iree_task_wait_check_condition(iree_task_wait_t* task) {
  // TODO(benvanik): conditions.
  task->header.flags |= IREE_TASK_FLAG_WAIT_COMPLETED;
  return true;
}

void iree_task_wait_retire(iree_task_wait_t* task,
                           iree_task_submission_t* pending_submission) {
  IREE_TRACE_ZONE_BEGIN(z0);
  // TODO(benvanik): allow deinit'ing the wait handle (if transient).
  iree_task_retire(&task->header, pending_submission, iree_ok_status());
  IREE_TRACE_ZONE_END(z0);
}

//==============================================================================
// IREE_TASK_TYPE_DISPATCH_* utilities
//==============================================================================

// Returns an XXBBGGRR color (red in the lowest bits).
// Must not be 0 (tracy will ignore).
static uint32_t iree_task_tile_to_color(
    const iree_task_tile_context_t* tile_context);

#if defined(IREE_TASK_TRACING_PER_TILE_COLORS)

// TODO(#4017): optimize this to compute entire slices at once and fold in the
// work grid location code.
static uint32_t iree_math_hsv_to_xrgb(const uint8_t h, const uint8_t s,
                                      const uint8_t v) {
  // NOTE: this is matching with tracy's TracyColor.cpp implementation so that
  // our colors fit nicely in the UI.
  const uint8_t reg = h / 43;
  const uint8_t rem = (h - (reg * 43)) * 6;
  const uint8_t p = (v * (255 - s)) >> 8;
  const uint8_t q = (v * (255 - ((s * rem) >> 8))) >> 8;
  const uint8_t t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8;

  // clang-format off
  uint8_t r, g, b;
  switch (reg) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  // clang-format on

  uint32_t xrgb = (r << 16) | (g << 8) | b;
  xrgb |= (xrgb ? 0 : 1);  // ensure never zero
  return xrgb;
}

static uint32_t iree_task_tile_to_color(
    const iree_task_tile_context_t* tile_context) {
  // TODO(#4017): optimize such that it's always on when tracing is
  // enabled by amortizing the cost across the entire slice.

  // Picked to try to make it easy to see gradients from tiles along the same x,
  // y, and z (in that order). x is the fastest changing dimension and as such
  // should all have the same hue, while z is the slowest changing dimension and
  // should have different hues.
  uint8_t h = (tile_context->workgroup_xyz[1] /
               (float)(tile_context->workgroup_count[1])) *
              255;
  h = (h * 11400714819323198485ull) & 0xFF;
  uint8_t s = 100 - (tile_context->workgroup_xyz[2] /
                     (float)(tile_context->workgroup_count[2])) *
                        100;
  uint8_t v = (tile_context->workgroup_xyz[0] /
               (float)(tile_context->workgroup_count[0])) *
                  50 +
              50;
  return iree_math_hsv_to_xrgb(h, s, v);
}

#else

static uint32_t iree_task_tile_to_color(
    const iree_task_tile_context_t* tile_context) {
  return 0;  // use default tracy colors
}

#endif  // IREE_TASK_TRACING_PER_TILE_COLORS

void iree_task_dispatch_statistics_merge(
    const iree_task_dispatch_statistics_t* source,
    iree_task_dispatch_statistics_t* target) {
  // TODO(benvanik): statistics.
}

//==============================================================================
// IREE_TASK_TYPE_DISPATCH
//==============================================================================

static void iree_task_dispatch_initialize_base(
    iree_task_scope_t* scope, iree_task_dispatch_closure_t closure,
    const uint32_t workgroup_size[3], iree_task_dispatch_t* out_task) {
  iree_task_initialize(IREE_TASK_TYPE_DISPATCH, scope, &out_task->header);
  out_task->closure = closure;
  memcpy(out_task->workgroup_size, workgroup_size,
         sizeof(out_task->workgroup_size));
  out_task->local_memory_size = 0;
  iree_atomic_store_intptr(&out_task->status, 0, iree_memory_order_release);
  memset(&out_task->statistics, 0, sizeof(out_task->statistics));
}

void iree_task_dispatch_initialize(iree_task_scope_t* scope,
                                   iree_task_dispatch_closure_t closure,
                                   const uint32_t workgroup_size[3],
                                   const uint32_t workgroup_count[3],
                                   iree_task_dispatch_t* out_task) {
  iree_task_dispatch_initialize_base(scope, closure, workgroup_size, out_task);
  memcpy(out_task->workgroup_count.value, workgroup_count,
         sizeof(out_task->workgroup_count.value));
}

void iree_task_dispatch_initialize_indirect(
    iree_task_scope_t* scope, iree_task_dispatch_closure_t closure,
    const uint32_t workgroup_size[3], const uint32_t* workgroup_count_ptr,
    iree_task_dispatch_t* out_task) {
  iree_task_dispatch_initialize_base(scope, closure, workgroup_size, out_task);
  out_task->header.flags |= IREE_TASK_FLAG_DISPATCH_INDIRECT;
  out_task->workgroup_count.ptr = workgroup_count_ptr;
}

void iree_task_dispatch_issue_sliced(iree_task_dispatch_t* dispatch_task,
                                     iree_task_pool_t* slice_task_pool,
                                     iree_task_submission_t* pending_submission,
                                     iree_task_post_batch_t* post_batch) {
  IREE_TRACE_ZONE_BEGIN(z0);

  // Mark the dispatch as having been issued; the next time it retires it'll be
  // because all work has completed.
  dispatch_task->header.flags |= IREE_TASK_FLAG_DISPATCH_RETIRE;

  // Fetch the workgroup count (directly or indirectly).
  // By the task being ready to execute we know any dependencies on the
  // indirection buffer have been satisfied and its safe to read.
  uint32_t workgroup_count[3];
  if (dispatch_task->header.flags & IREE_TASK_FLAG_DISPATCH_INDIRECT) {
    memcpy(workgroup_count, dispatch_task->workgroup_count.ptr,
           sizeof(workgroup_count));
  } else {
    memcpy(workgroup_count, dispatch_task->workgroup_count.value,
           sizeof(workgroup_count));
  }
  uint32_t total_workgroup_count =
      workgroup_count[0] * workgroup_count[1] * workgroup_count[2];
  if (total_workgroup_count == 0) {
    // No workgroups to execute - bail early.
    iree_task_dispatch_retire(dispatch_task, pending_submission);
    IREE_TRACE_ZONE_END(z0);
    return;
  }

#if IREE_TRACING_FEATURES & IREE_TRACING_FEATURE_INSTRUMENTATION
  char xyz_string[32];
  int xyz_string_length =
      snprintf(xyz_string, IREE_ARRAYSIZE(xyz_string), "%ux%ux%u",
               workgroup_count[0], workgroup_count[1], workgroup_count[2]);
  IREE_TRACE_ZONE_APPEND_TEXT_STRING_VIEW(z0, xyz_string, xyz_string_length);
#endif  // IREE_TRACING_FEATURES & IREE_TRACING_FEATURE_INSTRUMENTATION

  // Divide up all tiles into slices, our finest-granularity scheduling task.
  const uint32_t tiles_per_slice_x = IREE_TASK_DISPATCH_TILES_PER_SLICE_X;
  const uint32_t tiles_per_slice_y = IREE_TASK_DISPATCH_TILES_PER_SLICE_Y;
  const uint32_t tiles_per_slice_z = IREE_TASK_DISPATCH_TILES_PER_SLICE_Z;
  uint32_t slice_count_x = iree_max(1, workgroup_count[0] / tiles_per_slice_x);
  uint32_t slice_count_y = iree_max(1, workgroup_count[1] / tiles_per_slice_y);
  uint32_t slice_count_z = iree_max(1, workgroup_count[2] / tiles_per_slice_z);

  // Compute how many slices each worker will process.
  uint32_t slice_count = slice_count_x * slice_count_y * slice_count_z;
  iree_host_size_t worker_count = iree_task_post_batch_worker_count(post_batch);
  uint32_t slices_per_worker = iree_max(1, slice_count / worker_count);

  // Randomize starting worker.
  iree_host_size_t worker_offset = iree_task_post_batch_select_worker(
      post_batch, dispatch_task->header.affinity_set);
  iree_host_size_t worker_index = worker_offset;

  // TODO(benvanik): rework this with some science. For now we just iteratively
  // divide up the space from outer->inner scheduling dimension, but ideally
  // we'd use some fun cray-style torus scheduling or hilbert curve magic to
  // try to ensure better locality using worker constructive sharing masks.
  // TODO(benvanik): observe affinity_set here when dividing ranges.
  iree_host_size_t worker_slice_count = 0;
  for (uint32_t slice_z = 0; slice_z < slice_count_z; ++slice_z) {
    for (uint32_t slice_y = 0; slice_y < slice_count_y; ++slice_y) {
      for (uint32_t slice_x = 0; slice_x < slice_count_x; ++slice_x) {
        uint32_t workgroup_base[3];
        workgroup_base[0] = slice_x * tiles_per_slice_x;
        workgroup_base[1] = slice_y * tiles_per_slice_y;
        workgroup_base[2] = slice_z * tiles_per_slice_z;
        uint32_t workgroup_range[3];
        workgroup_range[0] = iree_min(workgroup_count[0],
                                      workgroup_base[0] + tiles_per_slice_x) -
                             1;
        workgroup_range[1] = iree_min(workgroup_count[1],
                                      workgroup_base[1] + tiles_per_slice_y) -
                             1;
        workgroup_range[2] = iree_min(workgroup_count[2],
                                      workgroup_base[2] + tiles_per_slice_z) -
                             1;

        // Allocate and initialize the slice.
        iree_task_dispatch_slice_t* slice_task =
            iree_task_dispatch_slice_allocate(dispatch_task, workgroup_base,
                                              workgroup_range, workgroup_count,
                                              slice_task_pool);

        // Enqueue on the worker selected for the task.
        iree_task_post_batch_enqueue(post_batch, worker_index % worker_count,
                                     &slice_task->header);
        if (++worker_slice_count >= slices_per_worker) {
          ++worker_index;
          worker_slice_count = 0;
        }
      }
    }
  }

  // NOTE: the dispatch is not retired until all slices complete. Upon the last
  // slice completing the lucky worker will retire the task inline and
  // potentially queue up more ready tasks that follow.
  //
  // The gotcha here is that it's possible for there to be zero slices within
  // a dispatch (if, for example, and indirect dispatch had its workgroup counts
  // set to zero to prevent it from running). We check for that here.
  if (slice_count == 0) {
    iree_task_dispatch_retire(dispatch_task, pending_submission);
  }

  IREE_TRACE_ZONE_END(z0);
}

void iree_task_dispatch_issue_sharded(
    iree_task_dispatch_t* dispatch_task, iree_task_pool_t* shard_task_pool,
    iree_task_submission_t* pending_submission,
    iree_task_post_batch_t* post_batch) {
  IREE_TRACE_ZONE_BEGIN(z0);

  // Mark the dispatch as having been issued; the next time it retires it'll be
  // because all work has completed.
  dispatch_task->header.flags |= IREE_TASK_FLAG_DISPATCH_RETIRE;

  iree_task_dispatch_shard_state_t* shared_state =
      &dispatch_task->shared.shard_state;

  // Fetch the workgroup count (directly or indirectly).
  if (dispatch_task->header.flags & IREE_TASK_FLAG_DISPATCH_INDIRECT) {
    // By the task being ready to execute we know any dependencies on the
    // indirection buffer have been satisfied and its safe to read. We perform
    // the indirection here and convert the dispatch to a direct one such that
    // following code can read the value.
    // TODO(benvanik): non-one-shot command buffers won't be able to do this as
    // the intent is that they can be dynamic per execution.
    const uint32_t* source_ptr = dispatch_task->workgroup_count.ptr;
    memcpy(dispatch_task->workgroup_count.value, source_ptr,
           sizeof(dispatch_task->workgroup_count.value));
    dispatch_task->header.flags ^= IREE_TASK_FLAG_DISPATCH_INDIRECT;
  }
  const uint32_t* workgroup_count = dispatch_task->workgroup_count.value;

#if IREE_TRACING_FEATURES & IREE_TRACING_FEATURE_INSTRUMENTATION
  char xyz_string[32];
  int xyz_string_length =
      snprintf(xyz_string, IREE_ARRAYSIZE(xyz_string), "%ux%ux%u",
               workgroup_count[0], workgroup_count[1], workgroup_count[2]);
  IREE_TRACE_ZONE_APPEND_TEXT_STRING_VIEW(z0, xyz_string, xyz_string_length);
#endif  // IREE_TRACING_FEATURES & IREE_TRACING_FEATURE_INSTRUMENTATION

  // Setup the iteration space for shards to pull work from the complete grid.
  iree_atomic_store_int32(&shared_state->tile_index, 0,
                          iree_memory_order_relaxed);
  shared_state->tile_count =
      workgroup_count[0] * workgroup_count[1] * workgroup_count[2];

  // Compute shard count - almost always worker_count unless we are a very small
  // dispatch (1x1x1, etc).
  iree_host_size_t worker_count = iree_task_post_batch_worker_count(post_batch);
  iree_host_size_t shard_count =
      iree_min(shared_state->tile_count, worker_count);

  // Compute how many tiles we want each shard to reserve at a time from the
  // larger grid. A higher number reduces overhead and improves locality while
  // a lower number reduces maximum worst-case latency (coarser work stealing).
  if (shared_state->tile_count <
      worker_count * IREE_TASK_DISPATCH_MAX_TILES_PER_SHARD_RESERVATION) {
    // Grid is small - allow it to be eagerly sliced up.
    shared_state->tiles_per_reservation = 1;
  } else {
    shared_state->tiles_per_reservation =
        IREE_TASK_DISPATCH_MAX_TILES_PER_SHARD_RESERVATION;
  }

  // Randomize starting worker.
  iree_host_size_t worker_offset = iree_task_post_batch_select_worker(
      post_batch, dispatch_task->header.affinity_set);
  iree_host_size_t worker_index = worker_offset;

  for (iree_host_size_t i = 0; i < shard_count; ++i) {
    // Allocate and initialize the shard.
    iree_task_dispatch_shard_t* shard_task = iree_task_dispatch_shard_allocate(
        dispatch_task, shared_state, shard_task_pool);

    // Enqueue on the worker selected for the task.
    iree_task_post_batch_enqueue(post_batch, worker_index % worker_count,
                                 &shard_task->header);
    ++worker_index;
  }

  // NOTE: the dispatch is not retired until all shards complete. Upon the last
  // shard completing the lucky worker will retire the task inline and
  // potentially queue up more ready tasks that follow.
  //
  // The gotcha here is that it's possible for there to be zero shards within
  // a dispatch (if, for example, and indirect dispatch had its workgroup counts
  // set to zero to prevent it from running). We check for that here.
  if (shard_count == 0) {
    iree_task_dispatch_retire(dispatch_task, pending_submission);
  }

  IREE_TRACE_ZONE_END(z0);
}

void iree_task_dispatch_retire(iree_task_dispatch_t* dispatch_task,
                               iree_task_submission_t* pending_submission) {
  IREE_TRACE_ZONE_BEGIN(z0);

  // TODO(benvanik): attach statistics to the tracy zone.

  // Merge the statistics from the dispatch into the scope so we can track all
  // of the work without tracking all the dispatches at a global level.
  iree_task_dispatch_statistics_merge(
      &dispatch_task->statistics,
      &dispatch_task->header.scope->dispatch_statistics);

  // Consume the status of the dispatch that may have been set from a workgroup
  // and notify the scope. We need to do this here so that each slice/shard
  // retires before we discard any subsequent tasks: otherwise a failure of
  // one shard would discard the shared dispatch task (and potentially
  // everything) while other shards were still running. We also want to avoid
  // fine-grained synchronization across slices/shards that would occur by each
  // checking to see if any other has hit an error; failure in a dispatch should
  // be so exceedingly rare that allowing some shards to complete after one
  // encounters an error is not a problem.
  iree_status_t status = (iree_status_t)iree_atomic_exchange_intptr(
      &dispatch_task->status, 0, iree_memory_order_seq_cst);

  iree_task_retire(&dispatch_task->header, pending_submission, status);
  IREE_TRACE_ZONE_END(z0);
}

//==============================================================================
// IREE_TASK_TYPE_DISPATCH_SLICE
//==============================================================================

void iree_task_dispatch_slice_initialize(iree_task_dispatch_t* dispatch_task,
                                         const uint32_t workgroup_base[3],
                                         const uint32_t workgroup_range[3],
                                         const uint32_t workgroup_count[3],
                                         iree_task_dispatch_slice_t* out_task) {
  iree_task_initialize(IREE_TASK_TYPE_DISPATCH_SLICE,
                       dispatch_task->header.scope, &out_task->header);
  iree_task_set_completion_task(&out_task->header, &dispatch_task->header);
  out_task->closure = dispatch_task->closure;
  out_task->dispatch_status = &dispatch_task->status;

  memcpy(out_task->workgroup_base, workgroup_base,
         sizeof(out_task->workgroup_base));
  memcpy(out_task->workgroup_range, workgroup_range,
         sizeof(out_task->workgroup_range));
  memcpy(out_task->workgroup_size, dispatch_task->workgroup_size,
         sizeof(out_task->workgroup_size));
  memcpy(out_task->workgroup_count, workgroup_count,
         sizeof(out_task->workgroup_count));

  // Each slice requires at most this amount of memory from the worker-local
  // pool.
  out_task->local_memory_size = dispatch_task->local_memory_size;

  // Wire up dispatch statistics; we'll track on the slice while we run and
  // then the per-slice statistics will roll up into the dispatch statistics.
  out_task->dispatch_statistics = &dispatch_task->statistics;
  memset(&out_task->slice_statistics, 0, sizeof(out_task->slice_statistics));
}

iree_task_dispatch_slice_t* iree_task_dispatch_slice_allocate(
    iree_task_dispatch_t* dispatch_task, const uint32_t workgroup_base[3],
    const uint32_t workgroup_range[3], const uint32_t workgroup_count[3],
    iree_task_pool_t* slice_task_pool) {
  iree_task_dispatch_slice_t* slice_task = NULL;
  iree_status_t status =
      iree_task_pool_acquire(slice_task_pool, (iree_task_t**)&slice_task);
  if (!iree_status_is_ok(status)) {
    iree_status_ignore(status);
    return NULL;
  }
  iree_task_dispatch_slice_initialize(dispatch_task, workgroup_base,
                                      workgroup_range, workgroup_count,
                                      slice_task);
  slice_task->header.pool = slice_task_pool;
  return slice_task;
}

void iree_task_dispatch_slice_execute(
    iree_task_dispatch_slice_t* task, iree_byte_span_t local_memory,
    iree_task_submission_t* pending_submission) {
  IREE_TRACE_ZONE_BEGIN(z0);
  IREE_TRACE_ZONE_SET_COLOR(z0,
                            iree_math_ptr_to_xrgb(task->closure.user_context));

  // TODO(benvanik): coroutine support. Ideally this function can be called
  // multiple times for the same slice, and we'll have a way to ready up the
  // slices on the same workers (some per-worker suspended list?).

  // Prepare context shared for all tiles in the slice.
  iree_task_tile_context_t tile_context;
  memcpy(&tile_context.workgroup_size, task->workgroup_size,
         sizeof(tile_context.workgroup_size));
  memcpy(&tile_context.workgroup_count, task->workgroup_count,
         sizeof(tile_context.workgroup_count));
  tile_context.statistics = &task->slice_statistics;

  // Map only the requested amount of worker local memory into the tile context.
  // This ensures that how much memory is used by some executions does not
  // inadvertently leak over into other executions.
  if (IREE_UNLIKELY(task->local_memory_size > local_memory.data_length)) {
    iree_task_retire(
        &task->header, pending_submission,
        iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                         "dispatch requires %ub of local memory but only "
                         "%zub is available per-worker",
                         task->local_memory_size, local_memory.data_length));
    IREE_TRACE_ZONE_END(z0);
    return;
  }
  tile_context.local_memory =
      iree_make_byte_span(local_memory.data, task->local_memory_size);

  iree_status_t status = iree_ok_status();
  const uint32_t base_x = task->workgroup_base[0];
  const uint32_t base_y = task->workgroup_base[1];
  const uint32_t base_z = task->workgroup_base[2];
  const uint32_t range_x = task->workgroup_range[0];
  const uint32_t range_y = task->workgroup_range[1];
  const uint32_t range_z = task->workgroup_range[2];
  for (uint32_t z = base_z; z <= range_z; ++z) {
    tile_context.workgroup_xyz[2] = z;
    for (uint32_t y = base_y; y <= range_y; ++y) {
      tile_context.workgroup_xyz[1] = y;
      for (uint32_t x = base_x; x <= range_x; ++x) {
        tile_context.workgroup_xyz[0] = x;
        IREE_TRACE_ZONE_BEGIN_NAMED(z_tile,
                                    "iree_task_dispatch_slice_execute_tile");
        IREE_TRACE_ZONE_SET_COLOR(z_tile,
                                  iree_task_tile_to_color(&tile_context));

        // NOTE: these are useful for debugging but dramatically increase our
        // cost here; only enable if needed for tracking work distribution:
        IREE_TRACE_ZONE_APPEND_VALUE(z_tile, x);
        IREE_TRACE_ZONE_APPEND_VALUE(z_tile, y);
        IREE_TRACE_ZONE_APPEND_VALUE(z_tile, z);
        // IREE_TRACE_ZONE_APPEND_VALUE(z_tile, (uint64_t)task->closure.fn);

        status = task->closure.fn(task->closure.user_context, &tile_context,
                                  pending_submission);

        IREE_TRACE_ZONE_END(z_tile);

        // If any tile fails we bail early from the loop. This doesn't match
        // what an accelerator would do but saves some unneeded work.
        // Note that other slices may have completed execution, be executing
        // concurrently with this one, or still be pending - this does not
        // have any influence on them and they may continue to execute even
        // after we bail from here.
        if (!iree_status_is_ok(status)) goto abort_slice;
      }
    }
  }
abort_slice:

  // Push aggregate statistics up to the dispatch.
  if (task->dispatch_statistics) {
    iree_task_dispatch_statistics_merge(&task->slice_statistics,
                                        task->dispatch_statistics);
  }

  // Propagate failures to the dispatch task.
  if (!iree_status_is_ok(status)) {
    iree_task_try_set_status(task->dispatch_status, status);
  }

  iree_task_retire(&task->header, pending_submission, iree_ok_status());
  IREE_TRACE_ZONE_END(z0);
}

//==============================================================================
// IREE_TASK_TYPE_DISPATCH_SHARD
//==============================================================================

void iree_task_dispatch_shard_initialize(
    iree_task_dispatch_t* dispatch_task,
    iree_task_dispatch_shard_state_t* shared_state,
    iree_task_dispatch_shard_t* out_task) {
  iree_task_initialize(IREE_TASK_TYPE_DISPATCH_SHARD,
                       dispatch_task->header.scope, &out_task->header);
  iree_task_set_completion_task(&out_task->header, &dispatch_task->header);
  out_task->dispatch_task = dispatch_task;
  out_task->shared_state = shared_state;
}

iree_task_dispatch_shard_t* iree_task_dispatch_shard_allocate(
    iree_task_dispatch_t* dispatch_task,
    iree_task_dispatch_shard_state_t* shared_state,
    iree_task_pool_t* shard_task_pool) {
  iree_task_dispatch_shard_t* shard_task = NULL;
  iree_status_t status =
      iree_task_pool_acquire(shard_task_pool, (iree_task_t**)&shard_task);
  if (!iree_status_is_ok(status)) {
    iree_status_ignore(status);
    return NULL;
  }
  iree_task_dispatch_shard_initialize(dispatch_task, shared_state, shard_task);
  shard_task->header.pool = shard_task_pool;
  return shard_task;
}

void iree_task_dispatch_shard_execute(
    iree_task_dispatch_shard_t* task, iree_byte_span_t local_memory,
    iree_task_submission_t* pending_submission) {
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_task_dispatch_t* dispatch_task = task->dispatch_task;
  IREE_TRACE_ZONE_SET_COLOR(
      z0, iree_math_ptr_to_xrgb(dispatch_task->closure.user_context));

  // Prepare context shared for all tiles in the shard.
  iree_task_dispatch_shard_state_t* shared_state = task->shared_state;
  iree_task_tile_context_t tile_context;
  memcpy(&tile_context.workgroup_size, dispatch_task->workgroup_size,
         sizeof(tile_context.workgroup_size));
  memcpy(&tile_context.workgroup_count, dispatch_task->workgroup_count.value,
         sizeof(tile_context.workgroup_count));
  uint32_t workgroup_count_x = tile_context.workgroup_count[0];
  uint32_t workgroup_count_y = tile_context.workgroup_count[1];

  // Map only the requested amount of worker local memory into the tile context.
  // This ensures that how much memory is used by some executions does not
  // inadvertently leak over into other executions.
  if (IREE_UNLIKELY(dispatch_task->local_memory_size >
                    local_memory.data_length)) {
    iree_task_retire(
        &task->header, pending_submission,
        iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                         "dispatch requires %ub of local memory but only "
                         "%zub is available per-worker",
                         dispatch_task->local_memory_size,
                         local_memory.data_length));
    IREE_TRACE_ZONE_END(z0);
    return;
  }
  tile_context.local_memory =
      iree_make_byte_span(local_memory.data, dispatch_task->local_memory_size);

  // We perform all our shard statistics work locally here and only push back to
  // the dispatch at the end; this avoids contention from each shard trying to
  // update the statistics together.
  iree_task_dispatch_statistics_t shard_statistics;
  memset(&shard_statistics, 0, sizeof(shard_statistics));
  tile_context.statistics = &shard_statistics;

  // Loop over all tiles until they are all processed.
  iree_status_t status = iree_ok_status();
  const uint32_t tile_count = shared_state->tile_count;
  const uint32_t tiles_per_reservation = shared_state->tiles_per_reservation;
  uint32_t tile_base = iree_atomic_fetch_add_int32(&shared_state->tile_index,
                                                   tiles_per_reservation,
                                                   iree_memory_order_relaxed);
  while (tile_base < tile_count) {
    const uint32_t tile_range =
        iree_min(tile_base + tiles_per_reservation, tile_count);
    for (uint32_t tile_index = tile_base; tile_index < tile_range;
         ++tile_index) {
      // TODO(benvanik): faster math here, especially knowing we pull off N
      // sequential indices per reservation.
      uint32_t tile_i = tile_index;
      tile_context.workgroup_xyz[0] = tile_i % workgroup_count_x;
      tile_i /= workgroup_count_x;
      tile_context.workgroup_xyz[1] = tile_i % workgroup_count_y;
      tile_i /= workgroup_count_y;
      tile_context.workgroup_xyz[2] = tile_i;

      IREE_TRACE_ZONE_BEGIN_NAMED(z_tile,
                                  "iree_task_dispatch_shard_execute_tile");
      IREE_TRACE_ZONE_SET_COLOR(z_tile, iree_task_tile_to_color(&tile_context));

      // NOTE: these are useful for debugging but dramatically increase our
      // cost here; only enable if needed for tracking work distribution:
      IREE_TRACE_ZONE_APPEND_VALUE(z_tile, tile_context.workgroup_xyz[0]);
      IREE_TRACE_ZONE_APPEND_VALUE(z_tile, tile_context.workgroup_xyz[1]);
      IREE_TRACE_ZONE_APPEND_VALUE(z_tile, tile_context.workgroup_xyz[2]);
      // IREE_TRACE_ZONE_APPEND_VALUE(z_tile, (uint64_t)task->closure.fn);

      status = dispatch_task->closure.fn(dispatch_task->closure.user_context,
                                         &tile_context, pending_submission);

      IREE_TRACE_ZONE_END(z_tile);

      // If any tile fails we bail early from the loop. This doesn't match
      // what an accelerator would do but saves some unneeded work.
      // Note that other slices may have completed execution, be executing
      // concurrently with this one, or still be pending - this does not
      // have any influence on them and they may continue to execute even
      // after we bail from here.
      if (!iree_status_is_ok(status)) goto abort_shard;
    }

    tile_base = iree_atomic_fetch_add_int32(&shared_state->tile_index,
                                            tiles_per_reservation,
                                            iree_memory_order_relaxed);
  }
abort_shard:

  // Push aggregate statistics up to the dispatch.
  iree_task_dispatch_statistics_merge(&shard_statistics,
                                      &dispatch_task->statistics);

  // Propagate failures to the dispatch task.
  if (!iree_status_is_ok(status)) {
    iree_task_try_set_status(&dispatch_task->status, status);
  }

  iree_task_retire(&task->header, pending_submission, iree_ok_status());
  IREE_TRACE_ZONE_END(z0);
}
