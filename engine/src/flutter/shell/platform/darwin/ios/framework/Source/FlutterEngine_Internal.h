// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_DARWIN_IOS_FRAMEWORK_SOURCE_FLUTTERENGINE_INTERNAL_H_
#define FLUTTER_SHELL_PLATFORM_DARWIN_IOS_FRAMEWORK_SOURCE_FLUTTERENGINE_INTERNAL_H_

#import "flutter/shell/platform/darwin/ios/framework/Headers/FlutterEngine.h"

#include "flutter/fml/memory/weak_ptr.h"
#include "flutter/fml/task_runner.h"
#include "flutter/shell/common/platform_view.h"
#include "flutter/shell/common/shell.h"

#include "flutter/shell/platform/embedder/embedder.h"

#import "flutter/shell/platform/darwin/ios/framework/Headers/FlutterEngine.h"
#import "flutter/shell/platform/darwin/ios/framework/Source/FlutterDartProject_Internal.h"
#import "flutter/shell/platform/darwin/ios/framework/Source/FlutterSceneLifeCycle_Internal.h"

NS_ASSUME_NONNULL_BEGIN

@interface FlutterEngine ()

// Indicates whether this engine has **ever** been manually registered to a scene.
@property(nonatomic, assign) BOOL manuallyRegisteredToScene;

- (fml::RefPtr<fml::TaskRunner>)platformTaskRunner;
- (fml::RefPtr<fml::TaskRunner>)uiTaskRunner;

- (FlutterEnginePluginSceneLifeCycleDelegate*)sceneLifeCycleDelegate;

- (void)launchEngine:(nullable NSString*)entrypoint
          libraryURI:(nullable NSString*)libraryOrNil
      entrypointArgs:(nullable NSArray<NSString*>*)entrypointArgs;
- (BOOL)createShell:(nullable NSString*)entrypoint
         libraryURI:(nullable NSString*)libraryOrNil
       initialRoute:(nullable NSString*)initialRoute;

- (void)notifyLowMemory;

/**
 * Creates one running FlutterEngine from another, sharing components between them.
 *
 * This results in a faster creation time and a smaller memory footprint engine.
 * This should only be called on a FlutterEngine that is running.
 */
- (FlutterEngine*)spawnWithEntrypoint:(nullable NSString*)entrypoint
                           libraryURI:(nullable NSString*)libraryURI
                         initialRoute:(nullable NSString*)initialRoute
                       entrypointArgs:(nullable NSArray<NSString*>*)entrypointArgs;

@property(nonatomic, readonly) FlutterDartProject* project;

/**
 * Returns the engine handle. Used in FlutterEngineTest.
 */
- (int64_t)engineIdentifier;

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

- (void)addSceneLifeCycleDelegate:(NSObject<FlutterSceneLifeCycleDelegate>*)delegate;

/*
 * Performs AppDelegate callback provided through the `FlutterImplicitEngineDelegate` protocol to
 * inform apps that the implicit `FlutterEngine` has initialized.
 */
- (BOOL)performImplicitEngineCallback;

/*
 * Creates a `FlutterEngineApplicationRegistrar` that can be used to access application-level
 * services, such as the engine's `FlutterBinaryMessenger` or `FlutterTextureRegistry`.
 */
- (NSObject<FlutterApplicationRegistrar>*)registrarForApplication:(NSString*)key;

@end

@interface FlutterImplicitEngineBridgeImpl : NSObject <FlutterImplicitEngineBridge>

@end

NS_ASSUME_NONNULL_END

#endif  // FLUTTER_SHELL_PLATFORM_DARWIN_IOS_FRAMEWORK_SOURCE_FLUTTERENGINE_INTERNAL_H_
