// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_WINDOWS_CLIENT_WRAPPER_INCLUDE_FLUTTER_PLUGIN_REGISTRAR_WINDOWS_H_
#define FLUTTER_SHELL_PLATFORM_WINDOWS_CLIENT_WRAPPER_INCLUDE_FLUTTER_PLUGIN_REGISTRAR_WINDOWS_H_

#include <flutter_windows.h>
#include <windows.h>

#include <memory>
#include <optional>

#include "plugin_registrar.h"

namespace flutter {

// A delegate callback for WindowProc delegation.
//
// Implementations should return a value only if they have handled the message
// and want to stop all further handling.
using WindowProcDelegate = std::function<std::optional<
    LRESULT>(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)>;

// An extension to PluginRegistrar providing access to Windows-specific
// functionality.
class PluginRegistrarWindows : public PluginRegistrar {
 public:
  // Creates a new PluginRegistrar. |core_registrar| and the messenger it
  // provides must remain valid as long as this object exists.
  explicit PluginRegistrarWindows(
      FlutterDesktopPluginRegistrarRef core_registrar)
      : PluginRegistrar(core_registrar) {}

  virtual ~PluginRegistrarWindows() {
    // Must be the first call.
    ClearPlugins();
  }

  // Prevent copying.
  PluginRegistrarWindows(PluginRegistrarWindows const&) = delete;
  PluginRegistrarWindows& operator=(PluginRegistrarWindows const&) = delete;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_WINDOWS_CLIENT_WRAPPER_INCLUDE_FLUTTER_PLUGIN_REGISTRAR_WINDOWS_H_
