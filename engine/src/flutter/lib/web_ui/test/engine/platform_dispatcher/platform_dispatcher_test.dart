// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:typed_data';

import 'package:test/bootstrap/browser.dart';
import 'package:test/test.dart';
import 'package:ui/src/engine.dart';
import 'package:ui/ui.dart' as ui;

import '../../common/test_initialization.dart';

/// Creates a DOM mouse event with fractional coordinates to avoid triggering
/// assistive technology detection logic.
///
/// The platform dispatcher's _isIntegerCoordinateNavigation() method detects
/// assistive technology clicks by checking for integer coordinates. Tests
/// should use fractional coordinates to simulate normal user clicks.
DomMouseEvent createTestClickEvent({
  double clientX = 1.5,
  double clientY = 1.5,
  bool bubbles = true,
}) {
  return createDomMouseEvent('click', <String, Object>{
    'clientX': clientX,
    'clientY': clientY,
    'bubbles': bubbles,
  });
}

void main() {
  internalBootstrapBrowserTest(() => testMain);
}

void testMain() {
  group('PlatformDispatcher', () {
    late EnginePlatformDispatcher dispatcher;

    setUpAll(() async {
      await bootstrapAndRunApp(withImplicitView: true);
    });

    setUp(() {
      dispatcher = EnginePlatformDispatcher();
    });

    tearDown(() {
      dispatcher.dispose();
    });

    test('responds to flutter/skia Skia.setResourceCacheMaxBytes', () async {
      const MethodCodec codec = JSONMethodCodec();
      final completer = Completer<ByteData?>();
      ui.PlatformDispatcher.instance.sendPlatformMessage(
        'flutter/skia',
        codec.encodeMethodCall(
          const MethodCall('Skia.setResourceCacheMaxBytes', 512 * 1000 * 1000),
        ),
        completer.complete,
      );

      final ByteData? response = await completer.future;
      expect(response, isNotNull);
      expect(codec.decodeEnvelope(response!), <bool>[true]);
    });

    test('responds to flutter/platform HapticFeedback.vibrate', () async {
      const MethodCodec codec = JSONMethodCodec();
      final completer = Completer<ByteData?>();
      ui.PlatformDispatcher.instance.sendPlatformMessage(
        'flutter/platform',
        codec.encodeMethodCall(const MethodCall('HapticFeedback.vibrate')),
        completer.complete,
      );

      final ByteData? response = await completer.future;
      expect(response, isNotNull);
      expect(codec.decodeEnvelope(response!), true);
    });

    test('responds to flutter/platform SystemChrome.setSystemUIOverlayStyle', () async {
      const MethodCodec codec = JSONMethodCodec();
      final completer = Completer<ByteData?>();
      ui.PlatformDispatcher.instance.sendPlatformMessage(
        'flutter/platform',
        codec.encodeMethodCall(
          const MethodCall('SystemChrome.setSystemUIOverlayStyle', <String, dynamic>{}),
        ),
        completer.complete,
      );

      final ByteData? response = await completer.future;
      expect(response, isNotNull);
      expect(codec.decodeEnvelope(response!), true);
    });

    test('responds to flutter/contextmenu enable', () async {
      const MethodCodec codec = JSONMethodCodec();
      final completer = Completer<ByteData?>();
      ui.PlatformDispatcher.instance.sendPlatformMessage(
        'flutter/contextmenu',
        codec.encodeMethodCall(const MethodCall('enableContextMenu')),
        completer.complete,
      );

      final ByteData? response = await completer.future;
      expect(response, isNotNull);
      expect(codec.decodeEnvelope(response!), true);
    });

    test('responds to flutter/contextmenu disable', () async {
      const MethodCodec codec = JSONMethodCodec();
      final completer = Completer<ByteData?>();
      ui.PlatformDispatcher.instance.sendPlatformMessage(
        'flutter/contextmenu',
        codec.encodeMethodCall(const MethodCall('disableContextMenu')),
        completer.complete,
      );

      final ByteData? response = await completer.future;
      expect(response, isNotNull);
      expect(codec.decodeEnvelope(response!), true);
    });

    group('parseBrowserLanguages', () {
      test('returns the default locale when no browser languages are present', () {
        EnginePlatformDispatcher.debugOverrideBrowserLanguages([]);
        addTearDown(() => EnginePlatformDispatcher.debugOverrideBrowserLanguages(null));

        expect(EnginePlatformDispatcher.parseBrowserLanguages(), const [ui.Locale('en', 'US')]);
      });

      test('returns locales list parsed from browser languages', () {
        EnginePlatformDispatcher.debugOverrideBrowserLanguages([
          'uk-UA',
          'en',
          'ar-Arab-SA',
          'zh-Hant-HK',
          'de-DE',
        ]);
        addTearDown(() => EnginePlatformDispatcher.debugOverrideBrowserLanguages(null));

        expect(EnginePlatformDispatcher.parseBrowserLanguages(), const [
          ui.Locale('uk', 'UA'),
          ui.Locale('en'),
          ui.Locale.fromSubtags(languageCode: 'ar', scriptCode: 'Arab', countryCode: 'SA'),
          ui.Locale.fromSubtags(languageCode: 'zh', scriptCode: 'Hant', countryCode: 'HK'),
          ui.Locale('de', 'DE'),
        ]);
      });
    });
  });
}
