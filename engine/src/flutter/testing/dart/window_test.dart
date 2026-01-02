// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:typed_data';
import 'dart:ui';

import 'package:test/test.dart';

void main() {
  test('window.sendPlatformMessage preserves callback zone', () {
    runZoned(() {
      final Zone innerZone = Zone.current;
      PlatformDispatcher.instance.sendPlatformMessage(
        'test',
        ByteData.view(Uint8List(0).buffer),
        expectAsync1((ByteData? data) {
          final Zone runZone = Zone.current;
          expect(runZone, isNotNull);
          expect(runZone, same(innerZone));
        }),
      );
    });
  });

  test('computePlatformResolvedLocale basic', () {
    final supportedLocales = <Locale>[
      const Locale.fromSubtags(languageCode: 'zh', scriptCode: 'Hans', countryCode: 'CN'),
      const Locale.fromSubtags(languageCode: 'fr', countryCode: 'FR'),
      const Locale.fromSubtags(languageCode: 'en', countryCode: 'US'),
      const Locale.fromSubtags(languageCode: 'en'),
    ];
    // The default implementation returns null due to lack of a real platform.
    final Locale? result = PlatformDispatcher.instance.computePlatformResolvedLocale(
      supportedLocales,
    );
    expect(result, null);
  });
}
