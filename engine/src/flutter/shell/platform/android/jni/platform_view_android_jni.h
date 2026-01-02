// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_ANDROID_JNI_PLATFORM_VIEW_ANDROID_JNI_H_
#define FLUTTER_SHELL_PLATFORM_ANDROID_JNI_PLATFORM_VIEW_ANDROID_JNI_H_

#include <utility>

#include "flutter/fml/mapping.h"

#include "flutter/lib/ui/window/platform_message.h"

#if FML_OS_ANDROID
#include "flutter/fml/platform/android/scoped_java_ref.h"
#endif

struct ASurfaceTransaction;

namespace flutter {

#if FML_OS_ANDROID
using JavaLocalRef = fml::jni::ScopedJavaLocalRef<jobject>;
#else
using JavaLocalRef = std::nullptr_t;
#endif

//------------------------------------------------------------------------------
/// Allows to call Java code running in the JVM from any thread. However, most
/// methods can only be called from the platform thread as that is where the
/// Java code runs.
///
/// This interface must not depend on the Android toolchain directly, so it can
/// be used in unit tests compiled with the host toolchain.
///
class PlatformViewAndroidJNI {
 public:
  virtual ~PlatformViewAndroidJNI();

  //----------------------------------------------------------------------------
  /// @brief      Sends a platform message. The message may be empty.
  ///
  virtual void FlutterViewHandlePlatformMessage(
      std::unique_ptr<flutter::PlatformMessage> message,
      int responseId) = 0;

  //----------------------------------------------------------------------------
  /// @brief      Responds to a platform message. The data may be a `nullptr`.
  ///
  virtual void FlutterViewHandlePlatformMessageResponse(
      int responseId,
      std::unique_ptr<fml::Mapping> data) = 0;

  //----------------------------------------------------------------------------
  /// @brief      Indicates that a hot restart is about to happen.
  ///
  virtual void FlutterViewOnPreEngineRestart() = 0;

  //----------------------------------------------------------------------------
  /// @brief      Computes the locale Android would select.
  ///
  virtual std::unique_ptr<std::vector<std::string>>
  FlutterViewComputePlatformResolvedLocale(
      std::vector<std::string> supported_locales_data) = 0;

  virtual bool RequestDartDeferredLibrary(int loading_unit_id) = 0;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_ANDROID_JNI_PLATFORM_VIEW_ANDROID_JNI_H_
