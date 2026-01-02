// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
part of dart.ui;

/// Signature of callbacks that have no arguments and return no data.
typedef VoidCallback = void Function();

/// Signature for responses to platform messages.
///
/// Used as a parameter to [PlatformDispatcher.sendPlatformMessage] and
/// [PlatformDispatcher.onPlatformMessage].
typedef PlatformMessageResponseCallback = void Function(ByteData? data);

/// Deprecated. Migrate to [ChannelBuffers.setListener] instead.
///
/// Signature for [PlatformDispatcher.onPlatformMessage].
@Deprecated(
  'Migrate to ChannelBuffers.setListener instead. '
  'This feature was deprecated after v3.11.0-20.0.pre.',
)
typedef PlatformMessageCallback =
    void Function(String name, ByteData? data, PlatformMessageResponseCallback? callback);

/// Signature for [PlatformDispatcher.onError].
///
/// If this method returns false, the engine may use some fallback method to
/// provide information about the error.
///
/// After calling this method, the process or the VM may terminate. Some severe
/// unhandled errors may not be able to call this method either, such as Dart
/// compilation errors or process terminating errors.
typedef ErrorCallback = bool Function(Object exception, StackTrace stackTrace);

@pragma('vm:entry-point')
ByteData? _wrapUnmodifiableByteData(ByteData? byteData) => byteData?.asUnmodifiableView();

/// A token that represents a root isolate.
class RootIsolateToken {
  RootIsolateToken._(this._token);

  /// An enumeration representing the root isolate (0 if not a root isolate).
  final int _token;

  /// The token for the root isolate that is executing this Dart code.  If this
  /// Dart code is not executing on a root isolate [instance] will be null.
  static final RootIsolateToken? instance = () {
    final int token = __getRootIsolateToken();
    return token == 0 ? null : RootIsolateToken._(token);
  }();

  @Native<Int64 Function()>(symbol: 'PlatformConfigurationNativeApi::GetRootIsolateToken')
  external static int __getRootIsolateToken();
}

/// Platform event dispatcher singleton.
///
/// The most basic interface to the host operating system's interface.
///
/// This is the central entry point for platform messages and configuration
/// events from the platform.
///
/// It exposes the core scheduler API, the input event callback, the graphics
/// drawing API, and other such core services.
///
/// It manages the list of the application's [views] as well as the
/// [configuration] of various platform attributes.
///
/// Consider avoiding static references to this singleton through
/// [PlatformDispatcher.instance] and instead prefer using a binding for
/// dependency resolution such as `WidgetsBinding.instance.platformDispatcher`.
/// See [PlatformDispatcher.instance] for more information about why this is
/// preferred.
class PlatformDispatcher {
  /// Private constructor, since only dart:ui is supposed to create one of
  /// these. Use [instance] to access the singleton.
  PlatformDispatcher._();

  /// The [PlatformDispatcher] singleton.
  ///
  /// Consider avoiding static references to this singleton through
  /// [PlatformDispatcher.instance] and instead prefer using a binding for
  /// dependency resolution such as `WidgetsBinding.instance.platformDispatcher`.
  ///
  /// Static access of this object means that Flutter has few, if any options to
  /// fake or mock the given object in tests. Even in cases where Dart offers
  /// special language constructs to forcefully shadow such properties, those
  /// mechanisms would only be reasonable for tests and they would not be
  /// reasonable for a future of Flutter where we legitimately want to select an
  /// appropriate implementation at runtime.
  ///
  /// The only place that `WidgetsBinding.instance.platformDispatcher` is
  /// inappropriate is if access to these APIs is required before the binding is
  /// initialized by invoking `runApp()` or
  /// `WidgetsFlutterBinding.instance.ensureInitialized()`. In that case, it is
  /// necessary (though unfortunate) to use the [PlatformDispatcher.instance]
  /// object statically.
  static PlatformDispatcher get instance => _instance;
  static final PlatformDispatcher _instance = PlatformDispatcher._();

  /// Opaque engine identifier for the engine running current isolate. Can be used
  /// in native code to retrieve the engine instance.
  /// The identifier is valid while the isolate is running.
  int? get engineId => _engineId;

  int? _engineId;

  List<Locale> _locales = [];

  /// Sends a message to a platform-specific plugin.
  ///
  /// The `name` parameter determines which plugin receives the message. The
  /// `data` parameter contains the message payload and is typically UTF-8
  /// encoded JSON but can be arbitrary data. If the plugin replies to the
  /// message, `callback` will be called with the response.
  ///
  /// The framework invokes [callback] in the same zone in which this method was
  /// called.
  void sendPlatformMessage(String name, ByteData? data, PlatformMessageResponseCallback? callback) {
    final String? error = _sendPlatformMessage(
      name,
      _zonedPlatformMessageResponseCallback(callback),
      data,
    );
    if (error != null) {
      throw Exception(error);
    }
  }

  String? _sendPlatformMessage(
    String name,
    PlatformMessageResponseCallback? callback,
    ByteData? data,
  ) => __sendPlatformMessage(name, callback, data);

  @Native<Handle Function(Handle, Handle, Handle)>(
    symbol: 'PlatformConfigurationNativeApi::SendPlatformMessage',
  )
  external static String? __sendPlatformMessage(
    String name,
    PlatformMessageResponseCallback? callback,
    ByteData? data,
  );

  /// Sends a message to a platform-specific plugin via a [SendPort].
  ///
  /// This operates similarly to [sendPlatformMessage] but is used when sending
  /// messages from background isolates. The [port] parameter allows Flutter to
  /// know which isolate to send the result to. The [name] parameter is the name
  /// of the channel communication will happen on. The [data] parameter is the
  /// payload of the message. The [identifier] parameter is a unique integer
  /// assigned to the message.
  void sendPortPlatformMessage(String name, ByteData? data, int identifier, SendPort port) {
    final String? error = _sendPortPlatformMessage(name, identifier, port.nativePort, data);
    if (error != null) {
      throw Exception(error);
    }
  }

  String? _sendPortPlatformMessage(String name, int identifier, int port, ByteData? data) =>
      __sendPortPlatformMessage(name, identifier, port, data);

  @Native<Handle Function(Handle, Handle, Handle, Handle)>(
    symbol: 'PlatformConfigurationNativeApi::SendPortPlatformMessage',
  )
  external static String? __sendPortPlatformMessage(
    String name,
    int identifier,
    int port,
    ByteData? data,
  );

  /// Registers the current isolate with the isolate identified with by the
  /// [token]. This is required if platform channels are to be used on a
  /// background isolate.
  void registerBackgroundIsolate(RootIsolateToken token) {
    DartPluginRegistrant.ensureInitialized();
    __registerBackgroundIsolate(token._token);
  }

  @Native<Void Function(Int64)>(symbol: 'PlatformConfigurationNativeApi::RegisterBackgroundIsolate')
  external static void __registerBackgroundIsolate(int rootIsolateId);

  /// Deprecated. Migrate to [ChannelBuffers.setListener] instead.
  ///
  /// Called whenever this platform dispatcher receives a message from a
  /// platform-specific plugin.
  ///
  /// The `name` parameter determines which plugin sent the message. The `data`
  /// parameter is the payload and is typically UTF-8 encoded JSON but can be
  /// arbitrary data.
  ///
  /// Message handlers must call the function given in the `callback` parameter.
  /// If the handler does not need to respond, the handler should pass null to
  /// the callback.
  ///
  /// The framework invokes this callback in the same zone in which the callback
  /// was set.
  @Deprecated(
    'Migrate to ChannelBuffers.setListener instead. '
    'This feature was deprecated after v3.11.0-20.0.pre.',
  )
  PlatformMessageCallback? get onPlatformMessage => _onPlatformMessage;
  PlatformMessageCallback? _onPlatformMessage;
  Zone _onPlatformMessageZone = Zone.root;
  @Deprecated(
    'Migrate to ChannelBuffers.setListener instead. '
    'This feature was deprecated after v3.11.0-20.0.pre.',
  )
  set onPlatformMessage(PlatformMessageCallback? callback) {
    _onPlatformMessage = callback;
    _onPlatformMessageZone = Zone.current;
  }

  /// Called by [_dispatchPlatformMessage].
  void _respondToPlatformMessage(int responseId, ByteData? data) =>
      __respondToPlatformMessage(responseId, data);

  @Native<Void Function(IntPtr, Handle)>(
    symbol: 'PlatformConfigurationNativeApi::RespondToPlatformMessage',
  )
  external static void __respondToPlatformMessage(int responseId, ByteData? data);

  /// Wraps the given [callback] in another callback that ensures that the
  /// original callback is called in the zone it was registered in.
  static PlatformMessageResponseCallback? _zonedPlatformMessageResponseCallback(
    PlatformMessageResponseCallback? callback,
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

  /// Send a message to the framework using the [ChannelBuffers].
  ///
  /// This method constructs the appropriate callback to respond
  /// with the given `responseId`. It should only be called for messages
  /// from the platform.
  void _dispatchPlatformMessage(String name, ByteData? data, int responseId) {
    if (name == ChannelBuffers.kControlChannelName) {
      try {
        channelBuffers.handleMessage(data!);
      } finally {
        _respondToPlatformMessage(responseId, null);
      }
    } else if (onPlatformMessage != null) {
      _invoke3<String, ByteData?, PlatformMessageResponseCallback>(
        onPlatformMessage,
        _onPlatformMessageZone,
        name,
        data,
        (ByteData? responseData) {
          _respondToPlatformMessage(responseId, responseData);
        },
      );
    } else {
      channelBuffers.push(name, data, (ByteData? responseData) {
        _respondToPlatformMessage(responseId, responseData);
      });
    }
  }

  /// Set the debug name associated with this platform dispatcher's root
  /// isolate.
  ///
  /// Normally debug names are automatically generated from the Dart port, entry
  /// point, and source file. For example: `main.dart$main-1234`.
  ///
  /// This can be combined with flutter tools `--isolate-filter` flag to debug
  /// specific root isolates. For example: `flutter attach --isolate-filter=[name]`.
  /// Note that this does not rename any child isolates of the root.
  void setIsolateDebugName(String name) => _setIsolateDebugName(name);

  @Native<Void Function(Handle)>(symbol: 'PlatformConfigurationNativeApi::SetIsolateDebugName')
  external static void _setIsolateDebugName(String name);

  /// Requests the Dart VM to adjusts the GC heuristics based on the requested `performance_mode`.
  ///
  /// This operation is a no-op of web. The request to change a performance may be ignored by the
  /// engine or not resolve in a predictable way.
  ///
  /// See [DartPerformanceMode] for more information on individual performance modes.
  void requestDartPerformanceMode(DartPerformanceMode mode) {
    _requestDartPerformanceMode(mode.index);
  }

  @Native<Int Function(Int)>(symbol: 'PlatformConfigurationNativeApi::RequestDartPerformanceMode')
  external static int _requestDartPerformanceMode(int mode);

  /// The embedder can specify data that the isolate can request synchronously
  /// on launch. This accessor fetches that data.
  ///
  /// This data is persistent for the duration of the Flutter application and is
  /// available even after isolate restarts. Because of this lifecycle, the size
  /// of this data must be kept to a minimum.
  ///
  /// For asynchronous communication between the embedder and isolate, a
  /// platform channel may be used.
  ByteData? getPersistentIsolateData() => _getPersistentIsolateData();

  @Native<Handle Function()>(symbol: 'PlatformConfigurationNativeApi::GetPersistentIsolateData')
  external static ByteData? _getPersistentIsolateData();

  /// The system-reported default locale of the device.
  ///
  /// This establishes the language and formatting conventions that application
  /// should, if possible, use to render their user interface.
  ///
  /// This is the first locale selected by the user and is the user's primary
  /// locale (the locale the device UI is displayed in)
  ///
  /// This is equivalent to `locales.first`, except that it will provide an
  /// undefined (using the language tag "und") non-null locale if the [locales]
  /// list has not been set or is empty.
  Locale get locale => locales.isEmpty ? const Locale.fromSubtags() : locales.first;

  /// The full system-reported supported locales of the device.
  ///
  /// This establishes the language and formatting conventions that application
  /// should, if possible, use to render their user interface.
  ///
  /// The list is ordered in order of priority, with lower-indexed locales being
  /// preferred over higher-indexed ones. The first element is the primary
  /// [locale].
  ///
  /// The [onLocaleChanged] callback is called whenever this value changes.
  ///
  /// See also:
  ///
  ///  * [WidgetsBindingObserver], for a mechanism at the widgets layer to
  ///    observe when this value changes.
  List<Locale> get locales => _locales;

  /// Performs the platform-native locale resolution.
  ///
  /// Each platform may return different results.
  ///
  /// If the platform fails to resolve a locale, then this will return null.
  ///
  /// This method returns synchronously and is a direct call to
  /// platform specific APIs without invoking method channels.
  Locale? computePlatformResolvedLocale(List<Locale> supportedLocales) {
    final supportedLocalesData = <String?>[];
    for (final locale in supportedLocales) {
      supportedLocalesData.add(locale.languageCode);
      supportedLocalesData.add(locale.countryCode);
      supportedLocalesData.add(locale.scriptCode);
    }

    final List<String> result = _computePlatformResolvedLocale(supportedLocalesData);

    if (result.isNotEmpty) {
      return Locale.fromSubtags(
        languageCode: result[0],
        countryCode: result[1] == '' ? null : result[1],
        scriptCode: result[2] == '' ? null : result[2],
      );
    }
    return null;
  }

  List<String> _computePlatformResolvedLocale(List<String?> supportedLocalesData) =>
      __computePlatformResolvedLocale(supportedLocalesData);

  @Native<Handle Function(Handle)>(
    symbol: 'PlatformConfigurationNativeApi::ComputePlatformResolvedLocale',
  )
  external static List<String> __computePlatformResolvedLocale(List<String?> supportedLocalesData);

  /// A callback that is invoked whenever [locale] changes value.
  ///
  /// The framework invokes this callback in the same zone in which the callback
  /// was set.
  ///
  /// See also:
  ///
  ///  * [WidgetsBindingObserver], for a mechanism at the widgets layer to
  ///    observe when this callback is invoked.
  VoidCallback? get onLocaleChanged => _onLocaleChanged;
  VoidCallback? _onLocaleChanged;
  Zone _onLocaleChangedZone = Zone.root;
  set onLocaleChanged(VoidCallback? callback) {
    _onLocaleChanged = callback;
    _onLocaleChangedZone = Zone.current;
  }

  // Called from the engine, via hooks.dart
  void _updateLocales(List<String> locales) {
    const stringsPerLocale = 4;
    final int numLocales = locales.length ~/ stringsPerLocale;
    final newLocales = <Locale>[];
    var localesDiffer = numLocales != _locales.length;
    for (var localeIndex = 0; localeIndex < numLocales; localeIndex++) {
      final String countryCode = locales[localeIndex * stringsPerLocale + 1];
      final String scriptCode = locales[localeIndex * stringsPerLocale + 2];

      newLocales.add(
        Locale.fromSubtags(
          languageCode: locales[localeIndex * stringsPerLocale],
          countryCode: countryCode.isEmpty ? null : countryCode,
          scriptCode: scriptCode.isEmpty ? null : scriptCode,
        ),
      );
      if (!localesDiffer && newLocales[localeIndex] != _locales[localeIndex]) {
        localesDiffer = true;
      }
    }
    if (!localesDiffer) {
      return;
    }
    _locales = newLocales;
    _invoke(onLocaleChanged, _onLocaleChangedZone);
  }

  // Called from the engine, via hooks.dart
  String _localeClosure() => locale.toString();

  ErrorCallback? _onError;
  Zone? _onErrorZone;

  /// A callback that is invoked when an unhandled error occurs in the root
  /// isolate.
  ///
  /// This callback must return `true` if it has handled the error. Otherwise,
  /// it must return `false` and a fallback mechanism such as printing to stderr
  /// will be used, as configured by the specific platform embedding via
  /// `Settings::unhandled_exception_callback`.
  ///
  /// The VM or the process may exit or become unresponsive after calling this
  /// callback. The callback will not be called for exceptions that cause the VM
  /// or process to terminate or become unresponsive before the callback can be
  /// invoked.
  ///
  /// This callback is not directly invoked by errors in child isolates of the
  /// root isolate. Programs that create new isolates must listen for errors on
  /// those isolates and forward the errors to the root isolate.
  ErrorCallback? get onError => _onError;
  set onError(ErrorCallback? callback) {
    _onError = callback;
    _onErrorZone = Zone.current;
  }

  bool _dispatchError(Object error, StackTrace stackTrace) {
    if (_onError == null) {
      return false;
    }
    assert(_onErrorZone != null);

    if (identical(_onErrorZone, Zone.current)) {
      return _onError!(error, stackTrace);
    } else {
      try {
        return _onErrorZone!.runBinary<bool, Object, StackTrace>(_onError!, error, stackTrace);
      } catch (e, s) {
        _onErrorZone!.handleUncaughtError(e, s);
        return false;
      }
    }
  }
}

/// An identifier used to select a user's language and formatting preferences.
///
/// This represents a [Unicode Language
/// Identifier](https://www.unicode.org/reports/tr35/#Unicode_language_identifier)
/// (i.e. without Locale extensions), except variants are not supported.
///
/// Locales are canonicalized according to the "preferred value" entries in the
/// [IANA Language Subtag
/// Registry](https://www.iana.org/assignments/language-subtag-registry/language-subtag-registry).
/// For example, `const Locale('he')` and `const Locale('iw')` are equal and
/// both have the [languageCode] `he`, because `iw` is a deprecated language
/// subtag that was replaced by the subtag `he`.
///
/// See also:
///
///  * [PlatformDispatcher.locale], which specifies the system's currently selected
///    [Locale].
class Locale {
  /// Creates a new Locale object. The first argument is the
  /// primary language subtag, the second is the region (also
  /// referred to as 'country') subtag.
  ///
  /// For example:
  ///
  /// ```dart
  /// const Locale swissFrench = Locale('fr', 'CH');
  /// const Locale canadianFrench = Locale('fr', 'CA');
  /// ```
  ///
  /// The primary language subtag must not be null. The region subtag is
  /// optional. When there is no region/country subtag, the parameter should
  /// be omitted or passed `null` instead of an empty-string.
  ///
  /// The subtag values are _case sensitive_ and must be one of the valid
  /// subtags according to CLDR supplemental data:
  /// [language](https://github.com/unicode-org/cldr/blob/master/common/validity/language.xml),
  /// [region](https://github.com/unicode-org/cldr/blob/master/common/validity/region.xml). The
  /// primary language subtag must be at least two and at most eight lowercase
  /// letters, but not four letters. The region subtag must be two
  /// uppercase letters or three digits. See the [Unicode Language
  /// Identifier](https://www.unicode.org/reports/tr35/#Unicode_language_identifier)
  /// specification.
  ///
  /// Validity is not checked by default, but some methods may throw away
  /// invalid data.
  ///
  /// See also:
  ///
  ///  * [Locale.fromSubtags], which also allows a [scriptCode] to be
  ///    specified.
  const Locale(this._languageCode, [this._countryCode])
    : assert(_languageCode != ''),
      scriptCode = null;

  /// Creates a new Locale object.
  ///
  /// The keyword arguments specify the subtags of the Locale.
  ///
  /// The subtag values are _case sensitive_ and must be valid subtags according
  /// to CLDR supplemental data:
  /// [language](https://github.com/unicode-org/cldr/blob/master/common/validity/language.xml),
  /// [script](https://github.com/unicode-org/cldr/blob/master/common/validity/script.xml) and
  /// [region](https://github.com/unicode-org/cldr/blob/master/common/validity/region.xml) for
  /// each of languageCode, scriptCode and countryCode respectively.
  ///
  /// The [languageCode] subtag is optional. When there is no language subtag,
  /// the parameter should be omitted or set to "und". When not supplied, the
  /// [languageCode] defaults to "und", an undefined language code.
  ///
  /// The [countryCode] subtag is optional. When there is no country subtag,
  /// the parameter should be omitted or passed `null` instead of an empty-string.
  ///
  /// Validity is not checked by default, but some methods may throw away
  /// invalid data.
  const Locale.fromSubtags({String languageCode = 'und', this.scriptCode, String? countryCode})
    : assert(languageCode != ''),
      _languageCode = languageCode,
      assert(scriptCode != ''),
      assert(countryCode != ''),
      _countryCode = countryCode;

  /// The primary language subtag for the locale.
  ///
  /// This must not be null. It may be 'und', representing 'undefined'.
  ///
  /// This is expected to be string registered in the [IANA Language Subtag
  /// Registry](https://www.iana.org/assignments/language-subtag-registry/language-subtag-registry)
  /// with the type "language". The string specified must match the case of the
  /// string in the registry.
  ///
  /// Language subtags that are deprecated in the registry and have a preferred
  /// code are changed to their preferred code. For example, `const
  /// Locale('he')` and `const Locale('iw')` are equal, and both have the
  /// [languageCode] `he`, because `iw` is a deprecated language subtag that was
  /// replaced by the subtag `he`.
  ///
  /// This must be a valid Unicode Language subtag as listed in [Unicode CLDR
  /// supplemental
  /// data](https://github.com/unicode-org/cldr/blob/master/common/validity/language.xml).
  ///
  /// See also:
  ///
  ///  * [Locale.fromSubtags], which describes the conventions for creating
  ///    [Locale] objects.
  String get languageCode => _deprecatedLanguageSubtagMap[_languageCode] ?? _languageCode;
  final String _languageCode;

  // This map is generated by //flutter/tools/gen_locale.dart
  // Mappings generated for language subtag registry as of 2019-02-27.
  static const Map<String, String> _deprecatedLanguageSubtagMap = <String, String>{
    'in': 'id', // Indonesian; deprecated 1989-01-01
    'iw': 'he', // Hebrew; deprecated 1989-01-01
    'ji': 'yi', // Yiddish; deprecated 1989-01-01
    'jw': 'jv', // Javanese; deprecated 2001-08-13
    'mo': 'ro', // Moldavian, Moldovan; deprecated 2008-11-22
    'aam': 'aas', // Aramanik; deprecated 2015-02-12
    'adp': 'dz', // Adap; deprecated 2015-02-12
    'aue': 'ktz', // ǂKxʼauǁʼein; deprecated 2015-02-12
    'ayx': 'nun', // Ayi (China); deprecated 2011-08-16
    'bgm': 'bcg', // Baga Mboteni; deprecated 2016-05-30
    'bjd': 'drl', // Bandjigali; deprecated 2012-08-12
    'ccq': 'rki', // Chaungtha; deprecated 2012-08-12
    'cjr': 'mom', // Chorotega; deprecated 2010-03-11
    'cka': 'cmr', // Khumi Awa Chin; deprecated 2012-08-12
    'cmk': 'xch', // Chimakum; deprecated 2010-03-11
    'coy': 'pij', // Coyaima; deprecated 2016-05-30
    'cqu': 'quh', // Chilean Quechua; deprecated 2016-05-30
    'drh': 'khk', // Darkhat; deprecated 2010-03-11
    'drw': 'prs', // Darwazi; deprecated 2010-03-11
    'gav': 'dev', // Gabutamon; deprecated 2010-03-11
    'gfx': 'vaj', // Mangetti Dune ǃXung; deprecated 2015-02-12
    'ggn': 'gvr', // Eastern Gurung; deprecated 2016-05-30
    'gti': 'nyc', // Gbati-ri; deprecated 2015-02-12
    'guv': 'duz', // Gey; deprecated 2016-05-30
    'hrr': 'jal', // Horuru; deprecated 2012-08-12
    'ibi': 'opa', // Ibilo; deprecated 2012-08-12
    'ilw': 'gal', // Talur; deprecated 2013-09-10
    'jeg': 'oyb', // Jeng; deprecated 2017-02-23
    'kgc': 'tdf', // Kasseng; deprecated 2016-05-30
    'kgh': 'kml', // Upper Tanudan Kalinga; deprecated 2012-08-12
    'koj': 'kwv', // Sara Dunjo; deprecated 2015-02-12
    'krm': 'bmf', // Krim; deprecated 2017-02-23
    'ktr': 'dtp', // Kota Marudu Tinagas; deprecated 2016-05-30
    'kvs': 'gdj', // Kunggara; deprecated 2016-05-30
    'kwq': 'yam', // Kwak; deprecated 2015-02-12
    'kxe': 'tvd', // Kakihum; deprecated 2015-02-12
    'kzj': 'dtp', // Coastal Kadazan; deprecated 2016-05-30
    'kzt': 'dtp', // Tambunan Dusun; deprecated 2016-05-30
    'lii': 'raq', // Lingkhim; deprecated 2015-02-12
    'lmm': 'rmx', // Lamam; deprecated 2014-02-28
    'meg': 'cir', // Mea; deprecated 2013-09-10
    'mst': 'mry', // Cataelano Mandaya; deprecated 2010-03-11
    'mwj': 'vaj', // Maligo; deprecated 2015-02-12
    'myt': 'mry', // Sangab Mandaya; deprecated 2010-03-11
    'nad': 'xny', // Nijadali; deprecated 2016-05-30
    'ncp': 'kdz', // Ndaktup; deprecated 2018-03-08
    'nnx': 'ngv', // Ngong; deprecated 2015-02-12
    'nts': 'pij', // Natagaimas; deprecated 2016-05-30
    'oun': 'vaj', // ǃOǃung; deprecated 2015-02-12
    'pcr': 'adx', // Panang; deprecated 2013-09-10
    'pmc': 'huw', // Palumata; deprecated 2016-05-30
    'pmu': 'phr', // Mirpur Panjabi; deprecated 2015-02-12
    'ppa': 'bfy', // Pao; deprecated 2016-05-30
    'ppr': 'lcq', // Piru; deprecated 2013-09-10
    'pry': 'prt', // Pray 3; deprecated 2016-05-30
    'puz': 'pub', // Purum Naga; deprecated 2014-02-28
    'sca': 'hle', // Sansu; deprecated 2012-08-12
    'skk': 'oyb', // Sok; deprecated 2017-02-23
    'tdu': 'dtp', // Tempasuk Dusun; deprecated 2016-05-30
    'thc': 'tpo', // Tai Hang Tong; deprecated 2016-05-30
    'thx': 'oyb', // The; deprecated 2015-02-12
    'tie': 'ras', // Tingal; deprecated 2011-08-16
    'tkk': 'twm', // Takpa; deprecated 2011-08-16
    'tlw': 'weo', // South Wemale; deprecated 2012-08-12
    'tmp': 'tyj', // Tai Mène; deprecated 2016-05-30
    'tne': 'kak', // Tinoc Kallahan; deprecated 2016-05-30
    'tnf': 'prs', // Tangshewi; deprecated 2010-03-11
    'tsf': 'taj', // Southwestern Tamang; deprecated 2015-02-12
    'uok': 'ema', // Uokha; deprecated 2015-02-12
    'xba': 'cax', // Kamba (Brazil); deprecated 2016-05-30
    'xia': 'acn', // Xiandao; deprecated 2013-09-10
    'xkh': 'waw', // Karahawyana; deprecated 2016-05-30
    'xsj': 'suj', // Subi; deprecated 2015-02-12
    'ybd': 'rki', // Yangbye; deprecated 2012-08-12
    'yma': 'lrr', // Yamphe; deprecated 2012-08-12
    'ymt': 'mtm', // Mator-Taygi-Karagas; deprecated 2015-02-12
    'yos': 'zom', // Yos; deprecated 2013-09-10
    'yuu': 'yug', // Yugh; deprecated 2014-02-28
  };

  /// The script subtag for the locale.
  ///
  /// This may be null, indicating that there is no specified script subtag.
  ///
  /// This must be a valid Unicode Language Identifier script subtag as listed
  /// in [Unicode CLDR supplemental
  /// data](https://github.com/unicode-org/cldr/blob/master/common/validity/script.xml).
  ///
  /// See also:
  ///
  ///  * [Locale.fromSubtags], which describes the conventions for creating
  ///    [Locale] objects.
  final String? scriptCode;

  /// The region subtag for the locale.
  ///
  /// This may be null, indicating that there is no specified region subtag.
  ///
  /// This is expected to be string registered in the [IANA Language Subtag
  /// Registry](https://www.iana.org/assignments/language-subtag-registry/language-subtag-registry)
  /// with the type "region". The string specified must match the case of the
  /// string in the registry.
  ///
  /// Region subtags that are deprecated in the registry and have a preferred
  /// code are changed to their preferred code. For example, `const Locale('de',
  /// 'DE')` and `const Locale('de', 'DD')` are equal, and both have the
  /// [countryCode] `DE`, because `DD` is a deprecated language subtag that was
  /// replaced by the subtag `DE`.
  ///
  /// See also:
  ///
  ///  * [Locale.fromSubtags], which describes the conventions for creating
  ///    [Locale] objects.
  String? get countryCode => _deprecatedRegionSubtagMap[_countryCode] ?? _countryCode;
  final String? _countryCode;

  // This map is generated by //flutter/tools/gen_locale.dart
  // Mappings generated for language subtag registry as of 2019-02-27.
  static const Map<String, String> _deprecatedRegionSubtagMap = <String, String>{
    'BU': 'MM', // Burma; deprecated 1989-12-05
    'DD': 'DE', // German Democratic Republic; deprecated 1990-10-30
    'FX': 'FR', // Metropolitan France; deprecated 1997-07-14
    'TP': 'TL', // East Timor; deprecated 2002-05-20
    'YD': 'YE', // Democratic Yemen; deprecated 1990-08-14
    'ZR': 'CD', // Zaire; deprecated 1997-07-14
  };

  @override
  bool operator ==(Object other) {
    if (identical(this, other)) {
      return true;
    }
    if (other is! Locale) {
      return false;
    }
    final String? thisCountryCode = countryCode;
    final String? otherCountryCode = other.countryCode;
    return other.languageCode == languageCode &&
        other.scriptCode ==
            scriptCode // scriptCode cannot be ''
            &&
        (other.countryCode ==
                thisCountryCode // Treat '' as equal to null.
                ||
            otherCountryCode != null && otherCountryCode.isEmpty && thisCountryCode == null ||
            thisCountryCode != null && thisCountryCode.isEmpty && other.countryCode == null);
  }

  @override
  int get hashCode => Object.hash(languageCode, scriptCode, countryCode == '' ? null : countryCode);

  static Locale? _cachedLocale;
  static String? _cachedLocaleString;

  /// Returns a string representing the locale.
  ///
  /// This identifier happens to be a valid Unicode Locale Identifier using
  /// underscores as separator, however it is intended to be used for debugging
  /// purposes only. For parsable results, use [toLanguageTag] instead.
  @keepToString
  @override
  String toString() {
    if (!identical(_cachedLocale, this)) {
      _cachedLocale = this;
      _cachedLocaleString = _rawToString('_');
    }
    return _cachedLocaleString!;
  }

  /// Returns a syntactically valid Unicode BCP47 Locale Identifier.
  ///
  /// Some examples of such identifiers: "en", "es-419", "hi-Deva-IN" and
  /// "zh-Hans-CN". See http://www.unicode.org/reports/tr35/ for technical
  /// details.
  String toLanguageTag() => _rawToString('-');

  String _rawToString(String separator) {
    final out = StringBuffer(languageCode);
    if (scriptCode != null && scriptCode!.isNotEmpty) {
      out.write('$separator$scriptCode');
    }
    final String? countryCode = _countryCode;
    if (countryCode != null && countryCode.isNotEmpty) {
      out.write('$separator${this.countryCode}');
    }
    return out.toString();
  }
}

/// Various performance modes for tuning the Dart VM's GC performance.
///
/// For the editor of this enum, please keep the order in sync with `Dart_PerformanceMode`
/// in [dart_api.h](https://github.com/dart-lang/sdk/blob/main/runtime/include/dart_api.h#L1302).
enum DartPerformanceMode {
  /// This is the default mode that the Dart VM is in.
  balanced,

  /// Optimize for low latency, at the expense of throughput and memory overhead
  /// by performing work in smaller batches (requiring more overhead) or by
  /// delaying work (requiring more memory). An embedder should not remain in
  /// this mode indefinitely.
  latency,

  /// Optimize for high throughput, at the expense of latency and memory overhead
  /// by performing work in larger batches with more intervening growth.
  throughput,

  /// Optimize for low memory, at the expensive of throughput and latency by more
  /// frequently performing work.
  memory,
}
