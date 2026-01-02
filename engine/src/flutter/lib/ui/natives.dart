// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
part of dart.ui;

/// Helper functions for Dart Plugin Registrants.
abstract final class DartPluginRegistrant {
  static bool _wasInitialized = false;

  /// Makes sure the that the Dart Plugin Registrant has been called for this
  /// isolate. This can safely be executed multiple times on the same isolate,
  /// but should not be called on the Root isolate.
  static void ensureInitialized() {
    if (!_wasInitialized) {
      _wasInitialized = true;
      _ensureInitialized();
    }
  }

  @Native<Void Function()>(symbol: 'DartPluginRegistrant_EnsureInitialized')
  external static void _ensureInitialized();
}

// Corelib 'print' implementation.
void _print(String arg) {
  _Logger._printString(arg);
}

void _printDebug(String arg) {
  _Logger._printDebugString(arg);
}

class _Logger {
  @Native<Void Function(Handle)>(symbol: 'DartRuntimeHooks::Logger_PrintString')
  external static void _printString(String? s);

  @Native<Void Function(Handle)>(symbol: 'DartRuntimeHooks::Logger_PrintDebugString')
  external static void _printDebugString(String? s);
}

// If we actually run on big endian machines, we'll need to do something smarter
// here. We don't use [Endian.Host] because it's not a compile-time
// constant and can't propagate into the set/get calls.
const Endian _kFakeHostEndian = Endian.little;

const bool _kReleaseMode = bool.fromEnvironment('dart.vm.product');

@Native<Void Function(Handle)>(symbol: 'DartRuntimeHooks::ScheduleMicrotask')
external void _scheduleMicrotask(void Function() callback);

@Native<Handle Function(Handle)>(symbol: 'DartRuntimeHooks::GetCallbackHandle')
external int? _getCallbackHandle(Function closure);

@Native<Handle Function(Int64)>(symbol: 'DartRuntimeHooks::GetCallbackFromHandle')
external Function? _getCallbackFromHandle(int handle);

typedef _PrintClosure = void Function(String line);

// Used by the embedder to initialize how printing is performed.
// See also https://github.com/dart-lang/sdk/blob/main/sdk/lib/_internal/vm/lib/print_patch.dart
@pragma('vm:entry-point')
_PrintClosure _getPrintClosure() => _print;

typedef _ScheduleImmediateClosure = void Function(void Function());

// Used by the embedder to initialize how microtasks are scheduled.
// See also https://github.com/dart-lang/sdk/blob/main/sdk/lib/_internal/vm/lib/schedule_microtask_patch.dart
@pragma('vm:entry-point')
_ScheduleImmediateClosure _getScheduleMicrotaskClosure() => _scheduleMicrotask;
