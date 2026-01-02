// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <UIKit/UIKit.h>

#define FML_USED_ON_EMBEDDER

#include "common/settings.h"

#include <memory>

#include "flutter/common/constants.h"
#include "flutter/fml/message_loop.h"
#include "flutter/fml/platform/darwin/platform_version.h"
#include "flutter/fml/trace_event.h"
#include "flutter/runtime/ptrace_check.h"
#include "flutter/shell/common/engine.h"
#include "flutter/shell/common/platform_view.h"
#include "flutter/shell/common/shell.h"
#include "flutter/shell/common/switches.h"
#include "flutter/shell/common/thread_host.h"
#import "flutter/shell/platform/darwin/common/InternalFlutterSwiftCommon/InternalFlutterSwiftCommon.h"
#import "flutter/shell/platform/darwin/common/command_line.h"
#import "flutter/shell/platform/darwin/common/framework/Source/FlutterBinaryMessengerRelay.h"
#import "flutter/shell/platform/darwin/ios/InternalFlutterSwift/InternalFlutterSwift.h"
#import "flutter/shell/platform/darwin/ios/framework/Source/FlutterDartProject_Internal.h"
#import "flutter/shell/platform/darwin/ios/framework/Source/FlutterDartVMServicePublisher.h"
#import "flutter/shell/platform/darwin/ios/framework/Source/FlutterEngine_Internal.h"
#import "flutter/shell/platform/darwin/ios/framework/Source/FlutterSharedApplication.h"
#import "flutter/shell/platform/darwin/ios/framework/Source/UIViewController+FlutterScreenAndSceneIfLoaded.h"
#import "flutter/shell/platform/darwin/ios/framework/Source/platform_message_response_darwin.h"
#import "flutter/shell/platform/darwin/ios/framework/Source/profiler_metrics_ios.h"
#import "flutter/shell/platform/darwin/ios/platform_view_ios.h"
#include "flutter/shell/profiling/sampling_profiler.h"

FLUTTER_ASSERT_ARC

/// Inheriting ThreadConfigurer and use iOS platform thread API to configure the thread priorities
/// Using iOS platform thread API to configure thread priority
static void IOSPlatformThreadConfigSetter(const fml::Thread::ThreadConfig& config) {
  // set thread name
  fml::Thread::SetCurrentThreadName(config);

  // set thread priority
  switch (config.priority) {
    case fml::Thread::ThreadPriority::kBackground: {
      pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
      [[NSThread currentThread] setThreadPriority:0];
      break;
    }
    case fml::Thread::ThreadPriority::kNormal: {
      pthread_set_qos_class_self_np(QOS_CLASS_DEFAULT, 0);
      [[NSThread currentThread] setThreadPriority:0.5];
      break;
    }
    case fml::Thread::ThreadPriority::kDisplay: {
      pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
      [[NSThread currentThread] setThreadPriority:1.0];
      sched_param param;
      int policy;
      pthread_t thread = pthread_self();
      if (!pthread_getschedparam(thread, &policy, &param)) {
        param.sched_priority = 50;
        pthread_setschedparam(thread, policy, &param);
      }
      break;
    }
  }
}

#pragma mark - Public exported constants

NSString* const FlutterDefaultDartEntrypoint = nil;
NSString* const FlutterDefaultInitialRoute = nil;

#pragma mark - Internal constants

NSString* const kFlutterKeyDataChannel = @"flutter/keydata";
static constexpr int kNumProfilerSamplesPerSec = 5;
NSString* const kFlutterApplicationRegistrarKey = @"io.flutter.flutter.application_registrar";

@interface FlutterEngineBaseRegistrar : NSObject <FlutterBaseRegistrar>

@property(nonatomic, weak) FlutterEngine* flutterEngine;
@property(nonatomic, readonly) NSString* key;

- (instancetype)initWithKey:(NSString*)key flutterEngine:(FlutterEngine*)flutterEngine;

@end

@interface FlutterEngineApplicationRegistrar
    : FlutterEngineBaseRegistrar <FlutterApplicationRegistrar>
@end

@interface FlutterEnginePluginRegistrar : FlutterEngineBaseRegistrar <FlutterPluginRegistrar>
@end

@interface FlutterEngine () <FlutterBinaryMessenger>

#pragma mark - Properties

@property(nonatomic, readonly) FlutterDartProject* dartProject;
@property(nonatomic, readonly, copy) NSString* labelPrefix;

@property(nonatomic, strong) FlutterEnginePluginSceneLifeCycleDelegate* sceneLifeCycleDelegate;

// Maintains a dictionary of plugin names that have registered with the engine.  Used by
// FlutterEnginePluginRegistrar to implement a FlutterPluginRegistrar.
@property(nonatomic, readonly) NSMutableDictionary* pluginPublications;
@property(nonatomic, readonly)
    NSMutableDictionary<NSString*, FlutterEngineBaseRegistrar*>* registrars;

@property(nonatomic, readwrite, copy) NSString* isolateId;
@property(nonatomic, copy) NSString* initialRoute;

@property(nonatomic, strong) FlutterDartVMServicePublisher* publisher;
@property(nonatomic, strong) FlutterConnectionCollection* connections;
@property(nonatomic, assign) int64_t nextTextureId;

#pragma mark - Channel properties

@property(nonatomic, strong) FlutterMethodChannel* localizationChannel;

@end

@implementation FlutterImplicitEngineBridgeImpl {
  FlutterEngine* _engine;
  NSObject<FlutterApplicationRegistrar>* _appRegistrar;
}

- (instancetype)initWithEngine:(FlutterEngine*)engine {
  self = [super init];
  if (self) {
    _engine = engine;
    _appRegistrar = [engine registrarForApplication:kFlutterApplicationRegistrarKey];
  }
  return self;
}

- (NSObject<FlutterPluginRegistry>*)pluginRegistry {
  return _engine;
}

- (NSObject<FlutterApplicationRegistrar>*)applicationRegistrar {
  return _appRegistrar;
}
@end

@implementation FlutterEngine {
  std::shared_ptr<flutter::ThreadHost> _threadHost;
  std::unique_ptr<flutter::Shell> _shell;

  std::shared_ptr<flutter::SamplingProfiler> _profiler;

  FlutterBinaryMessengerRelay* _binaryMessenger;
}

- (int64_t)engineIdentifier {
  return reinterpret_cast<int64_t>((__bridge void*)self);
}

- (instancetype)init {
  return [self initWithName:@"FlutterEngine" project:nil allowHeadlessExecution:YES];
}

- (instancetype)initWithName:(NSString*)labelPrefix {
  return [self initWithName:labelPrefix project:nil allowHeadlessExecution:YES];
}

- (instancetype)initWithName:(NSString*)labelPrefix project:(FlutterDartProject*)project {
  return [self initWithName:labelPrefix project:project allowHeadlessExecution:YES];
}

- (instancetype)initWithName:(NSString*)labelPrefix
                     project:(FlutterDartProject*)project
      allowHeadlessExecution:(BOOL)allowHeadlessExecution {
  return [self initWithName:labelPrefix
                     project:project
      allowHeadlessExecution:allowHeadlessExecution
          restorationEnabled:NO];
}

- (instancetype)initWithName:(NSString*)labelPrefix
                     project:(FlutterDartProject*)project
      allowHeadlessExecution:(BOOL)allowHeadlessExecution
          restorationEnabled:(BOOL)restorationEnabled {
  self = [super init];
  NSAssert(self, @"Super init cannot be nil");
  NSAssert(labelPrefix, @"labelPrefix is required");

  _labelPrefix = [labelPrefix copy];
  _dartProject = project ?: [[FlutterDartProject alloc] init];

  if (!EnableTracingIfNecessary(_dartProject.settings)) {
    NSLog(
        @"Cannot create a FlutterEngine instance in debug mode without Flutter tooling or "
        @"Xcode.\n\nTo launch in debug mode in iOS 14+, run flutter run from Flutter tools, run "
        @"from an IDE with a Flutter IDE plugin or run the iOS project from Xcode.\nAlternatively "
        @"profile and release mode apps can be launched from the home screen.");
    return nil;
  }

  _pluginPublications = [[NSMutableDictionary alloc] init];
  _registrars = [[NSMutableDictionary alloc] init];
  _binaryMessenger = [[FlutterBinaryMessengerRelay alloc] initWithParent:self];

  _connections = [[FlutterConnectionCollection alloc] init];

  NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
  [center addObserver:self
             selector:@selector(onMemoryWarning:)
                 name:UIApplicationDidReceiveMemoryWarningNotification
               object:nil];

  [self setUpLifecycleNotifications:center];

  [center addObserver:self
             selector:@selector(onLocaleUpdated:)
                 name:NSCurrentLocaleDidChangeNotification
               object:nil];

  self.sceneLifeCycleDelegate = [[FlutterEnginePluginSceneLifeCycleDelegate alloc] init];

  return self;
}

+ (FlutterEngine*)engineForIdentifier:(int64_t)identifier {
  NSAssert([[NSThread currentThread] isMainThread], @"Must be called on the main thread.");
  return (__bridge FlutterEngine*)reinterpret_cast<void*>(identifier);
}

- (void)setUpLifecycleNotifications:(NSNotificationCenter*)center {
  // If the application is not available, use the scene for lifecycle notifications if available.
  [center addObserver:self
             selector:@selector(sceneWillConnect:)
                 name:UISceneWillConnectNotification
               object:nil];
  if (!FlutterSharedApplication.isAvailable) {
    [center addObserver:self
               selector:@selector(sceneWillEnterForeground:)
                   name:UISceneWillEnterForegroundNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(sceneDidEnterBackground:)
                   name:UISceneDidEnterBackgroundNotification
                 object:nil];
    return;
  }
  [center addObserver:self
             selector:@selector(applicationWillEnterForeground:)
                 name:UIApplicationWillEnterForegroundNotification
               object:nil];
  [center addObserver:self
             selector:@selector(applicationDidEnterBackground:)
                 name:UIApplicationDidEnterBackgroundNotification
               object:nil];
}

- (void)sceneWillConnect:(NSNotification*)notification API_AVAILABLE(ios(13.0)) {
  UIScene* scene = notification.object;
  if (!FlutterSharedApplication.application.supportsMultipleScenes) {
    // Since there is only one scene, we can assume that the FlutterEngine is within this scene and
    // register it to the scene.
    // The FlutterEngine needs to be registered with the scene when the scene connects in order for
    // plugins to receive the `scene:willConnectToSession:options` event.
    // If we want to support multi-window on iPad later, we may need to add a way for deveopers to
    // register their FlutterEngine to the scene manually during this event.
    FlutterPluginSceneLifeCycleDelegate* sceneLifeCycleDelegate =
        [FlutterPluginSceneLifeCycleDelegate fromScene:scene];
    if (sceneLifeCycleDelegate != nil) {
      return [sceneLifeCycleDelegate engine:self receivedConnectNotificationFor:scene];
    }
  }
}

- (void)dealloc {
  /// Notify plugins of dealloc.  This should happen first in dealloc since the
  /// plugins may be talking to things like the binaryMessenger.
  [_pluginPublications enumerateKeysAndObjectsUsingBlock:^(id key, id object, BOOL* stop) {
    if ([object respondsToSelector:@selector(detachFromEngineForRegistrar:)]) {
      FlutterEngineBaseRegistrar* registrar = self.registrars[key];
      if ([registrar conformsToProtocol:@protocol(FlutterPluginRegistrar)]) {
        [object detachFromEngineForRegistrar:((id<FlutterPluginRegistrar>)registrar)];
      }
    }
  }];

  // nil out weak references.
  // TODO(cbracken): https://github.com/flutter/flutter/issues/156222
  // Ensure that FlutterEnginePluginRegistrar is using weak pointers, then eliminate this code.
  [_registrars enumerateKeysAndObjectsUsingBlock:^(id key, FlutterEngineBaseRegistrar* registrar,
                                                   BOOL* stop) {
    registrar.flutterEngine = nil;
  }];

  _binaryMessenger.parent = nil;

  NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
  [center removeObserver:self];
}

- (flutter::Shell&)shell {
  FML_DCHECK(_shell);
  return *_shell;
}

- (flutter::PlatformViewIOS*)platformView {
  if (!_shell) {
    return nullptr;
  }
  return static_cast<flutter::PlatformViewIOS*>(_shell->GetPlatformView().get());
}

- (fml::RefPtr<fml::TaskRunner>)platformTaskRunner {
  if (!_shell) {
    return {};
  }
  return _shell->GetTaskRunners().GetPlatformTaskRunner();
}

- (fml::RefPtr<fml::TaskRunner>)uiTaskRunner {
  if (!_shell) {
    return {};
  }
  return _shell->GetTaskRunners().GetUITaskRunner();
}

- (void)destroyContext {
  [self resetChannels];
  self.isolateId = nil;
  _shell.reset();
  _profiler.reset();
  _threadHost.reset();
}

- (NSURL*)vmServiceUrl {
  return self.publisher.url;
}

- (void)resetChannels {
  self.localizationChannel = nil;
}

- (void)startProfiler {
  FML_DCHECK(!_threadHost->name_prefix.empty());
  _profiler = std::make_shared<flutter::SamplingProfiler>(
      _threadHost->name_prefix.c_str(), _threadHost->profiler_thread->GetTaskRunner(),
      []() {
        flutter::ProfilerMetricsIOS profiler_metrics;
        return profiler_metrics.GenerateSample();
      },
      kNumProfilerSamplesPerSec);
  _profiler->Start();
}

// If you add a channel, be sure to also update `resetChannels`.
// Channels get a reference to the engine, and therefore need manual
// cleanup for proper collection.
- (void)setUpChannels {
  // This will be invoked once the shell is done setting up and the isolate ID
  // for the UI isolate is available.
  __weak FlutterEngine* weakSelf = self;
  [_binaryMessenger setMessageHandlerOnChannel:@"flutter/isolate"
                          binaryMessageHandler:^(NSData* message, FlutterBinaryReply reply) {
                            if (weakSelf) {
                              weakSelf.isolateId =
                                  [[FlutterStringCodec sharedInstance] decode:message];
                            }
                          }];

  self.localizationChannel =
      [[FlutterMethodChannel alloc] initWithName:@"flutter/localization"
                                 binaryMessenger:self.binaryMessenger
                                           codec:[FlutterJSONMethodCodec sharedInstance]];
}

- (void)launchEngine:(NSString*)entrypoint
          libraryURI:(NSString*)libraryOrNil
      entrypointArgs:(NSArray<NSString*>*)entrypointArgs {
  // Launch the Dart application with the inferred run configuration.
  flutter::RunConfiguration configuration =
      [self.dartProject runConfigurationForEntrypoint:entrypoint
                                         libraryOrNil:libraryOrNil
                                       entrypointArgs:entrypointArgs];

  configuration.SetEngineId(self.engineIdentifier);
  self.shell.RunEngine(std::move(configuration));
}

- (void)setUpShell:(std::unique_ptr<flutter::Shell>)shell
    withVMServicePublication:(BOOL)doesVMServicePublication {
  _shell = std::move(shell);
  [self setUpChannels];
  [self onLocaleUpdated:nil];
  self.publisher = [[FlutterDartVMServicePublisher alloc]
      initWithEnableVMServicePublication:doesVMServicePublication];
}

+ (BOOL)isProfilerEnabled {
  bool profilerEnabled = false;
#if (FLUTTER_RUNTIME_MODE == FLUTTER_RUNTIME_MODE_DEBUG) || \
    (FLUTTER_RUNTIME_MODE == FLUTTER_RUNTIME_MODE_PROFILE)
  profilerEnabled = true;
#endif
  return profilerEnabled;
}

+ (NSString*)generateThreadLabel:(NSString*)labelPrefix {
  static size_t s_shellCount = 0;
  return [NSString stringWithFormat:@"%@.%zu", labelPrefix, ++s_shellCount];
}

static flutter::ThreadHost MakeThreadHost(NSString* thread_label,
                                          const flutter::Settings& settings) {
  // The current thread will be used as the platform thread. Ensure that the message loop is
  // initialized.
  fml::MessageLoop::EnsureInitializedForCurrentThread();

  uint32_t threadHostType = flutter::ThreadHost::Type::kPlatform;
  if (settings.merged_platform_ui_thread == flutter::Settings::MergedPlatformUIThread::kDisabled) {
    threadHostType |= flutter::ThreadHost::Type::kUi;
  }

  if ([FlutterEngine isProfilerEnabled]) {
    threadHostType = threadHostType | flutter::ThreadHost::Type::kProfiler;
  }

  flutter::ThreadHost::ThreadHostConfig host_config(thread_label.UTF8String, threadHostType,
                                                    IOSPlatformThreadConfigSetter);

  host_config.ui_config =
      fml::Thread::ThreadConfig(flutter::ThreadHost::ThreadHostConfig::MakeThreadName(
                                    flutter::ThreadHost::Type::kUi, thread_label.UTF8String),
                                fml::Thread::ThreadPriority::kDisplay);

  return (flutter::ThreadHost){host_config};
}

static void SetEntryPoint(flutter::Settings* settings, NSString* entrypoint, NSString* libraryURI) {
  if (libraryURI) {
    FML_DCHECK(entrypoint) << "Must specify entrypoint if specifying library";
    settings->advisory_script_entrypoint = entrypoint.UTF8String;
    settings->advisory_script_uri = libraryURI.UTF8String;
  } else if (entrypoint) {
    settings->advisory_script_entrypoint = entrypoint.UTF8String;
    settings->advisory_script_uri = std::string("main.dart");
  } else {
    settings->advisory_script_entrypoint = std::string("main");
    settings->advisory_script_uri = std::string("main.dart");
  }
}

- (BOOL)createShell:(NSString*)entrypoint
         libraryURI:(NSString*)libraryURI
       initialRoute:(NSString*)initialRoute {
  if (_shell != nullptr) {
    [FlutterLogger logWarning:@"This FlutterEngine was already invoked."];
    return NO;
  }

  self.initialRoute = initialRoute;

  auto settings = [self.dartProject settings];
  if (initialRoute != nil) {
    self.initialRoute = initialRoute;
  } else if (settings.route.empty() == false) {
    self.initialRoute = [NSString stringWithUTF8String:settings.route.c_str()];
  }

  auto platformData = [self.dartProject defaultPlatformData];

  SetEntryPoint(&settings, entrypoint, libraryURI);

  NSString* threadLabel = [FlutterEngine generateThreadLabel:self.labelPrefix];
  _threadHost = std::make_shared<flutter::ThreadHost>();
  *_threadHost = MakeThreadHost(threadLabel, settings);

  __weak FlutterEngine* weakSelf = self;
  flutter::Shell::CreateCallback<flutter::PlatformView> on_create_platform_view =
      [weakSelf](flutter::Shell& shell) {
        FlutterEngine* strongSelf = weakSelf;
        if (!strongSelf) {
          return std::unique_ptr<flutter::PlatformViewIOS>();
        }
        return std::make_unique<flutter::PlatformViewIOS>(shell, shell.GetTaskRunners());
      };

  fml::RefPtr<fml::TaskRunner> ui_runner;
  if (settings.merged_platform_ui_thread == flutter::Settings::MergedPlatformUIThread::kEnabled) {
    ui_runner = fml::MessageLoop::GetCurrent().GetTaskRunner();
  } else {
    ui_runner = _threadHost->ui_thread->GetTaskRunner();
  }
  flutter::TaskRunners task_runners(threadLabel.UTF8String,                          // label
                                    fml::MessageLoop::GetCurrent().GetTaskRunner(),  // platform
                                    ui_runner                                        // ui
  );

  // Create the shell. This is a blocking operation.
  std::unique_ptr<flutter::Shell> shell = flutter::Shell::Create(
      /*platform_data=*/platformData,
      /*task_runners=*/task_runners,
      /*settings=*/settings,
      /*on_create_platform_view=*/on_create_platform_view);

  if (shell == nullptr) {
    NSString* errorMessage = [NSString
        stringWithFormat:@"Could not start a shell FlutterEngine with entrypoint: %@", entrypoint];
    [FlutterLogger logError:errorMessage];
  } else {
    [self setUpShell:std::move(shell)
        withVMServicePublication:settings.enable_vm_service_publication];
    if ([FlutterEngine isProfilerEnabled]) {
      [self startProfiler];
    }
  }

  return _shell != nullptr;
}

- (BOOL)performImplicitEngineCallback {
  id appDelegate = FlutterSharedApplication.application.delegate;
  if ([appDelegate conformsToProtocol:@protocol(FlutterImplicitEngineDelegate)]) {
    id<FlutterImplicitEngineDelegate> provider = (id<FlutterImplicitEngineDelegate>)appDelegate;
    [provider didInitializeImplicitFlutterEngine:[[FlutterImplicitEngineBridgeImpl alloc]
                                                     initWithEngine:self]];
    return YES;
  }
  return NO;
}

- (BOOL)run {
  return [self runWithEntrypoint:FlutterDefaultDartEntrypoint
                      libraryURI:nil
                    initialRoute:FlutterDefaultInitialRoute];
}

- (BOOL)runWithEntrypoint:(NSString*)entrypoint libraryURI:(NSString*)libraryURI {
  return [self runWithEntrypoint:entrypoint
                      libraryURI:libraryURI
                    initialRoute:FlutterDefaultInitialRoute];
}

- (BOOL)runWithEntrypoint:(NSString*)entrypoint {
  return [self runWithEntrypoint:entrypoint libraryURI:nil initialRoute:FlutterDefaultInitialRoute];
}

- (BOOL)runWithEntrypoint:(NSString*)entrypoint initialRoute:(NSString*)initialRoute {
  return [self runWithEntrypoint:entrypoint libraryURI:nil initialRoute:initialRoute];
}

- (BOOL)runWithEntrypoint:(NSString*)entrypoint
               libraryURI:(NSString*)libraryURI
             initialRoute:(NSString*)initialRoute {
  return [self runWithEntrypoint:entrypoint
                      libraryURI:libraryURI
                    initialRoute:initialRoute
                  entrypointArgs:nil];
}

- (BOOL)runWithEntrypoint:(NSString*)entrypoint
               libraryURI:(NSString*)libraryURI
             initialRoute:(NSString*)initialRoute
           entrypointArgs:(NSArray<NSString*>*)entrypointArgs {
  if ([self createShell:entrypoint libraryURI:libraryURI initialRoute:initialRoute]) {
    [self launchEngine:entrypoint libraryURI:libraryURI entrypointArgs:entrypointArgs];
  }

  return _shell != nullptr;
}

- (void)notifyLowMemory {
  if (_shell) {
    _shell->NotifyLowMemoryWarning();
  }
  [self.systemChannel sendMessage:@{@"type" : @"memoryPressure"}];
}

- (NSObject<FlutterBinaryMessenger>*)binaryMessenger {
  return _binaryMessenger;
}

// For test only. Ideally we should create a dependency injector for all dependencies and
// remove this.
- (void)setBinaryMessenger:(FlutterBinaryMessengerRelay*)binaryMessenger {
  // Discard the previous messenger and keep the new one.
  if (binaryMessenger != _binaryMessenger) {
    _binaryMessenger.parent = nil;
    _binaryMessenger = binaryMessenger;
  }
}

#pragma mark - FlutterBinaryMessenger

- (void)sendOnChannel:(NSString*)channel message:(NSData*)message {
  [self sendOnChannel:channel message:message binaryReply:nil];
}

- (void)sendOnChannel:(NSString*)channel
              message:(NSData*)message
          binaryReply:(FlutterBinaryReply)callback {
  NSParameterAssert(channel);
  NSAssert(_shell && _shell->IsSetup(),
           @"Sending a message before the FlutterEngine has been run.");
  fml::RefPtr<flutter::PlatformMessageResponseDarwin> response =
      (callback == nil) ? nullptr
                        : fml::MakeRefCounted<flutter::PlatformMessageResponseDarwin>(
                              ^(NSData* reply) {
                                callback(reply);
                              },
                              _shell->GetTaskRunners().GetPlatformTaskRunner());
  std::unique_ptr<flutter::PlatformMessage> platformMessage =
      (message == nil) ? std::make_unique<flutter::PlatformMessage>(channel.UTF8String, response)
                       : std::make_unique<flutter::PlatformMessage>(
                             channel.UTF8String, flutter::CopyNSDataToMapping(message), response);

  _shell->GetPlatformView()->DispatchPlatformMessage(std::move(platformMessage));
  // platformMessage takes ownership of response.
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
}

- (NSObject<FlutterTaskQueue>*)makeBackgroundTaskQueue {
  return flutter::PlatformMessageHandlerIos::MakeBackgroundTaskQueue();
}

- (FlutterBinaryMessengerConnection)setMessageHandlerOnChannel:(NSString*)channel
                                          binaryMessageHandler:
                                              (FlutterBinaryMessageHandler)handler {
  return [self setMessageHandlerOnChannel:channel binaryMessageHandler:handler taskQueue:nil];
}

- (FlutterBinaryMessengerConnection)
    setMessageHandlerOnChannel:(NSString*)channel
          binaryMessageHandler:(FlutterBinaryMessageHandler)handler
                     taskQueue:(NSObject<FlutterTaskQueue>* _Nullable)taskQueue {
  NSParameterAssert(channel);
  if (_shell && _shell->IsSetup()) {
    self.platformView->GetPlatformMessageHandlerIos()->SetMessageHandler(channel.UTF8String,
                                                                         handler, taskQueue);
    return [self.connections acquireConnectionForChannel:channel];
  } else {
    NSAssert(!handler, @"Setting a message handler before the FlutterEngine has been run.");
    // Setting a handler to nil for a channel that has not yet been set up is a no-op.
    return [FlutterConnectionCollection makeErrorConnectionWithErrorCode:-1L];
  }
}

- (void)cleanUpConnection:(FlutterBinaryMessengerConnection)connection {
  if (_shell && _shell->IsSetup()) {
    NSString* channel = [self.connections cleanupConnectionWithID:connection];
    if (channel.length > 0) {
      self.platformView->GetPlatformMessageHandlerIos()->SetMessageHandler(channel.UTF8String, nil,
                                                                           nil);
    }
  }
}

#pragma mark - FlutterTextureRegistry

- (NSString*)lookupKeyForAsset:(NSString*)asset {
  return [FlutterDartProject lookupKeyForAsset:asset];
}

- (NSString*)lookupKeyForAsset:(NSString*)asset fromPackage:(NSString*)package {
  return [FlutterDartProject lookupKeyForAsset:asset fromPackage:package];
}

- (id<FlutterPluginRegistry>)pluginRegistry {
  return self;
}

#pragma mark - FlutterPluginRegistry

- (NSObject<FlutterPluginRegistrar>*)registrarForPlugin:(NSString*)pluginKey {
  NSAssert(self.pluginPublications[pluginKey] == nil, @"Duplicate plugin key: %@", pluginKey);
  self.pluginPublications[pluginKey] = [NSNull null];
  FlutterEnginePluginRegistrar* result = [[FlutterEnginePluginRegistrar alloc] initWithKey:pluginKey
                                                                             flutterEngine:self];
  self.registrars[pluginKey] = result;
  return result;
}

- (NSObject<FlutterApplicationRegistrar>*)registrarForApplication:(NSString*)key {
  NSAssert(self.pluginPublications[key] == nil, @"Duplicate key: %@", key);
  self.pluginPublications[key] = [NSNull null];
  FlutterEngineApplicationRegistrar* result =
      [[FlutterEngineApplicationRegistrar alloc] initWithKey:key flutterEngine:self];
  self.registrars[key] = result;
  return result;
}

- (BOOL)hasPlugin:(NSString*)pluginKey {
  return _pluginPublications[pluginKey] != nil;
}

- (NSObject*)valuePublishedByPlugin:(NSString*)pluginKey {
  return _pluginPublications[pluginKey];
}

- (void)addSceneLifeCycleDelegate:(NSObject<FlutterSceneLifeCycleDelegate>*)delegate {
  [self.sceneLifeCycleDelegate addDelegate:delegate];
}

#pragma mark - Notifications

- (void)sceneWillEnterForeground:(NSNotification*)notification API_AVAILABLE(ios(13.0)) {
  [self flutterWillEnterForeground:notification];
}

- (void)sceneDidEnterBackground:(NSNotification*)notification API_AVAILABLE(ios(13.0)) {
  [self flutterDidEnterBackground:notification];
}

- (void)applicationWillEnterForeground:(NSNotification*)notification {
  [self flutterWillEnterForeground:notification];
}

- (void)applicationDidEnterBackground:(NSNotification*)notification {
  [self flutterDidEnterBackground:notification];
}

- (void)flutterWillEnterForeground:(NSNotification*)notification {
}

- (void)flutterDidEnterBackground:(NSNotification*)notification {
  [self notifyLowMemory];
}

- (void)onMemoryWarning:(NSNotification*)notification {
  [self notifyLowMemory];
}

#pragma mark - Locale updates

- (void)onLocaleUpdated:(NSNotification*)notification {
  // Get and pass the user's preferred locale list to dart:ui.
  NSMutableArray<NSString*>* localeData = [[NSMutableArray alloc] init];
  NSArray<NSString*>* preferredLocales = [NSLocale preferredLanguages];
  for (NSString* localeID in preferredLocales) {
    NSLocale* locale = [[NSLocale alloc] initWithLocaleIdentifier:localeID];
    NSString* languageCode = [locale objectForKey:NSLocaleLanguageCode];
    NSString* countryCode = [locale objectForKey:NSLocaleCountryCode];
    NSString* scriptCode = [locale objectForKey:NSLocaleScriptCode];
    NSString* variantCode = [locale objectForKey:NSLocaleVariantCode];
    if (!languageCode) {
      continue;
    }
    [localeData addObject:languageCode];
    [localeData addObject:(countryCode ? countryCode : @"")];
    [localeData addObject:(scriptCode ? scriptCode : @"")];
    [localeData addObject:(variantCode ? variantCode : @"")];
  }
  if (localeData.count == 0) {
    return;
  }
  [self.localizationChannel invokeMethod:@"setLocale" arguments:localeData];
}

- (FlutterEngine*)spawnWithEntrypoint:(/*nullable*/ NSString*)entrypoint
                           libraryURI:(/*nullable*/ NSString*)libraryURI
                         initialRoute:(/*nullable*/ NSString*)initialRoute
                       entrypointArgs:(/*nullable*/ NSArray<NSString*>*)entrypointArgs {
  NSAssert(_shell, @"Spawning from an engine without a shell (possibly not run).");
  FlutterEngine* result = [[FlutterEngine alloc] initWithName:self.labelPrefix
                                                      project:self.dartProject];
  flutter::RunConfiguration configuration =
      [self.dartProject runConfigurationForEntrypoint:entrypoint
                                         libraryOrNil:libraryURI
                                       entrypointArgs:entrypointArgs];

  configuration.SetEngineId(result.engineIdentifier);

  fml::WeakPtr<flutter::PlatformView> platform_view = _shell->GetPlatformView();
  FML_DCHECK(platform_view);

  // Lambda captures by pointers to ObjC objects are fine here because the
  // create call is synchronous.
  flutter::Shell::CreateCallback<flutter::PlatformView> on_create_platform_view =
      [](flutter::Shell& shell) {
        return std::make_unique<flutter::PlatformViewIOS>(shell, shell.GetTaskRunners());
      };

  std::unique_ptr<flutter::Shell> shell =
      _shell->Spawn(std::move(configuration), on_create_platform_view);

  result->_threadHost = _threadHost;
  result->_profiler = _profiler;
  [result setUpShell:std::move(shell) withVMServicePublication:NO];
  return result;
}

- (const flutter::ThreadHost&)threadHost {
  return *_threadHost;
}

- (FlutterDartProject*)project {
  return self.dartProject;
}

@end

@implementation FlutterEngineBaseRegistrar

- (instancetype)initWithKey:(NSString*)key flutterEngine:(FlutterEngine*)flutterEngine {
  self = [super init];
  NSAssert(self, @"Super init cannot be nil");
  _key = [key copy];
  _flutterEngine = flutterEngine;
  return self;
}

- (NSObject<FlutterBinaryMessenger>*)messenger {
  return _flutterEngine.binaryMessenger;
}

@end

@implementation FlutterEnginePluginRegistrar

- (void)publish:(NSObject*)value {
  self.flutterEngine.pluginPublications[self.key] = value;
}

- (void)addMethodCallDelegate:(NSObject<FlutterPlugin>*)delegate
                      channel:(FlutterMethodChannel*)channel {
  [channel setMethodCallHandler:^(FlutterMethodCall* call, FlutterResult result) {
    [delegate handleMethodCall:call result:result];
  }];
}

- (void)addApplicationDelegate:(NSObject<FlutterPlugin>*)delegate {
  id<UIApplicationDelegate> appDelegate = FlutterSharedApplication.application.delegate;
  if ([appDelegate conformsToProtocol:@protocol(FlutterAppLifeCycleProvider)]) {
    id<FlutterAppLifeCycleProvider> lifeCycleProvider =
        (id<FlutterAppLifeCycleProvider>)appDelegate;
    [lifeCycleProvider addApplicationLifeCycleDelegate:delegate];
  }
  if (![delegate conformsToProtocol:@protocol(FlutterSceneLifeCycleDelegate)]) {
    // TODO(vashworth): If the plugin doesn't conform to the FlutterSceneLifeCycleDelegate,
    // print a warning pointing to documentation: https://github.com/flutter/flutter/issues/175956
    // [FlutterLogger logWarning:[NSString stringWithFormat:@"Plugin %@ has not migrated to
    // scenes.", self.key]];
  }
}

- (void)addSceneDelegate:(NSObject<FlutterSceneLifeCycleDelegate>*)delegate {
  // If the plugin conforms to FlutterSceneLifeCycleDelegate, add it to the engine.
  [self.flutterEngine addSceneLifeCycleDelegate:delegate];
}

- (NSString*)lookupKeyForAsset:(NSString*)asset {
  return [self.flutterEngine lookupKeyForAsset:asset];
}

- (NSString*)lookupKeyForAsset:(NSString*)asset fromPackage:(NSString*)package {
  return [self.flutterEngine lookupKeyForAsset:asset fromPackage:package];
}

@end

@implementation FlutterEngineApplicationRegistrar
@end
