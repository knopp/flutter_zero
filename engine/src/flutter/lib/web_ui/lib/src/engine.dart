// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file is transformed during the build process into a single library with
// part files (`dart:_engine`) by performing the following:
//
//  - Replace all exports with part directives.
//  - Rewrite the libraries into `part of` part files without imports.
//  - Add imports to this file sufficient to cover the needs of `dart:_engine`.
//
// The code that performs the transformations lives in:
//
//  - https://github.com/flutter/flutter/blob/main/engine/src/flutter/web_sdk/sdk_rewriter.dart
// ignore: unnecessary_library_directive
library engine;

export 'engine/alarm_clock.dart';
export 'engine/app_bootstrap.dart';
export 'engine/arena.dart';
export 'engine/browser_detection.dart';
export 'engine/configuration.dart';
export 'engine/dom.dart';
export 'engine/initialization.dart';
export 'engine/js_interop/js_app.dart';
export 'engine/js_interop/js_loader.dart';
export 'engine/js_interop/js_promise.dart';
export 'engine/js_interop/js_typed_data.dart';
export 'engine/platform_dispatcher.dart';
export 'engine/plugins.dart';
export 'engine/profiler.dart';
export 'engine/safe_browser_api.dart';
export 'engine/services/buffers.dart';
export 'engine/services/message_codec.dart';
export 'engine/services/message_codecs.dart';
export 'engine/services/serialization.dart';
export 'engine/shader_data.dart';
export 'engine/util.dart';
export 'engine/validators.dart';
export 'engine/vector_math.dart';
