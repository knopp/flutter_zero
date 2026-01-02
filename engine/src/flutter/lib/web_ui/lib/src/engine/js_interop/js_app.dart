// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:js_interop';

/// The public JS API of a running Flutter Web App.
extension type FlutterApp._primary(JSObject _) implements JSObject {
  factory FlutterApp({
    required AddFlutterViewFn addView,
    required RemoveFlutterViewFn removeView,
  }) => FlutterApp._(addView: addView.toJS, removeView: ((int id) => removeView(id)).toJS);
  external factory FlutterApp._({required JSFunction addView, required JSFunction removeView});

  @JS('addView')
  external int addView();

  @JS('removeView')
  external void removeView(int id);
}

/// Typedef for the function that adds a new view to the app.
///
/// Returns the ID of the newly created view.
typedef AddFlutterViewFn = int Function();

/// Typedef for the function that removes a view from the app.
///
/// Returns the configuration used to create the view.
typedef RemoveFlutterViewFn = void Function(int);
