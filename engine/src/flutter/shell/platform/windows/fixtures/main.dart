// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:convert';
import 'dart:io' as io;
import 'dart:typed_data' show ByteData, Uint8List;
import 'dart:ui' as ui;

// Signals a waiting latch in the native test.
@pragma('vm:external-name', 'Signal')
external void signal();

// Signals a waiting latch in the native test, passing a boolean value.
@pragma('vm:external-name', 'SignalBoolValue')
external void signalBoolValue(bool value);

// Signals a waiting latch in the native test, passing a string value.
@pragma('vm:external-name', 'SignalStringValue')
external void signalStringValue(String value);

// Signals a waiting latch in the native test, which returns a value to the fixture.
@pragma('vm:external-name', 'SignalBoolReturn')
external bool signalBoolReturn();

void main() {}

@pragma('vm:entry-point')
void hiPlatformChannels() {
  ui.channelBuffers.setListener('hi', (
    ByteData? data,
    ui.PlatformMessageResponseCallback callback,
  ) async {
    ui.PlatformDispatcher.instance.sendPlatformMessage('hi', data, (ByteData? reply) {
      ui.PlatformDispatcher.instance.sendPlatformMessage('hi', reply, (ByteData? reply) {});
    });
    callback(data);
  });
}

@pragma('vm:entry-point')
Future<void> exitTestExit() async {
  final closed = Completer<ByteData?>();
  ui.channelBuffers.setListener('flutter/platform', (
    ByteData? data,
    ui.PlatformMessageResponseCallback callback,
  ) async {
    final String jsonString = json.encode(<Map<String, String>>[
      {'response': 'exit'},
    ]);
    final responseData = ByteData.sublistView(utf8.encode(jsonString));
    callback(responseData);
    closed.complete(data);
  });
  await closed.future;
}

@pragma('vm:entry-point')
Future<void> exitTestCancel() async {
  final closed = Completer<ByteData?>();
  ui.channelBuffers.setListener('flutter/platform', (
    ByteData? data,
    ui.PlatformMessageResponseCallback callback,
  ) async {
    final String jsonString = json.encode(<Map<String, String>>[
      {'response': 'cancel'},
    ]);
    final responseData = ByteData.sublistView(utf8.encode(jsonString));
    callback(responseData);
    closed.complete(data);
  });
  await closed.future;

  // Because the request was canceled, the below shall execute.
  final exited = Completer<ByteData?>();
  final String jsonString = json.encode(<String, dynamic>{
    'method': 'System.exitApplication',
    'args': <String, dynamic>{'type': 'required', 'exitCode': 0},
  });
  ui.PlatformDispatcher.instance.sendPlatformMessage(
    'flutter/platform',
    ByteData.sublistView(utf8.encode(jsonString)),
    (ByteData? reply) {
      exited.complete(reply);
    },
  );
  await exited.future;
}

@pragma('vm:entry-point')
void customEntrypoint() {}

@pragma('vm:entry-point')
void verifyNativeFunction() {
  signal();
}

@pragma('vm:entry-point')
void verifyNativeFunctionWithParameters() {
  signalBoolValue(true);
}

@pragma('vm:entry-point')
void verifyNativeFunctionWithReturn() {
  final bool value = signalBoolReturn();
  signalBoolValue(value);
}

@pragma('vm:entry-point')
void readPlatformExecutable() {
  signalStringValue(io.Platform.executable);
}

@pragma('vm:entry-point')
void mergedUIThread() {
  signal();
}

@pragma('vm:external-name', 'NotifyEngineId')
external void notifyEngineId(int? handle);

@pragma('vm:entry-point')
void testEngineId() {
  notifyEngineId(ui.PlatformDispatcher.instance.engineId);
}

@pragma('vm:entry-point')
void testWindowController() {
  signal();
}
