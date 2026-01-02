// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ignore_for_file: avoid_print

import 'dart:async';
import 'dart:convert';
import 'dart:core';
import 'dart:ffi' as ffi;
import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';
import 'dart:ui';

void main() {}

@pragma('vm:entry-point')
void customEntrypoint() {
  sayHiFromCustomEntrypoint();
}

@pragma('vm:external-name', 'SayHiFromCustomEntrypoint')
external void sayHiFromCustomEntrypoint();

@pragma('vm:entry-point')
void customEntrypoint1() {
  sayHiFromCustomEntrypoint1();
  sayHiFromCustomEntrypoint2();
  sayHiFromCustomEntrypoint3();
}

@pragma('vm:external-name', 'SayHiFromCustomEntrypoint1')
external void sayHiFromCustomEntrypoint1();
@pragma('vm:external-name', 'SayHiFromCustomEntrypoint2')
external void sayHiFromCustomEntrypoint2();
@pragma('vm:external-name', 'SayHiFromCustomEntrypoint3')
external void sayHiFromCustomEntrypoint3();

@pragma('vm:entry-point')
void terminateExitCodeHandler() {
  Process.runSync('ls', <String>[]);
}

@pragma('vm:entry-point')
void executableNameNotNull() {
  notifyStringValue(Platform.executable);
}

@pragma('vm:external-name', 'NotifyStringValue')
external void notifyStringValue(String value);
@pragma('vm:external-name', 'NotifyBoolValue')
external void notifyBoolValue(bool value);

@pragma('vm:entry-point')
void invokePlatformTaskRunner() {
  PlatformDispatcher.instance.sendPlatformMessage('OhHi', null, null);
}

@pragma('vm:entry-point')
void canSpecifyCustomUITaskRunner() {
  signalNativeTest();
  PlatformDispatcher.instance.sendPlatformMessage('OhHi', null, null);
}

@pragma('vm:entry-point')
void mergedPlatformUIThread() {
  signalNativeTest();
  PlatformDispatcher.instance.sendPlatformMessage('OhHi', null, null);
}

@pragma('vm:entry-point')
void uiTaskRunnerFlushesMicrotasks() {
  // Microtasks are always flushed at the beginning of the frame, hence the delay.
  Future.delayed(const Duration(milliseconds: 50), () {
    Future.microtask(() {
      signalNativeTest();
    });
  });
}

@pragma('vm:external-name', 'SignalNativeTest')
external void signalNativeTest();
@pragma('vm:external-name', 'SignalNativeCount')
external void signalNativeCount(int count);
@pragma('vm:external-name', 'SignalNativeMessage')
external void signalNativeMessage(String message);
@pragma('vm:external-name', 'NotifySemanticsEnabled')
external void notifySemanticsEnabled(bool enabled);
@pragma('vm:external-name', 'NotifyAccessibilityFeatures')
external void notifyAccessibilityFeatures(bool reduceMotion);
@pragma('vm:external-name', 'NotifySemanticsAction')
external void notifySemanticsAction(int nodeId, int action, List<int> data);

@ffi.Native<ffi.Void Function()>(symbol: 'FFISignalNativeTest')
external void ffiSignalNativeTest();

@pragma('vm:entry-point')
// ignore: non_constant_identifier_names
void platform_messages_response() {
  PlatformDispatcher.instance.onPlatformMessage =
      (String name, ByteData? data, PlatformMessageResponseCallback? callback) {
        callback!(data);
      };
  signalNativeTest();
}

@pragma('vm:entry-point')
// ignore: non_constant_identifier_names
void platform_messages_no_response() {
  PlatformDispatcher
      .instance
      .onPlatformMessage = (String name, ByteData? data, PlatformMessageResponseCallback? callback) {
    final Uint8List list = data!.buffer.asUint8List(data.offsetInBytes, data.lengthInBytes);
    signalNativeMessage(utf8.decode(list));
    // This does nothing because no one is listening on the other side. But complete the loop anyway
    // to make sure all null checking on response handles in the engine is in place.
    callback!(data);
  };
  signalNativeTest();
}

@pragma('vm:entry-point')
// ignore: non_constant_identifier_names
void null_platform_messages() {
  PlatformDispatcher.instance.onPlatformMessage =
      (String name, ByteData? data, PlatformMessageResponseCallback? callback) {
        // This checks if the platform_message null data is converted to Flutter null.
        signalNativeMessage((null == data).toString());
        callback!(data);
      };
  signalNativeTest();
}

@pragma('vm:entry-point')
// ignore: non_constant_identifier_names
void can_receive_locale_updates() {
  PlatformDispatcher.instance.onLocaleChanged = () {
    signalNativeCount(PlatformDispatcher.instance.locales.length);
  };
  signalNativeTest();
}

@pragma('vm:external-name', 'SendObjectToNativeCode')
external void sendObjectToNativeCode(dynamic object);

@pragma('vm:entry-point')
// ignore: non_constant_identifier_names
void objects_can_be_posted() {
  final port = ReceivePort();
  port.listen((dynamic message) {
    sendObjectToNativeCode(message);
  });
  signalNativeCount(port.sendPort.nativePort);
}

@pragma('vm:external-name', 'NativeArgumentsCallback')
external void nativeArgumentsCallback(List<String> args);

@pragma('vm:entry-point')
// ignore: non_constant_identifier_names
void custom_logger(List<String> args) {
  print('hello world');
}

@pragma('vm:entry-point')
// ignore: non_constant_identifier_names
void dart_entrypoint_args(List<String> args) {
  nativeArgumentsCallback(args);
}

@pragma('vm:entry-point')
// ignore: non_constant_identifier_names
Future<void> channel_listener_response() async {
  channelBuffers.setListener('test/listen', (
    ByteData? data,
    PlatformMessageResponseCallback callback,
  ) {
    callback(null);
  });
  signalNativeTest();
}
