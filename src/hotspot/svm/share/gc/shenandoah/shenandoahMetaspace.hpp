/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SVM_SHARE_GC_SHENANDOAH_SHENANDOAHMETASPACE_HPP
#define SVM_SHARE_GC_SHENANDOAH_SHENANDOAHMETASPACE_HPP

#include "oops/oop.hpp"
#include "svmMetaspace.hpp"

namespace svm_gc {

class InstanceKlass;
class Klass;

// Manages Java objects allocated in the Shenandoah metaspace regions (runtime-created DynamicHubs and
// their metadata arrays, see the SubstrateVM Metaspace interface). Until class unloading is
// supported, the metaspace follows the same model as the open image heap (and as G1Metaspace):
//  - Objects are always live and are never marked, evacuated, compacted, or reclaimed. The regions
//    are never part of a collection set, carry no generation affiliation, and are skipped by all
//    region iterations that only apply to the collected heap (is_image_heap_or_metaspace()).
//  - Their references into the collected heap are roots that are scanned, updated, and adjusted
//    together with the open image heap (see ShenandoahOpenImageHeapRoots). Reference stores use the
//    normal barriers.
//  - Unlike java.lang.ref.Reference objects in the image heap, those in the metaspace are not
//    expected (no SubstrateVM code allocates them there).
//
// Allocation and commitment:
// The metaspace occupies the first regions of the heap reservation (followed by the image heap).
// Objects are bump-allocated at run time under the Heap_lock, and allocation fails (returns
// nullptr) when the fixed reservation is exhausted. Region memory is committed on demand.
// Objects larger than a region are allocated in consecutive empty regions, which then become
// humongous start/continuation regions.
//
// Concurrency invariants:
// Allocations happen in VM state and therefore never overlap a safepoint, so the root scans in
// GC pauses see stable region tops. An allocation initializes the object before publishing the new
// top with a release store, so concurrent readers of the top (is_in_allocated_memory()) only see
// initialized objects. Objects allocated after a root scan contain no references until Java code
// stores them, and those stores go through the SATB and card barriers.
class ShenandoahMetaspace : public SVMMetaspace {
 public:
  // Must match MetaspaceAllocationKind in ShenandoahAllocationSupport on the SubstrateVM side.
  enum AllocationKind {
    DynamicHubAllocation = 0,
    ByteArrayAllocation = 1,
    IntArrayAllocation = 2,
    ObjectAllocation = 3,
    AllocationKindCount = 4
  };

 private:
  static uint _current_region;
  static uint _committed_regions;
  static size_t _used;
  static size_t _used_at_last_gc;
  static size_t _allocation_counts[AllocationKindCount];
  static size_t _allocation_sizes[AllocationKindCount];

  static oop allocate(Klass* klass, size_t word_size, int array_length, AllocationKind allocation_kind);
  static oop allocate_humongous(Klass* klass, size_t word_size, int array_length, AllocationKind allocation_kind);
  static oop initialize_object(HeapWord* memory, Klass* klass, size_t word_size, int array_length);
  static void record_allocation(size_t word_size, AllocationKind allocation_kind);
  static void commit_regions(uint end_region);

 public:
  void initialize();
  void update_used_at_last_gc() override;

  static oop allocate_instance(InstanceKlass* klass);
  static oop allocate_array(Klass* klass, int length, AllocationKind allocation_kind);

  bool is_in_allocated_memory(const void* address) const override;

  static size_t used() { return _used; }
  static size_t used_at_last_gc() { return _used_at_last_gc; }
  static size_t allocation_count(AllocationKind allocation_kind) { return _allocation_counts[allocation_kind]; }
  static size_t allocation_size(AllocationKind allocation_kind) { return _allocation_sizes[allocation_kind]; }
};

} // namespace svm_gc

#endif // SVM_SHARE_GC_SHENANDOAH_SHENANDOAHMETASPACE_HPP
