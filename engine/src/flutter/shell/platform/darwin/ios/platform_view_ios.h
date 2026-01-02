// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_DARWIN_IOS_PLATFORM_VIEW_IOS_H_
#define FLUTTER_SHELL_PLATFORM_DARWIN_IOS_PLATFORM_VIEW_IOS_H_

#include <memory>

#include "flutter/fml/closure.h"
#include "flutter/fml/macros.h"
#include "flutter/shell/common/platform_view.h"
#import "flutter/shell/platform/darwin/ios/platform_message_handler_ios.h"

@class FlutterViewController;

namespace flutter {

/**
 * A bridge connecting the platform agnostic shell and the iOS embedding.
 *
 * The shell provides and requests for UI related data and this PlatformView subclass fulfills
 * it with iOS specific capabilities. As an example, the iOS embedding (the `FlutterEngine` and the
 * `FlutterViewController`) sends pointer data to the shell and receives the shell's request for a
 * Impeller AiksContext and supplies it.
 *
 * Despite the name "view", this class is unrelated to UIViews on iOS and doesn't have the same
 * lifecycle. It's a long lived bridge owned by the `FlutterEngine` and can be attached and
 * detached sequentially to multiple `FlutterViewController`s and `FlutterView`s.
 */
class PlatformViewIOS final : public PlatformView {
 public:
  PlatformViewIOS(PlatformView::Delegate& delegate, const flutter::TaskRunners& task_runners);

  ~PlatformViewIOS() override;

  // |PlatformView|
  void OnPreEngineRestart() const override;

  // |PlatformView|
  void HandlePlatformMessage(std::unique_ptr<flutter::PlatformMessage> message) override;

  // |PlatformView|
  std::unique_ptr<std::vector<std::string>> ComputePlatformResolvedLocales(
      const std::vector<std::string>& supported_locale_data) override;

  std::shared_ptr<PlatformMessageHandlerIos> GetPlatformMessageHandlerIos() const {
    return platform_message_handler_;
  }

  std::shared_ptr<PlatformMessageHandler> GetPlatformMessageHandler() const override {
    return platform_message_handler_;
  }

 private:
  /// Smart pointer for use with objective-c observers.
  /// This guarantees we remove the observer.
  class ScopedObserver {
   public:
    ScopedObserver();
    ~ScopedObserver();
    void reset(id<NSObject> observer);
    ScopedObserver(const ScopedObserver&) = delete;
    ScopedObserver& operator=(const ScopedObserver&) = delete;

   private:
    id<NSObject> observer_ = nil;
  };

  std::string application_locale_;
  // Since the `ios_surface_` is created on the platform thread but
  // used on the raster thread we need to protect it with a mutex.
  std::mutex ios_surface_mutex_;
  ScopedObserver dealloc_view_controller_observer_;
  std::vector<std::string> platform_resolved_locale_;
  std::shared_ptr<PlatformMessageHandlerIos> platform_message_handler_;

  FML_DISALLOW_COPY_AND_ASSIGN(PlatformViewIOS);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_DARWIN_IOS_PLATFORM_VIEW_IOS_H_
