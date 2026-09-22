/*
 * Copyright (c) 1998, 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_RUNTIME_VMTHREAD_HPP
#define SHARE_RUNTIME_VMTHREAD_HPP

#include "runtime/atomic.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/perfDataTypes.hpp"
#include "runtime/nonJavaThread.hpp"
#include "runtime/task.hpp"
#include "runtime/vmOperation.hpp"


namespace svm_gc {

class VMThread {
 private:
  void evaluate_operation(VM_Operation* op);;
  // VM_Operation support
  static VM_Operation* _cur_vm_operation; // Current VM operation

 public:
  // Returns the current vm operation if any.
  static VM_Operation* vm_operation()             {
    assert(Thread::current()->is_VM_thread(), "Must be");
    return _cur_vm_operation;
  }

  // Sets the currently executing VM operation and returns the previous one.
  // Must only be called on the VM operation thread, around the actual execution
  // of the operation (see svm_gc_execute_vm_operation_main). Using save/restore
  // here (instead of in VMThread::execute, which may run on a queuing thread)
  // keeps _cur_vm_operation correct even when the VM operation thread executes a
  // nested VM operation (e.g. a GC triggered by an allocation failure).
  static VM_Operation* set_current_vm_operation(VM_Operation* op) {
    assert(Thread::current()->is_VM_thread(), "Must be");
    VM_Operation* prev = _cur_vm_operation;
    _cur_vm_operation = op;
    return prev;
  }

  static void restore_current_vm_operation(VM_Operation* prev) {
    assert(Thread::current()->is_VM_thread(), "Must be");
    _cur_vm_operation = prev;
  }

  // Execution of vm operation
  static void execute(VM_Operation* op);
};


} // namespace svm_gc

#endif // SHARE_RUNTIME_VMTHREAD_HPP
