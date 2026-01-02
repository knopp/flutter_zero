// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:js_interop';

import 'package:test/test.dart';
import 'package:ui/src/engine.dart' as engine;
import 'package:ui/src/engine/initialization.dart';
import 'package:ui/ui_web/src/ui_web.dart' as ui_web;

import 'fake_asset_manager.dart';

void setUpUnitTests({
  bool withImplicitView = false,
  bool setUpTestViewDimensions = true,
  ui_web.TestEnvironment testEnvironment = const ui_web.TestEnvironment.production(),
}) {
  late final FakeAssetScope debugFontsScope;
  setUpAll(() async {
    ui_web.TestEnvironment.setUp(testEnvironment);

    debugFontsScope = configureDebugFontsAssetScope(fakeAssetManager);
    debugOnlyAssetManager = fakeAssetManager;
    await bootstrapAndRunApp(withImplicitView: withImplicitView);
    engine.debugOverrideJsConfiguration(
      <String, Object?>{'fontFallbackBaseUrl': 'assets/fallback_fonts/'}.jsify()
          as engine.JsFlutterConfiguration?,
    );
  });

  tearDownAll(() async {
    fakeAssetManager.popAssetScope(debugFontsScope);
    ui_web.TestEnvironment.tearDown();
  });
}

Future<void> bootstrapAndRunApp({bool withImplicitView = false}) async {
  final completer = Completer<void>();
  await ui_web.bootstrapEngine(runApp: () => completer.complete());
  await completer.future;
}
