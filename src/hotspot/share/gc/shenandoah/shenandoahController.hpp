/*
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHCONTROLLER_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHCONTROLLER_HPP

#include "gc/shared/concurrentGCThread.hpp"
#include "gc/shared/gcCause.hpp"
#include "gc/shenandoah/shenandoahAllocRequest.hpp"
#include "gc/shenandoah/shenandoahSharedVariables.hpp"
#include "runtime/atomic.hpp"

namespace svm_gc {

/**
 * This interface exposes methods necessary for the heap to interact
 * with the threads responsible for driving the collection cycle.
 */
class ShenandoahController: public ConcurrentGCThread {
private:
  shenandoah_padding(0);
  volatile size_t _allocs_seen;
  shenandoah_padding(1);
  // A monotonically increasing GC count.
  volatile size_t _gc_id;
  shenandoah_padding(2);

protected:
  const Mutex::Rank WAITERS_LOCK_RANK = Mutex::safepoint - 5;
  const Mutex::Rank CONTROL_LOCK_RANK = Mutex::nosafepoint - 2;

  // While we could have a single lock for these, it may risk unblocking
  // GC waiters when alloc failure GC cycle finishes. We want instead
  // to make complete explicit cycle for demanding customers.
  Monitor _alloc_failure_waiters_lock;
  Monitor _gc_waiters_lock;

  // Increments the internal GC count.
  void update_gc_id();

public:
  ShenandoahController():
    _allocs_seen(0),
    _gc_id(0),
    _alloc_failure_waiters_lock(WAITERS_LOCK_RANK, "ShenandoahAllocFailureWaiters_lock", true),
    _gc_waiters_lock(WAITERS_LOCK_RANK, "ShenandoahGCWaiters_lock", true),
#ifdef SVM
    _svm_cycle_state(SVM_CYCLE_IDLE),
    _svm_inline_gc_count(0),
    _svm_pending_waiter_notify(0)
#endif // SVM
  { }

  // Request a collection cycle. This handles "explicit" gc requests
  // like System.gc and "implicit" gc requests, like metaspace oom.
  virtual void request_gc(GCCause::Cause cause) = 0;

  // This cancels the collection cycle and has an option to block
  // until another cycle completes successfully.
  void handle_alloc_failure(const ShenandoahAllocRequest& req, bool block);

  // Invoked for allocation failures during evacuation. This cancels
  // the collection cycle without blocking.
  void handle_alloc_failure_evac(size_t words);

  // Notify threads waiting for GC to complete.
  void notify_alloc_failure_waiters();
#ifdef SVM
  // Try-lock variant for the VM operation thread, which may run at a safepoint. Blocking on the
  // '_alloc_failure_waiters_lock' monitor can deadlock because a mutator at a safepoint can hold
  // the monitor's mutex across its native->VM transition.
  bool try_notify_alloc_failure_waiters();

  // Blocking notification of GC waiters, implemented by the concrete control threads.
  // Declared here for SVM such that svm_drain_pending_waiter_notifications() can deliver
  // deferred notifications.
  virtual void notify_gc_waiters() = 0;

  // Try-lock variant of notify_gc_waiters() (see try_notify_alloc_failure_waiters()).
  virtual bool try_notify_gc_waiters() = 0;

  // Runs a STW GC directly on the VM operation thread. In SVM the VM
  // operation thread is a Java thread that can allocate and therefore trigger
  // a GC while executing a VM operation. Because a STW GC is itself a VM
  // operation that must run on the VM operation thread, such a GC cannot be
  // delegated to the control thread which might be blocked waiting for the
  // VM thread. Instead the GC is executed inline and synchronously. This is
  // the shared skeleton, the mode-specific cycle is run by svm_run_inline_gc_cycle().
  void run_gc_on_vm_thread(GCCause::Cause cause);

  // Runs the mode-specific STW cycle of an inline GC.
  virtual void svm_run_inline_gc_cycle(GCCause::Cause cause) = 0;

  /*
   * All states of the single logical GC in one variable. A plain atomic is used rather than a Mutex
   * because the Control thread must hold the CONTROL states across code that acquires the Heap_lock
   * with rank safepoint inside VM_ShenandoahReferenceOperation::doit_prologue(), which no Mutex rank
   * above all in-cycle locks can express.
   *
   *          (control thread            (control thread waits for
   *           runs a cycle)              its STW VM operation)
   *   IDLE <----------------> CONTROL <------------------------> CONTROL_PARKED
   *    ^                                                            ^
   *    | (VM operation thread runs                (VM operation thread runs a GC
   *    |  a GC inline for itself)                  inline on the parked owner's cycle)
   *    v                                                            v
   *   VM_INLINE                                             VM_INLINE_BORROWED
   */
  enum SVMCycleState {
    SVM_CYCLE_IDLE,               // no cycle
    SVM_CYCLE_CONTROL,            // a Control thread owns the cycle and is running
    SVM_CYCLE_CONTROL_PARKED,     // the owning Control thread waits for a STW VM operation
    SVM_CYCLE_VM_INLINE,          // the VM operation thread runs a GC inline for itself
    SVM_CYCLE_VM_INLINE_BORROWED  // ... on the cycle taken over from a parked Control thread
  };
  volatile SVMCycleState _svm_cycle_state;

  // Incremented for every completed inline GC before the state leaves VM_INLINE*. A change
  // invalidates a cycle decision computed before: the inline GC cleared the cancellation and reset
  // the marking state (see run_service() and ShenandoahDegenGC::op_degenerated()).
  volatile size_t _svm_inline_gc_count;

  // Set when an inline GC on the VM operation thread could not deliver a waiter notification
  // (see try_notify_{gc,alloc_failure}_waiters()). Drained by the Control thread, which can
  // safely block on the waiter monitors.
  volatile int _svm_pending_waiter_notify;

  friend class ShenandoahSVMCycleOwnershipMark;
  friend class ShenandoahSVMParkedForVMOperationMark;

  // Cycle ownership for a Control thread (used via ShenandoahSVMCycleOwnershipMark). The acquisition
  // wait is bounded because the only other owner is the VM operation thread running an inline STW GC.
  void svm_acquire_cycle_ownership();
  void svm_release_cycle_ownership();

  // The window in which the owning Control thread waits for a STW VM operation (used via
  // ShenandoahSVMParkedForVMOperationMark, which VMThread::execute() creates).
  void svm_mark_cycle_parked();
  void svm_unmark_cycle_parked();

  // Takes the cycle over from an owner that is parked waiting for a STW VM operation, so that the
  // VM operation thread can run a GC inline. Only possible while no cycle is open, see
  // ShenandoahSVMParkedForVMOperationMark. svm_finish_inline_gc() hands the cycle back.
  bool svm_try_borrow_parked_cycle();

  // True if the VM operation thread currently runs an inline GC.
  bool svm_inline_gc_in_progress() const;

  // Called by the VM thread to publish an inline GC's completion and leave the VM_INLINE* state.
  void svm_finish_inline_gc();

  // Number of inline GCs the VM operation thread has completed. A change invalidates a cycle
  // decision that was computed before, see ShenandoahDegenGC::op_degenerated().
  size_t svm_inline_gc_count() const { return Atomic::load(&_svm_inline_gc_count); }

  // Hands a GC request that cannot run inline on the VM thread to the Control thread.
  void svm_defer_gc_request(GCCause::Cause cause);

  // Delivers any notification an inline GC had to defer while running on the VM thread.
  void svm_drain_pending_waiter_notifications();

  // Records a GC request for the Control thread WITHOUT waiting for it  because waiting is not
  // allowed on the VM thread. The generational Control thread also needs a generation recorded
  // here, hence the per-controller implementation.
  virtual void svm_record_gc_request(GCCause::Cause cause) = 0;
#endif // SVM

  // This is called for every allocation. The control thread accumulates
  // this value when idle. During the gc cycle, the control resets it
  // and reports it to the pacer.
  void pacing_notify_alloc(size_t words);

  // Zeros out the number of allocations seen since the last GC cycle.
  size_t reset_allocs_seen();

  // Return the value of a monotonic increasing GC count, maintained by the control thread.
  size_t get_gc_id();
};


#ifdef SVM
/*
 * Marks the window in which a Control thread waits for a STW VM operation that it handed to the VM
 * operation thread. While the owner waits, the VM operation thread may take the cycle over to run a
 * GC inline for itself (svm_try_borrow_parked_cycle()). Created by VMThread::execute() for every VM
 * operation a Control thread executes, so no GC call site needs to be instrumented.
 *
 * This is needed to make progress because in SVM the VM thread is a Java thread that can allocate
 * while executing a VM operation, and the Control thread cannot get any STW work executed until the VM
 * operation thread's current operation completes, while that operation may in turn be waiting for the
 * memory that only a GC can provide. Without taking the cycle over, both sides wait for each other.
 *
 * Taking the cycle over is safe precisely because the owner is parked and executes no GC code while
 * waiting, and the operation it waits for cannot run before the VM operation thread returns to its
 * operation loop. The inline GC and the owner's pending operation therefore never overlap, they are
 * serialized on the VM operation thread. It is restricted to owners that have no cycle open (see
 * svm_try_borrow_parked_cycle()). The STW cycles open theirs on the VM operation thread inside the
 * operation, so an owner parked for a degenerated or full GC holds none, whereas an owner parked for
 * a STW phase of a concurrent cycle does, and a second cycle must not be started underneath it.
 */
class ShenandoahSVMParkedForVMOperationMark : public StackObj {
private:
  ShenandoahController* const _controller;
  const bool                  _active;

public:
  ShenandoahSVMParkedForVMOperationMark(ShenandoahController* controller) :
    _controller(controller),
    // Only the Control thread parks, and only while it owns the cycle (it can also execute VM
    // operations outside of a cycle, e.g. handshake fallbacks). Reading the state without further
    // synchronization is safe because while this thread owns the cycle, no other thread writes the state.
    _active(controller != nullptr && Thread::current() == (Thread*)controller &&
            Atomic::load(&controller->_svm_cycle_state) == ShenandoahController::SVM_CYCLE_CONTROL) {
    if (_active) {
      _controller->svm_mark_cycle_parked();
    }
  }

  ~ShenandoahSVMParkedForVMOperationMark() {
    if (_active) {
      _controller->svm_unmark_cycle_parked();
    }
  }
};

// The Control thread owns the single logical GC for the duration of a full cycle.
class ShenandoahSVMCycleOwnershipMark : public StackObj {
  ShenandoahController* const _controller;
public:
  ShenandoahSVMCycleOwnershipMark(ShenandoahController* controller) : _controller(controller) {
    _controller->svm_acquire_cycle_ownership();
  }
  ~ShenandoahSVMCycleOwnershipMark() {
    _controller->svm_release_cycle_ownership();
  }
};
#endif // SVM

} // namespace svm_gc

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHCONTROLLER_HPP
