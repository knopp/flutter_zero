// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ignore_for_file: avoid_print

import 'dart:io';
import 'dart:typed_data';
import 'dart:ui';

@pragma('vm:external-name', 'SignalNativeTest')
external void signalNativeTest();

void main() {}

@pragma('vm:entry-point')
void empty() {}

/// Notifies the test of a string value.
///
/// This is used to notify the native side of the test of a string value from
/// the Dart fixture under test.
@pragma('vm:external-name', 'NotifyStringValue')
external void notifyStringValue(String s);

@pragma('vm:entry-point')
void executableNameNotNull() {
  notifyStringValue(Platform.executable);
}

@pragma('vm:entry-point')
void canLogToStdout() {
  // Emit hello world message to output then signal the test.
  print('Hello logging');
  signalNativeTest();
}

@pragma('vm:entry-point')
void nativeCallback() {
  signalNativeTest();
}

@pragma('vm:entry-point')
void sendFooMessage() {
  PlatformDispatcher.instance.sendPlatformMessage('foo', null, (ByteData? result) {});
}

@pragma('vm:external-name', 'NotifyEngineId')
external void notifyEngineId(int? engineId);

@pragma('vm:entry-point')
void testEngineId() {
  notifyEngineId(PlatformDispatcher.instance.engineId);
}
