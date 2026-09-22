/*
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/shared/gcId.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "runtime/vmThread.hpp"
#include "gc/shenandoah/shenandoahController.hpp"
#include "svmGlobalData.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.inline.hpp"

namespace svm_gc {

void ShenandoahController::pacing_notify_alloc(size_t words) {
  assert(ShenandoahPacing, "should only call when pacing is enabled");
  Atomic::add(&_allocs_seen, words, memory_order_relaxed);
}

size_t ShenandoahController::reset_allocs_seen() {
  return Atomic::xchg(&_allocs_seen, (size_t)0, memory_order_relaxed);
}

void ShenandoahController::update_gc_id() {
  Atomic::inc(&_gc_id);
}

size_t ShenandoahController::get_gc_id() {
  return Atomic::load(&_gc_id);
}

void ShenandoahController::handle_alloc_failure(const ShenandoahAllocRequest& req, bool block) {
  assert(current()->is_Java_thread(), "expect Java thread here");

  const bool is_humongous = ShenandoahHeapRegion::requires_humongous(req.size());
  const GCCause::Cause cause = is_humongous ? GCCause::_shenandoah_humongous_allocation_failure : GCCause::_allocation_failure;

  ShenandoahHeap* const heap = ShenandoahHeap::heap();

#ifdef SVM
  if (current()->is_VM_thread()) {
    // In SVM the VM operation thread is a regular Java thread that can allocate
    // (and thus run into allocation failures) while executing a VM operation.
    // It must not block waiting for the control thread to run a GC: a STW GC is
    // itself a VM operation that has to run on this very thread, and the control
    // thread may be blocked waiting for this thread to become available again.
    //
    // Instead, run the GC directly and synchronously on this thread. When this
    // returns the GC has completed, so there is nothing left to wait for and the
    // allocation can be retried by the caller.
    log_info(gc)("Failed to allocate %s, " PROPERFMT " (VM thread)", req.type_string(), PROPERFMTARGS(req.size() * HeapWordSize));
    run_gc_on_vm_thread(cause);
    return;
  }
#endif // SVM

  if (heap->cancel_gc(cause)) {
    log_info(gc)("Failed to allocate %s, " PROPERFMT, req.type_string(), PROPERFMTARGS(req.size() * HeapWordSize));
    request_gc(cause);
  }

  if (block) {
    MonitorLocker ml(&_alloc_failure_waiters_lock);
    while (!should_terminate() && ShenandoahCollectorPolicy::is_allocation_failure(heap->cancelled_cause())) {
      ml.wait();
    }
  }
}

void ShenandoahController::handle_alloc_failure_evac(size_t words) {

  ShenandoahHeap* const heap = ShenandoahHeap::heap();
  const bool is_humongous = ShenandoahHeapRegion::requires_humongous(words);
  const GCCause::Cause cause = is_humongous ? GCCause::_shenandoah_humongous_allocation_failure : GCCause::_shenandoah_allocation_failure_evac;

  if (heap->cancel_gc(cause)) {
    log_info(gc)("Failed to allocate " PROPERFMT " for evacuation", PROPERFMTARGS(words * HeapWordSize));
  }
}

void ShenandoahController::notify_alloc_failure_waiters() {
  MonitorLocker ml(&_alloc_failure_waiters_lock);
  ml.notify_all();
}

#ifdef SVM
void ShenandoahController::svm_acquire_cycle_ownership() {
  // Called by a Control thread before it runs a cycle. Only contended with an inline GC on the VM
  // operation thread, so the wait below is bounded by that GC.
  while (Atomic::cmpxchg(&_svm_cycle_state, SVM_CYCLE_IDLE, SVM_CYCLE_CONTROL) != SVM_CYCLE_IDLE) {
    os::naked_short_sleep(1);
  }
}

void ShenandoahController::svm_release_cycle_ownership() {
  assert(Atomic::load(&_svm_cycle_state) == SVM_CYCLE_CONTROL, "must be owned by this thread and not parked");
  Atomic::store(&_svm_cycle_state, SVM_CYCLE_IDLE);
}

void ShenandoahController::svm_mark_cycle_parked() {
  assert(Thread::current() == (Thread*)this, "only the owning Control thread parks");
  assert(Atomic::load(&_svm_cycle_state) == SVM_CYCLE_CONTROL, "must own the cycle");
  Atomic::store(&_svm_cycle_state, SVM_CYCLE_CONTROL_PARKED);
}

void ShenandoahController::svm_unmark_cycle_parked() {
  // No competing write can happen here: the VM operation thread only takes the cycle over while this
  // thread waits, and a borrowed inline GC has handed the state back (VM_INLINE_BORROWED ->
  // CONTROL_PARKED) before the operation this thread waited for could execute.
  assert(Atomic::load(&_svm_cycle_state) == SVM_CYCLE_CONTROL_PARKED, "borrowed cycle must have been handed back");
  Atomic::store(&_svm_cycle_state, SVM_CYCLE_CONTROL);
}

bool ShenandoahController::svm_try_borrow_parked_cycle() {
  assert(Thread::current()->is_VM_thread(), "must be called on the VM operation thread");
  // Only a parked owner's cycle can be taken over: the owner enters CONTROL_PARKED before it waits
  // and leaves it after it returns, so observing that state here means it executes no GC code.
  // And only if the owner has no cycle open the inline GC opens and closes a cycle of its own,
  // and a GC must not run inside another thread's live cycle, whose phases, marking state and
  // generation bookkeeping it would invalidate.
  if (ShenandoahHeap::heap()->gc_cause() != GCCause::_no_gc) {
    return false;
  }
  if (Atomic::cmpxchg(&_svm_cycle_state, SVM_CYCLE_CONTROL_PARKED, SVM_CYCLE_VM_INLINE_BORROWED) != SVM_CYCLE_CONTROL_PARKED) {
    return false;
  }
  log_info(gc)("Running a GC inline on the VM operation thread, on the cycle of the parked Control thread");
  return true;
}

bool ShenandoahController::svm_inline_gc_in_progress() const {
  const SVMCycleState state = Atomic::load(&_svm_cycle_state);
  return state == SVM_CYCLE_VM_INLINE || state == SVM_CYCLE_VM_INLINE_BORROWED;
}

void ShenandoahController::svm_finish_inline_gc() {
  assert(Thread::current()->is_VM_thread(), "must be called on VM thread");
  // Deliver the waiter notifications this GC satisfies. The corresponding monitors must NOT be waited
  // for here because the VM thread may be executing an outer VM operation at a safepoint, and a mutator
  // frozen for that safepoint can hold a waiter monitor's mutex across its native->VM transition.
  // Blocking would deadlock, because only the VM thread can end the safepoint. Defer what cannot be delivered.
  const bool gc_waiters_notified = try_notify_gc_waiters();
  const bool alloc_waiters_notified = ShenandoahHeap::heap()->cancelled_gc() || try_notify_alloc_failure_waiters();
  if (!gc_waiters_notified || !alloc_waiters_notified) {
    Atomic::store(&_svm_pending_waiter_notify, 1);
  }
  // Publish completion BEFORE leaving the inline state, so a Control thread that subsequently
  // acquires the cycle reliably observes the increment.
  Atomic::inc(&_svm_inline_gc_count);
  const SVMCycleState state = Atomic::load(&_svm_cycle_state);
  if (state == SVM_CYCLE_VM_INLINE_BORROWED) {
    // Hand the cycle back to its still parked owner.
    Atomic::store(&_svm_cycle_state, SVM_CYCLE_CONTROL_PARKED);
  } else {
    assert(state == SVM_CYCLE_VM_INLINE, "must be");
    Atomic::store(&_svm_cycle_state, SVM_CYCLE_IDLE);
  }
}

void ShenandoahController::run_gc_on_vm_thread(GCCause::Cause cause) {
  assert(Thread::current()->is_VM_thread(), "must only be called by the VM operation thread");

  // If an inline GC on the VM thread triggered another GC request, e.g. an allocation
  // failure while executing the inline GC itself, another GC cannot help so drop the request.
  if (svm_inline_gc_in_progress()) {
    log_info(gc)("GC request (%s) on the VM operation thread ignored: an inline GC is already running on this thread",
                 GCCause::to_string(cause));
    return;
  }

  // Take the cycle if it is idle, or if its owner is a Control thread that is parked waiting for
  // a STW VM operation with no cycle opened yet, in which case only this thread can make progress
  // (see ShenandoahSVMParkedForVMOperationMark). Otherwise, hand the request to the running owner.
  if (Atomic::cmpxchg(&_svm_cycle_state, SVM_CYCLE_IDLE, SVM_CYCLE_VM_INLINE) != SVM_CYCLE_IDLE &&
      !svm_try_borrow_parked_cycle()) {
    svm_defer_gc_request(cause);
    return;
  }

  // A VM operation requires the thread to be in VM state. While executing a VM
  // operation the VM thread typically transitioned to native before calling into
  // the GC C++ code (e.g. via the allocation slow path), so transition back if
  // needed and restore the original state afterwards.
  IsolateThread* const ithread = IsolateThread::current();
  const bool in_native = ithread->has_status_native();
  if (in_native) {
    SVMGlobalData::_slow_transition_native_to_vm(ithread);
  }
  assert(ithread->has_status_vm(), "must be in VM state to execute a VM operation");

  ShenandoahHeap* const heap = ShenandoahHeap::heap();
  {
    // Cannot uncommit bitmap slices during the cycle.
    ShenandoahNoUncommitMark forbid_region_uncommit(heap);

    // GC is starting, bump the internal ID so threads waiting in handle_requested_gc()
    // observe that a cycle completed.
    update_gc_id();
    GCIdMark gc_id_mark;

    heap->reset_bytes_allocated_since_gc_start();
    heap->set_forced_counters_update(true);
    heap->free_set()->log_status_under_lock();

    // Run the mode-specific STW collection. The cycle ends up in VMThread::execute(), which runs
    // the VM operation inline when called by the VM operation thread.
    svm_run_inline_gc_cycle(cause);

    // Report current free set state at the end of cycle, whether it is a normal completion or an abort.
    heap->free_set()->log_status_under_lock();

    {
      // Notify Universe about new heap usage. This has implications for global soft refs policy, and
      // we better report it every time heap usage goes down.
      ShenandoahHeapLocker locker(heap->lock());
      heap->update_capacity_and_used_at_gc();
    }

    // Signal that we have completed a visit to all live objects.
    heap->record_whole_heap_examined_timestamp();

    // Disable forced counters update, and update counters one more time to capture the state at the
    // end of the GC session.
    heap->handle_force_counters_update();
    heap->set_forced_counters_update(false);

    // Retract forceful part of soft refs policy
    heap->soft_ref_policy()->set_should_clear_all_soft_refs(false);

    heap->process_gc_stats();
  }

  // Deliver the waiter notifications this GC satisfies, publish its completion and leave the
  // inline state (safepoint-safe, see svm_finish_inline_gc()).
  svm_finish_inline_gc();

  if (in_native) {
    SVMGlobalData::_transition_vm_to_native(ithread);
  }
}

void ShenandoahController::svm_defer_gc_request(GCCause::Cause cause) {
  // The VM thread can't run an inline GC because a Control thread triggered GC is mid-cycle and the
  // VM thread must not wait because the running GCs next STW phase is a VM operation that would deadlock.
  // Instead, depending on the GC request cause we do:
  //  - For allocation failures cancel the running cycle with the standard non-blocking mutator protocol.
  //    The Control thread unwinds and degenerates as soon as this VM operation completes. The current
  //    allocation may still fail, which is the correct outcome for a GC the cycle cannot satisfy.
  //  - For explicit requests notify the Control thread such that the next cycle serves it. Stock HotSpot
  //    treats GC requests on the VM thread similarly, see CollectedHeap::collect_as_vm_thread().
  if (ShenandoahCollectorPolicy::is_allocation_failure(cause)) {
    if (ShenandoahHeap::heap()->cancel_gc(cause)) {
      log_info(gc)("Failed to allocate on the VM operation thread during a cycle: cancelling the cycle");
    }
    // The cancelled cycle unwinds into a degenerated (or full) STW collection, which is a VM
    // operation - and the only thread that can execute VM operations is the VM theres, currently stuck
    // inside a VM operation whose allocation keeps failing. Worse, the VM operation thread holds
    // the VM operation queue mutex for the whole execution of the current operation, so the
    // Control thread cannot even enqueue the STW collection so without help the two threads
    // deadlock. Yield to the VM operation queue by openin a bounded window for the Control thread
    // to enqueue, and execute whatever arrives. A degenerated/full collection then runs inline on
    // the VM thread through its normal wrapper protocol.
    // The caller retries the allocation after each yield, so this loops via the allocation slow path
    // until the collection has happened and the allocation succeeds.
    SVMGlobalData::_yield_to_queued_vm_operations(CompressedOops::base(), IsolateThread::current(),
                                                  NANOSECS_PER_MILLISEC);
  } else {
    log_info(gc)("GC request (%s) on the VM operation thread during a cycle: deferring to the control thread",
                 GCCause::to_string(cause));
    svm_record_gc_request(cause);
  }
}

void ShenandoahController::svm_drain_pending_waiter_notifications() {
  if (Atomic::cmpxchg(&_svm_pending_waiter_notify, 1, 0) == 1) {
    // An inline GC could not deliver at least one notification, deliver both: spurious notifications
    // are harmless because all waiters re-check their conditions in a loop.
    notify_gc_waiters();
    notify_alloc_failure_waiters();
  }
}

bool ShenandoahController::try_notify_alloc_failure_waiters() {
  if (!_alloc_failure_waiters_lock.try_lock()) {
    return false;
  }
  _alloc_failure_waiters_lock.notify_all();
  _alloc_failure_waiters_lock.unlock();
  return true;
}
#endif // SVM

} // namespace svm_gc
