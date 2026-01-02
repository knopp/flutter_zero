// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_DARWIN_MACOS_FRAMEWORK_SOURCE_FLUTTERENGINE_INTERNAL_H_
#define FLUTTER_SHELL_PLATFORM_DARWIN_MACOS_FRAMEWORK_SOURCE_FLUTTERENGINE_INTERNAL_H_

#include <vector>
#import "flutter/shell/platform/darwin/macos/framework/Headers/FlutterEngine.h"

#import <Cocoa/Cocoa.h>

#include <memory>

#include "flutter/shell/platform/embedder/embedder.h"

NS_ASSUME_NONNULL_BEGIN

#pragma mark - Typedefs

typedef void (^FlutterTerminationCallback)(id _Nullable sender);

#pragma mark - Enumerations

/**
 * An enum for defining the different request types allowed when requesting an
 * application exit.
 *
 * Must match the entries in the `AppExitType` enum in the Dart code.
 */
typedef NS_ENUM(NSInteger, FlutterAppExitType) {
  kFlutterAppExitTypeCancelable = 0,
  kFlutterAppExitTypeRequired = 1,
};

/**
 * An enum for defining the different responses the framework can give to an
 * application exit request from the engine.
 *
 * Must match the entries in the `AppExitResponse` enum in the Dart code.
 */
typedef NS_ENUM(NSInteger, FlutterAppExitResponse) {
  kFlutterAppExitResponseCancel = 0,
  kFlutterAppExitResponseExit = 1,
};

@interface FlutterEngine ()

/**
 * True if the engine is currently running.
 */
@property(nonatomic, readonly) BOOL running;

/**
 * Function pointers for interacting with the embedder.h API.
 */
@property(nonatomic) FlutterEngineProcTable& embedderAPI;

/**
 * True if the semantics is enabled. The Flutter framework starts sending
 * semantics update through the embedder as soon as it is set to YES.
 */
@property(nonatomic) BOOL semanticsEnabled;

/**
 * The executable name for the current process.
 */
@property(nonatomic, readonly, nonnull) NSString* executableName;

/**
 * The command line arguments array for the engine.
 */
@property(nonatomic, readonly) std::vector<std::string> switches;

/**
 * Returns engine for the identifier. The identifier must be valid for an engine
 * that is currently running, otherwise the behavior is undefined.
 *
 * The identifier can be obtained in Dart code through
 * `PlatformDispatcher.instance.engineId`.
 *
 * This function must be called on the main thread.
 */
+ (nullable FlutterEngine*)engineForIdentifier:(int64_t)identifier;

@end

NS_ASSUME_NONNULL_END

#endif  // FLUTTER_SHELL_PLATFORM_DARWIN_MACOS_FRAMEWORK_SOURCE_FLUTTERENGINE_INTERNAL_H_
