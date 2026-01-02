// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <OCMock/OCMock.h>
#import <XCTest/XCTest.h>

#import "flutter/shell/platform/darwin/ios/framework/Headers/FlutterAppDelegate.h"
#import "flutter/shell/platform/darwin/ios/framework/Headers/FlutterEngine.h"
#import "flutter/shell/platform/darwin/ios/framework/Source/FlutterAppDelegate_Internal.h"
#import "flutter/shell/platform/darwin/ios/framework/Source/FlutterEngine_Internal.h"
#import "flutter/shell/platform/darwin/ios/framework/Source/FlutterEngine_Test.h"

FLUTTER_ASSERT_ARC

@interface FlutterAppDelegateTest : XCTestCase
@property(strong) FlutterAppDelegate* appDelegate;
@property(strong) FlutterViewController* viewController;
@property(strong) id mockMainBundle;
@property(strong) id mockNavigationChannel;
@property(strong) FlutterEngine* engine;

// Retain callback until the tests are done.
// https://github.com/flutter/flutter/issues/74267
@property(strong) id mockEngineFirstFrameCallback;
@end

@implementation FlutterAppDelegateTest

- (void)setUp {
  [super setUp];

  id mockMainBundle = OCMClassMock([NSBundle class]);
  OCMStub([mockMainBundle mainBundle]).andReturn(mockMainBundle);
  self.mockMainBundle = mockMainBundle;

  FlutterAppDelegate* appDelegate = [[FlutterAppDelegate alloc] init];
  self.appDelegate = appDelegate;

  FlutterEngine* engine = OCMClassMock([FlutterEngine class]);
  self.engine = engine;

  id mockEngineFirstFrameCallback = [OCMArg invokeBlockWithArgs:@NO, nil];
  self.mockEngineFirstFrameCallback = mockEngineFirstFrameCallback;
}

- (void)tearDown {
  // Explicitly stop mocking the NSBundle class property.
  [self.mockMainBundle stopMocking];
  [super tearDown];
}

- (void)testReleasesWindowOnDealloc {
  __weak UIWindow* weakWindow;
  @autoreleasepool {
    id mockWindow = OCMClassMock([UIWindow class]);
    FlutterAppDelegate* appDelegate = [[FlutterAppDelegate alloc] init];
    appDelegate.window = mockWindow;
    weakWindow = mockWindow;
    XCTAssertNotNil(weakWindow);
    [mockWindow stopMocking];
    mockWindow = nil;
    appDelegate = nil;
  }
  // App delegate has released the window.
  XCTAssertNil(weakWindow);
}

- (void)testGrabLaunchEngine {
  // Clear out the mocking of the root view controller.
  [self.mockMainBundle stopMocking];
  // Working with plugins forces the creation of an engine.
  XCTAssertFalse([self.appDelegate hasPlugin:@"hello"]);
  XCTAssertNotNil([self.appDelegate takeLaunchEngine]);
  XCTAssertNil([self.appDelegate takeLaunchEngine]);
}

- (void)testGrabLaunchEngineWithoutPlugins {
  XCTAssertNil([self.appDelegate takeLaunchEngine]);
}

- (void)testSetGetPluginRegistrant {
  id mockRegistrant = OCMProtocolMock(@protocol(FlutterPluginRegistrant));
  self.appDelegate.pluginRegistrant = mockRegistrant;
  XCTAssertEqual(self.appDelegate.pluginRegistrant, mockRegistrant);
}

- (void)testSetGetPluginRegistrantSelf {
  __weak FlutterAppDelegate* appDelegate = self.appDelegate;
  @autoreleasepool {
    appDelegate.pluginRegistrant = (id)appDelegate;
    self.appDelegate = nil;
  }
  // A retain cycle would keep this alive.
  XCTAssertNil(appDelegate);
}

@end
