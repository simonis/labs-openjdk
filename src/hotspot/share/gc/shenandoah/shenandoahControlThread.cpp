/*
 * Copyright (c) 2013, 2021, Red Hat, Inc. All rights reserved.
 * Copyright (C) 2022 THL A29 Limited, a Tencent company. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "gc/shenandoah/heuristics/shenandoahHeuristics.hpp"
#include "gc/shenandoah/mode/shenandoahMode.hpp"
#include "gc/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc/shenandoah/shenandoahConcurrentGC.hpp"
#include "gc/shenandoah/shenandoahControlThread.hpp"
#include "gc/shenandoah/shenandoahDegeneratedGC.hpp"
#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahFullGC.hpp"
#include "gc/shenandoah/shenandoahGeneration.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahMonitoringSupport.hpp"
#include "gc/shenandoah/shenandoahPacer.inline.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "gc/shenandoah/shenandoahVMOperations.hpp"
#include "logging/log.hpp"
#include "memory/metaspaceStats.hpp"
#include "memory/metaspaceUtils.hpp"

namespace svm_gc {

ShenandoahControlThread::ShenandoahControlThread() :
  ShenandoahController(),
  _requested_gc_cause(GCCause::_no_cause_specified),
  _degen_point(ShenandoahGC::_degenerated_outside_cycle),
  _control_lock(CONTROL_LOCK_RANK, "ShenandoahControl_lock", true) {
  set_name("Shenandoah Control Thread");
  create_and_start();
}

void ShenandoahControlThread::run_service() {
  ShenandoahHeap* const heap = ShenandoahHeap::heap();
  const GCMode default_mode = concurrent_normal;
  const GCCause::Cause default_cause = GCCause::_shenandoah_concurrent_gc;
  int sleep = ShenandoahControlIntervalMin;

  double last_sleep_adjust_time = os::elapsedTime();

  ShenandoahCollectorPolicy* const policy = heap->shenandoah_policy();
  ShenandoahHeuristics* const heuristics = heap->heuristics();
  while (!should_terminate()) {
#ifdef SVM
    // Deliver notifications an inline GC on the VM operation thread had to defer.
    svm_drain_pending_waiter_notifications();
    const size_t svm_inline_gc_count_at_decision = Atomic::load(&_svm_inline_gc_count);
#endif // SVM
    const GCCause::Cause cancelled_cause = heap->cancelled_cause();
    if (cancelled_cause == GCCause::_shenandoah_stop_vm) {
      break;
    }

    // Figure out if we have pending requests.
    const bool alloc_failure_pending = ShenandoahCollectorPolicy::is_allocation_failure(cancelled_cause);
    bool is_gc_requested = _gc_requested.is_set();
    const GCCause::Cause requested_gc_cause = _requested_gc_cause;

    // This control loop iteration has seen this much allocation.
    const size_t allocs_seen = reset_allocs_seen();

    // Choose which GC mode to run in. The block below should select a single mode.
    GCMode mode = none;
    GCCause::Cause cause = GCCause::_last_gc_cause;
    ShenandoahGC::ShenandoahDegenPoint degen_point = ShenandoahGC::_degenerated_unset;

    if (alloc_failure_pending) {
      // Allocation failure takes precedence: we have to deal with it first thing
      heuristics->log_trigger("Handle Allocation Failure");

      cause = GCCause::_allocation_failure;

      // Consume the degen point, and seed it with default value
      degen_point = _degen_point;
      _degen_point = ShenandoahGC::_degenerated_outside_cycle;

      if (ShenandoahDegeneratedGC && heuristics->should_degenerate_cycle()) {
        heuristics->record_allocation_failure_gc();
        policy->record_alloc_failure_to_degenerated(degen_point);
        mode = stw_degenerated;
      } else {
        heuristics->record_allocation_failure_gc();
        policy->record_alloc_failure_to_full();
        mode = stw_full;
      }
    } else if (is_gc_requested) {
      cause = requested_gc_cause;
      heuristics->log_trigger("GC request (%s)", GCCause::to_string(cause));
      heuristics->record_requested_gc();

      if (ShenandoahCollectorPolicy::should_run_full_gc(cause)) {
        mode = stw_full;
      } else {
        mode = default_mode;
        // Unload and clean up everything
        heap->set_unload_classes(heuristics->can_unload_classes());
      }
    } else {
      // Potential normal cycle: ask heuristics if it wants to act
      if (heuristics->should_start_gc()) {
        mode = default_mode;
        cause = default_cause;
      }

      // Ask policy if this cycle wants to process references or unload classes
      heap->set_unload_classes(heuristics->should_unload_classes());
    }

    // Blow all soft references on this cycle, if handling allocation failure,
    // either implicit or explicit GC request,  or we are requested to do so unconditionally.
    if (alloc_failure_pending || is_gc_requested || ShenandoahAlwaysClearSoftRefs) {
      heap->soft_ref_policy()->set_should_clear_all_soft_refs(true);
    }

    const bool gc_requested = (mode != none);
    assert (!gc_requested || cause != GCCause::_last_gc_cause, "GC cause should be set");

    if (gc_requested) {
#ifdef SVM
      // Own the (single) logical GC for the whole cycle. The VM operation thread try-acquires
      // the ownership token before running a GC inline (see run_gc_on_vm_thread): holding it here
      // for the complete cycle guarantees that an inline GC can never interleave with this cycle's
      // GC state and phase tracking. Only ever contended by that inline GC, so the acquisition
      // below only blocks while an inline GC is running, which is exactly when it must wait.
      ShenandoahSVMCycleOwnershipMark svm_cycle_ownership(this);
      if (Atomic::load(&_svm_inline_gc_count) != svm_inline_gc_count_at_decision) {
        // An inline GC ran on the VM operation thread between this loop's decision and the
        // ownership acquisition above. The decision inputs are stale: the inline GC cleared
        // the cancellation and reset the marking state, so e.g. a degenerated cycle that
        // expected to resume from a mid-cycle point (with complete marking) would fire
        // "assert(is_mark_complete()): Marking must be completed". Re-evaluate from scratch.
        log_info(gc)("Cycle decision invalidated by an inline GC on the VM operation thread, re-evaluating");
        continue;
      }
#endif // SVM

      // Cannot uncommit bitmap slices during concurrent reset
      ShenandoahNoUncommitMark forbid_region_uncommit(heap);

      // GC is starting, bump the internal ID
      update_gc_id();

      GCIdMark gc_id_mark;

      heuristics->cancel_trigger_request();

      heap->reset_bytes_allocated_since_gc_start();

#ifndef SVM
      MetaspaceCombinedStats meta_sizes = MetaspaceUtils::get_combined_statistics();
#endif // !SVM

      // If GC was requested, we are sampling the counters even without actual triggers
      // from allocation machinery. This captures GC phases more accurately.
      heap->set_forced_counters_update(true);

      // If GC was requested, we better dump freeset data for performance debugging
      heap->free_set()->log_status_under_lock();

      switch (mode) {
        case concurrent_normal:
          service_concurrent_normal_cycle(cause);
          break;
        case stw_degenerated:
          service_stw_degenerated_cycle(cause, degen_point);
          break;
        case stw_full:
          service_stw_full_cycle(cause);
          break;
        default:
          ShouldNotReachHere();
      }

      // If this was the requested GC cycle, notify waiters about it
      if (is_gc_requested) {
        notify_gc_waiters();
      }

      // If this cycle completed without being cancelled, notify waiters about it
      if (!heap->cancelled_gc()) {
        notify_alloc_failure_waiters();
      }

      // Report current free set state at the end of cycle, whether
      // it is a normal completion, or the abort.
      heap->free_set()->log_status_under_lock();

      {
        // Notify Universe about new heap usage. This has implications for
        // global soft refs policy, and we better report it every time heap
        // usage goes down.
        ShenandoahHeapLocker locker(heap->lock());
        heap->update_capacity_and_used_at_gc();
      }

      // Signal that we have completed a visit to all live objects.
      heap->record_whole_heap_examined_timestamp();

      // Disable forced counters update, and update counters one more time
      // to capture the state at the end of GC session.
      heap->handle_force_counters_update();
      heap->set_forced_counters_update(false);

      // Retract forceful part of soft refs policy
      heap->soft_ref_policy()->set_should_clear_all_soft_refs(false);

      // Clear metaspace oom flag, if current cycle unloaded classes
      if (heap->unload_classes()) {
        heuristics->clear_metaspace_oom();
      }

      // Manage and print gc stats
      heap->process_gc_stats();

#ifndef SVM
      // Print Metaspace change following GC (if logging is enabled).
      MetaspaceUtils::print_metaspace_change(meta_sizes);

      // GC is over, we are at idle now
      if (ShenandoahPacing) {
        heap->pacer()->setup_for_idle();
      }
#endif // !SVM
    } else {
#ifndef SVM
      // Report to pacer that we have seen this many words allocated
      if (ShenandoahPacing && (allocs_seen > 0)) {
        heap->pacer()->report_alloc(allocs_seen);
      }
#endif // !SVM
    }

    // Check if we have seen a new target for soft max heap size or if a gc was requested.
    // Either of these conditions will attempt to uncommit regions.
    if (ShenandoahUncommit) {
      if (heap->check_soft_max_changed()) {
        heap->notify_soft_max_changed();
      } else if (is_gc_requested) {
        heap->notify_explicit_gc_requested();
      }
    }

    // Wait before performing the next action. If allocation happened during this wait,
    // we exit sooner, to let heuristics re-evaluate new conditions. If we are at idle,
    // back off exponentially.
    const double current = os::elapsedTime();
    if (heap->has_changed()) {
      sleep = ShenandoahControlIntervalMin;
    } else if ((current - last_sleep_adjust_time) * 1000 > ShenandoahControlIntervalAdjustPeriod){
      sleep = MIN2<int>(ShenandoahControlIntervalMax, MAX2(1, sleep * 2));
      last_sleep_adjust_time = current;
    }

    MonitorLocker ml(&_control_lock, Mutex::_no_safepoint_check_flag);
    ml.wait(sleep);
  }
}

void ShenandoahControlThread::service_concurrent_normal_cycle(GCCause::Cause cause) {
  // Normal cycle goes via all concurrent phases. If allocation failure (af) happens during
  // any of the concurrent phases, it first degrades to Degenerated GC and completes GC there.
  // If second allocation failure happens during Degenerated GC cycle (for example, when GC
  // tries to evac something and no memory is available), cycle degrades to Full GC.
  //
  // There are also a shortcut through the normal cycle: immediate garbage shortcut, when
  // heuristics says there are no regions to compact, and all the collection comes from immediately
  // reclaimable regions.
  //
  // ................................................................................................
  //
  //                                    (immediate garbage shortcut)                Concurrent GC
  //                             /-------------------------------------------\
  //                             |                                           |
  //                             |                                           |
  //                             |                                           |
  //                             |                                           v
  // [START] ----> Conc Mark ----o----> Conc Evac --o--> Conc Update-Refs ---o----> [END]
  //                   |                    |                 |              ^
  //                   | (af)               | (af)            | (af)         |
  // ..................|....................|.................|..............|.......................
  //                   |                    |                 |              |
  //                   |                    |                 |              |      Degenerated GC
  //                   v                    v                 v              |
  //               STW Mark ----------> STW Evac ----> STW Update-Refs ----->o
  //                   |                    |                 |              ^
  //                   | (af)               | (af)            | (af)         |
  // ..................|....................|.................|..............|.......................
  //                   |                    |                 |              |
  //                   |                    v                 |              |      Full GC
  //                   \------------------->o<----------------/              |
  //                                        |                                |
  //                                        v                                |
  //                                      Full GC  --------------------------/
  //
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  if (check_cancellation_or_degen(ShenandoahGC::_degenerated_outside_cycle)) {
    log_info(gc)("Cancelled");
    return;
  }

  ShenandoahGCSession session(cause, heap->global_generation());

#ifndef SVM
  TraceCollectorStats tcs(heap->monitoring_support()->concurrent_collection_counters());
#endif // !SVM

  ShenandoahConcurrentGC gc(heap->global_generation(), false);
  if (gc.collect(cause)) {
    // Cycle is complete.  There were no failed allocation requests and no degeneration, so count this as good progress.
    heap->notify_gc_progress();
    heap->global_generation()->heuristics()->record_success_concurrent();
    heap->shenandoah_policy()->record_success_concurrent(false, gc.abbreviated());
    heap->log_heap_status("At end of GC");
  } else {
    assert(heap->cancelled_gc(), "Must have been cancelled");
    check_cancellation_or_degen(gc.degen_point());
    heap->log_heap_status("At end of cancelled GC");
  }
}

bool ShenandoahControlThread::check_cancellation_or_degen(ShenandoahGC::ShenandoahDegenPoint point) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  if (heap->cancelled_gc()) {
    if (heap->cancelled_cause() == GCCause::_shenandoah_stop_vm) {
      return true;
    }

    if (ShenandoahCollectorPolicy::is_allocation_failure(heap->cancelled_cause())) {
#ifdef SVM
      if (_degen_point != ShenandoahGC::_degenerated_outside_cycle) {
        // A GC that the VM thread ran inline cancelled our cycle again after we already
        // recorded a degeneration point. It cancels so that we unwind rather than resume
        // phases whose state it reset (see run_gc_on_vm_thread()) so keep the point we
        // have because the degenerated cycle that follows detects the inline GC and upgrades
        // to a full GC anyway (see ShenandoahDegenGC::op_degenerated()).
        return true;
      }
#endif // SVM
      assert (_degen_point == ShenandoahGC::_degenerated_outside_cycle,
              "Should not be set yet: %s", ShenandoahGC::degen_point_to_string(_degen_point));
      _degen_point = point;
      return true;
    }

    fatal("Unexpected reason for cancellation: %s", GCCause::to_string(heap->cancelled_cause()));
  }
  return false;
}

void ShenandoahControlThread::stop_service() {
  ShenandoahHeap::heap()->cancel_gc(GCCause::_shenandoah_stop_vm);
}

void ShenandoahControlThread::service_stw_full_cycle(GCCause::Cause cause) {
  ShenandoahHeap* const heap = ShenandoahHeap::heap();
#ifndef SVM
  // In SVM the ShenandoahGCSession is created inside VM_ShenandoahFullGC::doit()
  // so that the GC cycle bookkeeping (which asserts that no other cycle is active)
  // always runs on the single VM operation thread. This serializes a GC driven by
  // the control thread with a GC the VM operation thread runs inline for itself.
  ShenandoahGCSession session(cause, heap->global_generation());
#endif // !SVM

  ShenandoahFullGC gc;
  gc.collect(cause);
}

void ShenandoahControlThread::service_stw_degenerated_cycle(GCCause::Cause cause, ShenandoahGC::ShenandoahDegenPoint point) {
  assert (point != ShenandoahGC::_degenerated_unset, "Degenerated point should be set");
  ShenandoahHeap* const heap = ShenandoahHeap::heap();
#ifndef SVM
  // In SVM the ShenandoahGCSession is created inside VM_ShenandoahDegeneratedGC::doit().
  ShenandoahGCSession session(cause, heap->global_generation());
#endif // !SVM

  ShenandoahDegenGC gc(point, heap->global_generation());
  gc.collect(cause);
}

void ShenandoahControlThread::request_gc(GCCause::Cause cause) {
  if (ShenandoahCollectorPolicy::should_handle_requested_gc(cause)) {
    handle_requested_gc(cause);
  }
}

void ShenandoahControlThread::notify_control_thread(GCCause::Cause cause) {
  // Although setting gc request is under _controller_lock, the read side (run_service())
  // does not take the lock. We need to enforce following order, so that read side sees
  // latest requested gc cause when the flag is set.
  MonitorLocker controller(&_control_lock, Mutex::_no_safepoint_check_flag);
  _requested_gc_cause = cause;
  _gc_requested.set();
  controller.notify();
}

void ShenandoahControlThread::handle_requested_gc(GCCause::Cause cause) {
  if (should_terminate()) {
    log_info(gc)("Control thread is terminating, no more GCs");
    return;
  }

  // For normal requested GCs (System.gc) we want to block the caller. However,
  // for whitebox requested GC, we want to initiate the GC and return immediately.
  // The whitebox caller thread will arrange for itself to wait until the GC notifies
  // it that has reached the requested breakpoint (phase in the GC).
  if (cause == GCCause::_wb_breakpoint) {
    notify_control_thread(cause);
    return;
  }

#ifdef SVM
  if (Thread::current()->is_VM_thread()) {
    // The VM operation thread requested a GC (e.g. System.gc() invoked from within
    // a VM operation). It cannot delegate to the control thread because the STW GC
    // is itself a VM operation that must run on this thread. Run it inline.
    run_gc_on_vm_thread(cause);
    return;
  }
#endif // SVM

  // Make sure we have at least one complete GC cycle before unblocking
  // from the explicit GC request.
  //
  // This is especially important for weak references cleanup and/or native
  // resources (e.g. DirectByteBuffers) machinery: when explicit GC request
  // comes very late in the already running cycle, it would miss lots of new
  // opportunities for cleanup that were made available before the caller
  // requested the GC.

  MonitorLocker ml(&_gc_waiters_lock);
  size_t current_gc_id = get_gc_id();
  size_t required_gc_id = current_gc_id + 1;
  while (current_gc_id < required_gc_id && !should_terminate()) {
    notify_control_thread(cause);
    ml.wait();
    current_gc_id = get_gc_id();
  }
}

void ShenandoahControlThread::notify_gc_waiters() {
  _gc_requested.unset();
  MonitorLocker ml(&_gc_waiters_lock);
  ml.notify_all();
}

#ifdef SVM
bool ShenandoahControlThread::try_notify_gc_waiters() {
  _gc_requested.unset();
  if (!_gc_waiters_lock.try_lock()) {
    return false;
  }
  _gc_waiters_lock.notify_all();
  _gc_waiters_lock.unlock();
  return true;
}

void ShenandoahControlThread::svm_run_inline_gc_cycle(GCCause::Cause cause) {
  ShenandoahHeap* const heap = ShenandoahHeap::heap();
  // Decide how to collect. This mirrors the allocation-failure handling in run_service(), i.e. for
  // an allocation failure we first try a STW degenerated GC if degenerated GCs are enabled and the
  // heuristics allow it. A degenerated GC automatically upgrades to a full GC when it cannot make
  // sufficient progress. For any other cause (e.g. an explicit System.gc()) we run a full GC right
  // away. The ShenandoahGCSession is created in the VM operation's doit() so it serializes with a
  // GC the control thread may have queued.
  ShenandoahCollectorPolicy* const policy = heap->shenandoah_policy();
  ShenandoahHeuristics* const heuristics = heap->heuristics();
  const bool alloc_failure = ShenandoahCollectorPolicy::is_allocation_failure(cause);

  // Log the trigger and record the request, mirroring run_service().
  if (alloc_failure) {
    heuristics->log_trigger("Handle Allocation Failure in SVM Operation thread");
  } else {
    heuristics->log_trigger("GC request (%s) in SVM Operation thread", GCCause::to_string(cause));
    heuristics->record_requested_gc();
  }

  if (alloc_failure && ShenandoahDegeneratedGC && heuristics->should_degenerate_cycle()) {
    heuristics->record_allocation_failure_gc();
    // We always degenerate from "outside the cycle". Unlike run_service(), we do not consume the
    // control thread's _degen_point because that field records where the Control thread's concurrent
    // cycle was cancelled but that is owned/mutated by the Control thread, and it is semantically
    // unrelated to this thread's allocation failure. This thread is not resuming a concurrent
    // cycle but instead starts a fresh, self-contained STW collection, which is exactly what
    // _degenerated_outside_cycle means.
    policy->record_alloc_failure_to_degenerated(ShenandoahGC::_degenerated_outside_cycle);
    // The degenerated cycle falls back to a full GC automatically if it fails.
    service_stw_degenerated_cycle(cause, ShenandoahGC::_degenerated_outside_cycle);
  } else {
    if (alloc_failure) {
      heuristics->record_allocation_failure_gc();
      policy->record_alloc_failure_to_full();
    }
    // Blow all soft references: this is the last resort GC.
    heap->soft_ref_policy()->set_should_clear_all_soft_refs(true);
    service_stw_full_cycle(cause);
  }
}
#endif // SVM

} // namespace svm_gc
