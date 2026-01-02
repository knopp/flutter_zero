// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// For documentation see https://github.com/flutter/flutter/blob/main/engine/src/flutter/lib/ui/painting.dart
part of ui;

class ImmutableBuffer {
  ImmutableBuffer._(this._length);
  static Future<ImmutableBuffer> fromUint8List(Uint8List list) async {
    final instance = ImmutableBuffer._(list.length);
    instance._list = list;
    return instance;
  }

  static Future<ImmutableBuffer> fromAsset(String assetKey) async {
    throw UnsupportedError('ImmutableBuffer.fromAsset is not supported on the web.');
  }

  static Future<ImmutableBuffer> fromFilePath(String path) async {
    throw UnsupportedError('ImmutableBuffer.fromFilePath is not supported on the web.');
  }

  Uint8List? _list;

  int get length => _length;
  final int _length;

  bool get debugDisposed {
    late bool disposed;
    assert(() {
      disposed = _list == null;
      return true;
    }());
    return disposed;
  }

  void dispose() => _list = null;
}
