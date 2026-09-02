/*
 * Copyright (c) 2013, 2021, Red Hat, Inc. All rights reserved.
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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHCONTROLTHREAD_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHCONTROLTHREAD_HPP

#include "gc/shared/concurrentGCThread.hpp"
#include "gc/shared/gcCause.hpp"
#include "gc/shenandoah/shenandoahController.hpp"
#include "gc/shenandoah/shenandoahGC.hpp"
#include "gc/shenandoah/shenandoahPadding.hpp"
#include "gc/shenandoah/shenandoahSharedVariables.hpp"
#include "runtime/vmOperation.hpp"

namespace svm_gc {

class ShenandoahControlThread: public ShenandoahController {
  friend class VMStructs;

private:
  typedef enum {
    none,
    concurrent_normal,
    stw_degenerated,
    stw_full
  } GCMode;

  ShenandoahSharedFlag _gc_requested;
  GCCause::Cause       _requested_gc_cause;
  ShenandoahGC::ShenandoahDegenPoint _degen_point;

  // This lock is used to coordinate waking up the control thread
  Monitor _control_lock;

public:
  ShenandoahControlThread();

  void run_service() override;
  void stop_service() override;

  void request_gc(GCCause::Cause cause) override;

private:
  // Sets the requested cause and flag and notifies the control thread
  void notify_control_thread(GCCause::Cause cause);

  bool check_cancellation_or_degen(ShenandoahGC::ShenandoahDegenPoint point);
  void service_concurrent_normal_cycle(GCCause::Cause cause);
  void service_stw_full_cycle(GCCause::Cause cause);
  void service_stw_degenerated_cycle(GCCause::Cause cause, ShenandoahGC::ShenandoahDegenPoint point);

  void notify_gc_waiters();

  // Handle GC request.
  // Blocks until GC is over.
  void handle_requested_gc(GCCause::Cause cause);

#ifdef SVM
public:
  // Runs a STW full GC directly on the VM operation thread. In SVM the VM
  // operation thread is a Java thread that can allocate (and therefore trigger
  // a GC) while executing a VM operation. Because a STW GC is itself a VM
  // operation that must run on the VM operation thread, such a GC cannot be
  // delegated to the control thread (which might be blocked waiting for this
  // very thread). The GC is therefore executed inline and synchronously.
  //
  // Additionally, only one logical GC may be active at a time because GC state,
  // and the phase tracking in ShenandoahTimingsTracker::_current_phase, assume a
  // single owner. The control thread owns the cycle-ownership token (_svm_cycle_owner)
  // for the whole duration of every cycle it runs and this method try-acquires it.
  // If the token is unavailable because the control thread is in the middle of a
  // concurrent cycle, the GC cannot run inline and the request is handed to the
  // control thread asynchronously instead.
  void run_gc_on_vm_thread(GCCause::Cause cause) override;

private:
  enum SVMCycleOwner {
    SVM_CYCLE_IDLE = 0,
    SVM_CYCLE_OWNER_CONTROL_THREAD = 1,
    SVM_CYCLE_OWNER_VM_THREAD = 2
  };

  // Ownership token for the single logical GC. Owned by the control thread for
  // the whole duration of every cycle it runs, and by the VM operation thread
  // for the duration of an inline GC. A plain atomic (rather than a Mutex) is
  // used because the control thread must hold it across code that acquires
  // Heap_lock (with rank safepoint) inside VM_ShenandoahReferenceOperation::
  // doit_prologue(), which no Mutex rank above all in-cycle locks can express.
  volatile SVMCycleOwner _svm_cycle_owner;

  // Incremented by run_gc_on_vm_thread() for every completed inline GC before it releases
  // the ownership token. The control thread samples it at the top of its service loop and
  // re-checks after acquiring cycle ownership: a change means an inline GC ran in between,
  // invalidating the already-computed cycle decision like cancellation cause, degeneration
  // point, mark completeness, etc. which must then be re-evaluated.
  volatile size_t _svm_inline_gc_count;

  // Set when an inline GC on the VM operation thread could not deliver a waiter
  // notification (see try_notify_alloc_failure_waiters). Drained by the control thread,
  // which can block on the waiter monitors safely.
  volatile int _svm_pending_gc_waiters_notify;
  volatile int _svm_pending_alloc_failure_notify;

  // Try-lock variant of notify_gc_waiters(); see try_notify_alloc_failure_waiters().
  bool try_notify_gc_waiters();

  // Delivers any notification an inline GC had to defer.
  void svm_drain_pending_waiter_notifications();

public:
  // Blocking acquire/release for the control thread. The wait is bounded: the
  // only other owner is the VM operation thread running an inline STW GC.
  void svm_acquire_cycle_ownership();
  void svm_release_cycle_ownership();
#endif // SVM
};

} // namespace svm_gc

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHCONTROLTHREAD_HPP
