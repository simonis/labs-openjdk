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

#include "svmMetaspace.hpp"

namespace svm_gc {

// Minimal metaspace for Shenandoah.
//
// Runtime class loading (and therefore an actually populated metaspace) is not supported for
// Shenandoah yet: the Native Image side only registers a Metaspace when RuntimeClassLoading is
// enabled, and in that case the reserved metaspace size that the collector receives is 0. This class
// exists only to satisfy the shared CollectedHeap interface, which (unlike before the G1 metaspace
// merge) requires every collector to register an SVMMetaspace. It reports a permanently empty
// metaspace. If/when runtime class loading is brought to Shenandoah, this needs a real
// implementation analogous to G1Metaspace.
class ShenandoahMetaspace : public SVMMetaspace {
 private:
  static size_t _used;
  static size_t _used_at_last_gc;

 public:
  void initialize();
  void update_used_at_last_gc() override;
  bool is_in_allocated_memory(const void* address) const override;

  static size_t used() { return _used; }
  static size_t used_at_last_gc() { return _used_at_last_gc; }
};

} // namespace svm_gc

#endif // SVM_SHARE_GC_SHENANDOAH_SHENANDOAHMETASPACE_HPP
