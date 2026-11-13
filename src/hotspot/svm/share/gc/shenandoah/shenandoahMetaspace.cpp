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

#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.inline.hpp"
#include "oops/arrayOop.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/klass.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"
#include "utilities/align.hpp"
#include "utilities/copy.hpp"
#include "svmGlobalData.hpp"
#include "gc/shenandoah/shenandoahMetaspace.hpp"

namespace svm_gc {

uint ShenandoahMetaspace::_current_region = 0;
uint ShenandoahMetaspace::_committed_regions = 0;
size_t ShenandoahMetaspace::_used = 0;
size_t ShenandoahMetaspace::_used_at_last_gc = 0;
size_t ShenandoahMetaspace::_allocation_counts[AllocationKindCount];
size_t ShenandoahMetaspace::_allocation_sizes[AllocationKindCount];

void ShenandoahMetaspace::initialize() {
  _current_region = 0;
  _committed_regions = 0;
  for (uint i = 0; i < AllocationKindCount; i++) {
    _allocation_counts[i] = 0;
    _allocation_sizes[i] = 0;
  }
  _used = 0;
  _used_at_last_gc = 0;
  SVMMetaspace::set_metaspace(this);
}

void ShenandoahMetaspace::update_used_at_last_gc() {
  _used_at_last_gc = _used;
}

oop ShenandoahMetaspace::allocate_instance(InstanceKlass* klass) {
  assert(klass != nullptr && klass->is_instance_klass(), "must be");
  return allocate(klass, klass->size_helper(), -1, ObjectAllocation);
}

oop ShenandoahMetaspace::allocate_array(Klass* klass, int length, AllocationKind allocation_kind) {
  assert(klass != nullptr, "must be");
  assert(length >= 0, "must be");

  int layout_helper = klass->layout_helper();
  int element_shift = Klass::layout_helper_log2_element_size(layout_helper);
  size_t header_size = Klass::layout_helper_header_size(layout_helper);
  if ((size_t)length > ((SIZE_MAX - header_size) >> element_shift)) {
    return nullptr;
  }

  size_t size_in_bytes = header_size + ((size_t)length << element_shift);
  size_t word_size = align_object_size(heap_word_size(size_in_bytes));
  return allocate(klass, word_size, length, allocation_kind);
}

oop ShenandoahMetaspace::allocate(Klass* klass, size_t word_size, int array_length, AllocationKind allocation_kind) {
  DEBUG_ONLY(JavaThread::current()->check_for_valid_safepoint_state());
  assert(!SafepointSynchronize::is_at_safepoint(), "metaspace allocation must not happen at a safepoint");
  assert(word_size > 0, "must be");

  size_t region_words = ShenandoahHeapRegion::region_size_words();
  if (word_size > (size_t)SVMGlobalData::_metaspace_regions * region_words) {
    return nullptr;
  }

  MutexLocker ml(Heap_lock);
  if (word_size > region_words) {
    return allocate_humongous(klass, word_size, array_length, allocation_kind);
  }

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  // A failed allocation must leave the current region available for smaller objects.
  for (uint region_index = _current_region; region_index < (uint)SVMGlobalData::_metaspace_regions; region_index++) {
    ShenandoahHeapRegion* region = heap->get_region(region_index);
    assert(region->is_metaspace(), "must be");
    if (region->is_humongous()) {
      continue;
    }

    HeapWord* mem = region->top();
    if (pointer_delta(region->end(), mem) < word_size) {
      continue;
    }
    commit_regions(region_index + 1);

    oop result = initialize_object(mem, klass, word_size, array_length);
    region->set_top_release(mem + word_size);
    _current_region = region_index;
    record_allocation(word_size, allocation_kind);
    return result;
  }

  return nullptr;
}

oop ShenandoahMetaspace::allocate_humongous(Klass* klass, size_t word_size, int array_length, AllocationKind allocation_kind) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  uint first_region = _current_region;
  while (first_region < (uint)SVMGlobalData::_metaspace_regions && heap->get_region(first_region)->top() != heap->get_region(first_region)->bottom()) {
    first_region++;
  }

  size_t region_words = ShenandoahHeapRegion::region_size_words();
  uint region_count = (uint)align_up(word_size, region_words) / (uint)region_words;
  uint end_region = first_region + region_count;
  if (end_region > (uint)SVMGlobalData::_metaspace_regions) {
    return nullptr;
  }
  commit_regions(end_region);

  ShenandoahHeapRegion* first = heap->get_region(first_region);
  HeapWord* memory = first->bottom();
  HeapWord* object_end = memory + word_size;
  first->make_metaspace_humongous_start();
  for (uint i = first_region + 1; i < end_region; i++) {
    heap->get_region(i)->make_metaspace_humongous_cont();
  }

  oop result = initialize_object(memory, klass, word_size, array_length);
  for (uint i = first_region; i + 1 < end_region; i++) {
    ShenandoahHeapRegion* region = heap->get_region(i);
    region->set_top_release(region->end());
  }
  heap->get_region(end_region - 1)->set_top_release(object_end);

  _current_region = end_region;
  record_allocation(word_size, allocation_kind);
  return result;
}

oop ShenandoahMetaspace::initialize_object(HeapWord* memory, Klass* klass, size_t word_size, int array_length) {
  Copy::fill_to_aligned_words(memory, word_size);
  if (array_length >= 0) {
    arrayOopDesc::set_length(memory, array_length);
  }
  oopDesc::initialize_obj_header(memory, klass);
  return cast_to_oop(memory);
}

void ShenandoahMetaspace::record_allocation(size_t word_size, AllocationKind allocation_kind) {
  size_t size_in_bytes = word_size * HeapWordSize;
  _used += size_in_bytes;
  _allocation_counts[allocation_kind]++;
  _allocation_sizes[allocation_kind] += size_in_bytes;
}

void ShenandoahMetaspace::commit_regions(uint end_region) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  while (_committed_regions < end_region) {
    ShenandoahHeapRegion* region = heap->get_region(_committed_regions);
    assert(region->is_metaspace(), "must be");
    os::commit_memory_or_exit((char*)region->bottom(), ShenandoahHeapRegion::region_size_bytes(), false,
                              "Cannot commit metaspace memory");
    _committed_regions++;
  }
}

bool ShenandoahMetaspace::is_in_allocated_memory(const void* address) const {
  if (!is_in_address_space(address)) {
    return false;
  }

  ShenandoahHeapRegion* region = ShenandoahHeap::heap()->heap_region_containing(address);
  const HeapWord* value = static_cast<const HeapWord*>(address);
  return region != nullptr && region->is_metaspace() && value >= region->bottom() && value < region->top_acquire();
}

} // namespace svm_gc
