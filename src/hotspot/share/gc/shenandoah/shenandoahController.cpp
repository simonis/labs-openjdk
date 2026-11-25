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

#include "gc/shared/gc_globals.hpp"
#include "gc/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc/shenandoah/shenandoahController.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.inline.hpp"
#ifdef SVM
#include "gc/shared/suspendibleThreadSet.hpp"
#include "runtime/vmThread.hpp"
#include "svmGlobalData.hpp"
#endif // SVM

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
    svm_handle_alloc_failure_on_vm_thread(req, cause, block);
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
void ShenandoahController::svm_handle_alloc_failure_on_vm_thread(const ShenandoahAllocRequest& req, GCCause::Cause cause, bool block) {
  // We can not get here while executing a native GC VM operation because the native code that runs
  // inside a GC operation and the uninterruptible Java code it calls does not allocate in the Java heap.
  // Reaching here during the execution of a STW GC operation would deadlock the Shenandoah Control and
  // VM thread and is therefore considered a bug.
  guarantee(VMThread::vm_operation() == nullptr,
            "Java heap allocation of %s, " PROPERFMT " on the VM operation thread inside GC operation %d",
            req.type_string(), PROPERFMTARGS(req.size() * HeapWordSize), (int) VMThread::vm_operation()->type());

  // Like a mutator (see handle_alloc_failure()), cancel the cycle and request a GC.
  // For the VM thread, request_gc() only records the request and never waits.
  ShenandoahHeap* const heap = ShenandoahHeap::heap();
  if (heap->cancel_gc(cause)) {
    log_info(gc)("Failed to allocate %s, " PROPERFMT " (VM operation thread)", req.type_string(), PROPERFMTARGS(req.size() * HeapWordSize));
    request_gc(cause);
  }

  if (!block) {
    return;
  }

  // Wait like a mutator until the allocation failure was handled. Unlike a mutator, this thread
  // must not block because the STW operations the Control thread needs to handle the allocation
  // failure (e.g. a degenerated or full GC) can only be executed by the VM thread. The Control
  // thread can't even queue those operations, because the VM thread holds the VM operation queue
  // lock while it already executes a VM operation. So the Control thread simply hands its operations
  // over and the VM thread (i.e. this thread) executes them as nested VM operations while it waits.
  //
  // If the VM thread already executes a VM operation at a safepoint, the safepoint also suspended the
  // GC threads (see ShenandoahHeap::safepoint_synchronize_begin()). The Control thread then cannot finish
  // its cancelled concurrent phase and never reaches the STW operation it would hand over. So let the GC
  // threads run while the VM thread waits (all Java threads remain stopped at the safepoint), and suspend
  // them again for the duration of every handed-over STW operation and before returning.
  const bool release_gc_threads = SuspendibleThreadSet::should_yield();
  if (release_gc_threads) {
    SuspendibleThreadSet::desynchronize();
  }
  address const heap_base = CompressedOops::base();
  IsolateThread* const thread = IsolateThread::current();
  while (!should_terminate() && ShenandoahCollectorPolicy::is_allocation_failure(heap->cancelled_cause())) {
    if (SVMGlobalData::_wait_for_handed_over_gc_operation(heap_base, thread, NANOSECS_PER_MILLISEC)) {
      if (release_gc_threads) {
        SuspendibleThreadSet::synchronize();
      }
      SVMGlobalData::_execute_handed_over_gc_operation(heap_base, thread);
      if (release_gc_threads) {
        SuspendibleThreadSet::desynchronize();
      }
    }
  }
  if (release_gc_threads) {
    SuspendibleThreadSet::synchronize();
  }
}
#endif // SVM

} // namespace svm_gc
