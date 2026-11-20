/*
 * Copyright (c) 2014, 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SVM_SHARE_GC_SHENANDOAH_SHENANDOAHCODEROOTSET_HPP
#define SVM_SHARE_GC_SHENANDOAH_SHENANDOAHCODEROOTSET_HPP

#include "code/codeCache.hpp"
#include "utilities/globalDefinitions.hpp"

namespace svm_gc {

class ShenandoahCodeRootSetHashTable;
class nmethod;

// NOTE: copy of G1CodeRootSet.
//
// We need this copy because SVM is missing method entry barriers and can't walk thread
// stacks concurrently which makes it impossible to use Shenandoah's HotSpot code for nmethod
// handling.
//
// The set of nmethods (runtime-compiled code) that reference objects in a Shenandoah heap region
// (the "code roots" of the region, see ShenandoahHeapRegion::add_code_root()). Like in G1, this
// allows the GC to only process the nmethods that reference objects in the collection set when it
// evacuates objects or updates references, instead of walking all nmethods.
// This class is thread safe.
class ShenandoahCodeRootSet {
  ShenandoahCodeRootSetHashTable* _table;
  DEBUG_ONLY(mutable bool _is_iterating;)

 public:
  ShenandoahCodeRootSet();
  ~ShenandoahCodeRootSet();

  void add(nmethod* method);
  bool remove(nmethod* method);
  bool contains(nmethod* method);
  void clear();

  // Prepare for MT iteration. Must be called before nmethods_do.
  void reset_table_scanner();
  void nmethods_do(NMethodClosure* blk) const;

  bool is_empty() { return length() == 0;}

  // Length in elements
  size_t length() const;

  // Memory size in bytes taken by this set.
  size_t mem_size();
};

} // namespace svm_gc

#endif // SVM_SHARE_GC_SHENANDOAH_SHENANDOAHCODEROOTSET_HPP
