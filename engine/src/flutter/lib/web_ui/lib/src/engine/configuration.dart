// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// JavaScript API a Flutter Web application can use to configure the Web
/// Engine.
///
/// The configuration is passed from JavaScript to the engine as part of the
/// bootstrap process, through the `FlutterEngineInitializer.initializeEngine`
/// JS method, with an (optional) object of type [JsFlutterConfiguration].
///
/// This library also supports the legacy method of setting a plain JavaScript
/// object set as the `flutterConfiguration` property of the top-level `window`
/// object, but that approach is now deprecated and will warn users.
///
/// Both methods are **disallowed** to be used at the same time.
///
/// Example:
///
///     _flutter.loader.loadEntrypoint({
///       // ...
///       onEntrypointLoaded: async function(engineInitializer) {
///         let appRunner = await engineInitializer.initializeEngine({
///           // JsFlutterConfiguration goes here...
///           canvasKitBaseUrl: "https://example.com/my-custom-canvaskit/",
///         });
///         appRunner.runApp();
///       }
///     });
///
/// Example of the **deprecated** style (this will issue a JS console warning!):
///
///     <script>
///       window.flutterConfiguration = {
///         canvasKitBaseUrl: "https://example.com/my-custom-canvaskit/"
///       };
///     </script>
///
/// Configuration properties supplied via this object override those supplied
/// using the corresponding environment variables. For example, if both the
/// `canvasKitBaseUrl` config entry and the `FLUTTER_WEB_CANVASKIT_URL`
/// environment variables are provided, the `canvasKitBaseUrl` entry is used.

@JS()
library configuration;

import 'dart:js_interop';

import 'package:meta/meta.dart';
import 'dom.dart';

enum CanvasKitVariant {
  /// The appropriate variant is chosen based on the browser.
  ///
  /// This is the default variant.
  auto,

  /// The full variant that can be used in any browser.
  full,

  /// The variant that is optimized for Chromium browsers.
  ///
  /// WARNING: In most cases, you should use [auto] instead of this variant. Using
  /// this variant in a non-Chromium browser will result in a broken app.
  chromium,

  /// The variant that contains the new WebParagraph implementation on top of Chrome's Text Clusters
  /// API: https://github.com/fserb/canvas2D/blob/master/spec/enhanced-textmetrics.md
  ///
  /// WARNING: This is an experimental variant that's not yet ready for production use.
  experimentalWebParagraph,
}

/// The Web Engine configuration for the current application.
FlutterConfiguration get configuration {
  if (_debugConfiguration != null) {
    return _debugConfiguration!;
  }
  return _configuration ??= FlutterConfiguration.legacy(_jsConfiguration);
}

FlutterConfiguration? _configuration;

FlutterConfiguration? _debugConfiguration;

/// Overrides the initial test configuration with new values coming from `newConfig`.
///
/// The initial test configuration (AKA `_jsConfiguration`) is set in the
/// `test_platform.dart` file. See: `window.flutterConfiguration` in `_testBootstrapHandler`.
///
/// The result of calling this method each time is:
///
///     [configuration] = _jsConfiguration + newConfig
///
/// Subsequent calls to this method don't *add* more to an already overridden
/// configuration; this method always starts from an original `_jsConfiguration`,
/// and adds `newConfig` to it.
///
/// If `newConfig` is null, [configuration] resets to the initial `_jsConfiguration`.
///
/// This must be called before the engine is initialized. Calling it after the
/// engine is initialized will result in some of the properties not taking
/// effect because they are consumed during initialization.
@visibleForTesting
void debugOverrideJsConfiguration(JsFlutterConfiguration? newConfig) {
  if (newConfig != null) {
    _debugConfiguration = configuration.withOverrides(newConfig);
  } else {
    _debugConfiguration = null;
  }
}

/// Supplies Web Engine configuration properties.
class FlutterConfiguration {
  /// Constructs an unitialized configuration object.
  @visibleForTesting
  FlutterConfiguration();

  /// Constucts a "tainted by JS globals" configuration object.
  ///
  /// This configuration style is deprecated. It will warn the user about the
  /// new API (if used)
  FlutterConfiguration.legacy(JsFlutterConfiguration? config) {
    if (config != null) {
      _usedLegacyConfigStyle = true;
      _configuration = config;
    }
    // Warn the user of the deprecated behavior.
    assert(() {
      if (config != null) {
        domWindow.console.warn(
          'window.flutterConfiguration is now deprecated.\n'
          'Use engineInitializer.initializeEngine(config) instead.\n'
          'See: https://docs.flutter.dev/development/platform-integration/web/initialization',
        );
      }
      return true;
    }());
  }

  FlutterConfiguration withOverrides(JsFlutterConfiguration? overrides) {
    final newJsConfig =
        objectConstructor.assign(
              <String, Object>{}.jsify(),
              _configuration.jsify(),
              overrides.jsify(),
            )
            as JsFlutterConfiguration;
    final newConfig = FlutterConfiguration();
    newConfig._configuration = newJsConfig;
    return newConfig;
  }

  bool _usedLegacyConfigStyle = false;
  JsFlutterConfiguration? _configuration;

  /// Sets a value for [_configuration].
  ///
  /// This method is called by the engine initialization process, through the
  /// [initEngineServices] method.
  ///
  /// This method throws an AssertionError, if the _configuration object has
  /// been set to anything non-null through the [FlutterConfiguration.legacy]
  /// constructor.
  void setUserConfiguration(JsFlutterConfiguration? configuration) {
    if (configuration != null) {
      assert(
        !_usedLegacyConfigStyle,
        'Use engineInitializer.initializeEngine(config) only. '
        'Using the (deprecated) window.flutterConfiguration and initializeEngine '
        'configuration simultaneously is not supported.',
      );

      _configuration = configuration;
    }
  }

  // Static constant parameters.
  //
  // These properties affect tree shaking and therefore cannot be supplied at
  // runtime. They must be static constants for the compiler to remove dead code
  // effectively.

  static const bool flutterWebUseSkwasm = bool.fromEnvironment('FLUTTER_WEB_USE_SKWASM');

  /// Enable the Skia-based rendering backend.
  ///
  /// Using flutter tools option "--web-renderer=canvaskit" would set the value to
  /// true.
  ///
  /// Using flutter tools option "--web-renderer=html" would set the value to false.
  static const bool useSkia = bool.fromEnvironment('FLUTTER_WEB_USE_SKIA');

  // Runtime parameters.
  //
  // These parameters can be supplied either as environment variables, or at
  // runtime. Runtime-supplied values take precedence over environment
  // variables.

  /// The absolute base URL of the location of the `assets` directory of the app.
  ///
  /// This value is useful when Flutter web assets are deployed to a separate
  /// domain (or subdirectory) from which the index.html is served, for example:
  ///
  /// * Application: https://www.my-app.com/
  /// * Flutter Assets: https://cdn.example.com/my-app/build-hash/assets/
  ///
  /// The `assetBase` value would be set to:
  ///
  /// * `'https://cdn.example.com/my-app/build-hash/'`
  ///
  /// It is also useful in the case that a Flutter web application is embedded
  /// into another web app, in a way that the `<base>` tag of the index.html
  /// cannot be set (because it'd break the host app), for example:
  ///
  /// * Application: https://www.my-app.com/
  /// * Flutter Assets: https://www.my-app.com/static/companion/flutter/assets/
  ///
  /// The `assetBase` would be set to:
  ///
  /// * `'/static/companion/flutter/'`
  ///
  /// Do not confuse this configuration value with [canvasKitBaseUrl].
  String? get assetBase => _configuration?.assetBase;

  /// Returns a `nonce` to allowlist the inline styles that Flutter web needs.
  ///
  /// See: https://developer.mozilla.org/en-US/docs/Web/HTML/Global_attributes/nonce
  String? get nonce => _configuration?.nonce;

  bool get forceSingleThreadedSkwasm => _configuration?.forceSingleThreadedSkwasm ?? false;
}

@JS('window.flutterConfiguration')
external JsFlutterConfiguration? get _jsConfiguration;

/// The JS bindings for the object that's set as `window.flutterConfiguration`.
extension type JsFlutterConfiguration._(JSObject _) implements JSObject {
  external JsFlutterConfiguration({
    String? assetBase,
    String? nonce,
    bool? forceSingleThreadedSkwasm,
  });

  external String? get assetBase;
  external String? get nonce;
  external bool? get forceSingleThreadedSkwasm;
}
