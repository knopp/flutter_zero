// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <Foundation/Foundation.h>

#import <OCMock/OCMock.h>
#import <XCTest/XCTest.h>

#import <objc/runtime.h>

#import "flutter/common/settings.h"
#include "flutter/fml/synchronization/sync_switch.h"
#include "flutter/fml/synchronization/waitable_event.h"
#import "flutter/shell/platform/darwin/common/framework/Headers/FlutterMacros.h"
#import "flutter/shell/platform/darwin/common/framework/Source/FlutterBinaryMessengerRelay.h"
#import "flutter/shell/platform/darwin/ios/framework/Source/FlutterDartProject_Internal.h"
#import "flutter/shell/platform/darwin/ios/framework/Source/FlutterEngine_Internal.h"
#import "flutter/shell/platform/darwin/ios/framework/Source/FlutterEngine_Test.h"
#import "flutter/shell/platform/darwin/ios/framework/Source/FlutterSceneLifeCycle_Internal.h"
#import "flutter/shell/platform/darwin/ios/framework/Source/FlutterSharedApplication.h"
#import "flutter/shell/platform/darwin/ios/platform_view_ios.h"
FLUTTER_ASSERT_ARC

@protocol TestFlutterPluginWithSceneEvents <NSObject, FlutterPlugin, FlutterSceneLifeCycleDelegate>
@end

@interface FlutterEngineSpy : FlutterEngine
@property(nonatomic) BOOL ensureSemanticsEnabledCalled;
@end

@implementation FlutterEngineSpy

- (void)ensureSemanticsEnabled {
  _ensureSemanticsEnabledCalled = YES;
}

@end

@interface FlutterEngine ()

@end

/// FlutterBinaryMessengerRelay used for testing that setting FlutterEngine.binaryMessenger to
/// the current instance doesn't trigger a use-after-free bug.
///
/// See: testSetBinaryMessengerToSameBinaryMessenger
@interface FakeBinaryMessengerRelay : FlutterBinaryMessengerRelay
@property(nonatomic, assign) BOOL failOnDealloc;
@end

@implementation FakeBinaryMessengerRelay
- (void)dealloc {
  if (_failOnDealloc) {
    XCTFail("FakeBinaryMessageRelay should not be deallocated");
  }
}
@end

@interface FlutterEngineTest : XCTestCase
@end

@implementation FlutterEngineTest

- (void)setUp {
}

- (void)tearDown {
}

- (void)testCreate {
  FlutterDartProject* project = [[FlutterDartProject alloc] init];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"foobar" project:project];
  XCTAssertNotNil(engine);
}

- (void)testShellGetters {
  FlutterDartProject* project = [[FlutterDartProject alloc] init];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"foobar" project:project];
  XCTAssertNotNil(engine);

  // Ensure getters don't deref _shell when it's null, and instead return nullptr.
  XCTAssertEqual(engine.platformTaskRunner.get(), nullptr);
  XCTAssertEqual(engine.uiTaskRunner.get(), nullptr);
}

- (void)testInfoPlist {
  // Check the embedded Flutter.framework Info.plist, not the linked dylib.
  NSURL* flutterFrameworkURL =
      [NSBundle.mainBundle.privateFrameworksURL URLByAppendingPathComponent:@"Flutter.framework"];
  NSBundle* flutterBundle = [NSBundle bundleWithURL:flutterFrameworkURL];
  XCTAssertEqualObjects(flutterBundle.bundleIdentifier, @"io.flutter.flutter");

  NSDictionary<NSString*, id>* infoDictionary = flutterBundle.infoDictionary;

  // OS version can have one, two, or three digits: "8", "8.0", "8.0.0"
  NSError* regexError = NULL;
  NSRegularExpression* osVersionRegex =
      [NSRegularExpression regularExpressionWithPattern:@"((0|[1-9]\\d*)\\.)*(0|[1-9]\\d*)"
                                                options:NSRegularExpressionCaseInsensitive
                                                  error:&regexError];
  XCTAssertNil(regexError);

  // Smoke test the test regex.
  NSString* testString = @"9";
  NSUInteger versionMatches =
      [osVersionRegex numberOfMatchesInString:testString
                                      options:NSMatchingAnchored
                                        range:NSMakeRange(0, testString.length)];
  XCTAssertEqual(versionMatches, 1UL);
  testString = @"9.1";
  versionMatches = [osVersionRegex numberOfMatchesInString:testString
                                                   options:NSMatchingAnchored
                                                     range:NSMakeRange(0, testString.length)];
  XCTAssertEqual(versionMatches, 1UL);
  testString = @"9.0.1";
  versionMatches = [osVersionRegex numberOfMatchesInString:testString
                                                   options:NSMatchingAnchored
                                                     range:NSMakeRange(0, testString.length)];
  XCTAssertEqual(versionMatches, 1UL);
  testString = @".0.1";
  versionMatches = [osVersionRegex numberOfMatchesInString:testString
                                                   options:NSMatchingAnchored
                                                     range:NSMakeRange(0, testString.length)];
  XCTAssertEqual(versionMatches, 0UL);

  // Test Info.plist values.
  NSString* minimumOSVersion = infoDictionary[@"MinimumOSVersion"];
  versionMatches = [osVersionRegex numberOfMatchesInString:minimumOSVersion
                                                   options:NSMatchingAnchored
                                                     range:NSMakeRange(0, minimumOSVersion.length)];
  XCTAssertEqual(versionMatches, 1UL);

  // SHA length is 40.
  XCTAssertEqual(((NSString*)infoDictionary[@"FlutterEngine"]).length, 40UL);

  // {clang_version} placeholder is 15 characters. The clang string version
  // is longer than that, so check if the placeholder has been replaced, without
  // actually checking a literal string, which could be different on various machines.
  XCTAssertTrue(((NSString*)infoDictionary[@"ClangVersion"]).length > 15UL);
}

- (void)testDeallocated {
  __weak FlutterEngine* weakEngine = nil;
  @autoreleasepool {
    FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"foobar"];
    weakEngine = engine;
    [engine run];
    XCTAssertNotNil(weakEngine);
  }
  XCTAssertNil(weakEngine);
}

- (void)testSendMessageBeforeRun {
  FlutterDartProject* project = [[FlutterDartProject alloc] init];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"foobar" project:project];
  XCTAssertNotNil(engine);
  XCTAssertThrows([engine.binaryMessenger
      sendOnChannel:@"foo"
            message:[@"bar" dataUsingEncoding:NSUTF8StringEncoding]
        binaryReply:nil]);
}

- (void)testSetMessageHandlerBeforeRun {
  FlutterDartProject* project = [[FlutterDartProject alloc] init];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"foobar" project:project];
  XCTAssertNotNil(engine);
  XCTAssertThrows([engine.binaryMessenger
      setMessageHandlerOnChannel:@"foo"
            binaryMessageHandler:^(NSData* _Nullable message, FlutterBinaryReply _Nonnull reply){

            }]);
}

- (void)testNilSetMessageHandlerBeforeRun {
  FlutterDartProject* project = [[FlutterDartProject alloc] init];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"foobar" project:project];
  XCTAssertNotNil(engine);
  XCTAssertNoThrow([engine.binaryMessenger setMessageHandlerOnChannel:@"foo"
                                                 binaryMessageHandler:nil]);
}

- (void)testNotifyPluginOfDealloc {
  id plugin = OCMProtocolMock(@protocol(FlutterPlugin));
  OCMStub([plugin detachFromEngineForRegistrar:[OCMArg any]]);
  @autoreleasepool {
    FlutterDartProject* project = [[FlutterDartProject alloc] init];
    FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"engine" project:project];
    NSObject<FlutterPluginRegistrar>* registrar = [engine registrarForPlugin:@"plugin"];
    [registrar publish:plugin];
  }
  OCMVerify([plugin detachFromEngineForRegistrar:[OCMArg any]]);
}

- (void)testSetBinaryMessengerToSameBinaryMessenger {
  FakeBinaryMessengerRelay* fakeBinaryMessenger = [[FakeBinaryMessengerRelay alloc] init];

  FlutterEngine* engine = [[FlutterEngine alloc] init];
  [engine setBinaryMessenger:fakeBinaryMessenger];

  // Verify that the setter doesn't free the old messenger before setting the new messenger.
  fakeBinaryMessenger.failOnDealloc = YES;
  [engine setBinaryMessenger:fakeBinaryMessenger];

  // Don't fail when ARC releases the binary messenger.
  fakeBinaryMessenger.failOnDealloc = NO;
}

- (void)testSpawn {
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"foobar"];
  [engine run];
  FlutterEngine* spawn = [engine spawnWithEntrypoint:nil
                                          libraryURI:nil
                                        initialRoute:nil
                                      entrypointArgs:nil];
  XCTAssertNotNil(spawn);
}

- (void)testEngineId {
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"foobar"];
  [engine run];
  int64_t id1 = engine.engineIdentifier;
  XCTAssertTrue(id1 != 0);
  FlutterEngine* spawn = [engine spawnWithEntrypoint:nil
                                          libraryURI:nil
                                        initialRoute:nil
                                      entrypointArgs:nil];
  int64_t id2 = spawn.engineIdentifier;
  XCTAssertEqual([FlutterEngine engineForIdentifier:id1], engine);
  XCTAssertEqual([FlutterEngine engineForIdentifier:id2], spawn);
}

- (void)testSetHandlerAfterRun {
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"foobar"];
  XCTestExpectation* gotMessage = [self expectationWithDescription:@"gotMessage"];
  dispatch_async(dispatch_get_main_queue(), ^{
    NSObject<FlutterPluginRegistrar>* registrar = [engine registrarForPlugin:@"foo"];
    fml::AutoResetWaitableEvent latch;
    [engine run];
    flutter::Shell& shell = engine.shell;
    fml::TaskRunner::RunNowOrPostTask(
        engine.shell.GetTaskRunners().GetUITaskRunner(), [&latch, &shell] {
          flutter::Engine::Delegate& delegate = shell;
          auto message = std::make_unique<flutter::PlatformMessage>("foo", nullptr);
          delegate.OnEngineHandlePlatformMessage(std::move(message));
          latch.Signal();
        });
    latch.Wait();
    [registrar.messenger setMessageHandlerOnChannel:@"foo"
                               binaryMessageHandler:^(NSData* message, FlutterBinaryReply reply) {
                                 [gotMessage fulfill];
                               }];
  });
  [self waitForExpectations:@[ gotMessage ]];
}

// - (void)testThreadPrioritySetCorrectly {
//   XCTestExpectation* prioritiesSet = [self expectationWithDescription:@"prioritiesSet"];
//   prioritiesSet.expectedFulfillmentCount = 2;

//   IMP mockSetThreadPriority =
//       imp_implementationWithBlock(^(NSThread* thread, double threadPriority) {
//         if ([thread.name hasSuffix:@".raster"]) {
//           XCTAssertEqual(threadPriority, 1.0);
//           [prioritiesSet fulfill];
//         } else if ([thread.name hasSuffix:@".io"]) {
//           XCTAssertEqual(threadPriority, 0.5);
//           [prioritiesSet fulfill];
//         }
//       });
//   Method method = class_getInstanceMethod([NSThread class], @selector(setThreadPriority:));
//   IMP originalSetThreadPriority = method_getImplementation(method);
//   method_setImplementation(method, mockSetThreadPriority);

//   FlutterEngine* engine = [[FlutterEngine alloc] init];
//   [engine run];
//   [self waitForExpectations:@[ prioritiesSet ]];

//   method_setImplementation(method, originalSetThreadPriority);
// }

- (void)testLifeCycleNotificationDidEnterBackgroundForApplication {
  FlutterDartProject* project = [[FlutterDartProject alloc] init];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"foobar" project:project];
  [engine run];
  NSNotification* sceneNotification =
      [NSNotification notificationWithName:UISceneDidEnterBackgroundNotification
                                    object:nil
                                  userInfo:nil];
  NSNotification* applicationNotification =
      [NSNotification notificationWithName:UIApplicationDidEnterBackgroundNotification
                                    object:nil
                                  userInfo:nil];
  id mockEngine = OCMPartialMock(engine);
  [NSNotificationCenter.defaultCenter postNotification:sceneNotification];
  [NSNotificationCenter.defaultCenter postNotification:applicationNotification];
  OCMVerify(times(1), [mockEngine applicationDidEnterBackground:[OCMArg any]]);
}

- (void)testLifeCycleNotificationDidEnterBackgroundForScene {
  id mockBundle = OCMPartialMock([NSBundle mainBundle]);
  OCMStub([mockBundle objectForInfoDictionaryKey:@"NSExtension"]).andReturn(@{
    @"NSExtensionPointIdentifier" : @"com.apple.share-services"
  });
  FlutterDartProject* project = [[FlutterDartProject alloc] init];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"foobar" project:project];
  [engine run];
  NSNotification* sceneNotification =
      [NSNotification notificationWithName:UISceneDidEnterBackgroundNotification
                                    object:nil
                                  userInfo:nil];
  NSNotification* applicationNotification =
      [NSNotification notificationWithName:UIApplicationDidEnterBackgroundNotification
                                    object:nil
                                  userInfo:nil];
  id mockEngine = OCMPartialMock(engine);
  [NSNotificationCenter.defaultCenter postNotification:sceneNotification];
  [NSNotificationCenter.defaultCenter postNotification:applicationNotification];
  OCMVerify(times(1), [mockEngine sceneDidEnterBackground:[OCMArg any]]);
  [mockBundle stopMocking];
}

- (void)testLifeCycleNotificationWillEnterForegroundForApplication {
  FlutterDartProject* project = [[FlutterDartProject alloc] init];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"foobar" project:project];
  [engine run];
  NSNotification* sceneNotification =
      [NSNotification notificationWithName:UISceneWillEnterForegroundNotification
                                    object:nil
                                  userInfo:nil];
  NSNotification* applicationNotification =
      [NSNotification notificationWithName:UIApplicationWillEnterForegroundNotification
                                    object:nil
                                  userInfo:nil];
  id mockEngine = OCMPartialMock(engine);
  [NSNotificationCenter.defaultCenter postNotification:sceneNotification];
  [NSNotificationCenter.defaultCenter postNotification:applicationNotification];
  OCMVerify(times(1), [mockEngine applicationWillEnterForeground:[OCMArg any]]);
}

- (void)testLifeCycleNotificationWillEnterForegroundForScene {
  id mockBundle = OCMPartialMock([NSBundle mainBundle]);
  OCMStub([mockBundle objectForInfoDictionaryKey:@"NSExtension"]).andReturn(@{
    @"NSExtensionPointIdentifier" : @"com.apple.share-services"
  });
  FlutterDartProject* project = [[FlutterDartProject alloc] init];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"foobar" project:project];
  [engine run];
  NSNotification* sceneNotification =
      [NSNotification notificationWithName:UISceneWillEnterForegroundNotification
                                    object:nil
                                  userInfo:nil];
  NSNotification* applicationNotification =
      [NSNotification notificationWithName:UIApplicationWillEnterForegroundNotification
                                    object:nil
                                  userInfo:nil];
  id mockEngine = OCMPartialMock(engine);
  [NSNotificationCenter.defaultCenter postNotification:sceneNotification];
  [NSNotificationCenter.defaultCenter postNotification:applicationNotification];
  OCMVerify(times(1), [mockEngine sceneWillEnterForeground:[OCMArg any]]);
  [mockBundle stopMocking];
}

- (void)testLifeCycleNotificationSceneWillConnect {
  FlutterDartProject* project = [[FlutterDartProject alloc] init];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"foobar" project:project];
  [engine run];
  id mockScene = OCMClassMock([UIWindowScene class]);
  id mockLifecycleProvider = OCMProtocolMock(@protocol(FlutterSceneLifeCycleProvider));
  id mockLifecycleDelegate = OCMClassMock([FlutterPluginSceneLifeCycleDelegate class]);
  OCMStub([mockScene delegate]).andReturn(mockLifecycleProvider);
  OCMStub([mockLifecycleProvider sceneLifeCycleDelegate]).andReturn(mockLifecycleDelegate);

  NSNotification* sceneNotification =
      [NSNotification notificationWithName:UISceneWillConnectNotification
                                    object:mockScene
                                  userInfo:nil];

  [NSNotificationCenter.defaultCenter postNotification:sceneNotification];
  OCMVerify(times(1), [mockLifecycleDelegate engine:engine
                          receivedConnectNotificationFor:mockScene]);
}

- (void)testCanMergePlatformAndUIThread {
#if defined(TARGET_IPHONE_SIMULATOR) && TARGET_IPHONE_SIMULATOR
  auto settings = FLTDefaultSettingsForBundle();
  FlutterDartProject* project = [[FlutterDartProject alloc] initWithSettings:settings];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"foobar" project:project];
  [engine run];

  XCTAssertEqual(engine.shell.GetTaskRunners().GetUITaskRunner(),
                 engine.shell.GetTaskRunners().GetPlatformTaskRunner());
#endif  // defined(TARGET_IPHONE_SIMULATOR) && TARGET_IPHONE_SIMULATOR
}

- (void)testCanUnMergePlatformAndUIThread {
#if defined(TARGET_IPHONE_SIMULATOR) && TARGET_IPHONE_SIMULATOR
  auto settings = FLTDefaultSettingsForBundle();
  settings.merged_platform_ui_thread = flutter::Settings::MergedPlatformUIThread::kDisabled;
  FlutterDartProject* project = [[FlutterDartProject alloc] initWithSettings:settings];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"foobar" project:project];
  [engine run];

  XCTAssertNotEqual(engine.shell.GetTaskRunners().GetUITaskRunner(),
                    engine.shell.GetTaskRunners().GetPlatformTaskRunner());
#endif  // defined(TARGET_IPHONE_SIMULATOR) && TARGET_IPHONE_SIMULATOR
}

- (void)testAddSceneDelegateToRegistrar {
  FlutterDartProject* project = [[FlutterDartProject alloc] init];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"engine" project:project];
  id mockEngine = OCMPartialMock(engine);
  NSObject<FlutterPluginRegistrar>* registrar = [mockEngine registrarForPlugin:@"plugin"];
  id mockPlugin = OCMProtocolMock(@protocol(TestFlutterPluginWithSceneEvents));
  [registrar addSceneDelegate:mockPlugin];

  OCMVerify(times(1), [mockEngine addSceneLifeCycleDelegate:[OCMArg any]]);
}

- (void)testNotifyAppDelegateOfEngineInitialization {
  FlutterDartProject* project = [[FlutterDartProject alloc] init];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"engine" project:project];

  id mockApplication = OCMClassMock([UIApplication class]);
  OCMStub([mockApplication sharedApplication]).andReturn(mockApplication);
  id mockAppDelegate = OCMProtocolMock(@protocol(FlutterImplicitEngineDelegate));
  OCMStub([mockApplication delegate]).andReturn(mockAppDelegate);

  [engine performImplicitEngineCallback];
  OCMVerify(times(1), [mockAppDelegate didInitializeImplicitFlutterEngine:[OCMArg any]]);
}

- (void)testRegistrarForPlugin {
  FlutterDartProject* project = [[FlutterDartProject alloc] init];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"engine" project:project];
  FlutterEngine* mockEngine = OCMPartialMock(engine);
  id mockBinaryMessenger = OCMProtocolMock(@protocol(FlutterBinaryMessenger));
  OCMStub([mockEngine binaryMessenger]).andReturn(mockBinaryMessenger);

  NSString* pluginKey = @"plugin";
  NSString* assetKey = @"asset";

  NSObject<FlutterPluginRegistrar>* registrar = [mockEngine registrarForPlugin:pluginKey];

  XCTAssertTrue([registrar respondsToSelector:@selector(messenger)]);
  XCTAssertTrue([registrar respondsToSelector:@selector(publish:)]);
  XCTAssertTrue([registrar respondsToSelector:@selector(addMethodCallDelegate:channel:)]);
  XCTAssertTrue([registrar respondsToSelector:@selector(addApplicationDelegate:)]);
  XCTAssertTrue([registrar respondsToSelector:@selector(lookupKeyForAsset:)]);
  XCTAssertTrue([registrar respondsToSelector:@selector(lookupKeyForAsset:fromPackage:)]);

  // Verify messenger, textures, and viewController forwards to FlutterEngine
  XCTAssertEqual(registrar.messenger, mockBinaryMessenger);

  // Verify publish forwards to FlutterEngine
  id plugin = OCMProtocolMock(@protocol(FlutterPlugin));
  [registrar publish:plugin];
  XCTAssertEqual(mockEngine.pluginPublications[pluginKey], plugin);

  // Verify lookupKeyForAsset:, lookupKeyForAsset:fromPackage forward to engine
  [registrar lookupKeyForAsset:assetKey];
  OCMVerify(times(1), [mockEngine lookupKeyForAsset:assetKey]);
  [registrar lookupKeyForAsset:assetKey fromPackage:pluginKey];
  OCMVerify(times(1), [mockEngine lookupKeyForAsset:assetKey fromPackage:pluginKey]);
}

- (void)testRegistrarForApplication {
  FlutterDartProject* project = [[FlutterDartProject alloc] init];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"engine" project:project];
  FlutterEngine* mockEngine = OCMPartialMock(engine);
  id mockBinaryMessenger = OCMProtocolMock(@protocol(FlutterBinaryMessenger));
  OCMStub([mockEngine binaryMessenger]).andReturn(mockBinaryMessenger);

  NSString* pluginKey = @"plugin";

  NSObject<FlutterApplicationRegistrar>* registrar = [mockEngine registrarForApplication:pluginKey];

  XCTAssertTrue([registrar respondsToSelector:@selector(messenger)]);
  XCTAssertFalse([registrar respondsToSelector:@selector(viewController)]);
  XCTAssertFalse([registrar respondsToSelector:@selector(publish:)]);
  XCTAssertFalse([registrar respondsToSelector:@selector(addMethodCallDelegate:channel:)]);
  XCTAssertFalse([registrar respondsToSelector:@selector(addApplicationDelegate:)]);
  XCTAssertFalse([registrar respondsToSelector:@selector(lookupKeyForAsset:)]);
  XCTAssertFalse([registrar respondsToSelector:@selector(lookupKeyForAsset:fromPackage:)]);

  // Verify messenger and textures forwards to FlutterEngine
  XCTAssertEqual(registrar.messenger, mockBinaryMessenger);
}

@end
