// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "flutter/shell/platform/darwin/ios/platform_view_ios.h"
#include <memory>

#include <utility>

#include "flutter/common/task_runners.h"
#include "flutter/fml/synchronization/waitable_event.h"
#include "flutter/fml/trace_event.h"
#import "flutter/shell/platform/darwin/common/InternalFlutterSwiftCommon/InternalFlutterSwiftCommon.h"

FLUTTER_ASSERT_ARC

namespace flutter {

PlatformViewIOS::PlatformViewIOS(PlatformView::Delegate& delegate,
                                 const flutter::TaskRunners& task_runners)
    : PlatformView(delegate, task_runners),
      platform_message_handler_(
          new PlatformMessageHandlerIos(task_runners.GetPlatformTaskRunner())) {}

PlatformViewIOS::~PlatformViewIOS() = default;

// |PlatformView|
void PlatformViewIOS::HandlePlatformMessage(std::unique_ptr<flutter::PlatformMessage> message) {
  platform_message_handler_->HandlePlatformMessage(std::move(message));
}

// |PlatformView|
void PlatformViewIOS::OnPreEngineRestart() const {}

// |PlatformView|
std::unique_ptr<std::vector<std::string>> PlatformViewIOS::ComputePlatformResolvedLocales(
    const std::vector<std::string>& supported_locale_data) {
  size_t localeDataLength = 3;
  NSMutableArray<NSString*>* supported_locale_identifiers =
      [NSMutableArray arrayWithCapacity:supported_locale_data.size() / localeDataLength];
  for (size_t i = 0; i < supported_locale_data.size(); i += localeDataLength) {
    NSDictionary<NSString*, NSString*>* dict = @{
      NSLocaleLanguageCode : [NSString stringWithUTF8String:supported_locale_data[i].c_str()]
          ?: @"",
      NSLocaleCountryCode : [NSString stringWithUTF8String:supported_locale_data[i + 1].c_str()]
          ?: @"",
      NSLocaleScriptCode : [NSString stringWithUTF8String:supported_locale_data[i + 2].c_str()]
          ?: @""
    };
    [supported_locale_identifiers addObject:[NSLocale localeIdentifierFromComponents:dict]];
  }
  NSArray<NSString*>* result =
      [NSBundle preferredLocalizationsFromArray:supported_locale_identifiers];

  // Output format should be either empty or 3 strings for language, country, and script.
  std::unique_ptr<std::vector<std::string>> out = std::make_unique<std::vector<std::string>>();

  if (result != nullptr && [result count] > 0) {
    NSLocale* locale = [NSLocale localeWithLocaleIdentifier:[result firstObject]];
    NSString* languageCode = [locale languageCode];
    out->emplace_back(languageCode == nullptr ? "" : languageCode.UTF8String);
    NSString* countryCode = [locale countryCode];
    out->emplace_back(countryCode == nullptr ? "" : countryCode.UTF8String);
    NSString* scriptCode = [locale scriptCode];
    out->emplace_back(scriptCode == nullptr ? "" : scriptCode.UTF8String);
  }
  return out;
}

PlatformViewIOS::ScopedObserver::ScopedObserver() {}

PlatformViewIOS::ScopedObserver::~ScopedObserver() {
  if (observer_) {
    [[NSNotificationCenter defaultCenter] removeObserver:observer_];
  }
}

void PlatformViewIOS::ScopedObserver::reset(id<NSObject> observer) {
  if (observer != observer_) {
    if (observer_) {
      [[NSNotificationCenter defaultCenter] removeObserver:observer_];
    }
    observer_ = observer;
  }
}

}  // namespace flutter
