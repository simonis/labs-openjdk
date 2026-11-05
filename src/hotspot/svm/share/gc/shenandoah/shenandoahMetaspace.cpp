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

#include "gc/shenandoah/shenandoahMetaspace.hpp"
#include "svmMetaspace.hpp"

namespace svm_gc {

size_t ShenandoahMetaspace::_used = 0;
size_t ShenandoahMetaspace::_used_at_last_gc = 0;

void ShenandoahMetaspace::initialize() {
  _used = 0;
  _used_at_last_gc = 0;
  SVMMetaspace::set_metaspace(this);
}

void ShenandoahMetaspace::update_used_at_last_gc() {
  _used_at_last_gc = _used;
}

bool ShenandoahMetaspace::is_in_allocated_memory(const void* address) const {
  // Shenandoah never allocates in metaspace (runtime class loading is not supported yet), so no
  // address is ever part of allocated metaspace.
  return false;
}

} // namespace svm_gc
