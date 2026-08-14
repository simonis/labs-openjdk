/*
 * Copyright (c) 2020, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "code/compressedStream.hpp"
#include "compiler/oopMap.hpp"
#include "exports/sharedGCStructs.h"
#include "svmCodeReferenceMapDecoder.hpp"

/**
 * Code reference maps in SVM support negative offsets and use a form of delta-encoding where the distance (gap) between
 * reference map entries is encoded. n references that are adjacent to each other and that have the same size are encoded
 * as one run with a count of n.
 *
 * - The reference map [8, 16, 32, 40, 64] gets encoded as follows, assuming that it only contains uncompressed references
 *   and no derived references:
 *   - 8, 16 will be encoded as a run with gap=8 and count=2
 *   - 32, 40 will be encoded as a run with gap=8 and count=2
 *   - 64 will be encoded as a run with gap=16 and count=1
 * - If a reference is used as a base for derived references then that base reference gets encoded in a separate run
 *   together with all derived offsets. However, the encoding for such a run is different from the normal encoding.
 */

namespace svm_gc {

void SVMCodeReferenceMapDecoder::walk_offsets_from_pointer(u_char *base_address, u_char *encoded_reference_map, jlong reference_map_index, OopClosure *f) {
  assert(encoded_reference_map != nullptr, "must be");
  assert(reference_map_index >= 0, "must be");
  assert(f != nullptr, "must be");

  int uncompressed_size = oopSize;
  int compressed_size = heapOopSize;

  // the logic below was ported from the Java method CodeReferenceMapDecoder.walkOffsetsFromPointer
  CompressedReadStream stream(encoded_reference_map, reference_map_index);
  u_char *obj_ref = base_address;
  bool first_run = true;
  while (true) {
    // Size of gap in bytes (negative means the next pointer has derived pointers)
    jlong gap = stream.read_signed_int();
    // Number of pointers (sign distinguishes between compression and uncompression)
    jlong count = stream.read_signed_int();

    if (gap == 0 && count == 0) {
      break; // reached end of table
    }

    bool derived = false;
    if (!first_run && gap < 0) {
      /* Derived pointer run */
      gap = -(gap + 1);
      derived = true;
    }
    first_run = false;

    obj_ref += (size_t)gap;
    bool compressed = (count < 0);
    size_t ref_size = compressed ? compressed_size : uncompressed_size;
    count = (count < 0) ? -count : count;

    if (derived) {
      /*
       * Derived-pointer run.
       *
       * Stream layout: a run with a negative gap describes ONE base-reference slot (at obj_ref
       * after applying the gap), immediately followed in the stream by 'count' signed slot
       * distances. Each distance identifies another stack slot, relative to the base slot's
       * location, holding a pointer DERIVED from the base reference: an interior address such as
       * 'base + arrayHeader + i * elementSize' that compiled code kept live across the safepoint.
       * (So unlike a normal run, 'count' is the number of derived offsets, not reference slots.)
       *
       * A derived slot must not be visited as if it held an object reference - it points into
       * the middle of an object. Instead, when the base object moves, the derived slot must
       * shift by the same displacement. This follows HotSpot's three-step protocol:
       *
       *   1. DerivedPointerTable::add(derived_slot, base_slot) runs while BOTH slots still hold
       *      their old values and records the interior offset (*derived_slot - *base_slot).
       *   2. The base slot is visited (f->do_oop below), which may rewrite it to the object's
       *      new address.
       *   3. After all base slots are updated, DerivedPointerTable::update_pointers() rewrites
       *      each derived slot as *base_slot + recorded offset, re-reading the base slot to pick
       *      up its NEW value.
       *
       * Hence two rules in the code below: add() must be called BEFORE do_oop() on the base slot
       * (step 1 needs the old base value), and both receive SLOT ADDRESSES rather than values
       * (step 3 must re-read the updated base slot).
       *
       * Stream synchronization: the 'count' derived distances are part of the encoded stream and
       * must ALWAYS be consumed, even when the DerivedPointerTable is inactive (walks that do not
       * move objects, e.g. stack scans for concurrent marking). Skipping them would desynchronize
       * the decoder from the encoding grammar and misparse the remainder of this frame's
       * reference map, making the GC treat arbitrary stack words as references.
       *
       * In HotSpot this protocol lives in the OopMap/frame machinery (a derived-oop closure
       * invoked during OopMapStream iteration). SubstrateVM walks frames through this decoder
       * instead, so the DerivedPointerTable calls are made directly here.
       */
      if (DerivedPointerTable::is_active()) {
        /* count in this case is the number of derived references for this base pointer */
        for (size_t d = 0; d < count; d++) {
          /* Offset in words from the base reference to the derived reference */
          jlong ref_offset = stream.read_signed_int();

          u_char *derived_ref;
          if (ref_offset >= 0) {
            derived_ref = obj_ref + ((size_t)ref_offset) * ref_size;
          } else {
            derived_ref = obj_ref - ((size_t)-ref_offset) * ref_size;
          }

          guarantee(!compressed, "Derived references must not be compressed.");
          DerivedPointerTable::add((derived_pointer*)derived_ref, (derived_base*)obj_ref);
        }
      } else {
        /* Even if the DerivedPointerTable is inactive, we must still consume the offsets from the stream */
        for (size_t d = 0; d < count; d++) {
          stream.read_signed_int();
        }
      }

      if (compressed) {
        f->do_oop((narrowOop*)obj_ref);
      } else {
        f->do_oop((oop*)obj_ref);
      }

      obj_ref += ref_size;
    } else {
      if (compressed) {
        for (size_t c = 0; c < count; c += 1) {
          f->do_oop((narrowOop*)obj_ref);
          obj_ref += ref_size;
        }
      } else {
        for (size_t c = 0; c < count; c += 1) {
          f->do_oop((oop*)obj_ref);
          obj_ref += ref_size;
        }
      }
    }
  }
}

} // namespace svm_gc

