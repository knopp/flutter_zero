// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "flutter/shell/platform/darwin/macos/framework/Headers/FlutterEngine.h"
#import "flutter/shell/platform/darwin/macos/framework/Source/FlutterEngine_Internal.h"

#include <objc/objc.h>

#include <algorithm>
#include <functional>
#include <thread>
#include <vector>

#include "flutter/fml/synchronization/waitable_event.h"
#include "flutter/lib/ui/window/platform_message.h"
#import "flutter/shell/platform/darwin/common/framework/Headers/FlutterChannels.h"
#import "flutter/shell/platform/darwin/common/framework/Source/FlutterBinaryMessengerRelay.h"
#import "flutter/shell/platform/darwin/common/test_utils_swift/test_utils_swift.h"
#import "flutter/shell/platform/darwin/macos/InternalFlutterSwift/InternalFlutterSwift.h"
#import "flutter/shell/platform/darwin/macos/framework/Headers/FlutterAppDelegate.h"
#import "flutter/shell/platform/darwin/macos/framework/Headers/FlutterAppLifecycleDelegate.h"
#import "flutter/shell/platform/darwin/macos/framework/Headers/FlutterPluginMacOS.h"
#import "flutter/shell/platform/darwin/macos/framework/Source/FlutterDartProject_Internal.h"
#import "flutter/shell/platform/darwin/macos/framework/Source/FlutterEngineTestUtils.h"
#include "flutter/shell/platform/embedder/embedder.h"
#include "flutter/shell/platform/embedder/embedder_engine.h"
#include "flutter/shell/platform/embedder/test_utils/proc_table_replacement.h"
#include "flutter/testing/stream_capture.h"
#include "flutter/testing/test_dart_native_resolver.h"
#include "flutter/testing/testing.h"
#include "gtest/gtest.h"

// CREATE_NATIVE_ENTRY and MOCK_ENGINE_PROC are leaky by design
// NOLINTBEGIN(clang-analyzer-core.StackAddressEscape)

@interface PlainAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation PlainAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication* _Nonnull)sender {
  // Always cancel, so that the test doesn't exit.
  return NSTerminateCancel;
}
@end

#pragma mark -

@interface FakeLifecycleProvider : NSObject <FlutterAppLifecycleProvider, NSApplicationDelegate>

@property(nonatomic, strong, readonly) NSPointerArray* registeredDelegates;

// True if the given delegate is currently registered.
- (BOOL)hasDelegate:(nonnull NSObject<FlutterAppLifecycleDelegate>*)delegate;
@end

@implementation FakeLifecycleProvider {
  /**
   * All currently registered delegates.
   *
   * This does not use NSPointerArray or any other weak-pointer
   * system, because a weak pointer will be nil'd out at the start of dealloc, which will break
   * queries. E.g., if a delegate is dealloc'd without being unregistered, a weak pointer array
   * would no longer contain that pointer even though removeApplicationLifecycleDelegate: was never
   * called, causing tests to pass incorrectly.
   */
  std::vector<void*> _delegates;
}

- (void)addApplicationLifecycleDelegate:(nonnull NSObject<FlutterAppLifecycleDelegate>*)delegate {
  _delegates.push_back((__bridge void*)delegate);
}

- (void)removeApplicationLifecycleDelegate:
    (nonnull NSObject<FlutterAppLifecycleDelegate>*)delegate {
  auto delegateIndex = std::find(_delegates.begin(), _delegates.end(), (__bridge void*)delegate);
  NSAssert(delegateIndex != _delegates.end(),
           @"Attempting to unregister a delegate that was not registered.");
  _delegates.erase(delegateIndex);
}

- (BOOL)hasDelegate:(nonnull NSObject<FlutterAppLifecycleDelegate>*)delegate {
  return std::find(_delegates.begin(), _delegates.end(), (__bridge void*)delegate) !=
         _delegates.end();
}

@end

#pragma mark -

@interface FakeAppDelegatePlugin : NSObject <FlutterPlugin>
@end

@implementation FakeAppDelegatePlugin
+ (void)registerWithRegistrar:(id<FlutterPluginRegistrar>)registrar {
}
@end

#pragma mark -

@interface MockableFlutterEngine : FlutterEngine
@end

@implementation MockableFlutterEngine
- (NSArray<NSScreen*>*)screens {
  id mockScreen = OCMClassMock([NSScreen class]);
  OCMStub([mockScreen backingScaleFactor]).andReturn(2.0);
  OCMStub([mockScreen deviceDescription]).andReturn(@{
    @"NSScreenNumber" : [NSNumber numberWithInt:10]
  });
  OCMStub([mockScreen frame]).andReturn(NSMakeRect(10, 20, 30, 40));
  return [NSArray arrayWithObject:mockScreen];
}
@end

#pragma mark -

namespace flutter::testing {

TEST_F(FlutterEngineTest, CanLaunch) {
  FlutterEngine* engine = GetFlutterEngine();
  EXPECT_TRUE([engine runWithEntrypoint:@"main"]);
  ASSERT_TRUE(engine.running);
}

TEST_F(FlutterEngineTest, HasNonNullExecutableName) {
  FlutterEngine* engine = GetFlutterEngine();
  std::string executable_name = [[engine executableName] UTF8String];
  ASSERT_FALSE(executable_name.empty());

  // Block until notified by the Dart test of the value of Platform.executable.
  BOOL signaled = NO;
  AddNativeCallback("NotifyStringValue", CREATE_NATIVE_ENTRY([&](Dart_NativeArguments args) {
                      const auto dart_string = tonic::DartConverter<std::string>::FromDart(
                          Dart_GetNativeArgument(args, 0));
                      EXPECT_EQ(executable_name, dart_string);
                      signaled = YES;
                    }));

  // Launch the test entrypoint.
  EXPECT_TRUE([engine runWithEntrypoint:@"executableNameNotNull"]);

  while (!signaled) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1, YES);
  }
}

#ifndef FLUTTER_RELEASE
TEST_F(FlutterEngineTest, Switches) {
  setenv("FLUTTER_ENGINE_SWITCHES", "2", 1);
  setenv("FLUTTER_ENGINE_SWITCH_1", "abc", 1);
  setenv("FLUTTER_ENGINE_SWITCH_2", "foo=\"bar, baz\"", 1);

  FlutterEngine* engine = GetFlutterEngine();
  std::vector<std::string> switches = engine.switches;
  ASSERT_EQ(switches.size(), 2UL);
  EXPECT_EQ(switches[0], "--abc");
  EXPECT_EQ(switches[1], "--foo=\"bar, baz\"");

  unsetenv("FLUTTER_ENGINE_SWITCHES");
  unsetenv("FLUTTER_ENGINE_SWITCH_1");
  unsetenv("FLUTTER_ENGINE_SWITCH_2");
}
#endif  // !FLUTTER_RELEASE

TEST_F(FlutterEngineTest, MessengerSend) {
  FlutterEngine* engine = GetFlutterEngine();
  EXPECT_TRUE([engine runWithEntrypoint:@"main"]);

  NSData* test_message = [@"a message" dataUsingEncoding:NSUTF8StringEncoding];
  bool called = false;

  engine.embedderAPI.SendPlatformMessage = MOCK_ENGINE_PROC(
      SendPlatformMessage, ([&called, test_message](auto engine, auto message) {
        called = true;
        EXPECT_STREQ(message->channel, "test");
        EXPECT_EQ(memcmp(message->message, test_message.bytes, message->message_size), 0);
        return kSuccess;
      }));

  [engine.binaryMessenger sendOnChannel:@"test" message:test_message];
  EXPECT_TRUE(called);
}

TEST_F(FlutterEngineTest, CanLogToStdout) {
  // Block until completion of print statement.
  BOOL signaled = NO;
  AddNativeCallback("SignalNativeTest",
                    CREATE_NATIVE_ENTRY([&](Dart_NativeArguments args) { signaled = YES; }));

  // Replace stdout stream buffer with our own.
  FlutterStringOutputWriter* writer = [[FlutterStringOutputWriter alloc] init];
  writer.expectedOutput = @"Hello logging";
  FlutterLogger.outputWriter = writer;

  // Launch the test entrypoint.
  FlutterEngine* engine = GetFlutterEngine();
  EXPECT_TRUE([engine runWithEntrypoint:@"canLogToStdout"]);
  ASSERT_TRUE(engine.running);

  while (!signaled) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1, YES);
  }

  // Verify hello world was written to stdout.
  EXPECT_TRUE(writer.gotExpectedOutput);
}

TEST_F(FlutterEngineTest, NativeCallbacks) {
  BOOL latch_called = NO;
  AddNativeCallback("SignalNativeTest",
                    CREATE_NATIVE_ENTRY([&](Dart_NativeArguments args) { latch_called = YES; }));

  FlutterEngine* engine = GetFlutterEngine();
  EXPECT_TRUE([engine runWithEntrypoint:@"nativeCallback"]);
  ASSERT_TRUE(engine.running);

  while (!latch_called) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1, YES);
  }
  ASSERT_TRUE(latch_called);
}

TEST_F(FlutterEngineTest, DartEntrypointArguments) {
  NSString* fixtures = @(flutter::testing::GetFixturesPath());
  FlutterDartProject* project = [[FlutterDartProject alloc]
      initWithAssetsPath:fixtures
             ICUDataPath:[fixtures stringByAppendingString:@"/icudtl.dat"]];

  project.dartEntrypointArguments = @[ @"arg1", @"arg2" ];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"test" project:project];

  bool called = false;
  auto original_init = engine.embedderAPI.Initialize;
  engine.embedderAPI.Initialize = MOCK_ENGINE_PROC(
      Initialize,
      ([&called, &original_init](size_t version, const FlutterProjectArgs* args, void* user_data,
                                 FLUTTER_API_SYMBOL(FlutterEngine) * engine_out) {
        called = true;
        EXPECT_EQ(args->dart_entrypoint_argc, 2);
        NSString* arg1 = [[NSString alloc] initWithCString:args->dart_entrypoint_argv[0]
                                                  encoding:NSUTF8StringEncoding];
        NSString* arg2 = [[NSString alloc] initWithCString:args->dart_entrypoint_argv[1]
                                                  encoding:NSUTF8StringEncoding];

        EXPECT_TRUE([arg1 isEqualToString:@"arg1"]);
        EXPECT_TRUE([arg2 isEqualToString:@"arg2"]);

        return original_init(version, args, user_data, engine_out);
      }));

  EXPECT_TRUE([engine runWithEntrypoint:@"main"]);
  EXPECT_TRUE(called);
  [engine shutDownEngine];
}

// Verify that the engine is not retained indirectly via the binary messenger held by channels and
// plugins. Previously, FlutterEngine.binaryMessenger returned the engine itself, and thus plugins
// could cause a retain cycle, preventing the engine from being deallocated.
// FlutterEngine.binaryMessenger now returns a FlutterBinaryMessengerRelay whose weak pointer back
// to the engine is cleared when the engine is deallocated.
// Issue: https://github.com/flutter/flutter/issues/116445
TEST_F(FlutterEngineTest, FlutterBinaryMessengerDoesNotRetainEngine) {
  __weak FlutterEngine* weakEngine;
  id<FlutterBinaryMessenger> binaryMessenger = nil;
  @autoreleasepool {
    // Create a test engine.
    NSString* fixtures = @(flutter::testing::GetFixturesPath());
    FlutterDartProject* project = [[FlutterDartProject alloc]
        initWithAssetsPath:fixtures
               ICUDataPath:[fixtures stringByAppendingString:@"/icudtl.dat"]];
    FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"test" project:project];
    weakEngine = engine;
    binaryMessenger = engine.binaryMessenger;
  }

  // Once the engine has been deallocated, verify the weak engine pointer is nil, and thus not
  // retained by the relay.
  EXPECT_NE(binaryMessenger, nil);
  EXPECT_EQ(weakEngine, nil);
}

TEST_F(FlutterEngineTest, PublishedValueNilForUnknownPlugin) {
  NSString* fixtures = @(flutter::testing::GetFixturesPath());
  FlutterDartProject* project = [[FlutterDartProject alloc]
      initWithAssetsPath:fixtures
             ICUDataPath:[fixtures stringByAppendingString:@"/icudtl.dat"]];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"test" project:project];

  EXPECT_EQ([engine valuePublishedByPlugin:@"NoSuchPlugin"], nil);
}

TEST_F(FlutterEngineTest, PublishedValueNSNullIfNoPublishedValue) {
  NSString* fixtures = @(flutter::testing::GetFixturesPath());
  FlutterDartProject* project = [[FlutterDartProject alloc]
      initWithAssetsPath:fixtures
             ICUDataPath:[fixtures stringByAppendingString:@"/icudtl.dat"]];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"test" project:project];
  NSString* pluginName = @"MyPlugin";
  // Request the registarar to register the plugin as existing.
  [engine registrarForPlugin:pluginName];

  // The documented behavior is that a plugin that exists but hasn't published
  // anything returns NSNull, rather than nil, as on iOS.
  EXPECT_EQ([engine valuePublishedByPlugin:pluginName], [NSNull null]);
}

TEST_F(FlutterEngineTest, PublishedValueReturnsLastPublished) {
  NSString* fixtures = @(flutter::testing::GetFixturesPath());
  FlutterDartProject* project = [[FlutterDartProject alloc]
      initWithAssetsPath:fixtures
             ICUDataPath:[fixtures stringByAppendingString:@"/icudtl.dat"]];
  FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"test" project:project];
  NSString* pluginName = @"MyPlugin";
  id<FlutterPluginRegistrar> registrar = [engine registrarForPlugin:pluginName];

  NSString* firstValue = @"A published value";
  NSArray* secondValue = @[ @"A different published value" ];

  [registrar publish:firstValue];
  EXPECT_EQ([engine valuePublishedByPlugin:pluginName], firstValue);

  [registrar publish:secondValue];
  EXPECT_EQ([engine valuePublishedByPlugin:pluginName], secondValue);
}

// If a channel overrides a previous channel with the same name, cleaning
// the previous channel should not affect the new channel.
//
// This is important when recreating classes that uses a channel, because the
// new instance would create the channel before the first class is deallocated
// and clears the channel.
TEST_F(FlutterEngineTest, MessengerCleanupConnectionWorks) {
  FlutterEngine* engine = GetFlutterEngine();
  EXPECT_TRUE([engine runWithEntrypoint:@"main"]);

  NSString* channel = @"_test_";
  NSData* channel_data = [channel dataUsingEncoding:NSUTF8StringEncoding];

  // Mock SendPlatformMessage so that if a message is sent to
  // "test/send_message", act as if the framework has sent an empty message to
  // the channel marked by the `sendOnChannel:message:` call's message.
  engine.embedderAPI.SendPlatformMessage = MOCK_ENGINE_PROC(
      SendPlatformMessage, ([](auto engine_, auto message_) {
        if (strcmp(message_->channel, "test/send_message") == 0) {
          // The simplest message that is acceptable to a method channel.
          std::string message = R"|({"method": "a"})|";
          std::string channel(reinterpret_cast<const char*>(message_->message),
                              message_->message_size);
          reinterpret_cast<EmbedderEngine*>(engine_)
              ->GetShell()
              .GetPlatformView()
              ->HandlePlatformMessage(std::make_unique<PlatformMessage>(
                  channel.c_str(), fml::MallocMapping::Copy(message.c_str(), message.length()),
                  fml::RefPtr<PlatformMessageResponse>()));
        }
        return kSuccess;
      }));

  __block int record = 0;

  FlutterMethodChannel* channel1 =
      [FlutterMethodChannel methodChannelWithName:channel
                                  binaryMessenger:engine.binaryMessenger
                                            codec:[FlutterJSONMethodCodec sharedInstance]];
  [channel1 setMethodCallHandler:^(FlutterMethodCall* call, FlutterResult result) {
    record += 1;
  }];

  [engine.binaryMessenger sendOnChannel:@"test/send_message" message:channel_data];
  EXPECT_EQ(record, 1);

  FlutterMethodChannel* channel2 =
      [FlutterMethodChannel methodChannelWithName:channel
                                  binaryMessenger:engine.binaryMessenger
                                            codec:[FlutterJSONMethodCodec sharedInstance]];
  [channel2 setMethodCallHandler:^(FlutterMethodCall* call, FlutterResult result) {
    record += 10;
  }];

  [engine.binaryMessenger sendOnChannel:@"test/send_message" message:channel_data];
  EXPECT_EQ(record, 11);

  [channel1 setMethodCallHandler:nil];

  [engine.binaryMessenger sendOnChannel:@"test/send_message" message:channel_data];
  EXPECT_EQ(record, 21);
}

TEST_F(FlutterEngineTest, ResponseAfterEngineDied) {
  FlutterEngine* engine = GetFlutterEngine();
  FlutterBasicMessageChannel* channel = [[FlutterBasicMessageChannel alloc]
         initWithName:@"foo"
      binaryMessenger:engine.binaryMessenger
                codec:[FlutterStandardMessageCodec sharedInstance]];
  __block BOOL didCallCallback = NO;
  [channel setMessageHandler:^(id message, FlutterReply callback) {
    ShutDownEngine();
    callback(nil);
    didCallCallback = YES;
  }];
  EXPECT_TRUE([engine runWithEntrypoint:@"sendFooMessage"]);
  engine = nil;

  while (!didCallCallback) {
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
  }
}

TEST_F(FlutterEngineTest, ResponseFromBackgroundThread) {
  FlutterEngine* engine = GetFlutterEngine();
  FlutterBasicMessageChannel* channel = [[FlutterBasicMessageChannel alloc]
         initWithName:@"foo"
      binaryMessenger:engine.binaryMessenger
                codec:[FlutterStandardMessageCodec sharedInstance]];
  __block BOOL didCallCallback = NO;
  [channel setMessageHandler:^(id message, FlutterReply callback) {
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
      callback(nil);
      dispatch_async(dispatch_get_main_queue(), ^{
        didCallCallback = YES;
      });
    });
  }];
  EXPECT_TRUE([engine runWithEntrypoint:@"sendFooMessage"]);

  while (!didCallCallback) {
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
  }
}

TEST_F(FlutterEngineTest, CanGetEngineForId) {
  FlutterEngine* engine = GetFlutterEngine();

  BOOL signaled = NO;
  std::optional<int64_t> engineId;
  AddNativeCallback("NotifyEngineId", CREATE_NATIVE_ENTRY([&](Dart_NativeArguments args) {
                      const auto argument = Dart_GetNativeArgument(args, 0);
                      if (!Dart_IsNull(argument)) {
                        const auto id = tonic::DartConverter<int64_t>::FromDart(argument);
                        engineId = id;
                      }
                      signaled = YES;
                    }));

  EXPECT_TRUE([engine runWithEntrypoint:@"testEngineId"]);
  while (!signaled) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1, YES);
  }

  EXPECT_TRUE(engineId.has_value());
  if (!engineId.has_value()) {
    return;
  }
  EXPECT_EQ(engine, [FlutterEngine engineForIdentifier:*engineId]);
  ShutDownEngine();
}

TEST_F(FlutterEngineTest, IgnoresTerminationRequestIfNotFlutterAppDelegate) {
  id<NSApplicationDelegate> previousDelegate = [[NSApplication sharedApplication] delegate];
  id<NSApplicationDelegate> plainDelegate = [[PlainAppDelegate alloc] init];
  [NSApplication sharedApplication].delegate = plainDelegate;

  // Creating the engine shouldn't fail here, even though the delegate isn't a
  // FlutterAppDelegate.
  CreateMockFlutterEngine();

  // Asking to terminate the app should cancel.
  EXPECT_EQ([[[NSApplication sharedApplication] delegate] applicationShouldTerminate:NSApp],
            NSTerminateCancel);

  [NSApplication sharedApplication].delegate = previousDelegate;
}

TEST_F(FlutterEngineTest, ForwardsPluginDelegateRegistration) {
  id<NSApplicationDelegate> previousDelegate = [[NSApplication sharedApplication] delegate];
  FakeLifecycleProvider* fakeAppDelegate = [[FakeLifecycleProvider alloc] init];
  [NSApplication sharedApplication].delegate = fakeAppDelegate;

  FakeAppDelegatePlugin* plugin = [[FakeAppDelegatePlugin alloc] init];
  FlutterEngine* engine = CreateMockFlutterEngine();

  [[engine registrarForPlugin:@"TestPlugin"] addApplicationDelegate:plugin];

  EXPECT_TRUE([fakeAppDelegate hasDelegate:plugin]);

  [NSApplication sharedApplication].delegate = previousDelegate;
}

TEST_F(FlutterEngineTest, UnregistersPluginsOnEngineDestruction) {
  id<NSApplicationDelegate> previousDelegate = [[NSApplication sharedApplication] delegate];
  FakeLifecycleProvider* fakeAppDelegate = [[FakeLifecycleProvider alloc] init];
  [NSApplication sharedApplication].delegate = fakeAppDelegate;

  FakeAppDelegatePlugin* plugin = [[FakeAppDelegatePlugin alloc] init];

  @autoreleasepool {
    FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"test" project:nil];

    [[engine registrarForPlugin:@"TestPlugin"] addApplicationDelegate:plugin];
    EXPECT_TRUE([fakeAppDelegate hasDelegate:plugin]);
  }

  // When the engine is released, it should unregister any plugins it had
  // registered on its behalf.
  EXPECT_FALSE([fakeAppDelegate hasDelegate:plugin]);

  [NSApplication sharedApplication].delegate = previousDelegate;
}

}  // namespace flutter::testing

// NOLINTEND(clang-analyzer-core.StackAddressEscape)
