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

namespace svm_gc {

#ifdef SVM
/*
 * Control flow between the threads that take part in a GC on SubstrateVM
 * ========================================================================
 *
 *   J  Java application threads (mutators). Allocate, execute barriers, stop at safepoints.
 *   V  SubstrateVM VM operation thread. A Java thread that executes all VM operations,
 *      including the GC STW operations of Shenandoah. It holds the VM operation queue mutex M
 *      for the whole execution of a VM operation and is the only thread that starts safepoints. It
 *      can allocate (and fail to allocate) in the Java heap inside non-GC VM operations.
 *   C  Shenandoah Control thread (native, not attached). The only thread that drives GC cycles.
 *   W  Native Shenandoah GC worker threads. They run the parallel parts of the concurrent phases
 *      (joined to the SuspendibleThreadSet, STS) and the STW operations (WorkerThreads::run_task()).
 *
 * Every SVM safepoint synchronizes the STS (see ShenandoahHeap::safepoint_synchronize_begin()), so W
 * is suspended in its concurrent phases until the safepoint ends. C itself is not part of the STS.
 *
 * 1. A STW operation of a cycle (init/final mark, init/final update refs, final roots,
 *    degenerated GC, full GC, handshake fallback) while V is idle. See VMThread::execute()
 *    and NativeGCVMOperationSupport / ShenandoahVMOperations on the SVM side.
 *
 *    C                                    V                                   J, W
 *    | VMThread::execute(op)              | waits for work (M released)       |
 *    |-- tryEnqueue (try-lock M) -------->|                                   |
 *    | wait for EXECUTE_PROLOGUE          | wrapper operation:                |
 *    |<-----------------------------------| status EXECUTE_PROLOGUE, wait     |
 *    | doit_prologue() (Heap_lock)        |                                   |
 *    |-- status EXECUTE_VM_OPERATION ---->| start safepoint ----------------->| J stop, W suspended
 *    | wait for ADJUST_THREAD_STATUS      | op->doit() (parallel parts in W)  |
 *    |                                    | end safepoint ------------------->| J, W resume
 *    |<-----------------------------------| status ADJUST_THREAD_STATUS, wait |
 *    |-- status FINISHED ---------------->| wrapper finished                  |
 *    | doit_epilogue(), wait for finished |                                   |
 *
 *    If V executes another VM operation (holds M), the try-lock fails and C waits up to 1 ms
 *    before it tries again (case 5 is the exception). C never blocks on M.
 *
 * 2. Allocation failure of J (handle_alloc_failure()): J cancels the cycle, requests a GC and blocks
 *    on _alloc_failure_waiters_lock. C sees the cancellation, the concurrent phase unwinds (W bail
 *    out), C runs a degenerated or full GC (case 1) and notifies J, which retries the allocation.
 *
 * 3. Explicit GC from J (e.g. System.gc()): J requests a GC and blocks on _gc_waiters_lock until a
 *    complete cycle ran (handle_requested_gc()). C runs the cycle (case 1 for each of its STW operations).
 *
 * 4. Explicit GC from V (e.g. System.gc() inside a VM operation): V only records the request
 *    (svm_record_gc_request()) and returns without waiting, because the STW operations need V. C runs the
 *    cycle after V finished its VM operation.
 *
 * 5. Allocation failure of V inside a VM operation (svm_handle_alloc_failure_on_vm_thread()): V
 *    cancels the cycle and requests a GC like J. It can't block like J, because only V can execute
 *    the STW operations C needs, and C can't even queue them while V holds M. So V waits in a loop
 *    and executes the operations that C hands over:
 *
 *    C                                    V                                   J, W
 *    | concurrent phase (in W)            | VM operation, allocation fails    |
 *    |                                    | cancel_gc(), request_gc()         |
 *    |                                    | [STS desynchronize] ------------->| W resume, J stay stopped
 *    | sees the cancellation,             | wait for a hand-over (1 ms)       |
 *    | phase unwinds (W bail out)         |   ... check cancellation, wait    |
 *    | VMThread::execute(degenerated op)  |                                   |
 *    | tryEnqueue fails (V holds M)       |                                   |
 *    |-- hand over (V waits) ------------>| [STS synchronize] --------------->| W suspended
 *    | prologue / epilogue as in case 1   | wrapper operation as in case 1,   |
 *    |                                    | at V's current safepoint or at a  |
 *    |                                    | new one. The degenerated (or full)|
 *    |                                    | GC clears the cancellation.       |
 *    |                                    | [STS desynchronize] ------------->| W resume
 *    | notifies waiting threads           | loop ends: allocation failure was |
 *    |                                    | handled                           |
 *    |                                    | [STS synchronize] --------------->| W suspended
 *    |                                    | retry the allocation              |
 *
 *    The steps in [...] only happen if V executes its VM operation at a safepoint. That safepoint
 *    suspended W, so the cancelled phase could not unwind and C would never reach the operation it
 *    hands over. C must not wait for anything else that V's safepoint holds; e.g. C changes the
 *    gc state concurrently without a lock (see ShenandoahHeap::set_gc_state_concurrent()).
 *    Handshakes that C needs before it reaches its degenerated GC are handed over the same way.
 *    A full GC that a degenerated GC upgrades to is just another handed-over operation.
 *
 * 6. Allocation failure of V inside a GC STW operation: not allowed (guarantee()). C waits for this
 *    very operation, and the code inside it is uninterruptible and does not allocate in the heap.
 *
 * 7. Evacuation failure of any thread (handle_alloc_failure_evac()): cancels the cycle without
 *    blocking. C continues with a degenerated GC as in case 2.
 */
#endif // SVM

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
    _gc_waiters_lock(WAITERS_LOCK_RANK, "ShenandoahGCWaiters_lock", true)
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
  // In SVM, the VM operation thread is a Java thread that can allocate (and fail to allocate) while
  // it executes a VM operation. It handles an allocation failure like a mutator: it cancels the cycle,
  // requests a GC from the Control thread and waits until the allocation failure was handled. Only the
  // Control thread drives GC cycles, but their STW operations can only be executed by the VM operation
  // thread, so while it waits, the VM thread executes the GC operations the Control thread hands over
  // to it as nested VM operations.
  void svm_handle_alloc_failure_on_vm_thread(const ShenandoahAllocRequest& req, GCCause::Cause cause, bool block);

  // Records a GC request of the VM operation thread for the Control thread without waiting for it,
  // like HotSpot does for GC requests of its VM thread (see CollectedHeap::collect_as_vm_thread()).
  // The generational Control thread also needs a generation recorded here, hence the per-controller
  // implementation.
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

} // namespace svm_gc

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHCONTROLLER_HPP
