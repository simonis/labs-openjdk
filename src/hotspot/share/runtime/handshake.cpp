/*
 * Copyright (c) 2017, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "runtime/handshake.hpp"
#include "runtime/thread.hpp"
#include "runtime/threadSMR.hpp"
#include "runtime/vmOperation.hpp"
#include "runtime/vmOperations.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/debug.hpp"

namespace svm_gc {

#ifdef SVM

// Substrate VM does not support JDK's thread-local handshakes (JEP 312). We
// therefore use the same workaround that JDK 11 used for platforms without
// thread-local handshake support (i.e. -XX:-ThreadLocalHandshakes): a regular
// VM operation that brings all threads to a full safepoint and then applies the
// HandshakeClosure to every (Java) thread. See VM_HandshakeFallbackOperation in
// JDK 11's src/hotspot/share/runtime/handshake.cpp.
//
// Substrate VM provides its own JavaThreadIteratorWithHandle (see
// svm/share/runtime/threadSMR.hpp) which walks the isolate's thread list and
// asserts that it is only used at a safepoint. As this operation runs at a
// safepoint, the iteration loop below is identical to the JDK 11 original.
class VM_HandshakeFallbackOperation : public VM_Operation {
private:
  HandshakeClosure* const _handshake_cl;
  Thread* const          _target_thread;
  const bool             _all_threads;
  bool                   _thread_alive;

public:
  VM_HandshakeFallbackOperation(HandshakeClosure* hs_cl) :
    _handshake_cl(hs_cl), _target_thread(nullptr), _all_threads(true), _thread_alive(false) {}
  VM_HandshakeFallbackOperation(HandshakeClosure* hs_cl, Thread* target) :
    _handshake_cl(hs_cl), _target_thread(target), _all_threads(false), _thread_alive(false) {}

  void doit() override {
    for (JavaThreadIteratorWithHandle jtiwh; JavaThread* t = jtiwh.next(); ) {
      if (_all_threads || t == _target_thread) {
        if (t == _target_thread) {
          _thread_alive = true;
        }
        _handshake_cl->do_thread(t);
      }
    }
  }

  VMOp_Type type() const override { return VMOp_HandshakeFallback; }
  bool thread_alive() const { return _thread_alive; }
};

#endif // SVM

void Handshake::execute(HandshakeClosure* hs_cl) {
#ifdef SVM
  VM_HandshakeFallbackOperation op(hs_cl);
  VMThread::execute(&op);
#else
  Unimplemented();
#endif // SVM
}

} // namespace svm_gc
