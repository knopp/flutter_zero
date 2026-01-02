// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ignore_for_file: avoid_print

import 'dart:async';
import 'dart:convert' show json, utf8;
import 'dart:isolate';
import 'dart:typed_data';
import 'dart:ui';

void expect(Object? a, Object? b) {
  if (a != b) {
    throw AssertionError('Expected $a to == $b');
  }
}

void main() {}

@pragma('vm:entry-point')
void mainNotifyNative() {
  notifyNative();
}

@pragma('vm:entry-point')
void onErrorA() {
  PlatformDispatcher.instance.onError = (Object error, StackTrace? stack) {
    notifyErrorA(error.toString());
    return true;
  };
  Future<void>.delayed(const Duration(seconds: 2)).then((_) {
    throw Exception('I should be coming from A');
  });
}

@pragma('vm:entry-point')
void onErrorB() {
  PlatformDispatcher.instance.onError = (Object error, StackTrace? stack) {
    notifyErrorB(error.toString());
    return true;
  };
  throw Exception('I should be coming from B');
}

@pragma('vm:external-name', 'NotifyErrorA')
external void notifyErrorA(String message);
@pragma('vm:external-name', 'NotifyErrorB')
external void notifyErrorB(String message);

@pragma('vm:entry-point')
void emptyMain() {}

@pragma('vm:entry-point')
void fixturesAreFunctionalMain() {
  sayHiFromFixturesAreFunctionalMain();
}

@pragma('vm:external-name', 'SayHiFromFixturesAreFunctionalMain')
external void sayHiFromFixturesAreFunctionalMain();

@pragma('vm:entry-point')
@pragma('vm:external-name', 'NotifyNative')
external void notifyNative();

@pragma('vm:entry-point')
void thousandCallsToNative() {
  for (var i = 0; i < 1000; i++) {
    notifyNative();
  }
}

void secondaryIsolateMain(String message) {
  print('Secondary isolate got message: $message');
  notifyNative();
}

@pragma('vm:entry-point')
void testCanLaunchSecondaryIsolate() {
  Isolate.spawn(secondaryIsolateMain, 'Hello from root isolate.');
  notifyNative();
}

@pragma('vm:entry-point')
void testSkiaResourceCacheSendsResponse() {
  void callback(ByteData? data) {
    if (data == null) {
      throw AssertionError('Response must not be null.');
    }
    final String response = utf8.decode(data.buffer.asUint8List());
    final List<bool> jsonResponse = (json.decode(response) as List).cast<bool>();
    if (!jsonResponse[0]) {
      throw AssertionError('Response was not true');
    }
    notifyNative();
  }

  const jsonRequest = '''
{
  "method": "Skia.setResourceCacheMaxBytes",
  "args": 10000
}''';
  PlatformDispatcher.instance.sendPlatformMessage(
    'flutter/skia',
    ByteData.sublistView(utf8.encode(jsonRequest)),
    callback,
  );
}

@pragma('vm:entry-point')
void canAccessIsolateLaunchData() {
  notifyMessage(
    utf8.decode(PlatformDispatcher.instance.getPersistentIsolateData()!.buffer.asUint8List()),
  );
}

@pragma('vm:entry-point')
void performanceModeImpactsNotifyIdle() {
  notifyNativeBool(false);
  PlatformDispatcher.instance.requestDartPerformanceMode(DartPerformanceMode.latency);
  notifyNativeBool(true);
  PlatformDispatcher.instance.requestDartPerformanceMode(DartPerformanceMode.balanced);
}

@pragma('vm:external-name', 'NotifyMessage')
external void notifyMessage(String string);

@pragma('vm:external-name', 'NotifyLocalTime')
external void notifyLocalTime(String string);

@pragma('vm:external-name', 'WaitFixture')
external bool waitFixture();

// Return local date-time as a string, to an hour resolution.  So, "2020-07-23
// 14:03:22" will become "2020-07-23 14".
String localTimeAsString() {
  final DateTime now = DateTime.now().toLocal();
  // This is: "$y-$m-$d $h:$min:$sec.$ms$us";
  final timeStr = now.toString();
  // Forward only "$y-$m-$d $h" for timestamp comparison.  Not using DateTime
  // formatting since package:intl is not available.
  return timeStr.split(':')[0];
}

@pragma('vm:entry-point')
void localtimesMatch() {
  notifyLocalTime(localTimeAsString());
}

@pragma('vm:entry-point')
void timezonesChange() {
  do {
    notifyLocalTime(localTimeAsString());
  } while (waitFixture());
}

@pragma('vm:external-name', 'NotifyCanAccessResource')
external void notifyCanAccessResource(bool success);

@pragma('vm:external-name', 'NotifySetAssetBundlePath')
external void notifySetAssetBundlePath();

@pragma('vm:entry-point')
Future<void> canAccessResourceFromAssetDir() async {
  notifySetAssetBundlePath();
  PlatformDispatcher.instance.sendPlatformMessage(
    'flutter/assets',
    ByteData.sublistView(utf8.encode('kernel_blob.bin')),
    (ByteData? byteData) {
      notifyCanAccessResource(byteData != null);
    },
  );
}

@pragma('vm:external-name', 'NotifyNativeWhenEngineRun')
external void notifyNativeWhenEngineRun(bool success);

@pragma('vm:external-name', 'NotifyNativeWhenEngineSpawn')
external void notifyNativeWhenEngineSpawn(bool success);

@pragma('vm:entry-point')
void canReceiveArgumentsWhenEngineRun(List<String> args) {
  notifyNativeWhenEngineRun(args.length == 2 && args[0] == 'foo' && args[1] == 'bar');
}

@pragma('vm:entry-point')
void canReceiveArgumentsWhenEngineSpawn(List<String> args) {
  notifyNativeWhenEngineSpawn(args.length == 2 && args[0] == 'arg1' && args[1] == 'arg2');
}

@pragma('vm:entry-point')
void frameCallback(Object? image, int durationMilliseconds, String decodeError) {
  if (image == null) {
    throw Exception('Expeccted image in frame callback to be non-null');
  }
}

@pragma('vm:entry-point')
Future<void> included() async {}

Future<void> excluded() async {}

class IsolateParam {
  const IsolateParam(this.sendPort, this.rawHandle);
  final SendPort sendPort;
  final int rawHandle;
}

@pragma('vm:entry-point')
Future<void> runCallback(IsolateParam param) async {
  try {
    final func =
        PluginUtilities.getCallbackFromHandle(CallbackHandle.fromRawHandle(param.rawHandle))!
            as Future<dynamic> Function();
    await func.call();
    param.sendPort.send(true);
  } on NoSuchMethodError {
    param.sendPort.send(false);
  }
}

@pragma('vm:entry-point')
@pragma('vm:external-name', 'NotifyNativeBool')
external void notifyNativeBool(bool value);

@pragma('vm:entry-point')
Future<void> testPluginUtilitiesCallbackHandle() async {
  var port = ReceivePort();
  await Isolate.spawn(
    runCallback,
    IsolateParam(port.sendPort, PluginUtilities.getCallbackHandle(included)!.toRawHandle()),
    onError: port.sendPort,
  );
  final dynamic result1 = await port.first;
  if (result1 != true) {
    print('Expected $result1 to == true');
    notifyNativeBool(false);
    return;
  }
  port.close();
  if (const bool.fromEnvironment('dart.vm.product')) {
    port = ReceivePort();
    await Isolate.spawn(
      runCallback,
      IsolateParam(port.sendPort, PluginUtilities.getCallbackHandle(excluded)!.toRawHandle()),
      onError: port.sendPort,
    );
    final dynamic result2 = await port.first;
    if (result2 != false) {
      print('Expected $result2 to == false');
      notifyNativeBool(false);
      return;
    }
    port.close();
  }
  notifyNativeBool(true);
}

@pragma('vm:entry-point')
Future<void> testThatAssetLoadingHappensOnWorkerThread() async {
  try {
    await ImmutableBuffer.fromAsset('DoesNotExist');
  } catch (err) {
    /* Do nothing */
  }

  notifyNative();
}

@pragma('vm:external-name', 'ReportEngineId')
external void _reportEngineId(int? identifier);

@pragma('vm:entry-point')
void providesEngineId() {
  _reportEngineId(PlatformDispatcher.instance.engineId);
}
