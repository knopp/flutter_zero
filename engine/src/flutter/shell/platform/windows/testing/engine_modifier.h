// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_WINDOWS_TESTING_ENGINE_MODIFIER_H_
#define FLUTTER_SHELL_PLATFORM_WINDOWS_TESTING_ENGINE_MODIFIER_H_

#include "flutter/shell/platform/windows/flutter_windows_engine.h"

#include "flutter/fml/macros.h"

namespace flutter {

// A test utility class providing the ability to access and alter various
// private fields in an Engine instance.
//
// This simply provides a way to access the normally-private embedder proc
// table, so the lifetime of any changes made to the proc table is that of the
// engine object, not this helper.
class EngineModifier {
 public:
  explicit EngineModifier(FlutterWindowsEngine* engine) : engine_(engine) {}

  // Returns the engine's embedder API proc table, allowing for modification.
  //
  // Modifications are to the engine, and will last for the lifetime of the
  // engine unless overwritten again.
  FlutterEngineProcTable& embedder_api() { return engine_->embedder_api_; }

  // Run the FlutterWindowsEngine's handler that runs right before an engine
  // restart. This resets the keyboard's state if it exists.
  void Restart() { engine_->OnPreEngineRestart(); }

 private:
  FlutterWindowsEngine* engine_;

  FML_DISALLOW_COPY_AND_ASSIGN(EngineModifier);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_WINDOWS_TESTING_ENGINE_MODIFIER_H_
