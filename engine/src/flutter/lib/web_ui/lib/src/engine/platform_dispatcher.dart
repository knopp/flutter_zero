// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:meta/meta.dart';
import 'package:ui/ui.dart' as ui;
import 'package:ui/ui_web/src/ui_web.dart' as ui_web;

import '../engine.dart';

/// When set to true, all platform messages will be printed to the console.
const bool debugPrintPlatformMessages = false;

const StandardMethodCodec standardCodec = StandardMethodCodec();
const JSONMethodCodec jsonCodec = JSONMethodCodec();

/// Platform event dispatcher.
///
/// This is the central entry point for platform messages and configuration
/// events from the platform.
class EnginePlatformDispatcher extends ui.PlatformDispatcher {
  /// Private constructor, since only dart:ui is supposed to create one of
  /// these.
  EnginePlatformDispatcher() {
    _addLocaleChangedListener();
    registerHotRestartListener(dispose);
  }

  /// The [EnginePlatformDispatcher] singleton.
  static EnginePlatformDispatcher get instance => _instance;
  static final EnginePlatformDispatcher _instance = EnginePlatformDispatcher();

  PlatformConfiguration configuration = PlatformConfiguration(locales: parseBrowserLanguages());

  void dispose() {
    _removeLocaleChangedListener();
  }

  /// Receives all events related to platform configuration changes.
  @override
  ui.VoidCallback? get onPlatformConfigurationChanged => _onPlatformConfigurationChanged;
  ui.VoidCallback? _onPlatformConfigurationChanged;
  Zone? _onPlatformConfigurationChangedZone;
  @override
  set onPlatformConfigurationChanged(ui.VoidCallback? callback) {
    _onPlatformConfigurationChanged = callback;
    _onPlatformConfigurationChangedZone = Zone.current;
  }

  /// Engine code should use this method instead of the callback directly.
  /// Otherwise zones won't work properly.
  void invokeOnPlatformConfigurationChanged() {
    invoke(_onPlatformConfigurationChanged, _onPlatformConfigurationChangedZone);
  }

  @override
  int? get engineId => null;

  @override
  void sendPlatformMessage(
    String name,
    ByteData? data,
    ui.PlatformMessageResponseCallback? callback,
  ) {
    _sendPlatformMessage(name, data, _zonedPlatformMessageResponseCallback(callback));
  }

  @override
  void sendPortPlatformMessage(String name, ByteData? data, int identifier, Object port) {
    throw Exception("Isolates aren't supported in web.");
  }

  @override
  void registerBackgroundIsolate(ui.RootIsolateToken token) {
    throw Exception("Isolates aren't supported in web.");
  }

  // TODO(ianh): Deprecate onPlatformMessage once the framework is moved over
  // to using channel buffers exclusively.
  @override
  ui.PlatformMessageCallback? get onPlatformMessage => _onPlatformMessage;
  ui.PlatformMessageCallback? _onPlatformMessage;
  Zone? _onPlatformMessageZone;
  @override
  set onPlatformMessage(ui.PlatformMessageCallback? callback) {
    _onPlatformMessage = callback;
    _onPlatformMessageZone = Zone.current;
  }

  /// Engine code should use this method instead of the callback directly.
  /// Otherwise zones won't work properly.
  void invokeOnPlatformMessage(
    String name,
    ByteData? data,
    ui.PlatformMessageResponseCallback callback,
  ) {
    if (name == ui.ChannelBuffers.kControlChannelName) {
      // TODO(ianh): move this logic into ChannelBuffers once we remove onPlatformMessage
      try {
        ui.channelBuffers.handleMessage(data!);
      } finally {
        callback(null);
      }
    } else if (_onPlatformMessage != null) {
      invoke3<String, ByteData?, ui.PlatformMessageResponseCallback>(
        _onPlatformMessage,
        _onPlatformMessageZone,
        name,
        data,
        callback,
      );
    } else {
      ui.channelBuffers.push(name, data, callback);
    }
  }

  /// Wraps the given [callback] in another callback that ensures that the
  /// original callback is called in the zone it was registered in.
  static ui.PlatformMessageResponseCallback? _zonedPlatformMessageResponseCallback(
    ui.PlatformMessageResponseCallback? callback,
  ) {
    if (callback == null) {
      return null;
    }

    // Store the zone in which the callback is being registered.
    final Zone registrationZone = Zone.current;

    return (ByteData? data) {
      registrationZone.runUnaryGuarded(callback, data);
    };
  }

  void _sendPlatformMessage(
    String name,
    ByteData? data,
    ui.PlatformMessageResponseCallback? callback,
  ) {
    // In widget tests we want to bypass processing of platform messages.
    var returnImmediately = false;
    assert(() {
      if (ui_web.TestEnvironment.instance.ignorePlatformMessages) {
        returnImmediately = true;
      }
      return true;
    }());

    if (returnImmediately) {
      return;
    }

    if (debugPrintPlatformMessages) {
      print('Sent platform message on channel: "$name"');
    }

    var allowDebugEcho = false;
    assert(() {
      allowDebugEcho = true;
      return true;
    }());

    if (allowDebugEcho && name == 'flutter/debug-echo') {
      // Echoes back the data unchanged. Used for testing purposes.
      replyToPlatformMessage(callback, data);
      return;
    }

    switch (name) {
      /// This should be in sync with shell/common/shell.cc
      case 'flutter/assets':
        final String url = utf8.decode(data!.buffer.asUint8List());
        _handleFlutterAssetsMessage(url, callback);
        return;

      // Dispatched by the bindings to delay service worker initialization.
      case 'flutter/service_worker':
        domWindow.dispatchEvent(createDomEvent('Event', 'flutter-first-frame'));
        return;
    }

    if (pluginMessageCallHandler != null) {
      pluginMessageCallHandler!(name, data, callback);
      return;
    }

    // Passing [null] to [callback] indicates that the platform message isn't
    // implemented. Look at [MethodChannel.invokeMethod] to see how [null] is
    // handled.
    replyToPlatformMessage(callback, null);
  }

  Future<void> _handleFlutterAssetsMessage(
    String url,
    ui.PlatformMessageResponseCallback? callback,
  ) async {
    try {
      final response = await ui_web.assetManager.loadAsset(url) as HttpFetchResponse;
      final ByteBuffer assetData = await response.asByteBuffer();
      replyToPlatformMessage(callback, assetData.asByteData());
    } catch (error) {
      printWarning('Error while trying to load an asset: $error');
      replyToPlatformMessage(callback, null);
    }
  }

  /// This is equivalent to `locales.first`, except that it will provide an
  /// undefined (using the language tag "und") non-null locale if the [locales]
  /// list has not been set or is empty.
  ///
  /// We use the first locale in the [locales] list instead of the browser's
  /// built-in `navigator.language` because browsers do not agree on the
  /// implementation.
  ///
  /// See also:
  ///
  /// * https://developer.mozilla.org/en-US/docs/Web/API/NavigatorLanguage/languages,
  ///   which explains browser quirks in the implementation notes.
  @override
  ui.Locale get locale => locales.isEmpty ? const ui.Locale.fromSubtags() : locales.first;

  /// The full system-reported supported locales of the device.
  ///
  /// This establishes the language and formatting conventions that application
  /// should, if possible, use to render their user interface.
  ///
  /// The list is ordered in order of priority, with lower-indexed locales being
  /// preferred over higher-indexed ones. The first element is the primary [locale].
  ///
  /// The [onLocaleChanged] callback is called whenever this value changes.
  ///
  /// See also:
  ///
  ///  * [WidgetsBindingObserver], for a mechanism at the widgets layer to
  ///    observe when this value changes.
  @override
  List<ui.Locale> get locales => configuration.locales;

  // A subscription to the 'languagechange' event of 'window'.
  DomSubscription? _onLocaleChangedSubscription;

  /// Configures the [_onLocaleChangedSubscription].
  void _addLocaleChangedListener() {
    if (_onLocaleChangedSubscription != null) {
      return;
    }
    updateLocales(); // First time, for good measure.
    _onLocaleChangedSubscription = DomSubscription(
      domWindow,
      'languagechange',
      createDomEventListener((DomEvent _) {
        // Update internal config, then propagate the changes.
        updateLocales();
        invokeOnLocaleChanged();
      }),
    );
  }

  /// Removes the [_onLocaleChangedSubscription].
  void _removeLocaleChangedListener() {
    _onLocaleChangedSubscription?.cancel();
    _onLocaleChangedSubscription = null;
  }

  /// Performs the platform-native locale resolution.
  ///
  /// Each platform may return different results.
  ///
  /// If the platform fails to resolve a locale, then this will return null.
  ///
  /// This method returns synchronously and is a direct call to
  /// platform specific APIs without invoking method channels.
  @override
  ui.Locale? computePlatformResolvedLocale(List<ui.Locale> supportedLocales) {
    // TODO(garyq): Implement on web.
    return null;
  }

  /// A callback that is invoked whenever [locale] changes value.
  ///
  /// The framework invokes this callback in the same zone in which the
  /// callback was set.
  ///
  /// See also:
  ///
  ///  * [WidgetsBindingObserver], for a mechanism at the widgets layer to
  ///    observe when this callback is invoked.
  @override
  ui.VoidCallback? get onLocaleChanged => _onLocaleChanged;
  ui.VoidCallback? _onLocaleChanged;
  Zone? _onLocaleChangedZone;
  @override
  set onLocaleChanged(ui.VoidCallback? callback) {
    _onLocaleChanged = callback;
    _onLocaleChangedZone = Zone.current;
  }

  /// The locale used when we fail to get the list from the browser.
  static const ui.Locale _defaultLocale = ui.Locale('en', 'US');

  /// Sets locales to an empty list.
  ///
  /// The empty list is not a valid value for locales. This is only used for
  /// testing locale update logic.
  void debugResetLocales() {
    configuration = configuration.copyWith(locales: const <ui.Locale>[]);
  }

  // Called by `_onLocaleChangedSubscription` when browser languages change.
  void updateLocales() {
    configuration = configuration.copyWith(locales: parseBrowserLanguages());
  }

  /// Overrides the browser languages list.
  ///
  /// If [value] is null, resets the browser languages back to the real value.
  ///
  /// This is intended for tests only.
  @visibleForTesting
  static void debugOverrideBrowserLanguages(List<String>? value) {
    _browserLanguagesOverride = value;
  }

  static List<String>? _browserLanguagesOverride;

  @visibleForTesting
  static List<ui.Locale> parseBrowserLanguages() {
    // TODO(yjbanov): find a solution for IE
    final List<String>? languages = _browserLanguagesOverride ?? domWindow.navigator.languages;
    if (languages == null || languages.isEmpty) {
      // To make it easier for the app code, let's not leave the locales list
      // empty. This way there's fewer corner cases for apps to handle.
      return const <ui.Locale>[_defaultLocale];
    }

    final locales = <ui.Locale>[];
    for (final String language in languages) {
      final domLocale = DomLocale(language);
      locales.add(
        ui.Locale.fromSubtags(
          languageCode: domLocale.language,
          scriptCode: domLocale.script,
          countryCode: domLocale.region,
        ),
      );
    }

    assert(locales.isNotEmpty);
    return locales;
  }

  /// Engine code should use this method instead of the callback directly.
  /// Otherwise zones won't work properly.
  void invokeOnLocaleChanged() {
    invoke(_onLocaleChanged, _onLocaleChangedZone);
  }

  /// The setting indicating whether time should always be shown in the 24-hour
  /// format.
  ///
  /// This option is used by [showTimePicker].
  @override
  bool get alwaysUse24HourFormat => configuration.alwaysUse24HourFormat;

  // TODO(dnfield): make this work on web.
  // https://github.com/flutter/flutter/issues/100277
  ui.ErrorCallback? _onError;
  // ignore: unused_field
  late Zone _onErrorZone;
  @override
  ui.ErrorCallback? get onError => _onError;
  @override
  set onError(ui.ErrorCallback? callback) {
    _onError = callback;
    _onErrorZone = Zone.current;
  }

  /// In Flutter, platform messages are exchanged between threads so the
  /// messages and responses have to be exchanged asynchronously. We simulate
  /// that by adding a zero-length delay to the reply.
  void replyToPlatformMessage(ui.PlatformMessageResponseCallback? callback, ByteData? data) {
    Future<void>.delayed(Duration.zero).then((_) {
      if (callback != null) {
        callback(data);
      }
    });
  }
}

/// Invokes [callback] inside the given [zone].
void invoke(void Function()? callback, Zone? zone) {
  if (callback == null) {
    return;
  }

  assert(zone != null);

  if (identical(zone, Zone.current)) {
    callback();
  } else {
    zone!.runGuarded(callback);
  }
}

/// Invokes [callback] inside the given [zone] passing it [arg].
void invoke1<A>(void Function(A a)? callback, Zone? zone, A arg) {
  if (callback == null) {
    return;
  }

  assert(zone != null);

  if (identical(zone, Zone.current)) {
    callback(arg);
  } else {
    zone!.runUnaryGuarded<A>(callback, arg);
  }
}

/// Invokes [callback] inside the given [zone] passing it [arg1] and [arg2].
void invoke2<A1, A2>(void Function(A1 a1, A2 a2)? callback, Zone? zone, A1 arg1, A2 arg2) {
  if (callback == null) {
    return;
  }

  assert(zone != null);

  if (identical(zone, Zone.current)) {
    callback(arg1, arg2);
  } else {
    zone!.runGuarded(() {
      callback(arg1, arg2);
    });
  }
}

/// Invokes [callback] inside the given [zone] passing it [arg1], [arg2], and [arg3].
void invoke3<A1, A2, A3>(
  void Function(A1 a1, A2 a2, A3 a3)? callback,
  Zone? zone,
  A1 arg1,
  A2 arg2,
  A3 arg3,
) {
  if (callback == null) {
    return;
  }

  assert(zone != null);

  if (identical(zone, Zone.current)) {
    callback(arg1, arg2, arg3);
  } else {
    zone!.runGuarded(() {
      callback(arg1, arg2, arg3);
    });
  }
}

class PlatformConfiguration {
  const PlatformConfiguration({
    this.alwaysUse24HourFormat = false,
    this.locales = const <ui.Locale>[],
  });

  PlatformConfiguration apply({bool? alwaysUse24HourFormat, List<ui.Locale>? locales}) {
    return PlatformConfiguration(
      alwaysUse24HourFormat: alwaysUse24HourFormat ?? this.alwaysUse24HourFormat,
      locales: locales ?? this.locales,
    );
  }

  PlatformConfiguration copyWith({
    bool? alwaysUse24HourFormat,
    bool? semanticsEnabled,
    double? textScaleFactor,
    List<ui.Locale>? locales,
    String? defaultRouteName,
    String? systemFontFamily,
    double? lineHeightScaleFactorOverride,
    double? letterSpacingOverride,
    double? wordSpacingOverride,
    double? paragraphSpacingOverride,
  }) {
    return PlatformConfiguration(
      alwaysUse24HourFormat: alwaysUse24HourFormat ?? this.alwaysUse24HourFormat,
      locales: locales ?? this.locales,
    );
  }

  final bool alwaysUse24HourFormat;
  final List<ui.Locale> locales;
}
