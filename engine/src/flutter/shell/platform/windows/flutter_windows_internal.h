// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_WINDOWS_FLUTTER_WINDOWS_INTERNAL_H_
#define FLUTTER_SHELL_PLATFORM_WINDOWS_FLUTTER_WINDOWS_INTERNAL_H_

#include "flutter/shell/platform/windows/public/flutter_windows.h"

#if defined(__cplusplus)
extern "C" {
#endif

// Returns the engine associated with the given identifier. Engine identifier
// must be valid and for a running engine, otherwise the behavior is undefined.
//
// Identifier can be obtained from PlatformDispatcher.instance.engineId.
//
// This method must be called from the platform thread.
FLUTTER_EXPORT FlutterDesktopEngineRef
FlutterDesktopEngineForId(int64_t engine_id);

#if defined(__cplusplus)
}
#endif

#endif  // FLUTTER_SHELL_PLATFORM_WINDOWS_FLUTTER_WINDOWS_INTERNAL_H_
