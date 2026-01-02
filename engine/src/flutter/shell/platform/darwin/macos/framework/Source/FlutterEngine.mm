// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "flutter/shell/platform/darwin/macos/framework/Headers/FlutterEngine.h"
#include <pthread.h>
#import "flutter/shell/platform/darwin/macos/framework/Source/FlutterEngine_Internal.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <vector>

#include "flutter/common/constants.h"
#include "flutter/shell/platform/common/engine_switches.h"
#include "flutter/shell/platform/embedder/embedder.h"

#import "flutter/shell/platform/darwin/common/InternalFlutterSwiftCommon/InternalFlutterSwiftCommon.h"
#import "flutter/shell/platform/darwin/common/framework/Source/FlutterBinaryMessengerRelay.h"
#import "flutter/shell/platform/darwin/macos/InternalFlutterSwift/InternalFlutterSwift.h"
#import "flutter/shell/platform/darwin/macos/framework/Headers/FlutterAppDelegate.h"
#import "flutter/shell/platform/darwin/macos/framework/Source/FlutterDartProject_Internal.h"

@class FlutterEngineRegistrar;

using flutter::kFlutterImplicitViewId;

/**
 * Constructs and returns a FlutterLocale struct corresponding to |locale|, which must outlive
 * the returned struct.
 */
static FlutterLocale FlutterLocaleFromNSLocale(NSLocale* locale) {
  FlutterLocale flutterLocale = {};
  flutterLocale.struct_size = sizeof(FlutterLocale);
  flutterLocale.language_code = [[locale objectForKey:NSLocaleLanguageCode] UTF8String];
  flutterLocale.country_code = [[locale objectForKey:NSLocaleCountryCode] UTF8String];
  flutterLocale.script_code = [[locale objectForKey:NSLocaleScriptCode] UTF8String];
  flutterLocale.variant_code = [[locale objectForKey:NSLocaleVariantCode] UTF8String];
  return flutterLocale;
}

#pragma mark -

// Records an active handler of the messenger (FlutterEngine) that listens to
// platform messages on a given channel.
@interface FlutterEngineHandlerInfo : NSObject

- (instancetype)initWithConnection:(NSNumber*)connection
                           handler:(FlutterBinaryMessageHandler)handler;

@property(nonatomic, readonly) FlutterBinaryMessageHandler handler;
@property(nonatomic, readonly) NSNumber* connection;

@end

@implementation FlutterEngineHandlerInfo
- (instancetype)initWithConnection:(NSNumber*)connection
                           handler:(FlutterBinaryMessageHandler)handler {
  self = [super init];
  NSAssert(self, @"Super init cannot be nil");
  _connection = connection;
  _handler = handler;
  return self;
}
@end

#pragma mark -

/**
 * Private interface declaration for FlutterEngine.
 */
@interface FlutterEngine () <FlutterBinaryMessenger>

/**
 * A mutable array that holds one bool value that determines if responses to platform messages are
 * clear to execute. This value should be read or written only inside of a synchronized block and
 * will return `NO` after the FlutterEngine has been dealloc'd.
 */
@property(nonatomic, strong) NSMutableArray<NSNumber*>* isResponseValid;

/**
 * All delegates added via plugin calls to addApplicationDelegate.
 */
@property(nonatomic, strong) NSPointerArray* pluginAppDelegates;

/**
 * All registrars returned from registrarForPlugin:
 */
@property(nonatomic, readonly)
    NSMutableDictionary<NSString*, FlutterEngineRegistrar*>* pluginRegistrars;

/**
 * Sends the list of user-preferred locales to the Flutter engine.
 */
- (void)sendUserLocales;

/**
 * Handles a platform message from the engine.
 */
- (void)engineCallbackOnPlatformMessage:(const FlutterPlatformMessage*)message;

/**
 * Invoked right before the engine is restarted.
 *
 * This should reset states to as if the application has just started.  It
 * usually indicates a hot restart (Shift-R in Flutter CLI.)
 */
- (void)engineCallbackOnPreEngineRestart;

/**
 * Requests that the task be posted back the to the Flutter engine at the target time. The target
 * time is in the clock used by the Flutter engine.
 */
- (void)postMainThreadTask:(FlutterTask)task targetTimeInNanoseconds:(uint64_t)targetTime;

/**
 * Loads the AOT snapshots and instructions from the elf bundle (app_elf_snapshot.so) into _aotData,
 * if it is present in the assets directory.
 */
- (void)loadAOTData:(NSString*)assetsDir;

- (void)applicationWillTerminate:(NSNotification*)notification;

@end

#pragma mark -

/**
 * `FlutterPluginRegistrar` implementation handling a single plugin.
 */
@interface FlutterEngineRegistrar : NSObject <FlutterPluginRegistrar>
- (instancetype)initWithPlugin:(nonnull NSString*)pluginKey
                 flutterEngine:(nonnull FlutterEngine*)flutterEngine;

/**
 * The value published by this plugin, or NSNull if nothing has been published.
 *
 * The unusual NSNull is for the documented behavior of valuePublishedByPlugin:.
 */
@property(nonatomic, readonly, nonnull) NSObject* publishedValue;
@end

@implementation FlutterEngineRegistrar {
  NSString* _pluginKey;
  __weak FlutterEngine* _flutterEngine;
}

- (instancetype)initWithPlugin:(NSString*)pluginKey flutterEngine:(FlutterEngine*)flutterEngine {
  self = [super init];
  if (self) {
    _pluginKey = [pluginKey copy];
    _flutterEngine = flutterEngine;
    _publishedValue = [NSNull null];
  }
  return self;
}

#pragma mark - FlutterPluginRegistrar

- (id<FlutterBinaryMessenger>)messenger {
  return _flutterEngine.binaryMessenger;
}

- (void)addMethodCallDelegate:(nonnull id<FlutterPlugin>)delegate
                      channel:(nonnull FlutterMethodChannel*)channel {
  [channel setMethodCallHandler:^(FlutterMethodCall* call, FlutterResult result) {
    [delegate handleMethodCall:call result:result];
  }];
}

- (void)addApplicationDelegate:(NSObject<FlutterAppLifecycleDelegate>*)delegate {
  id<NSApplicationDelegate> appDelegate = [[NSApplication sharedApplication] delegate];
  if ([appDelegate conformsToProtocol:@protocol(FlutterAppLifecycleProvider)]) {
    id<FlutterAppLifecycleProvider> lifeCycleProvider =
        static_cast<id<FlutterAppLifecycleProvider>>(appDelegate);
    [lifeCycleProvider addApplicationLifecycleDelegate:delegate];
    [_flutterEngine.pluginAppDelegates addPointer:(__bridge void*)delegate];
  }
}

- (void)publish:(NSObject*)value {
  _publishedValue = value;
}

- (NSString*)lookupKeyForAsset:(NSString*)asset {
  return [FlutterDartProject lookupKeyForAsset:asset];
}

- (NSString*)lookupKeyForAsset:(NSString*)asset fromPackage:(NSString*)package {
  return [FlutterDartProject lookupKeyForAsset:asset fromPackage:package];
}

@end

// Callbacks provided to the engine. See the called methods for documentation.
#pragma mark - Static methods provided to engine configuration

static void OnPlatformMessage(const FlutterPlatformMessage* message, void* user_data) {
  FlutterEngine* engine = (__bridge FlutterEngine*)user_data;
  [engine engineCallbackOnPlatformMessage:message];
}

#pragma mark -

@implementation FlutterEngine {
  // The embedding-API-level engine object.
  FLUTTER_API_SYMBOL(FlutterEngine) _engine;

  // The project being run by this engine.
  FlutterDartProject* _project;

  // A mapping of channel names to the registered information for those channels.
  NSMutableDictionary<NSString*, FlutterEngineHandlerInfo*>* _messengerHandlers;

  // A self-incremental integer to assign to newly assigned channels as
  // identification.
  FlutterBinaryMessengerConnection _currentMessengerConnection;

  // Pointer to the Dart AOT snapshot and instruction data.
  _FlutterEngineAOTData* _aotData;

  // A method channel for miscellaneous platform functionality.
  FlutterMethodChannel* _platformChannel;

  // Proxy to allow plugins, channels to hold a weak reference to the binary messenger (self).
  FlutterBinaryMessengerRelay* _binaryMessenger;
}

static const int kMainThreadPriority = 47;

static void SetThreadPriority(FlutterThreadPriority priority) {
  if (priority == kDisplay) {
    pthread_t thread = pthread_self();
    sched_param param;
    int policy;
    if (!pthread_getschedparam(thread, &policy, &param)) {
      param.sched_priority = kMainThreadPriority;
      pthread_setschedparam(thread, policy, &param);
    }
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  }
}

- (instancetype)initWithName:(NSString*)labelPrefix project:(FlutterDartProject*)project {
  self = [super init];
  NSAssert(self, @"Super init cannot be nil");

  [FlutterRunLoop ensureMainLoopInitialized];

  _project = project ?: [[FlutterDartProject alloc] init];
  _messengerHandlers = [[NSMutableDictionary alloc] init];
  _pluginAppDelegates = [NSPointerArray weakObjectsPointerArray];
  _pluginRegistrars = [[NSMutableDictionary alloc] init];
  _currentMessengerConnection = 1;
  _semanticsEnabled = NO;
  _binaryMessenger = [[FlutterBinaryMessengerRelay alloc] initWithParent:self];
  _isResponseValid = [[NSMutableArray alloc] initWithCapacity:1];
  [_isResponseValid addObject:@YES];

  _embedderAPI.struct_size = sizeof(FlutterEngineProcTable);
  FlutterEngineGetProcAddresses(&_embedderAPI);

  NSNotificationCenter* notificationCenter = [NSNotificationCenter defaultCenter];
  [notificationCenter addObserver:self
                         selector:@selector(sendUserLocales)
                             name:NSCurrentLocaleDidChangeNotification
                           object:nil];

  [self setUpNotificationCenterListeners];

  return self;
}

- (void)dealloc {
  id<NSApplicationDelegate> appDelegate = [[NSApplication sharedApplication] delegate];
  if ([appDelegate conformsToProtocol:@protocol(FlutterAppLifecycleProvider)]) {
    id<FlutterAppLifecycleProvider> lifecycleProvider =
        static_cast<id<FlutterAppLifecycleProvider>>(appDelegate);

    // Unregister any plugins that registered as app delegates, since they are not guaranteed to
    // live after the engine is destroyed, and their delegation registration is intended to be bound
    // to the engine and its lifetime.
    for (id<FlutterAppLifecycleDelegate> delegate in _pluginAppDelegates) {
      if (delegate) {
        [lifecycleProvider removeApplicationLifecycleDelegate:delegate];
      }
    }
  }
  // Clear any published values, just in case a plugin has created a retain cycle with the
  // registrar.
  for (NSString* pluginName in _pluginRegistrars) {
    [_pluginRegistrars[pluginName] publish:[NSNull null]];
  }
  @synchronized(_isResponseValid) {
    [_isResponseValid removeAllObjects];
    [_isResponseValid addObject:@NO];
  }
  [self shutDownEngine];
  if (_aotData) {
    _embedderAPI.CollectAOTData(_aotData);
  }
}

- (FlutterTaskRunnerDescription)createPlatformThreadTaskDescription {
  static size_t sTaskRunnerIdentifiers = 0;
  FlutterTaskRunnerDescription cocoa_task_runner_description = {
      .struct_size = sizeof(FlutterTaskRunnerDescription),
      // Retain for use in post_task_callback. Released in destruction_callback.
      .user_data = (__bridge_retained void*)self,
      .runs_task_on_current_thread_callback = [](void* user_data) -> bool {
        return [[NSThread currentThread] isMainThread];
      },
      .post_task_callback = [](FlutterTask task, uint64_t target_time_nanos,
                               void* user_data) -> void {
        FlutterEngine* engine = (__bridge FlutterEngine*)user_data;
        [engine postMainThreadTask:task targetTimeInNanoseconds:target_time_nanos];
      },
      .identifier = ++sTaskRunnerIdentifiers,
      .destruction_callback =
          [](void* user_data) {
            // Balancing release for the retain when setting user_data above.
            FlutterEngine* engine = (__bridge_transfer FlutterEngine*)user_data;
            engine = nil;
          },
  };
  return cocoa_task_runner_description;
}

- (BOOL)runWithEntrypoint:(NSString*)entrypoint {
  if (self.running) {
    return NO;
  }

  // The first argument of argv is required to be the executable name.
  std::vector<const char*> argv = {[self.executableName UTF8String]};
  std::vector<std::string> switches = self.switches;

  // Enable Impeller only if specifically asked for from the project or cmdline arguments.
  if (_project.enableImpeller ||
      std::find(switches.begin(), switches.end(), "--enable-impeller=true") != switches.end()) {
    switches.push_back("--enable-impeller=true");
  }

  if (_project.enableFlutterGPU ||
      std::find(switches.begin(), switches.end(), "--enable-flutter-gpu=true") != switches.end()) {
    switches.push_back("--enable-flutter-gpu=true");
  }

  std::transform(switches.begin(), switches.end(), std::back_inserter(argv),
                 [](const std::string& arg) -> const char* { return arg.c_str(); });

  std::vector<const char*> dartEntrypointArgs;
  for (NSString* argument in [_project dartEntrypointArguments]) {
    dartEntrypointArgs.push_back([argument UTF8String]);
  }

  FlutterProjectArgs flutterArguments = {};
  flutterArguments.struct_size = sizeof(FlutterProjectArgs);
  flutterArguments.assets_path = _project.assetsPath.UTF8String;
  flutterArguments.icu_data_path = _project.ICUDataPath.UTF8String;
  flutterArguments.command_line_argc = static_cast<int>(argv.size());
  flutterArguments.command_line_argv = argv.empty() ? nullptr : argv.data();
  flutterArguments.platform_message_callback = (FlutterPlatformMessageCallback)OnPlatformMessage;

  flutterArguments.custom_dart_entrypoint = entrypoint.UTF8String;
  flutterArguments.shutdown_dart_vm_when_done = true;
  flutterArguments.dart_entrypoint_argc = dartEntrypointArgs.size();
  flutterArguments.dart_entrypoint_argv = dartEntrypointArgs.data();
  flutterArguments.root_isolate_create_callback = _project.rootIsolateCreateCallback;
  flutterArguments.log_message_callback = [](const char* tag, const char* message,
                                             void* user_data) {
    std::stringstream stream;
    if (tag && tag[0]) {
      stream << tag << ": ";
    }
    stream << message;
    std::string log = stream.str();
    [FlutterLogger logDirect:[NSString stringWithUTF8String:log.c_str()]];
  };

  flutterArguments.engine_id = reinterpret_cast<int64_t>((__bridge void*)self);

  BOOL mergedPlatformUIThread = YES;
  NSNumber* enableMergedPlatformUIThread =
      [[NSBundle mainBundle] objectForInfoDictionaryKey:@"FLTEnableMergedPlatformUIThread"];
  if (enableMergedPlatformUIThread != nil) {
    mergedPlatformUIThread = enableMergedPlatformUIThread.boolValue;
  }

  // The task description needs to be created separately for platform task
  // runner and UI task runner because each one has their own __bridge_retained
  // engine user data.
  FlutterTaskRunnerDescription platformTaskRunnerDescription =
      [self createPlatformThreadTaskDescription];
  std::optional<FlutterTaskRunnerDescription> uiTaskRunnerDescription;
  if (mergedPlatformUIThread) {
    uiTaskRunnerDescription = [self createPlatformThreadTaskDescription];
  }

  const FlutterCustomTaskRunners custom_task_runners = {
      .struct_size = sizeof(FlutterCustomTaskRunners),
      .platform_task_runner = &platformTaskRunnerDescription,
      .thread_priority_setter = SetThreadPriority,
      .ui_task_runner = uiTaskRunnerDescription ? &uiTaskRunnerDescription.value() : nullptr,
  };
  flutterArguments.custom_task_runners = &custom_task_runners;

  [self loadAOTData:_project.assetsPath];
  if (_aotData) {
    flutterArguments.aot_data = _aotData;
  }

  flutterArguments.on_pre_engine_restart_callback = [](void* user_data) {
    FlutterEngine* engine = (__bridge FlutterEngine*)user_data;
    [engine engineCallbackOnPreEngineRestart];
  };

  FlutterEngineResult result = _embedderAPI.Initialize(FLUTTER_ENGINE_VERSION, &flutterArguments,
                                                       (__bridge void*)(self), &_engine);
  if (result != kSuccess) {
    NSLog(@"Failed to initialize Flutter engine: error %d", result);
    return NO;
  }

  result = _embedderAPI.RunInitialized(_engine);
  if (result != kSuccess) {
    NSLog(@"Failed to run an initialized engine: error %d", result);
    return NO;
  }

  [self sendUserLocales];

  return YES;
}

- (void)loadAOTData:(NSString*)assetsDir {
  if (!_embedderAPI.RunsAOTCompiledDartCode()) {
    return;
  }

  BOOL isDirOut = false;  // required for NSFileManager fileExistsAtPath.
  NSFileManager* fileManager = [NSFileManager defaultManager];

  // This is the location where the test fixture places the snapshot file.
  // For applications built by Flutter tool, this is in "App.framework".
  NSString* elfPath = [NSString pathWithComponents:@[ assetsDir, @"app_elf_snapshot.so" ]];

  if (![fileManager fileExistsAtPath:elfPath isDirectory:&isDirOut]) {
    return;
  }

  FlutterEngineAOTDataSource source = {};
  source.type = kFlutterEngineAOTDataSourceTypeElfPath;
  source.elf_path = [elfPath cStringUsingEncoding:NSUTF8StringEncoding];

  auto result = _embedderAPI.CreateAOTData(&source, &_aotData);
  if (result != kSuccess) {
    NSLog(@"Failed to load AOT data from: %@", elfPath);
  }
}

- (id<FlutterBinaryMessenger>)binaryMessenger {
  return _binaryMessenger;
}

- (BOOL)running {
  return _engine != nullptr;
}

- (FlutterEngineProcTable&)embedderAPI {
  return _embedderAPI;
}

- (nonnull NSString*)executableName {
  return [[[NSProcessInfo processInfo] arguments] firstObject] ?: @"Flutter";
}

#pragma mark - Private methods

- (void)sendUserLocales {
  if (!self.running) {
    return;
  }

  // Create a list of FlutterLocales corresponding to the preferred languages.
  NSMutableArray<NSLocale*>* locales = [NSMutableArray array];
  std::vector<FlutterLocale> flutterLocales;
  flutterLocales.reserve(locales.count);
  for (NSString* localeID in [NSLocale preferredLanguages]) {
    NSLocale* locale = [[NSLocale alloc] initWithLocaleIdentifier:localeID];
    [locales addObject:locale];
    flutterLocales.push_back(FlutterLocaleFromNSLocale(locale));
  }
  // Convert to a list of pointers, and send to the engine.
  std::vector<const FlutterLocale*> flutterLocaleList;
  flutterLocaleList.reserve(flutterLocales.size());
  std::transform(flutterLocales.begin(), flutterLocales.end(),
                 std::back_inserter(flutterLocaleList),
                 [](const auto& arg) -> const auto* { return &arg; });
  _embedderAPI.UpdateLocales(_engine, flutterLocaleList.data(), flutterLocaleList.size());
}

- (void)engineCallbackOnPlatformMessage:(const FlutterPlatformMessage*)message {
  NSData* messageData = nil;
  if (message->message_size > 0) {
    messageData = [NSData dataWithBytesNoCopy:(void*)message->message
                                       length:message->message_size
                                 freeWhenDone:NO];
  }
  NSString* channel = @(message->channel);
  __block const FlutterPlatformMessageResponseHandle* responseHandle = message->response_handle;
  __block FlutterEngine* weakSelf = self;
  NSMutableArray* isResponseValid = self.isResponseValid;
  FlutterEngineSendPlatformMessageResponseFnPtr sendPlatformMessageResponse =
      _embedderAPI.SendPlatformMessageResponse;
  FlutterBinaryReply binaryResponseHandler = ^(NSData* response) {
    @synchronized(isResponseValid) {
      if (![isResponseValid[0] boolValue]) {
        // Ignore, engine was killed.
        return;
      }
      if (responseHandle) {
        sendPlatformMessageResponse(weakSelf->_engine, responseHandle,
                                    static_cast<const uint8_t*>(response.bytes), response.length);
        responseHandle = NULL;
      } else {
        NSLog(@"Error: Message responses can be sent only once. Ignoring duplicate response "
               "on channel '%@'.",
              channel);
      }
    }
  };

  FlutterEngineHandlerInfo* handlerInfo = _messengerHandlers[channel];
  if (handlerInfo) {
    handlerInfo.handler(messageData, binaryResponseHandler);
  } else {
    binaryResponseHandler(nil);
  }
}

- (void)engineCallbackOnPreEngineRestart {
}

/**
 * Note: Called from dealloc. Should not use accessors or other methods.
 */
- (void)shutDownEngine {
  if (_engine == nullptr) {
    return;
  }

  FlutterEngineResult result = _embedderAPI.Deinitialize(_engine);
  if (result != kSuccess) {
    NSLog(@"Could not de-initialize the Flutter engine: error %d", result);
  }

  result = _embedderAPI.Shutdown(_engine);
  if (result != kSuccess) {
    NSLog(@"Failed to shut down Flutter engine: error %d", result);
  }
  _engine = nullptr;
}

+ (FlutterEngine*)engineForIdentifier:(int64_t)identifier {
  NSAssert([[NSThread currentThread] isMainThread], @"Must be called on the main thread.");
  return (__bridge FlutterEngine*)reinterpret_cast<void*>(identifier);
}

- (void)setUpNotificationCenterListeners {
  NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
  [center addObserver:self
             selector:@selector(applicationWillTerminate:)
                 name:NSApplicationWillTerminateNotification
               object:nil];
}

- (void)applicationWillTerminate:(NSNotification*)notification {
  [self shutDownEngine];
}

- (std::vector<std::string>)switches {
  return flutter::GetSwitchesFromEnvironment();
}

#pragma mark - FlutterBinaryMessenger

- (void)sendOnChannel:(nonnull NSString*)channel message:(nullable NSData*)message {
  [self sendOnChannel:channel message:message binaryReply:nil];
}

- (void)sendOnChannel:(NSString*)channel
              message:(NSData* _Nullable)message
          binaryReply:(FlutterBinaryReply _Nullable)callback {
  FlutterPlatformMessageResponseHandle* response_handle = nullptr;
  if (callback) {
    struct Captures {
      FlutterBinaryReply reply;
    };
    auto captures = std::make_unique<Captures>();
    captures->reply = callback;
    auto message_reply = [](const uint8_t* data, size_t data_size, void* user_data) {
      auto captures = reinterpret_cast<Captures*>(user_data);
      NSData* reply_data = nil;
      if (data != nullptr && data_size > 0) {
        reply_data = [NSData dataWithBytes:static_cast<const void*>(data) length:data_size];
      }
      captures->reply(reply_data);
      delete captures;
    };

    FlutterEngineResult create_result = _embedderAPI.PlatformMessageCreateResponseHandle(
        _engine, message_reply, captures.get(), &response_handle);
    if (create_result != kSuccess) {
      NSLog(@"Failed to create a FlutterPlatformMessageResponseHandle (%d)", create_result);
      return;
    }
    captures.release();
  }

  FlutterPlatformMessage platformMessage = {
      .struct_size = sizeof(FlutterPlatformMessage),
      .channel = [channel UTF8String],
      .message = static_cast<const uint8_t*>(message.bytes),
      .message_size = message.length,
      .response_handle = response_handle,
  };

  FlutterEngineResult message_result = _embedderAPI.SendPlatformMessage(_engine, &platformMessage);
  if (message_result != kSuccess) {
    NSLog(@"Failed to send message to Flutter engine on channel '%@' (%d).", channel,
          message_result);
  }

  if (response_handle != nullptr) {
    FlutterEngineResult release_result =
        _embedderAPI.PlatformMessageReleaseResponseHandle(_engine, response_handle);
    if (release_result != kSuccess) {
      NSLog(@"Failed to release the response handle (%d).", release_result);
    };
  }
}

- (FlutterBinaryMessengerConnection)setMessageHandlerOnChannel:(nonnull NSString*)channel
                                          binaryMessageHandler:
                                              (nullable FlutterBinaryMessageHandler)handler {
  _currentMessengerConnection += 1;
  _messengerHandlers[channel] =
      [[FlutterEngineHandlerInfo alloc] initWithConnection:@(_currentMessengerConnection)
                                                   handler:[handler copy]];
  return _currentMessengerConnection;
}

- (void)cleanUpConnection:(FlutterBinaryMessengerConnection)connection {
  // Find the _messengerHandlers that has the required connection, and record its
  // channel.
  NSString* foundChannel = nil;
  for (NSString* key in [_messengerHandlers allKeys]) {
    FlutterEngineHandlerInfo* handlerInfo = [_messengerHandlers objectForKey:key];
    if ([handlerInfo.connection isEqual:@(connection)]) {
      foundChannel = key;
      break;
    }
  }
  if (foundChannel) {
    [_messengerHandlers removeObjectForKey:foundChannel];
  }
}

#pragma mark - FlutterPluginRegistry

- (id<FlutterPluginRegistrar>)registrarForPlugin:(NSString*)pluginName {
  id<FlutterPluginRegistrar> registrar = self.pluginRegistrars[pluginName];
  if (!registrar) {
    FlutterEngineRegistrar* registrarImpl =
        [[FlutterEngineRegistrar alloc] initWithPlugin:pluginName flutterEngine:self];
    self.pluginRegistrars[pluginName] = registrarImpl;
    registrar = registrarImpl;
  }
  return registrar;
}

- (nullable NSObject*)valuePublishedByPlugin:(NSString*)pluginName {
  return self.pluginRegistrars[pluginName].publishedValue;
}

#pragma mark - Task runner integration

- (void)postMainThreadTask:(FlutterTask)task targetTimeInNanoseconds:(uint64_t)targetTime {
  __weak FlutterEngine* weakSelf = self;

  const auto engine_time = _embedderAPI.GetCurrentTime();
  [FlutterRunLoop.mainRunLoop
      performAfterDelay:(targetTime - (double)engine_time) / NSEC_PER_SEC
                  block:^{
                    FlutterEngine* self = weakSelf;
                    if (self != nil && self->_engine != nil) {
                      auto result = _embedderAPI.RunTask(self->_engine, &task);
                      if (result != kSuccess) {
                        NSLog(@"Could not post a task to the Flutter engine.");
                      }
                    }
                  }];
}

@end
