/*
 * Copyright (c) 2021, 2022, Red Hat, Inc. All rights reserved.
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



#include "gc/shenandoah/shenandoahBarrierSet.hpp"
#include "gc/shenandoah/shenandoahClosures.inline.hpp"
#include "gc/shenandoah/shenandoahGeneration.hpp"
#include "gc/shenandoah/shenandoahMark.inline.hpp"
#include "gc/shenandoah/shenandoahReferenceProcessor.hpp"
#include "gc/shenandoah/shenandoahTaskqueue.inline.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "gc/shenandoah/shenandoahVerifier.hpp"
#ifdef SVM
#include "code/codeCache.hpp"
#include "code/nmethod.hpp"
#include "gc/shared/workerThread.hpp"
#include "gc/shenandoah/shenandoahRootProcessor.hpp"
#include "runtime/mutexLocker.hpp"
#include "svmHeapAddressSpace.hpp"
#endif // SVM

namespace svm_gc {

void ShenandoahMark::start_mark() {
  if (!CodeCache::is_gc_marking_cycle_active()) {
    CodeCache::on_gc_marking_cycle_start();
  }
}

void ShenandoahMark::end_mark() {
  // Unlike other GCs, we do not arm the nmethods
  // when marking terminates.
  if (!ShenandoahHeap::heap()->is_concurrent_old_mark_in_progress()) {
    CodeCache::on_gc_marking_cycle_finish();
  }
}

ShenandoahMark::ShenandoahMark(ShenandoahGeneration* generation) :
  _generation(generation),
  _task_queues(generation->task_queues()),
  _old_gen_task_queues(generation->old_gen_task_queues()) {
}

template <ShenandoahGenerationType GENERATION, bool CANCELLABLE, StringDedupMode STRING_DEDUP>
void ShenandoahMark::mark_loop_prework(uint w, TaskTerminator *t, ShenandoahReferenceProcessor *rp, StringDedup::Requests* const req, bool update_refs) {
  ShenandoahObjToScanQueue* q = get_queue(w);
  ShenandoahObjToScanQueue* old_q = get_old_queue(w);

  ShenandoahHeap* const heap = ShenandoahHeap::heap();
  ShenandoahLiveData* ld = heap->get_liveness_cache(w);

  // TODO: We can clean up this if we figure out how to do templated oop closures that
  // play nice with specialized_oop_iterators.
  if (update_refs) {
    using Closure = ShenandoahMarkUpdateRefsClosure<GENERATION>;
    Closure cl(q, rp, old_q);
    mark_loop_work<Closure, GENERATION, CANCELLABLE, STRING_DEDUP>(&cl, ld, w, t, req);
  } else {
    using Closure = ShenandoahMarkRefsClosure<GENERATION>;
    Closure cl(q, rp, old_q);
    mark_loop_work<Closure, GENERATION, CANCELLABLE, STRING_DEDUP>(&cl, ld, w, t, req);
  }

  heap->flush_liveness_cache(w);
}

template<bool CANCELLABLE, StringDedupMode STRING_DEDUP>
void ShenandoahMark::mark_loop(uint worker_id, TaskTerminator* terminator, ShenandoahReferenceProcessor *rp,
                               ShenandoahGenerationType generation, StringDedup::Requests* const req) {
  bool update_refs = ShenandoahHeap::heap()->has_forwarded_objects();
  switch (generation) {
    case YOUNG:
      mark_loop_prework<YOUNG, CANCELLABLE, STRING_DEDUP>(worker_id, terminator, rp, req, update_refs);
      break;
    case OLD:
      // Old generation collection only performs marking, it should not update references.
      mark_loop_prework<OLD, CANCELLABLE, STRING_DEDUP>(worker_id, terminator, rp, req, false);
      break;
    case GLOBAL:
      mark_loop_prework<GLOBAL, CANCELLABLE, STRING_DEDUP>(worker_id, terminator, rp, req, update_refs);
      break;
    case NON_GEN:
      mark_loop_prework<NON_GEN, CANCELLABLE, STRING_DEDUP>(worker_id, terminator, rp, req, update_refs);
      break;
    default:
      ShouldNotReachHere();
      break;
  }
}

void ShenandoahMark::mark_loop(uint worker_id, TaskTerminator* terminator, ShenandoahReferenceProcessor *rp,
                               ShenandoahGenerationType generation, bool cancellable, StringDedupMode dedup_mode, StringDedup::Requests* const req) {
  if (cancellable) {
    switch(dedup_mode) {
      case NO_DEDUP:
        mark_loop<true, NO_DEDUP>(worker_id, terminator, rp, generation, req);
        break;
      case ENQUEUE_DEDUP:
        mark_loop<true, ENQUEUE_DEDUP>(worker_id, terminator, rp, generation, req);
        break;
      case ALWAYS_DEDUP:
        mark_loop<true, ALWAYS_DEDUP>(worker_id, terminator, rp, generation, req);
        break;
    }
  } else {
    switch(dedup_mode) {
      case NO_DEDUP:
        mark_loop<false, NO_DEDUP>(worker_id, terminator, rp, generation, req);
        break;
      case ENQUEUE_DEDUP:
        mark_loop<false, ENQUEUE_DEDUP>(worker_id, terminator, rp, generation, req);
        break;
      case ALWAYS_DEDUP:
        mark_loop<false, ALWAYS_DEDUP>(worker_id, terminator, rp, generation, req);
        break;
    }
  }
}

template <class T, ShenandoahGenerationType GENERATION, bool CANCELLABLE, StringDedupMode STRING_DEDUP>
void ShenandoahMark::mark_loop_work(T* cl, ShenandoahLiveData* live_data, uint worker_id, TaskTerminator *terminator, StringDedup::Requests* const req) {
  uintx stride = ShenandoahMarkLoopStride;

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ShenandoahObjToScanQueueSet* queues = task_queues();
  ShenandoahObjToScanQueue* q;
  ShenandoahMarkTask t;

  // Do not use active_generation() : we must use the gc_generation() set by
  // ShenandoahGCScope on the ControllerThread's stack; no safepoint may
  // intervene to update active_generation, so we can't
  // shenandoah_assert_generations_reconciled() here.
  assert(heap->gc_generation()->type() == GENERATION, "Sanity: %d != %d", heap->gc_generation()->type(), GENERATION);
  heap->gc_generation()->ref_processor()->set_mark_closure(worker_id, cl);

  /*
   * Process outstanding queues, if any.
   *
   * There can be more queues than workers. To deal with the imbalance, we claim
   * extra queues first. Since marking can push new tasks into the queue associated
   * with this worker id, we come back to process this queue in the normal loop.
   */
  assert(queues->get_reserved() == heap->workers()->active_workers(),
         "Need to reserve proper number of queues: reserved: %u, active: %u", queues->get_reserved(), heap->workers()->active_workers());

  q = queues->claim_next();
  while (q != nullptr) {
    if (CANCELLABLE && heap->check_cancelled_gc_and_yield()) {
      return;
    }

    for (uint i = 0; i < stride; i++) {
      if (q->pop(t)) {
        do_task<T, GENERATION, STRING_DEDUP>(q, cl, live_data, req, &t, worker_id);
      } else {
        assert(q->is_empty(), "Must be empty");
        q = queues->claim_next();
        break;
      }
    }
  }
  q = get_queue(worker_id);
  ShenandoahObjToScanQueue* old_q = get_old_queue(worker_id);

  ShenandoahSATBBufferClosure<GENERATION> drain_satb(q, old_q);
  SATBMarkQueueSet& satb_mq_set = ShenandoahBarrierSet::satb_mark_queue_set();

  /*
   * Normal marking loop:
   */
  while (true) {
    if (CANCELLABLE && heap->check_cancelled_gc_and_yield()) {
      return;
    }
    while (satb_mq_set.completed_buffers_num() > 0) {
      satb_mq_set.apply_closure_to_completed_buffer(&drain_satb);
    }

    uint work = 0;
    for (uint i = 0; i < stride; i++) {
      if (q->pop(t) ||
          queues->steal(worker_id, t)) {
        do_task<T, GENERATION, STRING_DEDUP>(q, cl, live_data, req, &t, worker_id);
        work++;
      } else {
        break;
      }
    }

    if (work == 0) {
      // No work encountered in current stride, try to terminate.
      // Need to leave the STS here otherwise it might block safepoints.
      ShenandoahSuspendibleThreadSetLeaver stsl(CANCELLABLE);
      ShenandoahTerminatorTerminator tt(heap);
      if (terminator->offer_termination(&tt)) return;
    }
  }
}

#ifdef SVM
// Liveness of the objects that are referenced from nmethods, from the perspective of the
// generation that is marked: objects outside of it, as well as image heap and metaspace objects,
// are always live.
class ShenandoahSVMNMethodIsAliveClosure : public BoolObjectClosure {
private:
  ShenandoahHeap* const           _heap;
  ShenandoahMarkingContext* const _mark_context;

public:
  ShenandoahSVMNMethodIsAliveClosure() :
    _heap(ShenandoahHeap::heap()),
    _mark_context(ShenandoahHeap::heap()->marking_context()) {}

  bool do_object_b(oop obj) override {
    return SVMHeapAddressSpace::is_image_heap_or_metaspace_object(obj) ||
           !_heap->is_in_active_generation(obj) ||
           _mark_context->is_marked(obj);
  }
};

// NOTE: similar to G1ConditionalMarkCodeCacheClosure
//
// Code that is still callable is never unloaded: an nmethod is only unloaded if its tether is
// unreachable (the tether is reachable while the code can be entered or is on the stack) and if
// it is non-entrant or (with TreatRuntimeCodeInfoReferencesAsWeak) references an otherwise
// unreachable object. The GC only sets the state of the unloaded nmethods, they are invalidated and
// freed later during the same safepoint (see ShenandoahHeap::svm_clean_runtime_code_cache()).
//
// With 'remark' (i.e., at the end of concurrent marking), all oops of the unloaded nmethods are
// marked as well, so that the SATB mark bitmap stays consistent: the mutator may have copied
// nmethod oops into the Java heap (e.g., during deoptimization) without a pre-write barrier, so all
// nmethod oops must be treated as live from the start of marking.
class ShenandoahSVMConditionalMarkCodeCacheClosure : public NMethodClosure {
private:
  OopClosure* const        _mark_cl;
  BoolObjectClosure* const _is_alive;
  const bool               _remark;

  void mark_unloaded(nmethod* nm, nmethod::NMethodState new_state) {
    if (_remark) {
      nm->oops_do(_mark_cl);
    } else {
      // The object fields are needed to unload the nmethod.
      nm->oops_do_object_fields(_mark_cl);
    }
    // It is necessary to unregister all nmethods that will be freed during the current safepoint
    // right away. Later on, either the nmethod state would be incorrect or the oops would no longer
    // be valid (see G1ConditionalMarkCodeCacheClosure::unregister_nmethod()).
    {
      MutexLocker ml(StrongCodeRoots_lock, Mutex::_no_safepoint_check_flag);
      ShenandoahHeap::heap()->unregister_nmethod(nm);
    }
    nm->set_state(new_state);
    ShenandoahHeap::heap()->svm_set_has_unloaded_nmethods();
  }

public:
  ShenandoahSVMConditionalMarkCodeCacheClosure(OopClosure* mark_cl, BoolObjectClosure* is_alive, bool remark) :
    _mark_cl(mark_cl), _is_alive(is_alive), _remark(remark) {}

  void do_nmethod(nmethod* nm) override {
    if (nm->are_all_objects_in_image_heap()) {
      return;
    }

    oop tether = nm->tether();
    if (tether != nullptr && !_is_alive->do_object_b(tether)) {
      nmethod::NMethodState state = nm->state();
      if (state == nmethod::state_removed_from_code_cache) {
        mark_unloaded(nm, nmethod::state_pending_free);
        return;
      }

      if (state == nmethod::state_non_entrant ||
          (TreatRuntimeCodeInfoReferencesAsWeak && state == nmethod::state_code_constants_live && nm->has_weak_reference_to_unreachable_object(_is_alive))) {
        mark_unloaded(nm, nmethod::state_pending_removal_from_code_cache);
        return;
      }
    }

    nm->oops_do(_mark_cl);
  }
};

template <ShenandoahGenerationType GENERATION>
void ShenandoahMark::svm_mark_code_cache_work(uint worker_id, ShenandoahSVMCodeCacheRoots* all_code_roots, ShenandoahSVMRegionCodeRoots* young_code_roots,
                                              TaskTerminator* terminator, bool remark) {
  ShenandoahReferenceProcessor* rp = _generation->ref_processor();
  {
    ShenandoahMarkRefsClosure<GENERATION> mark_cl(get_queue(worker_id), rp, get_old_queue(worker_id));
    if (GENERATION == YOUNG) {
      // All oops of these nmethods are marked, also the weakly referenced ones, which keeps the SATB
      // mark bitmap consistent (see ShenandoahSVMConditionalMarkCodeCacheClosure).
      NMethodToOopClosure code_cl(&mark_cl, !NMethodToOopClosure::FixRelocations);
      young_code_roots->nmethods_do(&code_cl);
    } else {
      ShenandoahSVMNMethodIsAliveClosure is_alive;
      ShenandoahSVMConditionalMarkCodeCacheClosure code_cl(&mark_cl, &is_alive, remark);
      all_code_roots->nmethods_do(&code_cl);
    }
  }

  // Mark everything that is reachable from the nmethods.
  StringDedup::Requests requests;
  mark_loop(worker_id, terminator, rp, GENERATION, false /* not cancellable */, NO_DEDUP, &requests);
}

template <ShenandoahGenerationType GENERATION>
class ShenandoahSVMMarkCodeCacheTask : public WorkerTask {
private:
  ShenandoahMark* const        _mark;
  ShenandoahSVMCodeCacheRoots  _all_code_roots;
  ShenandoahSVMRegionCodeRoots _young_code_roots;
  TaskTerminator               _terminator;
  const bool                   _remark;

public:
  ShenandoahSVMMarkCodeCacheTask(ShenandoahMark* mark, uint nworkers, bool remark) :
    WorkerTask("Shenandoah SVM Mark Code Cache"),
    _mark(mark),
    _all_code_roots(ShenandoahPhaseTimings::finish_mark),
    _young_code_roots(ShenandoahPhaseTimings::finish_mark, ShenandoahSVMRegionCodeRoots::YOUNG),
    _terminator(nworkers, mark->task_queues()),
    _remark(remark) {}

  void work(uint worker_id) override {
    ShenandoahParallelWorkerSession worker_session(worker_id);
    _mark->svm_mark_code_cache_work<GENERATION>(worker_id, &_all_code_roots, &_young_code_roots, &_terminator, _remark);
  }
};

void ShenandoahMark::svm_mark_code_cache(bool remark) {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
  ShenandoahHeap* const heap = ShenandoahHeap::heap();
  if (CodeCache::code_info_table() == nullptr) {
    return;
  }

  uint nworkers = heap->workers()->active_workers();
  task_queues()->reserve(nworkers);
  switch (_generation->type()) {
    case YOUNG: {
      ShenandoahSVMMarkCodeCacheTask<YOUNG> task(this, nworkers, remark);
      heap->workers()->run_task(&task);
      break;
    }
    case OLD: {
      ShenandoahSVMMarkCodeCacheTask<OLD> task(this, nworkers, remark);
      heap->workers()->run_task(&task);
      break;
    }
    case GLOBAL: {
      ShenandoahSVMMarkCodeCacheTask<GLOBAL> task(this, nworkers, remark);
      heap->workers()->run_task(&task);
      break;
    }
    case NON_GEN: {
      ShenandoahSVMMarkCodeCacheTask<NON_GEN> task(this, nworkers, remark);
      heap->workers()->run_task(&task);
      break;
    }
    default:
      ShouldNotReachHere();
  }
  assert(task_queues()->is_empty(), "Should be empty");
}
#endif // SVM

} // namespace svm_gc
