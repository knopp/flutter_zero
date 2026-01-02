// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
part of dart.ui;

/// Generic callback signature, used by [_futurize].
typedef _Callback<T> = void Function(T result);

/// Signature for a method that receives a [_Callback].
///
/// Return value should be null on success, and a string error message on
/// failure.
typedef _Callbacker<T> = String? Function(_Callback<T?> callback);

// Converts a method that receives a value-returning callback to a method that
// returns a Future.
//
// Return a [String] to cause an [Exception] to be synchronously thrown with
// that string as a message.
//
// If the callback is called with null, the future completes with an error.
//
// Example usage:
//
// ```dart
// typedef IntCallback = void Function(int result);
//
// String? _doSomethingAndCallback(IntCallback callback) {
//   Timer(const Duration(seconds: 1), () { callback(1); });
// }
//
// Future<int> doSomething() {
//   return _futurize(_doSomethingAndCallback);
// }
// ```
//
// This function is private and so not directly tested. Instead, an exact copy of it
// has been inlined into the test at lib/ui/fixtures/ui_test.dart. if you change this
// function, then you must update the test.
//
// TODO(ianh): We should either automate the code duplication or just make it public.
Future<T> _futurize<T>(_Callbacker<T> callbacker) {
  final completer = Completer<T>.sync();
  // If the callback synchronously throws an error, then synchronously
  // rethrow that error instead of adding it to the completer. This
  // prevents the Zone from receiving an uncaught exception.
  var isSync = true;
  final String? error = callbacker((T? t) {
    if (t == null) {
      if (isSync) {
        throw Exception('operation failed');
      } else {
        completer.completeError(Exception('operation failed'));
      }
    } else {
      completer.complete(t);
    }
  });
  isSync = false;
  if (error != null) {
    throw Exception(error);
  }
  return completer.future;
}

/// A handle to a read-only byte buffer that is managed by the engine.
///
/// The creator of this object is responsible for calling [dispose] when it is
/// no longer needed.
base class ImmutableBuffer extends NativeFieldWrapperClass1 {
  ImmutableBuffer._(this._length);

  /// Creates a copy of the data from a [Uint8List] suitable for internal use
  /// in the engine.
  static Future<ImmutableBuffer> fromUint8List(Uint8List list) {
    final instance = ImmutableBuffer._(list.length);
    return _futurize((_Callback<void> callback) {
      return instance._init(list, callback);
    }).then((_) => instance);
  }

  /// Create a buffer from the asset with key [assetKey].
  ///
  /// Throws an [Exception] if the asset does not exist.
  static Future<ImmutableBuffer> fromAsset(String assetKey) {
    // The flutter tool converts all asset keys with spaces into URI
    // encoded paths (replacing ' ' with '%20', for example). We perform
    // the same encoding here so that users can load assets with the same
    // key they have written in the pubspec.
    final String encodedKey = Uri(path: Uri.encodeFull(assetKey)).path;
    final instance = ImmutableBuffer._(0);
    return _futurize((_Callback<int> callback) {
      return instance._initFromAsset(encodedKey, callback);
    }).then((int length) {
      if (length == -1) {
        throw Exception('Asset not found');
      }
      return instance.._length = length;
    });
  }

  /// Create a buffer from the file with [path].
  ///
  /// Throws an [Exception] if the asset does not exist.
  static Future<ImmutableBuffer> fromFilePath(String path) {
    final instance = ImmutableBuffer._(0);
    return _futurize((_Callback<int> callback) {
      return instance._initFromFile(path, callback);
    }).then((int length) {
      if (length == -1) {
        throw Exception('Could not load file at $path.');
      }
      return instance.._length = length;
    });
  }

  @Native<Handle Function(Handle, Handle, Handle)>(symbol: 'ImmutableBuffer::init')
  external String? _init(Uint8List list, _Callback<void> callback);

  @Native<Handle Function(Handle, Handle, Handle)>(symbol: 'ImmutableBuffer::initFromAsset')
  external String? _initFromAsset(String assetKey, _Callback<int> callback);

  @Native<Handle Function(Handle, Handle, Handle)>(symbol: 'ImmutableBuffer::initFromFile')
  external String? _initFromFile(String assetKey, _Callback<int> callback);

  /// The length, in bytes, of the underlying data.
  int get length => _length;
  int _length;

  bool _debugDisposed = false;

  /// Whether [dispose] has been called.
  ///
  /// This must only be used when asserts are enabled. Otherwise, it will throw.
  bool get debugDisposed {
    late bool disposed;
    assert(() {
      disposed = _debugDisposed;
      return true;
    }());
    return disposed;
  }

  /// Release the resources used by this object. The object is no longer usable
  /// after this method is called.
  ///
  /// The underlying memory allocated by this object will be retained beyond
  /// this call if it is still needed by another object that has not been
  /// disposed. For example, an [ImageDescriptor] that has not been disposed
  /// may still retain a reference to the memory from this buffer even if it
  /// has been disposed. Freeing that memory requires disposing all resources
  /// that may still hold it.
  void dispose() {
    assert(() {
      assert(!_debugDisposed);
      _debugDisposed = true;
      return true;
    }());
    _dispose();
  }

  /// This can't be a leaf call because the native function calls Dart API
  /// (Dart_SetNativeInstanceField).
  @Native<Void Function(Pointer<Void>)>(symbol: 'ImmutableBuffer::dispose')
  external void _dispose();
}
