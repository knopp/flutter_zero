// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:math' as math;
import 'dart:typed_data';

import 'package:ui/ui.dart' as ui;

import 'dom.dart';
import 'vector_math.dart';

final Float32List _tempRectData = Float32List(4);

/// Transforms a [ui.Rect] given the effective [transform].
///
/// The resulting rect is aligned to the pixel grid, i.e. two of
/// its sides are vertical and two are horizontal. In the presence of rotations
/// the rectangle is inflated such that it fits the rotated rectangle.
ui.Rect transformRectWithMatrix(Matrix4 transform, ui.Rect rect) {
  _tempRectData[0] = rect.left;
  _tempRectData[1] = rect.top;
  _tempRectData[2] = rect.right;
  _tempRectData[3] = rect.bottom;
  transformLTRB(transform, _tempRectData);
  return ui.Rect.fromLTRB(_tempRectData[0], _tempRectData[1], _tempRectData[2], _tempRectData[3]);
}

/// Temporary storage for intermediate data used by [transformLTRB].
///
/// WARNING: do not use this outside [transformLTRB]. Sharing this variable in
/// other contexts will lead to bugs.
final Float32List _tempPointData = Float32List(16);
final Matrix4 _tempPointMatrix = Matrix4.fromFloat32List(_tempPointData);

/// Transforms a rectangle given the effective [transform].
///
/// This is the same as [transformRect], except that the rect is specified
/// in terms of left, top, right, and bottom edge offsets.
void transformLTRB(Matrix4 transform, Float32List ltrb) {
  // Construct a matrix where each row represents a vector pointing at
  // one of the four corners of the (left, top, right, bottom) rectangle.
  // Using the row-major order allows us to multiply the matrix in-place
  // by the transposed current transformation matrix. The vector_math
  // library has a convenience function `multiplyTranspose` that performs
  // the multiplication without copying. This way we compute the positions
  // of all four points in a single matrix-by-matrix multiplication at the
  // cost of one `Matrix4` instance and one `Float32List` instance.
  //
  // The rejected alternative was to use `Vector3` for each point and
  // multiply by the current transform. However, that would cost us four
  // `Vector3` instances, four `Float32List` instances, and four
  // matrix-by-vector multiplications.
  //
  // `Float32List` initializes the array with zeros, so we do not have to
  // fill in every single element.

  // Row 0: top-left
  _tempPointData[0] = ltrb[0];
  _tempPointData[4] = ltrb[1];
  _tempPointData[8] = 0;
  _tempPointData[12] = 1;

  // Row 1: top-right
  _tempPointData[1] = ltrb[2];
  _tempPointData[5] = ltrb[1];
  _tempPointData[9] = 0;
  _tempPointData[13] = 1;

  // Row 2: bottom-left
  _tempPointData[2] = ltrb[0];
  _tempPointData[6] = ltrb[3];
  _tempPointData[10] = 0;
  _tempPointData[14] = 1;

  // Row 3: bottom-right
  _tempPointData[3] = ltrb[2];
  _tempPointData[7] = ltrb[3];
  _tempPointData[11] = 0;
  _tempPointData[15] = 1;

  _tempPointMatrix.multiplyTranspose(transform);

  // Handle non-homogenous matrices.
  double w = transform[15];
  if (w == 0.0) {
    w = 1.0;
  }

  ltrb[0] =
      math.min(
        math.min(math.min(_tempPointData[0], _tempPointData[1]), _tempPointData[2]),
        _tempPointData[3],
      ) /
      w;
  ltrb[1] =
      math.min(
        math.min(math.min(_tempPointData[4], _tempPointData[5]), _tempPointData[6]),
        _tempPointData[7],
      ) /
      w;
  ltrb[2] =
      math.max(
        math.max(math.max(_tempPointData[0], _tempPointData[1]), _tempPointData[2]),
        _tempPointData[3],
      ) /
      w;
  ltrb[3] =
      math.max(
        math.max(math.max(_tempPointData[4], _tempPointData[5]), _tempPointData[6]),
        _tempPointData[7],
      ) /
      w;
}

/// Returns true if [rect] contains every point that is also contained by the
/// [other] rect.
///
/// Points on the edges of both rectangles are also considered. For example,
/// this returns true when the two rects are equal to each other.
bool rectContainsOther(ui.Rect rect, ui.Rect other) {
  return rect.left <= other.left &&
      rect.top <= other.top &&
      rect.right >= other.right &&
      rect.bottom >= other.bottom;
}

/// Prints a warning message to the console.
///
/// This function can be overridden in tests. This could be useful, for example,
/// to verify that warnings are printed under certain circumstances.
void Function(String) printWarning = domWindow.console.warn;

/// Ensure a "meta" tag with [name] and [content] is set on the page.
void ensureMetaTag(String name, String content) {
  final DomElement? existingTag = domDocument.querySelector('meta[name=$name][content=$content]');

  if (existingTag == null) {
    final DomHTMLMetaElement meta = createDomHTMLMetaElement()
      ..name = name
      ..content = content;
    domDocument.head!.append(meta);
  }
}
