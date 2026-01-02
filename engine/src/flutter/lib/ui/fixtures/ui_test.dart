// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:typed_data';
import 'dart:ui';
import 'dart:isolate';
import 'dart:ffi' hide Size;

void main() {}

/// Mutiple tests use this to signal to the C++ side that they are ready for
/// validation.
@pragma('vm:external-name', 'Finish')
external void _finish();

@pragma('vm:entry-point')
void customOnErrorTrue() {
  PlatformDispatcher.instance.onError = (Object error, StackTrace? stack) {
    _finish();
    return true;
  };
  throw Exception('true');
}

@pragma('vm:entry-point')
void customOnErrorFalse() {
  PlatformDispatcher.instance.onError = (Object error, StackTrace? stack) {
    _finish();
    return false;
  };
  throw Exception('false');
}

@pragma('vm:entry-point')
void customOnErrorThrow() {
  PlatformDispatcher.instance.onError = (Object error, StackTrace? stack) {
    _finish();
    throw Exception('throw2');
  };
  throw Exception('throw1');
}

@pragma('vm:entry-point')
void setLatencyPerformanceMode() {
  PlatformDispatcher.instance.requestDartPerformanceMode(DartPerformanceMode.latency);
  _finish();
}

@pragma('vm:entry-point')
void platformMessagePortResponseTest() async {
  ReceivePort receivePort = ReceivePort();
  _callPlatformMessageResponseDartPort(receivePort.sendPort.nativePort);
  List<dynamic> resultList = await receivePort.first;
  int identifier = resultList[0] as int;
  Uint8List? bytes = resultList[1] as Uint8List?;
  ByteData result = ByteData.sublistView(bytes!);
  if (result.lengthInBytes == 100) {
    _finishCallResponse(true);
  } else {
    _finishCallResponse(false);
  }
}

@pragma('vm:entry-point')
void platformMessageResponseTest() {
  _callPlatformMessageResponseDart((ByteData? result) {
    if (result is ByteData && result.lengthInBytes == 100) {
      int value = result.getInt8(0);
      bool didThrowOnModify = false;
      try {
        result.setInt8(0, value);
      } catch (e) {
        didThrowOnModify = true;
      }
      // This should be a read only buffer.
      _finishCallResponse(didThrowOnModify);
    } else {
      _finishCallResponse(false);
    }
  });
}

@pragma('vm:external-name', 'CallPlatformMessageResponseDartPort')
external void _callPlatformMessageResponseDartPort(int port);
@pragma('vm:external-name', 'CallPlatformMessageResponseDart')
external void _callPlatformMessageResponseDart(void Function(ByteData? result) callback);
@pragma('vm:external-name', 'FinishCallResponse')
external void _finishCallResponse(bool didPass);

@pragma('vm:entry-point')
void messageCallback(dynamic data) {}

@pragma('vm:entry-point')
void hooksTests() async {
  Future<void> test(String name, FutureOr<void> Function() testFunction) async {
    try {
      await testFunction();
    } catch (e) {
      print('Test "$name" failed!');
      rethrow;
    }
  }

  void expectEquals(Object? value, Object? expected) {
    if (value != expected) {
      throw 'Expected $value to be $expected.';
    }
  }

  void expectIdentical(Object a, Object b) {
    if (!identical(a, b)) {
      throw 'Expected $a to be identical to $b.';
    }
  }

  void expectNotEquals(Object? value, Object? expected) {
    if (value == expected) {
      throw 'Expected $value to not be $expected.';
    }
  }

  await test('onError preserves the callback zone', () {
    late Zone originalZone;
    late Zone callbackZone;
    final Object error = Exception('foo');
    StackTrace? stackTrace;

    runZoned(() {
      originalZone = Zone.current;
      PlatformDispatcher.instance.onError = (Object exception, StackTrace? stackTrace) {
        callbackZone = Zone.current;
        expectIdentical(exception, error);
        expectNotEquals(stackTrace, null);
        return true;
      };
    });

    _callHook('_onError', 2, error, StackTrace.current);
    PlatformDispatcher.instance.onError = null;
    expectIdentical(originalZone, callbackZone);
  });

  await test(
    'PlatformDispatcher.locale returns unknown locale when locales is set to empty list',
    () {
      late Locale locale;
      int callCount = 0;
      runZoned(() {
        PlatformDispatcher.instance.onLocaleChanged = () {
          locale = PlatformDispatcher.instance.locale;
          callCount += 1;
        };
      });

      const Locale fakeLocale = Locale.fromSubtags(
        languageCode: '1',
        countryCode: '2',
        scriptCode: '3',
      );
      _callHook('_updateLocales', 1, <String>[
        fakeLocale.languageCode,
        fakeLocale.countryCode!,
        fakeLocale.scriptCode!,
        '',
      ]);
      if (callCount != 1) {
        throw 'Expected 1 call, have $callCount';
      }
      if (locale != fakeLocale) {
        throw 'Expected $locale to match $fakeLocale';
      }
      _callHook('_updateLocales', 1, <String>[]);
      if (callCount != 2) {
        throw 'Expected 2 calls, have $callCount';
      }

      if (locale != const Locale.fromSubtags()) {
        throw '$locale did not equal ${Locale.fromSubtags()}';
      }
      if (locale.languageCode != 'und') {
        throw '${locale.languageCode} did not equal "und"';
      }
    },
  );

  await test('deprecated region equals', () {
    // These are equal because ZR is deprecated and was mapped to CD.
    const Locale x = Locale('en', 'ZR');
    const Locale y = Locale('en', 'CD');
    expectEquals(x, y);
    expectEquals(x.countryCode, y.countryCode);
  });

  await test('onLocaleChanged preserves callback zone', () {
    late Zone innerZone;
    late Zone runZone;
    Locale? locale;

    runZoned(() {
      innerZone = Zone.current;
      PlatformDispatcher.instance.onLocaleChanged = () {
        runZone = Zone.current;
        locale = PlatformDispatcher.instance.locale;
      };
    });

    _callHook('_updateLocales', 1, <String>['en', 'US', '', '']);
    expectIdentical(runZone, innerZone);
    expectEquals(locale, const Locale('en', 'US'));
  });

  await test('_futureize handles callbacker sync error', () async {
    String? callbacker(void Function(Object? arg) cb) {
      return 'failure';
    }

    Object? error;
    try {
      await _futurize(callbacker);
    } catch (err) {
      error = err;
    }
    expectNotEquals(error, null);
  });

  await test('_futureize does not leak sync uncaught exceptions into the zone', () async {
    String? callbacker(void Function(Object? arg) cb) {
      cb(null); // indicates failure
    }

    Object? error;
    try {
      await _futurize(callbacker);
    } catch (err) {
      error = err;
    }
    expectNotEquals(error, null);
  });

  await test('_futureize does not leak async uncaught exceptions into the zone', () async {
    String? callbacker(void Function(Object? arg) cb) {
      Timer.run(() {
        cb(null); // indicates failure
      });
    }

    Object? error;
    try {
      await _futurize(callbacker);
    } catch (err) {
      error = err;
    }
    expectNotEquals(error, null);
  });

  await test('_futureize successfully returns a value sync', () async {
    String? callbacker(void Function(Object? arg) cb) {
      cb(true);
    }

    final Object? result = await _futurize(callbacker);

    expectEquals(result, true);
  });

  await test('onPlatformMessage preserves callback zone', () {
    late Zone innerZone;
    late Zone runZone;
    late String name;

    runZoned(() {
      innerZone = Zone.current;
      PlatformDispatcher.instance.onPlatformMessage = (String value, _, __) {
        runZone = Zone.current;
        name = value;
      };
    });

    _callHook('_dispatchPlatformMessage', 3, 'testName', null, 123456789);
    expectIdentical(runZone, innerZone);
    expectEquals(name, 'testName');
  });

  await test('_futureize successfully returns a value async', () async {
    String? callbacker(void Function(Object? arg) cb) {
      Timer.run(() {
        cb(true);
      });
    }

    final Object? result = await _futurize(callbacker);

    expectEquals(result, true);
  });

  await test('root isolate token', () async {
    if (RootIsolateToken.instance == null) {
      throw Exception('We should have a token on a root isolate.');
    }
    ReceivePort receivePort = ReceivePort();
    Isolate.spawn(_backgroundRootIsolateTestMain, receivePort.sendPort);
    bool didPass = await receivePort.first as bool;
    if (!didPass) {
      throw Exception('Background isolate found a root isolate id.');
    }
  });

  await test('send port message without registering', () async {
    ReceivePort receivePort = ReceivePort();
    Isolate.spawn(_backgroundIsolateSendWithoutRegistering, receivePort.sendPort);
    bool didError = await receivePort.first as bool;
    if (!didError) {
      throw Exception(
        'Expected an error when not registering a root isolate and sending port messages.',
      );
    }
  });

  _finish();
}

/// Sends `true` on [port] if the isolate executing the function is not a root
/// isolate.
void _backgroundRootIsolateTestMain(SendPort port) {
  port.send(RootIsolateToken.instance == null);
}

/// Sends `true` on [port] if [PlatformDispatcher.sendPortPlatformMessage]
/// throws an exception without calling
/// [PlatformDispatcher.registerBackgroundIsolate].
void _backgroundIsolateSendWithoutRegistering(SendPort port) {
  bool didError = false;
  ReceivePort messagePort = ReceivePort();
  try {
    PlatformDispatcher.instance.sendPortPlatformMessage('foo', null, 1, messagePort.sendPort);
  } catch (_) {
    didError = true;
  }
  port.send(didError);
}

typedef _Callback<T> = void Function(T result);
typedef _Callbacker<T> = String? Function(_Callback<T?> callback);

// This is an exact copy of the function defined in painting.dart. If you change either
// then you must change both.
Future<T> _futurize<T>(_Callbacker<T> callbacker) {
  final Completer<T> completer = Completer<T>.sync();
  // If the callback synchronously throws an error, then synchronously
  // rethrow that error instead of adding it to the completer. This
  // prevents the Zone from receiving an uncaught exception.
  bool sync = true;
  final String? error = callbacker((T? t) {
    if (t == null) {
      if (sync) {
        throw Exception('operation failed');
      } else {
        completer.completeError(Exception('operation failed'));
      }
    } else {
      completer.complete(t);
    }
  });
  sync = false;
  if (error != null) throw Exception(error);
  return completer.future;
}

@pragma('vm:external-name', 'CallHook')
external void _callHook(
  String name, [
  int argCount = 0,
  Object? arg0,
  Object? arg1,
  Object? arg2,
  Object? arg3,
  Object? arg4,
  Object? arg5,
  Object? arg6,
  Object? arg8,
  Object? arg9,
  Object? arg10,
  Object? arg11,
  Object? arg12,
  Object? arg13,
  Object? arg14,
  Object? arg15,
  Object? arg16,
  Object? arg17,
  Object? arg18,
  Object? arg19,
  Object? arg20,
  Object? arg21,
  Object? arg22,
  Object? arg23,
  Object? arg24,
  Object? arg25,
]);
