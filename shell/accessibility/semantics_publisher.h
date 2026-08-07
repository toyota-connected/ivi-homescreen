/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SHELL_ACCESSIBILITY_SEMANTICS_PUBLISHER_H_
#define SHELL_ACCESSIBILITY_SEMANTICS_PUBLISHER_H_

#include <cstdint>

#include <shell/platform/embedder/embedder.h>

#include "ihs/ihs_semantics.h"

#include "accessibility_tree.h"
#include "semantics_translator.h"

namespace accessibility {

// 2D affine transform, the shape FlutterTransformation carries. Kept separate
// from the embedder type so the composition below is testable on its own and
// reads as arithmetic rather than as struct plumbing.
//
// Row-major:
//   | a c e |
//   | b d f |
//   | 0 0 1 |
// The embedder's perspective row is ignored: Flutter's semantics transforms
// are affine in practice, and a projective divide here would silently produce
// nonsense rects for the one case that is not.
struct Affine {
  double a = 1.0, b = 0.0, c = 0.0, d = 1.0, e = 0.0, f = 0.0;
};

// Reads the affine part of an embedder transform.
Affine FromFlutter(const FlutterTransformation& t);

// `outer` applied after `inner` -- the parent-to-screen transform composed with
// a child's node-to-parent one gives the child's node-to-screen.
Affine Concat(const Affine& outer, const Affine& inner);

// Maps a rect through a transform and returns the axis-aligned bounds of the
// result. All four corners are mapped rather than just two, because rotation
// or skew makes the min/max corners not the ones you started with.
IhsSemanticsRect MapRect(const Affine& t, const FlutterRect& rect);

// Maps a derived role onto the hub's role enum.
IhsSemanticsRole ToIhsRole(Role role);

// Maps a derived action set onto the hub's action bitmask. The bit values are
// the hub ABI's own and deliberately unrelated to the framework's, so this is
// a real translation and not a cast.
uint64_t ToIhsActions(const NodeSpec& spec);

// Maps a single hub action bit back onto its framework equivalent, for the
// dispatch direction. Returns 0 when the bit has no framework counterpart --
// the two enumerations are independent by design, so a gap is possible and
// must be reported rather than forwarded as an empty mask.
//
// Single bit only: the framework acts on one action at a time, and a caller
// passing several has asked for something that cannot be honored.
FlutterSemanticsAction ToFlutterSemanticsAction(uint64_t ihs_action);

// Walks `tree` from the root, composing transforms down the traversal path,
// and publishes the result to the semantics hub. Screen-space rects are
// computed here because this is the side that has the transforms; publishing
// parent-local rects would push the work onto every consumer and they would
// disagree about it.
//
// Nodes unreachable from the root are not published: the hub promises index 0
// is the root and that every child index resolves, and an orphan satisfies
// neither. Returns the hub status, or IHS_SEMANTICS_OK when the tree has no
// root yet and there is nothing to say.
int PublishTree(const AccessibilityTree& tree);

}  // namespace accessibility

#endif  // SHELL_ACCESSIBILITY_SEMANTICS_PUBLISHER_H_
