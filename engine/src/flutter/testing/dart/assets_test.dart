// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';
import 'dart:ui';

import 'package:test/test.dart';

void main() {
  test('Loading an asset that does not exist returns null', () async {
    Object? error;
    try {
      await ImmutableBuffer.fromAsset('ThisDoesNotExist');
    } catch (err) {
      error = err;
    }
    expect(error, isNotNull);
    expect(error is Exception, true);
  });

  test('Loading a file that does not exist returns null', () async {
    Object? error;
    try {
      await ImmutableBuffer.fromFilePath('ThisDoesNotExist');
    } catch (err) {
      error = err;
    }
    expect(error, isNotNull);
    expect(error is Exception, true);
  });

  test('returns the bytes of a bundled asset', () async {
    final ImmutableBuffer buffer = await ImmutableBuffer.fromAsset('DashInNooglerHat.jpg');

    expect(buffer.length == 354679, true);
  });

  test('returns the bytes of a file', () async {
    final ImmutableBuffer buffer = await ImmutableBuffer.fromFilePath(
      'flutter/lib/ui/fixtures/DashInNooglerHat.jpg',
    );

    expect(buffer.length == 354679, true);
  });

  test('Can load an asset with a space in the key', () async {
    // This assets actual path is "fixtures/DashInNooglerHat%20WithSpace.jpg"
    final ImmutableBuffer buffer = await ImmutableBuffer.fromAsset(
      'DashInNooglerHat WithSpace.jpg',
    );

    expect(buffer.length == 354679, true);
  });

  test('can dispose immutable buffer', () async {
    final ImmutableBuffer buffer = await ImmutableBuffer.fromAsset('DashInNooglerHat.jpg');

    buffer.dispose();
  });
}
