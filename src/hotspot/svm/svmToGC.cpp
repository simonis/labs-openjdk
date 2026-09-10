/*
 * Copyright (c) 2019, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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
 */

#include "svmToGC.hpp"
#include "svmOopMap.hpp"
#include "ci/ciUtilities.hpp"
#include "code/nmethod.hpp"
#include "exports/sharedGCStructs.h"
#include "exports/shenandoahGCStructs.h"
#include "gc/shared/cardTable.hpp"
#include "gc/shared/collectedHeap.inline.hpp"
#include "gc/shared/gcArguments.hpp"
#include "gc/shared/gcCause.hpp"
#include "gc/shenandoah/shenandoahBarrierSet.inline.hpp"
#include "logging/logConfiguration.hpp"
#include "oops/arrayKlass.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/instanceStackChunkKlass.hpp"
#include "oops/instancePodKlass.hpp"
#include "oops/objArrayKlass.hpp"
#include "oops/objArrayOop.hpp"
#include "oops/oop.hpp"
#include "oops/podOop.hpp"
#include "oops/typeArrayKlass.hpp"
#include "oops/typeArrayOop.hpp"
#include "oops/typeArrayOop.inline.hpp"
#include "runtime/arguments.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/threads.hpp"
#include "runtime/vmOperations.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/defaultStream.hpp"
#include "utilities/vmError.hpp"

/*
 * NOTE (chaeubl):  When SVM calls any of the exported methods below, it is essential that the correct kind of transition is used for the call. Otherwise,
 * we will end up with either deadlocks or wrong behavior.
 *
 * Possible transitions:
 * - NO_TRANSITION: Similar to @Uninterruptible as the thread will remain in STATUS_JAVA. The C++ code must not block and it must not call back to Java.
 *   It is guaranteed that the C++ code won't be interrupted by a safepoint.
 *
 * - TO_VM: This is the most flexible mode. While the thread is in C++ code, safepoints are prevent. However, it is possible to explicitly do a transition
 *   to native if needed to allow safepoints (e.g., before a blocking call or when doing a call back into uninterruptible Java code). After doing the
 *   transition to native, it is guaranteed that SVM is able to reach a safepoint even if the C++ code blocks. Here is one example where we would end up
 *   in a deadlock if we didn't do a transition to native:
 *   - thread A does a slow-path allocation and acquires the GC-internal mutex
 *   - thread B does a slow-path allocation and blocks when it tries to acquire the GC-internal mutex
 *   - thread A schedules a VM operation that needs a safepoint
 *   - thread B still has STATUS_IN_VM, so SVM can't reach a safepoint and deadlocks
 *
 * - TO_NATIVE: The C++ code may execute blocking calls or may call back into Java code. However, the C++ code must be aware that safepoints can happen at
 *   ANY time. Writing the code in such a way is often not feasible so that this transition is only used for cases where we know that safepoints don't
 *   cause any problems.
 *
 * Some guidance for determining the correct transition:
 * - C++ code uses oops or must not be interrupted by a safepoint -> NO_TRANSITION or TO_VM
 * - C++ code may block or call back to Java -> TO_NATIVE or TO_VM
 */


namespace svm_gc {

static inline jlong convert_size_t_to_jlong(size_t val) {
  // In the 64-bit vm, a size_t can overflow a jlong (which is signed).
  NOT_LP64 (return (jlong)val;)
  LP64_ONLY(return (jlong)MIN2(val, (size_t)max_jlong);)
}

extern "C" {
// NO_TRANSITION - This method is called during startup, before anything else is initialized.
EXPORT_FOR_SVM void svm_gc_parse_options(int actual_native_image_version, int argc, char *argv[], char *image_build_hosted_args, char *image_build_runtime_args,
    size_t max_heap_address_space_size, size_t heap_base_alignment, size_t null_regions_size, size_t image_heap_size,
    int compressed_reference_shift, bool is_containerized, jlong container_memory_limit_in_bytes, int container_active_processor_count, ShenandoahHeapOptions *result) {
  // verify invariants
  int expected_native_image_version = 250101;
  guarantee(actual_native_image_version >= expected_native_image_version, "incompatible GC version: the native-image tries to use a GC that is too new");
  guarantee(actual_native_image_version <= expected_native_image_version, "incompatible GC version: the native-image tries to use a GC that is too old");
#ifdef SVM_COMPRESSED_REFERENCES
  guarantee(compressed_reference_shift > 0, "incompatible GC version: the native-image was built using -H:+UseCompressedReferences but the GC does not use compressed references");
#else
  guarantee(compressed_reference_shift == 0, "incompatible GC version: the native-image was built using -H:-UseCompressedReferences but the GC uses compressed references");
#endif
  guarantee(compressed_reference_shift == CompressedOopShift, "must be");

  // verify arguments
  guarantee(SVMIsolateData::_heap_base == nullptr, "GC doesn't support multiple isolates at the moment.");
  guarantee(argc >= 0, "must be");
  guarantee((argc == 0) == (argv == nullptr), "must be");
  guarantee(image_build_hosted_args != nullptr, "must be");
  guarantee(image_build_runtime_args != nullptr, "must be");
  guarantee(null_regions_size > 0, "must be");
  guarantee(image_heap_size > 0, "must be");

  SVMIsolateData::_argc = argc;
  SVMIsolateData::_argv = argv;
  SVMIsolateData::_max_heap_address_space_size = max_heap_address_space_size;

  SVMGlobalData::_heap_base_alignment = heap_base_alignment;
  SVMGlobalData::_null_regions_size = null_regions_size;
  SVMGlobalData::_image_heap_size = image_heap_size;
  SVMGlobalData::_image_build_hosted_args = image_build_hosted_args;
  SVMGlobalData::_image_build_runtime_args = image_build_runtime_args;

  // Container information needs to be set before the argument parsing
  SVMGlobalData::_is_containerized = is_containerized;
  SVMGlobalData::_container_memory_limit_in_bytes = container_memory_limit_in_bytes;
  SVMGlobalData::_container_active_processor_count = container_active_processor_count;

  Threads::parse_arguments();

  assert(is_aligned(MaxHeapSize, HeapAlignment), "must be");
  result->max_heap_size = MaxHeapSize;
  result->heap_address_space_size = MaxHeapSize + null_regions_size;
  result->physical_memory_size = FLAG_IS_DEFAULT(MaxRAM) ? os::physical_memory() : MaxRAM;

  // Note: `MaxHeapSize` already accounts for `image_heap_size` (see `Arguments::increase_by_image_heap_size()`)
  guarantee(MaxHeapSize <= max_heap_address_space_size, "Java heap must fit into its address space");
  guarantee(ReservedAddressSpaceSize == 0 || MaxHeapSize <= ReservedAddressSpaceSize, "heap address space size is invalid");
}

// NO_TRANSITION - Only called during startup by uninterruptible code before a safepoint can be triggered.
EXPORT_FOR_SVM ShenandoahInitState* svm_gc_create(IsolateThread *isolate_thread, char *heap_base,
    int closed_image_heap_regions, int open_image_heap_regions, typeArrayOop image_heap_region_types /* a Java byte[] */,
    typeArrayOop image_heap_region_free_spaces /* a Java int[] */,
    Klass *dynamic_hub_klass, InstanceKlass *filler_object_klass, TypeArrayKlass *filler_array_klass, Klass *string_klass, Klass *system_klass,
    objArrayOop static_object_fields, typeArrayOop static_primitive_fields, oop vm_operation_thread, oop safepoint, oop runtime_code_info_memory,
    int reference_map_compressed_offset_shift, SVMOopMap *thread_locals_reference_map,
    objArrayOop klasses_assumed_reachable_for_code_unloading, bool perf_data_support, bool use_string_inlining, bool closed_type_world,
    bool use_interface_hashing, int interface_hashing_max_id, int dynamic_hub_hashing_interface_mask, int dynamic_hub_hashing_shift_offset,
    char *offsets, int offsets_length,
    queueVmOperationFunc collect_for_allocation_op, queueVmOperationFunc collect_full_op, queueVmOperationFunc collect_degenerated_op, queueVmOperationFunc init_mark_op, queueVmOperationFunc final_mark_op, queueVmOperationFunc init_update_refs_op, queueVmOperationFunc final_update_refs_op, queueVmOperationFunc final_roots_op, queueVmOperationFunc handshake_fallback_op,
    vmOperationStatusFunc wait_for_vm_operation_execution_status, vmOperationStatusFunc update_vm_operation_execution_status,
    vmOperationDataFunc is_vm_operation_finished, yieldToQueuedVmOperationsFunc yield_to_queued_vm_operations,
    fetchThreadStackFramesFunc fetch_thread_stack_frames, freeThreadStackFramesFunc free_thread_stack_frames,
    fetchContinuationStackFramesFunc fetch_continuation_stack_frames, freeContinuationStackFramesFunc free_continuation_stack_frames,
    fetchCodeInfosFunc fetch_code_infos, freeCodeInfosFunc free_code_infos, cleanRuntimeCodeCacheFunc clean_runtime_code_cache,
    threadStateTransitionFunc transition_vm_to_native, fastThreadStateTransitionFunc fast_transition_native_to_vm, threadStateTransitionFunc slow_transition_native_to_vm,
    threadsLockFunc lock_threads_read, threadsLockFunc unlock_threads_read) {
  assert(isolate_thread->has_status_created(), "unexpected thread state");
  guarantee(SVMIsolateData::_heap_base == nullptr, "GC doesn't support multiple isolates at the moment.");

  // verify that gc_parse_options was executed properly
  guarantee(MaxNewSize >= 0, "must be");
  guarantee(TLABSize >= 0, "must be");

  // verify all arguments
  guarantee(isolate_thread != nullptr, "must be");
  guarantee(heap_base != nullptr, "must be");
  guarantee(closed_image_heap_regions >= 0, "must be");
  guarantee(open_image_heap_regions >= 0, "must be");
  guarantee(closed_image_heap_regions > 0 || open_image_heap_regions > 0, "must be");
  guarantee(image_heap_region_types != nullptr, "must be");
  guarantee(image_heap_region_free_spaces != nullptr, "must be");
  guarantee(dynamic_hub_klass != nullptr, "must be");
  guarantee(filler_object_klass != nullptr, "must be");
  guarantee(filler_array_klass != nullptr, "must be");
  guarantee(string_klass != nullptr, "must be");
  guarantee(system_klass != nullptr, "must be");
  guarantee(static_object_fields != nullptr, "must be");
  guarantee(static_primitive_fields != nullptr, "must be");
  guarantee(vm_operation_thread != nullptr, "must be");
  guarantee(safepoint != nullptr, "must be");
  guarantee(runtime_code_info_memory != nullptr, "must be");
  guarantee(reference_map_compressed_offset_shift == ReferenceMapCompressedOffsetShift, "must be");
  guarantee(thread_locals_reference_map != nullptr, "must be");
  guarantee(offsets != nullptr, "must be");
  guarantee(offsets_length > 0, "must be");
  guarantee(collect_for_allocation_op != nullptr, "must be");
  guarantee(collect_full_op != nullptr, "must be");
  guarantee(collect_degenerated_op != nullptr, "must be");
  guarantee(init_mark_op != nullptr, "must be");
  guarantee(final_mark_op != nullptr, "must be");
  guarantee(init_update_refs_op != nullptr, "must be");
  guarantee(final_update_refs_op != nullptr, "must be");
  guarantee(final_roots_op != nullptr, "must be");
  guarantee(handshake_fallback_op != nullptr, "must be");
  guarantee(wait_for_vm_operation_execution_status != nullptr, "must be");
  guarantee(update_vm_operation_execution_status != nullptr, "must be");
  guarantee(is_vm_operation_finished != nullptr, "must be");
  guarantee(yield_to_queued_vm_operations != nullptr, "must be");
  guarantee(fetch_thread_stack_frames != nullptr, "must be");
  guarantee(free_thread_stack_frames != nullptr, "must be");
  guarantee(transition_vm_to_native != nullptr, "must be");
  guarantee(fast_transition_native_to_vm != nullptr, "must be");
  guarantee(slow_transition_native_to_vm != nullptr, "must be");
  guarantee(lock_threads_read != nullptr, "must be");
  guarantee(unlock_threads_read != nullptr, "must be");
  guarantee(dynamic_hub_hashing_interface_mask == DynamicHubHashingInterfaceMask, "must be");
  guarantee(dynamic_hub_hashing_shift_offset == DynamicHubHashingShiftOffset, "must be");

  // apply arguments
  SVMIsolateData::_heap_base = heap_base;
  SVMIsolateData::_image_heap_region_types = image_heap_region_types;
  SVMIsolateData::_image_heap_region_free_spaces = image_heap_region_free_spaces;
  SVMIsolateData::_static_object_fields = static_object_fields;
  SVMIsolateData::_static_primitive_fields = static_primitive_fields;
  SVMIsolateData::_vm_operation_thread = vm_operation_thread;
  SVMIsolateData::_safepoint = safepoint;
  SVMIsolateData::_runtime_code_info_memory = runtime_code_info_memory;
  SVMIsolateData::_klasses_assumed_reachable_for_code_unloading = klasses_assumed_reachable_for_code_unloading;

  SVMGlobalData::_closed_image_heap_regions = closed_image_heap_regions;
  SVMGlobalData::_open_image_heap_regions = open_image_heap_regions;
  SVMGlobalData::_thread_locals_reference_map = thread_locals_reference_map;
  SVMGlobalData::_use_string_inlining = use_string_inlining;
  SVMGlobalData::_closed_type_world = closed_type_world;
  SVMGlobalData::_use_interface_hashing = use_interface_hashing;
  SVMGlobalData::_interface_hashing_max_id = interface_hashing_max_id;
  SVMGlobalData::_collect_for_allocation_op = collect_for_allocation_op;
  SVMGlobalData::_collect_full_op = collect_full_op;
  SVMGlobalData::_collect_degenerated_op = collect_degenerated_op;
  SVMGlobalData::_init_mark_op = init_mark_op;
  SVMGlobalData::_final_mark_op = final_mark_op;
  SVMGlobalData::_init_update_refs_op = init_update_refs_op;
  SVMGlobalData::_final_update_refs_op = final_update_refs_op;
  SVMGlobalData::_final_roots_op = final_roots_op;
  SVMGlobalData::_handshake_fallback_op = handshake_fallback_op;
  SVMGlobalData::_wait_for_vm_operation_execution_status = wait_for_vm_operation_execution_status;
  SVMGlobalData::_update_vm_operation_execution_status = update_vm_operation_execution_status;
  SVMGlobalData::_is_vm_operation_finished = is_vm_operation_finished;
  SVMGlobalData::_yield_to_queued_vm_operations = yield_to_queued_vm_operations;
  SVMGlobalData::_fetch_thread_stack_frames = fetch_thread_stack_frames;
  SVMGlobalData::_free_thread_stack_frames = free_thread_stack_frames;
  SVMGlobalData::_fetch_continuation_stack_frames = fetch_continuation_stack_frames;
  SVMGlobalData::_free_continuation_stack_frames = free_continuation_stack_frames;
  SVMGlobalData::_fetch_code_infos = fetch_code_infos;
  SVMGlobalData::_free_code_infos = free_code_infos;
  SVMGlobalData::_transition_vm_to_native = transition_vm_to_native;
  SVMGlobalData::_try_fast_transition_native_to_vm = fast_transition_native_to_vm;
  SVMGlobalData::_slow_transition_native_to_vm = slow_transition_native_to_vm;
  SVMGlobalData::_lock_threads_read = lock_threads_read;
  SVMGlobalData::_unlock_threads_read = unlock_threads_read;
  SVMGlobalData::_clean_runtime_code_cache = clean_runtime_code_cache;
  SVMGlobalData::initialize_offsets(offsets, offsets_length);
  SVMGlobalData::verify_offsets(perf_data_support);

  Universe::_dynamic_hub_klass = dynamic_hub_klass;
  Universe::_fillerArrayKlass = filler_array_klass;
  vmClasses::_string_klass = string_klass;
  vmClasses::_system_klass = system_klass;
  vmClasses::_filler_object_klass = filler_object_klass;
  CollectedHeap::set_filler_object_klass(filler_object_klass);
  guarantee(filler_object_klass->size_helper() == oopDesc::header_size(), "must be");

  // The option UsePerfData is a bit special as it depends on a hosted flag.
  if (!perf_data_support) {
    // AllowVMInspection was disabled when building the native-image. So, no matter which value is passed for UsePerfData
    // (at image build time or at runtime), we always need to disable UsePerfData.
    if (FLAG_SET_CMDLINE(UsePerfData, false) != JVMFlag::SUCCESS) {
      return nullptr;
    }
  }

  CompressedOops::initialize();
  jint result = Threads::create_vm(isolate_thread);

#if defined(PRINT_NI_OPTIONS) && !defined(PRODUCT)
  PRINT_NI_FLAGS;
#endif // PRINT_NI_OPTIONS && !PRODUCT

  if (result == JNI_OK) {
    // verify a couple more data structures (if those guarantees fail, check if there is a mismatch between the gc_constants below and the constants on Native Image side)
    guarantee(IsolateThread::get_first_thread() != nullptr, "main thread must be registered");
    guarantee(IsolateThread::get_first_thread()->next_thread() == nullptr, "at this point in time, only the main thread may exist");
    guarantee(Threads::number_of_non_daemon_threads() == 1, "only one application thread should be active");
    guarantee(SafepointSynchronize::get_safepoint_state() == SafepointSynchronize::not_at_safepoint, "must not be at a safepoint");

    // return a data structure with relevant offsets and constants (some of the values depend on the VM arguments)
    // TODO: 'card_table_offset' is not constant in Shenandoah (see JDK-8343468)
    shenandoah_init_state.card_table_address = nullptr; //(address)ci_card_table_address();
    // Base of the Shenandoah collection-set fast-test map (biased by heap_base >> region_shift, so it
    // is indexed directly by object_address >> region_shift). Allocated once with the heap, so the
    // base is stable for the isolate's lifetime; the compiled CAS heal barrier uses it to skip the
    // heal stub for references that are not in the collection set. See svm_gc_load_reference_barrier_heal.
    shenandoah_init_state.cset_fast_test_address = (void*) ShenandoahHeap::in_cset_fast_test_addr();
    shenandoah_init_state.tlab_top_offset = in_bytes(Thread::tlab_top_offset());
    shenandoah_init_state.tlab_end_offset = in_bytes(Thread::tlab_end_offset());
    shenandoah_init_state.card_table_shift = CardTable::card_shift();
    shenandoah_init_state.log_of_heap_region_grain_bytes = 20;
    shenandoah_init_state.java_thread_size = sizeof(JavaThread);
    shenandoah_init_state.vm_operation_data_size = sizeof(VM_OperationData);
    shenandoah_init_state.vm_operation_wrapper_data_size = sizeof(VM_OperationWrapperData);
    // Offsets of the SATB mark queue's index and buffer fields, relative to the per-thread gc_state
    // byte (which the generated barrier addresses via ShenandoahHeap.javaThreadTL). Used by the
    // inlined SATB pre-write barrier's buffer write; validated against ShenandoahConstants at startup.
    shenandoah_init_state.satb_index_offset =
        in_bytes(ShenandoahThreadLocalData::satb_mark_queue_index_offset()) - in_bytes(ShenandoahThreadLocalData::gc_state_offset());
    shenandoah_init_state.satb_buffer_offset =
        in_bytes(ShenandoahThreadLocalData::satb_mark_queue_buffer_offset()) - in_bytes(ShenandoahThreadLocalData::gc_state_offset());
    // Offset of the per-thread card-table base pointer, also relative to gc_state. The C++ side
    // maintains it (ShenandoahBarrierSet::on_thread_attach and on card-table swaps), and it is null
    // unless the current mode keeps a remembered set, which lets the inlined card-marking barrier of
    // generational mode skip itself in the other modes.
    shenandoah_init_state.card_table_offset =
        in_bytes(ShenandoahThreadLocalData::card_table_offset()) - in_bytes(ShenandoahThreadLocalData::gc_state_offset());
    shenandoah_init_state.mark_offset = oopDesc::mark_offset_in_bytes();
    shenandoah_init_state.gc_state_offset = in_bytes(ShenandoahThreadLocalData::gc_state_offset());
    shenandoah_init_state.dirty_card_value = CardTable::dirty_card_val();
    return &shenandoah_init_state;
  }
  return nullptr;
}

// NO_TRANSITION - Uninterruptible code that may be called by any Java thread.
EXPORT_FOR_SVM void svm_gc_update_option_value(oop optionName, jlong value) {
  assert(IsolateThread::current()->has_status_created() || IsolateThread::current()->has_status_java(), "unexpected thread state");

  bool update_logging = false;
  if (java_lang_String::equals(optionName, "VerboseGC")) {
    FLAG_SET_MGMT(VerboseGC, value == 1);
    update_logging = true;
  } else if (java_lang_String::equals(optionName, "PrintGC")) {
    FLAG_SET_MGMT(PrintGC, value == 1);
    update_logging = true;
  } else if (java_lang_String::equals(optionName, "DisableExplicitGC")) {
    FLAG_SET_MGMT(DisableExplicitGC, value == 1);
  } else {
    fatal("Only the values of certain GC options can be changed at run-time.");
  }

  if (update_logging) {
    LogConfiguration::disable_logging();
    if (PrintGC || VerboseGC) {
      LogConfiguration::configure_stdout(LogLevel::Info, !VerboseGC, LOG_TAGS(gc));
    }
  }
}

// NO_TRANSITION - Only called during teardown after all other threads were already torn down.
EXPORT_FOR_SVM bool svm_gc_teardown() {
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");
  SVMIsolateData::_during_teardown = true;
#ifdef ASSERT
  int thread_count = 0;
  for (JavaThreadIteratorWithHandle jtiwh; JavaThread *jt = jtiwh.next(); ) {
    thread_count++;
  }
  assert(thread_count == 1, "all other threads must have been stopped");
#endif // ASSERT
  Threads::destroy_vm();
  return true;
}

// NO_TRANSITION - Uninterruptible code that may be called by any Java thread.
EXPORT_FOR_SVM void svm_gc_attach_thread(IsolateThread *thread) {
  assert(thread->has_status_created(), "unexpected thread state");
  JavaThread *java_thread = new (thread->java_thread()) JavaThread();
  assert(is_aligned(java_thread, wordSize), "must be");
  java_thread->initialize_thread_current();
  java_thread->initialize();
}

// NO_TRANSITION - Uninterruptible code that may be called by any Java thread.
// When this method is called, the thread may already have the state "IGNORE_SAFEPOINT" but by holding the threads lock on the Native Image-side,
// it is guaranteed that no other thread can trigger a safepoint.
EXPORT_FOR_SVM void svm_gc_detach_thread(IsolateThread *thread) {
  assert(thread->has_status_java(), "unexpected thread state");
  JavaThread *java_thread = thread->java_thread();
  java_thread->~JavaThread();
}

// NO_TRANSITION - Uninterruptible code that may be called by any Java thread.
EXPORT_FOR_SVM void svm_gc_retire_tlab() {
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");
  if (UseTLAB) {
    JavaThread::current()->tlab().retire(nullptr);
  }
}

// NO_TRANSITION - Uninterruptible code that is only called by the VM thread during the safepoint handling. So, no other thread can trigger a safepoint in the meanwhile.
EXPORT_FOR_SVM void svm_gc_prepare_for_safepoint() {
  assert(Thread::current()->is_VM_thread(), "must be the VM thread");
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");
  Universe::heap()->safepoint_synchronize_begin();
}

// NO_TRANSITION - Uninterruptible code that is only called by the VM thread during the safepoint handling. So, no other thread can trigger a safepoint in the meanwhile.
EXPORT_FOR_SVM void svm_gc_end_safepoint() {
  assert(Thread::current()->is_VM_thread(), "must be the VM thread");
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");
  Universe::heap()->safepoint_synchronize_end();
}

// TO_VM - Called by any Java thread. Uses oops. May block. May cause a safepoint.
EXPORT_FOR_SVM void svm_gc_collect(int cause) {
  IsolateThread* thread = IsolateThread::current();
  assert(thread->has_status_vm(), "unexpected thread state");
  if (!DisableExplicitGC) {
    SVMGlobalData::_transition_vm_to_native(thread);
    Universe::heap()->collect(GCCause::_java_lang_system_gc);
    assert(thread->has_status_native_or_safepoint(), "must be");
    SVMGlobalData::_slow_transition_native_to_vm(thread);
    assert(thread->has_status_vm(), "must be");
   }
}

// TO_NATIVE - Only called from the VM thread.
EXPORT_FOR_SVM bool svm_gc_execute_vm_operation_prologue(VM_OperationData *data) {
  assert(Thread::current()->is_VM_thread(), "must be the VM thread");
  assert(IsolateThread::current()->has_status_native(), "unexpected thread state");
  return data->vm_operation()->doit_prologue();
}

// TO_NATIVE - Only called from the VM thread at a safepoint.
EXPORT_FOR_SVM void svm_gc_execute_vm_operation_main(VM_OperationData *data) {
  assert(Thread::current()->is_VM_thread(), "must be the VM thread");
  assert(SafepointSynchronize::get_safepoint_state() == SafepointSynchronize::at_safepoint, "must be at a safepoint");
  assert(IsolateThread::current()->has_status_native(), "unexpected thread state");
  // Record the operation that is currently executing on the VM operation thread.
  // This is read e.g. by is_at_shenandoah_safepoint(). Save/restore here (rather
  // than in VMThread::execute) so that it stays correct for nested VM operations
  // (e.g. a GC the VM operation thread runs inline due to an allocation failure).
  VM_Operation* const prev = VMThread::set_current_vm_operation(data->vm_operation());
  data->vm_operation()->evaluate();
  VMThread::restore_current_vm_operation(prev);
}

// TO_NATIVE - Only called from the VM thread.
EXPORT_FOR_SVM void svm_gc_execute_vm_operation_epilogue(VM_OperationData *data) {
  assert(Thread::current()->is_VM_thread(), "must be the VM thread");
  assert(IsolateThread::current()->has_status_native(), "unexpected thread state");
  data->vm_operation()->doit_epilogue();
}

// TO_VM - May be called by any Java thread. Uses oops. May block. May cause a safepoint.
EXPORT_FOR_SVM oop svm_gc_allocate_instance(InstanceKlass *k) {
  IsolateThread* thread = IsolateThread::current();
  assert(thread->has_status_vm(), "unexpected thread state");
  assert(k->is_instance_klass(), "must be");
  // The following call is potentially prone to deadlocks if it blocks. We therefore
  // have to ensure that we transition to native before we block, e.g. in Monitor::wait()
  // or in ShenandoahLock::contended_lock_internal()
  oop result = Universe::heap()->obj_allocate(k, k->size_helper());
  assert(thread->has_status_vm(), "must be");
  if (result != nullptr) {
    BarrierSet::barrier_set()->on_slowpath_allocation_exit(JavaThread::current(), result);
  }
  return result;
}

// TO_VM - May be called by any Java thread. Uses oops. May block. May cause a safepoint.
EXPORT_FOR_SVM oop svm_gc_allocate_array(ArrayKlass *k, int length) {
  IsolateThread* thread = IsolateThread::current();
  assert(thread->has_status_vm(), "unexpected thread state");
  assert(k->is_array_klass(), "must be");
  assert(length >= 0, "must be");

  oop result = nullptr;
  if (length >= 0 && length <= k->max_length()) {
    int size = k->object_size(length);
    // The following call is potentially prone to deadlocks if it blocks. We therefore
    // have to ensure that we transition to native before we block, e.g. in Monitor::wait()
    // or in ShenandoahLock::contended_lock_internal()
    result = Universe::heap()->array_allocate(k, size, length, true);
    assert(thread->has_status_vm(), "must be");
    if (result != nullptr) {
      BarrierSet::barrier_set()->on_slowpath_allocation_exit(JavaThread::current(), result);
    }
  }
  return result;
}

// TO_VM - May be called by any Java thread. Uses oops. May block. May cause a safepoint.
EXPORT_FOR_SVM oop svm_gc_allocate_stack_chunk(InstanceStackChunkKlass *k, int length) {
  assert(IsolateThread::current()->has_status_vm(), "unexpected thread state");
  assert(k->is_stack_chunk_instance_klass(), "must be");
  assert(length >= 0, "must be");

  oop result = k->allocate(length);
  if (result != nullptr) {
    BarrierSet::barrier_set()->on_slowpath_allocation_exit(JavaThread::current(), result);
  }
  return result;
}

// TO_VM - May be called by any Java thread. Uses oops. May block. May cause a safepoint.
EXPORT_FOR_SVM oop svm_gc_allocate_pod(InstancePodKlass *k, int length) {
  assert(IsolateThread::current()->has_status_vm(), "unexpected thread state");
  assert(k->is_pod_instance_klass(), "must be");
  assert(length >= 0, "must be");

  podOop result = k->allocate(length);
  if (result != nullptr) {
    BarrierSet::barrier_set()->on_slowpath_allocation_exit(JavaThread::current(), result);
  }
  return result;
}

// NO_TRANSITION - Uninterruptible code that may be called by any Java thread.
EXPORT_FOR_SVM void svm_gc_pin_object(oop o) {
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");
  Universe::heap()->pin_object(nullptr, o);
}

// NO_TRANSITION - Uninterruptible code that may be called by any Java thread.
EXPORT_FOR_SVM void svm_gc_unpin_object(oop o) {
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");
  Universe::heap()->unpin_object(nullptr, o);
}

// NO_TRANSITION - Uninterruptible code that may be called by any Java thread.
EXPORT_FOR_SVM void svm_gc_pre_write_barrier(oop obj) {
  // This may run very early during isolate creation, before the GC (and its barrier set) has
  // been installed. No GC can be in progress at that point, so there is nothing to do.
  if (BarrierSet::barrier_set() == nullptr || Thread::current_or_null() == nullptr) {
    return;
  }
  // satb_enqueue() internally checks whether SATB marking is active and whether 'obj' is
  // non-null. 'obj' is the previous (uncompressed) value of the reference field being overwritten.
  ShenandoahBarrierSet::barrier_set()->satb_enqueue(obj);
}

// SATB pre-write barrier variant that receives the previous value as a compressed
// (narrow) reference. Used by generated code so that it does not have to decode
// compressed references inline. The argument is pointer-width so that it can carry a
// full-width narrow reference: with isolates but without size-reducing compression
// (Graal CE) narrowOop is 8 bytes (a heap-base-relative offset), while with size-reducing
// compressed references (SVM_COMPRESSED_REFERENCES) narrowOop is 4 bytes and the value is
// simply zero-extended into the pointer-width argument.
// NO_TRANSITION - Uninterruptible code that may be called by any Java thread.
EXPORT_FOR_SVM void svm_gc_pre_write_barrier_narrow(uintptr_t narrow_pre_val) {
  if (BarrierSet::barrier_set() == nullptr || Thread::current_or_null() == nullptr) {
    return;
  }
  // Only decode/enqueue the previous value while concurrent marking is actually in progress
  // (this mirrors the guard inside satb_enqueue()). Outside of marking the enqueue would be a
  // no-op anyway, and the narrow word may hold uninitialized/non-reference data that would trip
  // the debug "object not in heap" assertion inside CompressedOops::decode().
  if (!ShenandoahHeap::heap()->is_concurrent_mark_in_progress()) {
    return;
  }
  oop pre_val = CompressedOops::decode((narrowOop) narrow_pre_val);
  ShenandoahBarrierSet::barrier_set()->satb_enqueue(pre_val);
}

// Load-reference barrier. 'obj' is the (uncompressed) reference that was just loaded.
// Returns the canonical (to-space) reference. load_reference_barrier() internally
// checks whether a barrier is actually required and returns 'obj' unchanged otherwise.
// It only touches the current thread when evacuation is in progress, so it is safe to
// call before the calling thread has been attached to the GC (early isolate creation).
// NO_TRANSITION - Uninterruptible code that may be called by any Java thread.
EXPORT_FOR_SVM oop svm_gc_load_reference_barrier(oop obj, void* load_addr) {
  // See svm_gc_pre_write_barrier: the barrier set may not be installed yet during early
  // isolate creation, in which case no objects are forwarded and there is nothing to do.
  if (BarrierSet::barrier_set() == nullptr || Thread::current_or_null() == nullptr) {
    return obj;
  }
  // Canonicalize 'obj' to its to-space location AND self-heal the memory location it was loaded
  // from (if known): the decorated barrier CAS-updates *load_addr from the stale from-space value
  // to the to-space value, so subsequent loads of the same slot take the inline fast path instead
  // of calling this stub again. This mirrors HotSpot's two-argument LRB runtime entries
  // (ShenandoahRuntime::load_reference_barrier_strong(oop, oop*)). 'load_addr' may be null when
  // the load location is unknown; the barrier then only canonicalizes the value.
  ShenandoahBarrierSet* const bs = ShenandoahBarrierSet::barrier_set();
  if (UseCompressedOops) {
    return bs->load_reference_barrier(ON_STRONG_OOP_REF, obj, reinterpret_cast<narrowOop*>(load_addr));
  } else {
    return bs->load_reference_barrier(ON_STRONG_OOP_REF, obj, reinterpret_cast<oop*>(load_addr));
  }
}

// Load-reference barrier for a referent loaded from a WEAK reference (java.lang.ref.Reference.get,
// Reference.refersTo on WeakReference, etc.). In addition to canonicalizing a from-space pointer
// (and self-healing the load location like the strong variant above), it must NOT resurrect an
// unreachable referent: during the concurrent weak-roots phase (after marking decided liveness,
// before the reference processor has cleared dead referents) a load of an unmarked referent returns
// null, exactly like the decorated C++ barrier (ON_WEAK_OOP_REF). Without this, a mutator could
// obtain a strong reference to an unmarked collection-set object, store it into a live object, and
// leave a dangling reference once the collection set is recycled (the object was never evacuated
// because it was never marked).
// NO_TRANSITION - Uninterruptible code that may be called by any Java thread.
EXPORT_FOR_SVM oop svm_gc_load_reference_barrier_weak(oop obj, void* load_addr) {
  if (BarrierSet::barrier_set() == nullptr || Thread::current_or_null() == nullptr) {
    return obj;
  }
  ShenandoahBarrierSet* const bs = ShenandoahBarrierSet::barrier_set();
  if (UseCompressedOops) {
    return bs->load_reference_barrier(ON_WEAK_OOP_REF, obj, reinterpret_cast<narrowOop*>(load_addr));
  } else {
    return bs->load_reference_barrier(ON_WEAK_OOP_REF, obj, reinterpret_cast<oop*>(load_addr));
  }
}

// Same as svm_gc_load_reference_barrier_weak, but for PHANTOM strength (Reference.refersTo on
// phantom references and weak-native accesses): dead referents are filtered with is_marked (any
// strength) rather than is_marked_strong.
// NO_TRANSITION - Uninterruptible code that may be called by any Java thread.
EXPORT_FOR_SVM oop svm_gc_load_reference_barrier_phantom(oop obj, void* load_addr) {
  if (BarrierSet::barrier_set() == nullptr || Thread::current_or_null() == nullptr) {
    return obj;
  }
  ShenandoahBarrierSet* const bs = ShenandoahBarrierSet::barrier_set();
  if (UseCompressedOops) {
    return bs->load_reference_barrier(ON_PHANTOM_OOP_REF, obj, reinterpret_cast<narrowOop*>(load_addr));
  } else {
    return bs->load_reference_barrier(ON_PHANTOM_OOP_REF, obj, reinterpret_cast<oop*>(load_addr));
  }
}

// Self-healing load-reference barrier for a reference field that is about to be atomically updated
// (compare-and-swap / getAndSet). Reads the current field value at 'addr', resolves it to its
// canonical (to-space) location and - if it was a from-space pointer - CAS-heals the field in place,
// so that a subsequent PLAIN atomic sees the to-space value and cannot suffer a concurrent-evacuation
// false negative (which would otherwise leave a stale from-space pointer in the field). This mirrors
// HotSpot's "fix up early" atomic barrier model (JDK-8384080 / JDK-8383810). It is only invoked on the
// slow path, i.e. when the heap has forwarded objects (evacuation / update-refs in progress).
// NO_TRANSITION - Uninterruptible code that may be called by any Java thread.
EXPORT_FOR_SVM void svm_gc_load_reference_barrier_heal(void* addr) {
  // See svm_gc_load_reference_barrier: the barrier set may not be installed yet during early
  // isolate creation, in which case no objects are forwarded and there is nothing to do.
  if (BarrierSet::barrier_set() == nullptr || Thread::current_or_null() == nullptr) {
    return;
  }
  ShenandoahBarrierSet* const bs = ShenandoahBarrierSet::barrier_set();
  if (UseCompressedOops) {
    narrowOop* const p = reinterpret_cast<narrowOop*>(addr);
    narrowOop v = *p;
    if (CompressedOops::is_null(v)) {
      return;
    }
    // The field holds a live reference (from-space or to-space); decode and canonicalize+heal it.
    oop obj = CompressedOops::decode_not_null(v);
    bs->load_reference_barrier(ON_STRONG_OOP_REF, obj, p);
  } else {
    oop* const p = reinterpret_cast<oop*>(addr);
    oop obj = *p;
    if (obj == nullptr) {
      return;
    }
    bs->load_reference_barrier(ON_STRONG_OOP_REF, obj, p);
  }
}

// NO_TRANSITION - Uninterruptible code that may be called by any Java thread.
EXPORT_FOR_SVM void svm_gc_post_write_barrier(void *card_addr) {
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");

  Unimplemented();
}

// NO_TRANSITION - Uninterruptible code that may be called by any Java thread.
EXPORT_FOR_SVM void svm_gc_dirty_all_references_of(stackChunkOop stackChunk) {
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");

  if (Universe::heap()->requires_barriers(stackChunk)) {
    stackChunk->do_barriers();
  }
}

// NO_TRANSITION - Uninterruptible code that may be called by any Java thread.
EXPORT_FOR_SVM jlong svm_gc_millis_since_last_whole_heap_examined() {
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");
  size_t n = Universe::heap()->millis_since_last_whole_heap_examined();
  return convert_size_t_to_jlong(n);
}

// TO_NATIVE - Only called by the reference handler thread. May block.
// Checks if an oop reference is non-null but that is fine as we hold the Heap_lock.
EXPORT_FOR_SVM bool svm_gc_has_reference_pending_list() {
  // see JVM_HasReferencePendingList
  assert(IsolateThread::current()->has_status_native_or_safepoint(), "unexpected thread state");
  MonitorLocker ml(Heap_lock);
  return Universe::has_reference_pending_list();
}

// TO_VM - Only called by the reference handler thread. Uses oops. May block.
EXPORT_FOR_SVM oop svm_gc_get_and_clear_reference_pending_list() {
  // see JVM_GetAndClearReferencePendingList
  assert(IsolateThread::current()->has_status_vm(), "unexpected thread state");
  MonitorLocker ml(Heap_lock);
  oop ref = Universe::reference_pending_list();
  if (ref != nullptr) {
    Universe::clear_reference_pending_list();
  }
  return ref;
}

// NO_TRANSITION - Uninterruptible code that is only called by the reference handler thread.
EXPORT_FOR_SVM uint64_t svm_gc_get_reference_pending_list_wakeup_count() {
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");
  return Universe::reference_pending_list_wakeup_count();
}

// TO_NATIVE - Only called by the reference handler thread. May block.
// Checks if an oop reference is non-null but that is fine as we hold the Heap_lock.
EXPORT_FOR_SVM bool svm_gc_wait_for_reference_pending_list(uint64_t initial_wakeup_count) {
  // see JVM_WaitForReferencePendingList
  assert(IsolateThread::current()->has_status_native_or_safepoint(), "unexpected thread state");
  MonitorLocker ml(Heap_lock);
  while (!Universe::has_reference_pending_list() && Universe::reference_pending_list_wakeup_count() == initial_wakeup_count) {
    ml.wait();
  }
  return Universe::reference_pending_list_wakeup_count() == initial_wakeup_count;
}

// TO_NATIVE - May be called by any thread. May block.
EXPORT_FOR_SVM void svm_gc_wake_up_reference_pending_list_waiters() {
  assert(IsolateThread::current()->has_status_native_or_safepoint(), "unexpected thread state");
  MonitorLocker ml(Heap_lock);
  Universe::request_reference_pending_list_waiters_wakeup();
  ml.notify_all();
}

// TO_NATIVE - Only called from the VM thread at a safepoint.
EXPORT_FOR_SVM void svm_gc_get_region_boundaries(ShenandoahRegionBoundaries *region_boundaries) {
  assert(Thread::current()->is_VM_thread(), "must be the VM thread");
  assert(IsolateThread::current()->has_status_native_or_safepoint(), "unexpected thread state");

  Unimplemented();
}

// NO_TRANSITION - Almost uninterruptible code that may be called from any Java thread.
EXPORT_FOR_SVM void svm_gc_register_object_fields(nmethod* nm) {
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");
  assert(nm->state() == nmethod::state_code_constants_live, "must be");

  Unimplemented();
}

// NO_TRANSITION - Almost uninterruptible code that may be called from any Java thread.
EXPORT_FOR_SVM void svm_gc_register_code_constants(nmethod* nm) {
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");
  assert(nm->state() == nmethod::state_code_constants_live, "must be");

  Unimplemented();
}

// NO_TRANSITION - Almost uninterruptible code that may be called from any Java thread.
EXPORT_FOR_SVM void svm_gc_register_frame_metadata(nmethod* nm) {
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");
  assert(nm->state() == nmethod::state_code_constants_live, "must be");

  Unimplemented();
}

// NO_TRANSITION - Almost uninterruptible code that may be called from any Java thread.
EXPORT_FOR_SVM void svm_gc_register_deopt_metadata(nmethod* nm) {
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");
  assert(nm->state() == nmethod::state_code_constants_live, "must be");

  Unimplemented();
}

// NO_TRANSITION - Only called when printing diagnostics.
EXPORT_FOR_SVM void svm_gc_get_internal_state(ShenandoahInternalState *gc_internal_data) {
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");

  if (gc_internal_data != nullptr) {
    CollectedHeap* ch = Universe::heap();
    gc_internal_data->total_collections = ch->total_collections();
    gc_internal_data->full_collections = ch->total_full_collections();
    gc_internal_data->card_table_size = 0;         // TODO
    gc_internal_data->card_table_start = nullptr;  // TODO
  }
}

// NO_TRANSITION - Only called when printing diagnostics.
EXPORT_FOR_SVM const char* svm_gc_get_current_thread_name() {
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");
  Thread* cur = Thread::current_or_null();
  if (cur != nullptr) {
    return cur->name();
  }
  return nullptr;
}

// NO_TRANSITION - Only called when printing diagnostics. SVM threads may still be running concurrently, so this is racy by design.
EXPORT_FOR_SVM bool svm_gc_get_region_info(int region_index, ShenandoahRegionInfo *region_info) {
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");

  ShenandoahHeapRegion *region = ShenandoahHeap::heap()->get_region(region_index);
  if (region_info != nullptr && region != nullptr) {
    region_info->bottom = (u_char*)region->bottom();
    region_info->end = (u_char*)region->end();
    region_info->top = (u_char*)region->top();
    region_info->region_type = region->state(); // TODO: region state is bigger than 'char' for image heap regions.
    return true;
  }
  return false;
}

// NO_TRANSITION - Can be called by any thread. This is racy by design.
EXPORT_FOR_SVM jlong svm_gc_get_thread_allocated_memory(IsolateThread *thread) {
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");
  return thread->java_thread()->cooked_allocated_bytes();
}

// TO_VM - May be called by any Java thread. May block.
EXPORT_FOR_SVM jlong svm_gc_get_used_memory() {
  assert(IsolateThread::current()->has_status_vm(), "unexpected thread state");
  MutexLocker x(Heap_lock);
  size_t n = Universe::heap()->used();
  assert(n >= SVMGlobalData::_image_heap_used, "must be");
  return convert_size_t_to_jlong(n - SVMGlobalData::_image_heap_used);
}

// TO_VM - May be called by any Java thread. May block.
EXPORT_FOR_SVM jlong svm_gc_get_free_memory() {
  assert(IsolateThread::current()->has_status_vm(), "unexpected thread state");
  CollectedHeap* ch = Universe::heap();
  size_t n;
  {
     MutexLocker x(Heap_lock);
     n = ch->capacity() - ch->used() - SVMGlobalData::_image_heap_waste;
  }
  return convert_size_t_to_jlong(n);
}

// NO_TRANSITION - Uninterruptible code that may be called by any Java thread.
EXPORT_FOR_SVM jlong svm_gc_get_total_memory() {
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");
  size_t n = Universe::heap()->capacity();
  assert(n >= SVMGlobalData::_image_heap_size, "must be");
  return convert_size_t_to_jlong(n - SVMGlobalData::_image_heap_size);
}

// NO_TRANSITION - Uninterruptible code that may be called by any Java thread.
EXPORT_FOR_SVM jlong svm_gc_get_max_memory() {
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");
  size_t n = Universe::heap()->max_capacity();
  assert(n >= SVMGlobalData::_image_heap_size, "must be");
  return convert_size_t_to_jlong(n - SVMGlobalData::_image_heap_size);
}

// NO_TRANSITION - Can be called by any thread.
EXPORT_FOR_SVM size_t svm_gc_get_used_memory_after_last_gc() {
  assert(IsolateThread::current()->has_status_java(), "unexpected thread state");
  size_t n = Universe::heap()->used_at_last_gc();
  assert(n >= SVMGlobalData::_image_heap_used, "must be");
  return convert_size_t_to_jlong(n - SVMGlobalData::_image_heap_used);
}

} // extern C

} // namespace svm_gc

