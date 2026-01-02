// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// This file defines an Application Binary Interface (ABI), which requires more
// stability than regular code to remain functional for exchanging messages
// between different versions of the embedding and the engine, to allow for both
// forward and backward compatibility.
//
// Specifically,
// - The order, type, and size of the struct members below must remain the same,
//   and members should not be removed.
// - New structures that are part of the ABI must be defined with "size_t
//   struct_size;" as their first member, which should be initialized using
//   "sizeof(Type)".
// - Enum values must not change or be removed.
// - Enum members without explicit values must not be reordered.
// - Function signatures (names, argument counts, argument order, and argument
//   type) cannot change.
// - The core behavior of existing functions cannot change.
// - Instead of nesting structures by value within another structure/union,
//   prefer nesting by pointer. This ensures that adding members to the nested
//   struct does not break the ABI of the parent struct/union.
// - Instead of array of structures, prefer array of pointers to structures.
//   This ensures that array indexing does not break if members are added
//   to the structure.
//
// These changes are allowed:
// - Adding new struct members at the end of a structure as long as the struct
//   is not nested within another struct by value.
// - Adding new enum members with a new value.
// - Renaming a struct member as long as its type, size, and intent remain the
//   same.
// - Renaming an enum member as long as its value and intent remains the same.
//
// It is expected that struct members and implicitly-valued enums will not
// always be declared in an order that is optimal for the reader, since members
// will be added over time, and they can't be reordered.
//
// Existing functions should continue to appear from the caller's point of view
// to operate as they did when they were first introduced, so introduce a new
// function instead of modifying the core behavior of a function (and continue
// to support the existing function with the previous behavior).

#if defined(__cplusplus)
extern "C" {
#endif

#ifndef FLUTTER_EXPORT
#define FLUTTER_EXPORT
#endif  // FLUTTER_EXPORT

#ifdef FLUTTER_API_SYMBOL_PREFIX
#define FLUTTER_EMBEDDING_CONCAT(a, b) a##b
#define FLUTTER_EMBEDDING_ADD_PREFIX(symbol, prefix) \
  FLUTTER_EMBEDDING_CONCAT(prefix, symbol)
#define FLUTTER_API_SYMBOL(symbol) \
  FLUTTER_EMBEDDING_ADD_PREFIX(symbol, FLUTTER_API_SYMBOL_PREFIX)
#else
#define FLUTTER_API_SYMBOL(symbol) symbol
#endif

#define FLUTTER_ENGINE_VERSION 1

typedef enum {
  kSuccess = 0,
  kInvalidLibraryVersion,
  kInvalidArguments,
  kInternalInconsistency,
} FlutterEngineResult;

/// Valid values for priority of Thread.
typedef enum {
  /// Suitable for threads that shouldn't disrupt high priority work.
  kBackground = 0,
  /// Default priority level.
  kNormal = 1,
  /// Suitable for threads which generate data for the display.
  kDisplay = 2,
} FlutterThreadPriority;

typedef struct _FlutterEngine* FLUTTER_API_SYMBOL(FlutterEngine);

typedef void (*VoidCallback)(void* /* user data */);
typedef bool (*BoolCallback)(void* /* user data */);

typedef void (*OnPreEngineRestartCallback)(void* /* user data */);

struct _FlutterPlatformMessageResponseHandle;
typedef struct _FlutterPlatformMessageResponseHandle
    FlutterPlatformMessageResponseHandle;

typedef struct {
  /// The size of this struct. Must be sizeof(FlutterPlatformMessage).
  size_t struct_size;
  const char* channel;
  const uint8_t* message;
  size_t message_size;
  /// The response handle on which to invoke
  /// `FlutterEngineSendPlatformMessageResponse` when the response is ready.
  /// `FlutterEngineSendPlatformMessageResponse` must be called for all messages
  /// received by the embedder. Failure to call
  /// `FlutterEngineSendPlatformMessageResponse` will cause a memory leak. It is
  /// not safe to send multiple responses on a single response object.
  const FlutterPlatformMessageResponseHandle* response_handle;
} FlutterPlatformMessage;

typedef void (*FlutterPlatformMessageCallback)(
    const FlutterPlatformMessage* /* message*/,
    void* /* user data */);

typedef void (*FlutterDataCallback)(const uint8_t* /* data */,
                                    size_t /* size */,
                                    void* /* user data */);

/// An update to whether a message channel has a listener set or not.
typedef struct {
  /// The size of the struct. Must be sizeof(FlutterChannelUpdate).
  size_t struct_size;
  /// The name of the channel.
  const char* channel;
  /// True if a listener has been set, false if one has been cleared.
  bool listening;
} FlutterChannelUpdate;

typedef void (*FlutterChannelUpdateCallback)(
    const FlutterChannelUpdate* /* channel update */,
    void* /* user data */);

typedef struct _FlutterTaskRunner* FlutterTaskRunner;

typedef struct {
  FlutterTaskRunner runner;
  uint64_t task;
} FlutterTask;

typedef void (*FlutterTaskRunnerPostTaskCallback)(
    FlutterTask /* task */,
    uint64_t /* target time nanos */,
    void* /* user data */);

/// An interface used by the Flutter engine to execute tasks at the target time
/// on a specified thread. There should be a 1-1 relationship between a thread
/// and a task runner. It is undefined behavior to run a task on a thread that
/// is not associated with its task runner.
typedef struct {
  /// The size of this struct. Must be sizeof(FlutterTaskRunnerDescription).
  size_t struct_size;
  void* user_data;
  /// May be called from any thread. Should return true if tasks posted on the
  /// calling thread will be run on that same thread.
  ///
  /// @attention     This field is required.
  BoolCallback runs_task_on_current_thread_callback;
  /// May be called from any thread. The given task should be executed by the
  /// embedder on the thread associated with that task runner by calling
  /// `FlutterEngineRunTask` at the given target time. The system monotonic
  /// clock should be used for the target time. The target time is the absolute
  /// time from epoch (NOT a delta) at which the task must be returned back to
  /// the engine on the correct thread. If the embedder needs to calculate a
  /// delta, `FlutterEngineGetCurrentTime` may be called and the difference used
  /// as the delta.
  ///
  /// @attention     This field is required.
  FlutterTaskRunnerPostTaskCallback post_task_callback;
  /// A unique identifier for the task runner. If multiple task runners service
  /// tasks on the same thread, their identifiers must match.
  size_t identifier;
  /// The callback invoked when the task runner is destroyed.
  VoidCallback destruction_callback;
} FlutterTaskRunnerDescription;

typedef struct {
  /// The size of this struct. Must be sizeof(FlutterCustomTaskRunners).
  size_t struct_size;
  /// Specify the task runner for the thread on which the `FlutterEngineRun`
  /// call is made. The same task runner description can be specified for both
  /// the render and platform task runners. This makes the Flutter engine use
  /// the same thread for both task runners.
  const FlutterTaskRunnerDescription* platform_task_runner;
  /// Specify the task runner for the thread on which the render tasks will be
  /// run. The same task runner description can be specified for both the render
  /// and platform task runners. This makes the Flutter engine use the same
  /// thread for both task runners.
  const FlutterTaskRunnerDescription* render_task_runner;
  /// Specify a callback that is used to set the thread priority for embedder
  /// task runners.
  void (*thread_priority_setter)(FlutterThreadPriority);
  /// Specify the task runner for the thread on which the UI tasks will be run.
  /// This may be same as platform_task_runner, in which case the Flutter engine
  /// will run the UI isolate on platform thread.
  const FlutterTaskRunnerDescription* ui_task_runner;
} FlutterCustomTaskRunners;

typedef struct {
  /// This size of this struct. Must be sizeof(FlutterLocale).
  size_t struct_size;
  /// The language code of the locale. For example, "en". This is a required
  /// field. The string must be null terminated. It may be collected after the
  /// call to `FlutterEngineUpdateLocales`.
  const char* language_code;
  /// The country code of the locale. For example, "US". This is a an optional
  /// field. The string must be null terminated if present. It may be collected
  /// after the call to `FlutterEngineUpdateLocales`. If not present, a
  /// `nullptr` may be specified.
  const char* country_code;
  /// The script code of the locale. This is a an optional field. The string
  /// must be null terminated if present. It may be collected after the call to
  /// `FlutterEngineUpdateLocales`. If not present, a `nullptr` may be
  /// specified.
  const char* script_code;
  /// The variant code of the locale. This is a an optional field. The string
  /// must be null terminated if present. It may be collected after the call to
  /// `FlutterEngineUpdateLocales`. If not present, a `nullptr` may be
  /// specified.
  const char* variant_code;
} FlutterLocale;

/// Callback that returns the system locale.
///
/// Embedders that implement this callback should return the `FlutterLocale`
/// from the `supported_locales` list that most closely matches the
/// user/device's preferred locale.
///
/// This callback does not currently provide the user_data baton.
/// https://github.com/flutter/flutter/issues/79826
typedef const FlutterLocale* (*FlutterComputePlatformResolvedLocaleCallback)(
    const FlutterLocale** /* supported_locales*/,
    size_t /* Number of locales*/);

typedef int64_t FlutterEngineDartPort;

typedef enum {
  kFlutterEngineDartObjectTypeNull,
  kFlutterEngineDartObjectTypeBool,
  kFlutterEngineDartObjectTypeInt32,
  kFlutterEngineDartObjectTypeInt64,
  kFlutterEngineDartObjectTypeDouble,
  kFlutterEngineDartObjectTypeString,
  /// The object will be made available to Dart code as an instance of
  /// Uint8List.
  kFlutterEngineDartObjectTypeBuffer,
} FlutterEngineDartObjectType;

typedef struct {
  /// The size of this struct. Must be sizeof(FlutterEngineDartBuffer).
  size_t struct_size;
  /// An opaque baton passed back to the embedder when the
  /// buffer_collect_callback is invoked. The engine does not interpret this
  /// field in any way.
  void* user_data;
  /// This is an optional field.
  ///
  /// When specified, the engine will assume that the buffer is owned by the
  /// embedder. When the data is no longer needed by any isolate, this callback
  /// will be made on an internal engine managed thread. The embedder is free to
  /// collect the buffer here. When this field is specified, it is the embedders
  /// responsibility to keep the buffer alive and not modify it till this
  /// callback is invoked by the engine. The user data specified in the callback
  /// is the value of `user_data` field in this struct.
  ///
  /// When NOT specified, the VM creates an internal copy of the buffer. The
  /// caller is free to modify the buffer as necessary or collect it immediately
  /// after the call to `FlutterEnginePostDartObject`.
  ///
  /// @attention      The buffer_collect_callback is will only be invoked by the
  ///                 engine when the `FlutterEnginePostDartObject` method
  ///                 returns kSuccess. In case of non-successful calls to this
  ///                 method, it is the embedders responsibility to collect the
  ///                 buffer.
  VoidCallback buffer_collect_callback;
  /// A pointer to the bytes of the buffer. When the buffer is owned by the
  /// embedder (by specifying the `buffer_collect_callback`), Dart code may
  /// modify that embedder owned buffer. For this reason, it is important that
  /// this buffer not have page protections that restrict writing to this
  /// buffer.
  uint8_t* buffer;
  /// The size of the buffer.
  size_t buffer_size;
} FlutterEngineDartBuffer;

/// This struct specifies the native representation of a Dart object that can be
/// sent via a send port to any isolate in the VM that has the corresponding
/// receive port.
///
/// All fields in this struct are copied out in the call to
/// `FlutterEnginePostDartObject` and the caller is free to reuse or collect
/// this struct after that call.
typedef struct {
  FlutterEngineDartObjectType type;
  union {
    bool bool_value;
    int32_t int32_value;
    int64_t int64_value;
    double double_value;
    /// A null terminated string. This string will be copied by the VM in the
    /// call to `FlutterEnginePostDartObject` and must be collected by the
    /// embedder after that call is made.
    const char* string_value;
    const FlutterEngineDartBuffer* buffer_value;
  };
} FlutterEngineDartObject;

/// This enum allows embedders to determine the type of the engine thread in the
/// FlutterNativeThreadCallback. Based on the thread type, the embedder may be
/// able to tweak the thread priorities for optimum performance.
typedef enum {
  /// The Flutter Engine considers the thread on which the FlutterEngineRun call
  /// is made to be the platform thread. There is only one such thread per
  /// engine instance.
  kFlutterNativeThreadTypePlatform,
  /// This is the thread the Flutter Engine uses to execute rendering commands
  /// based on the selected client rendering API. There is only one such thread
  /// per engine instance.
  kFlutterNativeThreadTypeUI,
  /// Multiple threads are used by the Flutter engine to perform long running
  /// background tasks.
  kFlutterNativeThreadTypeWorker,
} FlutterNativeThreadType;

/// A callback made by the engine in response to
/// `FlutterEnginePostCallbackOnAllNativeThreads` on all internal thread.
typedef void (*FlutterNativeThreadCallback)(FlutterNativeThreadType type,
                                            void* user_data);

/// AOT data source type.
typedef enum {
  kFlutterEngineAOTDataSourceTypeElfPath
} FlutterEngineAOTDataSourceType;

/// This struct specifies one of the various locations the engine can look for
/// AOT data sources.
typedef struct {
  FlutterEngineAOTDataSourceType type;
  union {
    /// Absolute path to an ELF library file.
    const char* elf_path;
  };
} FlutterEngineAOTDataSource;

// Logging callback for Dart application messages.
//
// The `tag` parameter contains a null-terminated string containing a logging
// tag or component name that can be used to identify system log messages from
// the app. The `message` parameter contains a null-terminated string
// containing the message to be logged. `user_data` is a user data baton passed
// in `FlutterEngineRun`.
typedef void (*FlutterLogMessageCallback)(const char* /* tag */,
                                          const char* /* message */,
                                          void* /* user_data */);

/// An opaque object that describes the AOT data that can be used to launch a
/// FlutterEngine instance in AOT mode.
typedef struct _FlutterEngineAOTData* FlutterEngineAOTData;

typedef struct {
  /// The size of this struct. Must be sizeof(FlutterProjectArgs).
  size_t struct_size;

  /// The path to the Flutter assets directory containing project assets. The
  /// string can be collected after the call to `FlutterEngineRun` returns. The
  /// string must be NULL terminated.

  const char* assets_path;
  /// The path to the Dart file containing the `main` entry point.
  /// The string can be collected after the call to `FlutterEngineRun` returns.
  /// The string must be NULL terminated.
  ///
  /// @deprecated     As of Dart 2, running from Dart source is no longer
  ///                 supported. Dart code should now be compiled to kernel form
  ///                 and will be loaded by from `kernel_blob.bin` in the assets
  ///                 directory. This struct member is retained for ABI
  ///                 stability.
  const char* main_path__unused__;

  /// The path to the `.packages` file for the project. The string can be
  /// collected after the call to `FlutterEngineRun` returns. The string must be
  /// NULL terminated.
  ///
  /// @deprecated    As of Dart 2, running from Dart source is no longer
  ///                supported. Dart code should now be compiled to kernel form
  ///                and will be loaded by from `kernel_blob.bin` in the assets
  ///                directory. This struct member is retained for ABI
  ///                stability.

  const char* packages_path__unused__;

  /// The path to the `icudtl.dat` file for the project. The string can be
  /// collected after the call to `FlutterEngineRun` returns. The string must
  /// be NULL terminated.
  const char* icu_data_path;

  /// The command line argument count used to initialize the project.
  int command_line_argc;

  /// The command line arguments used to initialize the project. The strings can
  /// be collected after the call to `FlutterEngineRun` returns. The strings
  /// must be `NULL` terminated.
  ///
  /// @attention     The first item in the command line (if specified at all) is
  ///                interpreted as the executable name. So if an engine flag
  ///                needs to be passed into the same, it needs to not be the
  ///                very first item in the list.
  ///
  /// The set of engine flags are only meant to control
  /// unstable features in the engine. Deployed applications should not pass any
  /// command line arguments at all as they may affect engine stability at
  /// runtime in the presence of un-sanitized input. The list of currently
  /// recognized engine flags and their descriptions can be retrieved from the
  /// `switches.h` engine source file.
  const char* const* command_line_argv;

  /// The callback invoked by the engine in order to give the embedder the
  /// chance to respond to platform messages from the Dart application.
  /// The callback will be invoked on the thread on which the `FlutterEngineRun`
  /// call is made. The second parameter, `user_data`, is supplied when
  /// `FlutterEngineRun` or `FlutterEngineInitialize` is called.
  FlutterPlatformMessageCallback platform_message_callback;

  /// The VM snapshot data buffer used in AOT operation. This buffer must be
  /// mapped in as read-only. For more information refer to the documentation on
  /// the Wiki at
  /// https://github.com/flutter/flutter/wiki/Flutter-engine-operation-in-AOT-Mode
  const uint8_t* vm_snapshot_data;

  /// The size of the VM snapshot data buffer.  If vm_snapshot_data is a symbol
  /// reference, 0 may be passed here.
  size_t vm_snapshot_data_size;

  /// The VM snapshot instructions buffer used in AOT operation. This buffer
  /// must be mapped in as read-execute. For more information refer to the
  /// documentation on the Wiki at
  /// https://github.com/flutter/flutter/wiki/Flutter-engine-operation-in-AOT-Mode
  const uint8_t* vm_snapshot_instructions;

  /// The size of the VM snapshot instructions buffer. If
  /// vm_snapshot_instructions is a symbol reference, 0 may be passed here.
  size_t vm_snapshot_instructions_size;

  /// The isolate snapshot data buffer used in AOT operation. This buffer must
  /// be mapped in as read-only. For more information refer to the documentation
  /// on the Wiki at
  /// https://github.com/flutter/flutter/wiki/Flutter-engine-operation-in-AOT-Mode
  const uint8_t* isolate_snapshot_data;

  /// The size of the isolate snapshot data buffer.  If isolate_snapshot_data is
  /// a symbol reference, 0 may be passed here.
  size_t isolate_snapshot_data_size;

  /// The isolate snapshot instructions buffer used in AOT operation. This
  /// buffer must be mapped in as read-execute. For more information refer to
  /// the documentation on the Wiki at
  /// https://github.com/flutter/flutter/wiki/Flutter-engine-operation-in-AOT-Mode
  const uint8_t* isolate_snapshot_instructions;

  /// The size of the isolate snapshot instructions buffer. If
  /// isolate_snapshot_instructions is a symbol reference, 0 may be passed here.
  size_t isolate_snapshot_instructions_size;

  /// The callback invoked by the engine in root isolate scope. Called
  /// immediately after the root isolate has been created and marked runnable.
  VoidCallback root_isolate_create_callback;

  /// The name of a custom Dart entrypoint. This is optional and specifying a
  /// null or empty entrypoint makes the engine look for a method named "main"
  /// in the root library of the application.
  ///
  /// Care must be taken to ensure that the custom entrypoint is not tree-shaken
  /// away. Usually, this is done using the `@pragma('vm:entry-point')`
  /// decoration.
  const char* custom_dart_entrypoint;

  /// Typically the Flutter engine create and manages its internal threads. This
  /// optional argument allows for the specification of task runner interfaces
  /// to event loops managed by the embedder on threads it creates.
  const FlutterCustomTaskRunners* custom_task_runners;

  /// All `FlutterEngine` instances in the process share the same Dart VM. When
  /// the first engine is launched, it starts the Dart VM as well. It used to be
  /// the case that it was not possible to shutdown the Dart VM cleanly and
  /// start it back up in the process in a safe manner. This issue has since
  /// been patched. Unfortunately, applications already began to make use of the
  /// fact that shutting down the Flutter engine instance left a running VM in
  /// the process. Since a Flutter engine could be launched on any thread,
  /// applications would "warm up" the VM on another thread by launching
  /// an engine with no isolates and then shutting it down immediately. The main
  /// Flutter application could then be started on the main thread without
  /// having to incur the Dart VM startup costs at that time. With the new
  /// behavior, this "optimization" immediately becomes massive performance
  /// pessimization as the VM would be started up in the "warm up" phase, shut
  /// down there and then started again on the main thread. Changing this
  /// behavior was deemed to be an unacceptable breaking change. Embedders that
  /// wish to shutdown the Dart VM when the last engine is terminated in the
  /// process should opt into this behavior by setting this flag to true.
  bool shutdown_dart_vm_when_done;

  /// Max size of the old gen heap for the Dart VM in MB, or 0 for unlimited, -1
  /// for default value.
  ///
  /// See also:
  /// https://github.com/dart-lang/sdk/blob/ca64509108b3e7219c50d6c52877c85ab6a35ff2/runtime/vm/flag_list.h#L150
  int64_t dart_old_gen_heap_size;

  /// The AOT data to be used in AOT operation.
  ///
  /// Embedders should instantiate and destroy this object via the
  /// FlutterEngineCreateAOTData and FlutterEngineCollectAOTData methods.
  ///
  /// Embedders can provide either snapshot buffers or aot_data, but not both.
  FlutterEngineAOTData aot_data;

  /// A callback that computes the locale the platform would natively resolve
  /// to.
  ///
  /// The input parameter is an array of FlutterLocales which represent the
  /// locales supported by the app. One of the input supported locales should
  /// be selected and returned to best match with the user/device's preferred
  /// locale. The implementation should produce a result that as closely
  /// matches what the platform would natively resolve to as possible.
  FlutterComputePlatformResolvedLocaleCallback
      compute_platform_resolved_locale_callback;

  /// The command line argument count for arguments passed through to the Dart
  /// entrypoint.
  int dart_entrypoint_argc;

  /// The command line arguments passed through to the Dart entrypoint. The
  /// strings must be `NULL` terminated.
  ///
  /// The strings will be copied out and so any strings passed in here can
  /// be safely collected after initializing the engine with
  /// `FlutterProjectArgs`.
  const char* const* dart_entrypoint_argv;

  // Logging callback for Dart application messages.
  //
  // This callback is used by embedder to log print messages from the running
  // Flutter application. This callback is made on an internal engine managed
  // thread and embedders must re-thread if necessary. Performing blocking calls
  // in this callback may introduce application jank.
  FlutterLogMessageCallback log_message_callback;

  // A tag string associated with application log messages.
  //
  // A log message tag string that can be used convey application, subsystem,
  // or component name to embedder's logger. This string will be passed to to
  // callbacks on `log_message_callback`. Defaults to "flutter" if unspecified.
  const char* log_tag;

  // A callback that is invoked right before the engine is restarted.
  //
  // This optional callback is typically used to reset states to as if the
  // engine has just been started, and usually indicates the user has requested
  // a hot restart (Shift-R in the Flutter CLI.) It is not called the first time
  // the engine starts.
  //
  // The first argument is the `user_data` from `FlutterEngineInitialize`.
  OnPreEngineRestartCallback on_pre_engine_restart_callback;

  /// The callback invoked by the engine in response to a channel listener
  /// being registered on the framework side. The callback is invoked from
  /// a task posted to the platform thread.
  FlutterChannelUpdateCallback channel_update_callback;

  /// Opaque identifier provided by the engine. Accessible in Dart code through
  /// `PlatformDispatcher.instance.engineId`. Can be used in native code to
  /// retrieve the engine instance that is running the Dart code.
  int64_t engine_id;
} FlutterProjectArgs;

#ifndef FLUTTER_ENGINE_NO_PROTOTYPES

// NOLINTBEGIN(google-objc-function-naming)

//------------------------------------------------------------------------------
/// @brief      Creates the necessary data structures to launch a Flutter Dart
///             application in AOT mode. The data may only be collected after
///             all FlutterEngine instances launched using this data have been
///             terminated.
///
/// @param[in]  source    The source of the AOT data.
/// @param[out] data_out  The AOT data on success. Unchanged on failure.
///
/// @return     Returns if the AOT data could be successfully resolved.
///
FLUTTER_EXPORT
FlutterEngineResult FlutterEngineCreateAOTData(
    const FlutterEngineAOTDataSource* source,
    FlutterEngineAOTData* data_out);

//------------------------------------------------------------------------------
/// @brief      Collects the AOT data.
///
/// @warning    The embedder must ensure that this call is made only after all
///             FlutterEngine instances launched using this data have been
///             terminated, and that all of those instances were launched with
///             the FlutterProjectArgs::shutdown_dart_vm_when_done flag set to
///             true.
///
/// @param[in]  data   The data to collect.
///
/// @return     Returns if the AOT data was successfully collected.
///
FLUTTER_EXPORT
FlutterEngineResult FlutterEngineCollectAOTData(FlutterEngineAOTData data);

//------------------------------------------------------------------------------
/// @brief      Initialize and run a Flutter engine instance and return a handle
///             to it. This is a convenience method for the pair of calls to
///             `FlutterEngineInitialize` and `FlutterEngineRunInitialized`.
///
/// @note       This method of running a Flutter engine works well except in
///             cases where the embedder specifies custom task runners via
///             `FlutterProjectArgs::custom_task_runners`. In such cases, the
///             engine may need the embedder to post tasks back to it before
///             `FlutterEngineRun` has returned. Embedders can only post tasks
///             to the engine if they have a handle to the engine. In such
///             cases, embedders are advised to get the engine handle by calling
///             `FlutterEngineInitialize`. Then they can call
///             `FlutterEngineRunInitialized` knowing that they will be able to
///             service custom tasks on other threads with the engine handle.
///
/// @param[in]  version    The Flutter embedder API version. Must be
///                        FLUTTER_ENGINE_VERSION.
/// @param[in]  config     The renderer configuration.
/// @param[in]  args       The Flutter project arguments.
/// @param      user_data  A user data baton passed back to embedders in
///                        callbacks.
/// @param[out] engine_out The engine handle on successful engine creation.
///
/// @return     The result of the call to run the Flutter engine.
///
FLUTTER_EXPORT
FlutterEngineResult FlutterEngineRun(size_t version,
                                     const FlutterProjectArgs* args,
                                     void* user_data,
                                     FLUTTER_API_SYMBOL(FlutterEngine) *
                                         engine_out);

//------------------------------------------------------------------------------
/// @brief      Shuts down a Flutter engine instance. The engine handle is no
///             longer valid for any calls in the embedder API after this point.
///             Making additional calls with this handle is undefined behavior.
///
/// @note       This de-initializes the Flutter engine instance (via an implicit
///             call to `FlutterEngineDeinitialize`) if necessary.
///
/// @param[in]  engine  The Flutter engine instance to collect.
///
/// @return     The result of the call to shutdown the Flutter engine instance.
///
FLUTTER_EXPORT
FlutterEngineResult FlutterEngineShutdown(FLUTTER_API_SYMBOL(FlutterEngine)
                                              engine);

//------------------------------------------------------------------------------
/// @brief      Initialize a Flutter engine instance. This does not run the
///             Flutter application code till the `FlutterEngineRunInitialized`
///             call is made. Besides Flutter application code, no tasks are
///             scheduled on embedder managed task runners either. This allows
///             embedders providing custom task runners to the Flutter engine to
///             obtain a handle to the Flutter engine before the engine can post
///             tasks on these task runners.
///
/// @param[in]  version    The Flutter embedder API version. Must be
///                        FLUTTER_ENGINE_VERSION.
/// @param[in]  config     The renderer configuration.
/// @param[in]  args       The Flutter project arguments.
/// @param      user_data  A user data baton passed back to embedders in
///                        callbacks.
/// @param[out] engine_out The engine handle on successful engine creation.
///
/// @return     The result of the call to initialize the Flutter engine.
///
FLUTTER_EXPORT
FlutterEngineResult FlutterEngineInitialize(size_t version,
                                            const FlutterProjectArgs* args,
                                            void* user_data,
                                            FLUTTER_API_SYMBOL(FlutterEngine) *
                                                engine_out);

//------------------------------------------------------------------------------
/// @brief      Stops running the Flutter engine instance. After this call, the
///             embedder is also guaranteed that no more calls to post tasks
///             onto custom task runners specified by the embedder are made. The
///             Flutter engine handle still needs to be collected via a call to
///             `FlutterEngineShutdown`.
///
/// @param[in]  engine    The running engine instance to de-initialize.
///
/// @return     The result of the call to de-initialize the Flutter engine.
///
FLUTTER_EXPORT
FlutterEngineResult FlutterEngineDeinitialize(FLUTTER_API_SYMBOL(FlutterEngine)
                                                  engine);

//------------------------------------------------------------------------------
/// @brief      Runs an initialized engine instance. An engine can be
///             initialized via `FlutterEngineInitialize`. An initialized
///             instance can only be run once. During and after this call,
///             custom task runners supplied by the embedder are expected to
///             start servicing tasks.
///
/// @param[in]  engine  An initialized engine instance that has not previously
///                     been run.
///
/// @return     The result of the call to run the initialized Flutter
///             engine instance.
///
FLUTTER_EXPORT
FlutterEngineResult FlutterEngineRunInitialized(
    FLUTTER_API_SYMBOL(FlutterEngine) engine);

FLUTTER_EXPORT
FlutterEngineResult FlutterEngineSendPlatformMessage(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    const FlutterPlatformMessage* message);

//------------------------------------------------------------------------------
/// @brief     Creates a platform message response handle that allows the
///            embedder to set a native callback for a response to a message.
///            This handle may be set on the `response_handle` field of any
///            `FlutterPlatformMessage` sent to the engine.
///
///            The handle must be collected via a call to
///            `FlutterPlatformMessageReleaseResponseHandle`. This may be done
///            immediately after a call to `FlutterEngineSendPlatformMessage`
///            with a platform message whose response handle contains the handle
///            created using this call. In case a handle is created but never
///            sent in a message, the release call must still be made. Not
///            calling release on the handle results in a small memory leak.
///
///            The user data baton passed to the data callback is the one
///            specified in this call as the third argument.
///
/// @see       FlutterPlatformMessageReleaseResponseHandle()
///
/// @param[in]  engine         A running engine instance.
/// @param[in]  data_callback  The callback invoked by the engine when the
///                            Flutter application send a response on the
///                            handle.
/// @param[in]  user_data      The user data associated with the data callback.
/// @param[out] response_out   The response handle created when this call is
///                            successful.
///
/// @return     The result of the call.
///
FLUTTER_EXPORT
FlutterEngineResult FlutterPlatformMessageCreateResponseHandle(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    FlutterDataCallback data_callback,
    void* user_data,
    FlutterPlatformMessageResponseHandle** response_out);

//------------------------------------------------------------------------------
/// @brief      Collects the handle created using
///             `FlutterPlatformMessageCreateResponseHandle`.
///
/// @see        FlutterPlatformMessageCreateResponseHandle()
///
/// @param[in]  engine     A running engine instance.
/// @param[in]  response   The platform message response handle to collect.
///                        These handles are created using
///                        `FlutterPlatformMessageCreateResponseHandle()`.
///
/// @return     The result of the call.
///
FLUTTER_EXPORT
FlutterEngineResult FlutterPlatformMessageReleaseResponseHandle(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    FlutterPlatformMessageResponseHandle* response);

//------------------------------------------------------------------------------
/// @brief      Send a response from the native side to a platform message from
///             the Dart Flutter application.
///
/// @param[in]  engine       The running engine instance.
/// @param[in]  handle       The platform message response handle.
/// @param[in]  data         The data to associate with the platform message
///                          response.
/// @param[in]  data_length  The length of the platform message response data.
///
/// @return     The result of the call.
///
FLUTTER_EXPORT
FlutterEngineResult FlutterEngineSendPlatformMessageResponse(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    const FlutterPlatformMessageResponseHandle* handle,
    const uint8_t* data,
    size_t data_length);

//------------------------------------------------------------------------------
/// @brief      This API is only meant to be used by platforms that need to
///             flush tasks on a message loop not controlled by the Flutter
///             engine.
///
/// @deprecated This API will be deprecated and is not part of the stable API.
///             Please use the custom task runners API by setting an
///             appropriate `FlutterProjectArgs::custom_task_runners`
///             interface. This will yield better performance and the
///             interface is stable.
///
/// @return     The result of the call.
///
FLUTTER_EXPORT
FlutterEngineResult __FlutterEngineFlushPendingTasksNow();

//------------------------------------------------------------------------------
/// @brief      A profiling utility. Logs a trace duration begin event to the
///             timeline. If the timeline is unavailable or disabled, this has
///             no effect. Must be balanced with an duration end event (via
///             `FlutterEngineTraceEventDurationEnd`) with the same name on the
///             same thread. Can be called on any thread. Strings passed into
///             the function will NOT be copied when added to the timeline. Only
///             string literals may be passed in.
///
/// @param[in]  name  The name of the trace event.
///
FLUTTER_EXPORT
void FlutterEngineTraceEventDurationBegin(const char* name);

//-----------------------------------------------------------------------------
/// @brief      A profiling utility. Logs a trace duration end event to the
///             timeline. If the timeline is unavailable or disabled, this has
///             no effect. This call must be preceded by a trace duration begin
///             call (via `FlutterEngineTraceEventDurationBegin`) with the same
///             name on the same thread. Can be called on any thread. Strings
///             passed into the function will NOT be copied when added to the
///             timeline. Only string literals may be passed in.
///
/// @param[in]  name  The name of the trace event.
///
FLUTTER_EXPORT
void FlutterEngineTraceEventDurationEnd(const char* name);

//-----------------------------------------------------------------------------
/// @brief      A profiling utility. Logs a trace duration instant event to the
///             timeline. If the timeline is unavailable or disabled, this has
///             no effect. Can be called on any thread. Strings passed into the
///             function will NOT be copied when added to the timeline. Only
///             string literals may be passed in.
///
/// @param[in]  name  The name of the trace event.
///
FLUTTER_EXPORT
void FlutterEngineTraceEventInstant(const char* name);

//------------------------------------------------------------------------------
/// @brief      Get the current time in nanoseconds from the clock used by the
///             flutter engine. This is the system monotonic clock.
///
/// @return     The current time in nanoseconds.
///
FLUTTER_EXPORT
uint64_t FlutterEngineGetCurrentTime();

//------------------------------------------------------------------------------
/// @brief      Inform the engine to run the specified task. This task has been
///             given to the embedder via the
///             `FlutterTaskRunnerDescription.post_task_callback`. This call
///             must only be made at the target time specified in that callback.
///             Running the task before that time is undefined behavior.
///
/// @param[in]  engine     A running engine instance.
/// @param[in]  task       the task handle.
///
/// @return     The result of the call.
///
FLUTTER_EXPORT
FlutterEngineResult FlutterEngineRunTask(FLUTTER_API_SYMBOL(FlutterEngine)
                                             engine,
                                         const FlutterTask* task);

//------------------------------------------------------------------------------
/// @brief      Notify a running engine instance that the locale has been
///             updated. The preferred locale must be the first item in the list
///             of locales supplied. The other entries will be used as a
///             fallback.
///
/// @param[in]  engine         A running engine instance.
/// @param[in]  locales        The updated locales in the order of preference.
/// @param[in]  locales_count  The count of locales supplied.
///
/// @return     Whether the locale updates were applied.
///
FLUTTER_EXPORT
FlutterEngineResult FlutterEngineUpdateLocales(FLUTTER_API_SYMBOL(FlutterEngine)
                                                   engine,
                                               const FlutterLocale** locales,
                                               size_t locales_count);

//------------------------------------------------------------------------------
/// @brief      Returns if the Flutter engine instance will run AOT compiled
///             Dart code. This call has no threading restrictions.
///
///             For embedder code that is configured for both AOT and JIT mode
///             Dart execution based on the Flutter engine being linked to, this
///             runtime check may be used to appropriately configure the
///             `FlutterProjectArgs`. In JIT mode execution, the kernel
///             snapshots must be present in the Flutter assets directory
///             specified in the `FlutterProjectArgs`. For AOT execution, the
///             fields `vm_snapshot_data`, `vm_snapshot_instructions`,
///             `isolate_snapshot_data` and `isolate_snapshot_instructions`
///             (along with their size fields) must be specified in
///             `FlutterProjectArgs`.
///
/// @return     True, if AOT Dart code is run. JIT otherwise.
///
FLUTTER_EXPORT
bool FlutterEngineRunsAOTCompiledDartCode(void);

//------------------------------------------------------------------------------
/// @brief      Posts a Dart object to specified send port. The corresponding
///             receive port for send port can be in any isolate running in the
///             VM. This isolate can also be the root isolate for an
///             unrelated engine. The engine parameter is necessary only to
///             ensure the call is not made when no engine (and hence no VM) is
///             running.
///
///             Unlike the platform messages mechanism, there are no threading
///             restrictions when using this API. Message can be posted on any
///             thread and they will be made available to isolate on which the
///             corresponding send port is listening.
///
///             However, it is the embedders responsibility to ensure that the
///             call is not made during an ongoing call the
///             `FlutterEngineDeinitialize` or `FlutterEngineShutdown` on
///             another thread.
///
/// @param[in]  engine     A running engine instance.
/// @param[in]  port       The send port to send the object to.
/// @param[in]  object     The object to send to the isolate with the
///                        corresponding receive port.
///
/// @return     If the message was posted to the send port.
///
FLUTTER_EXPORT
FlutterEngineResult FlutterEnginePostDartObject(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    FlutterEngineDartPort port,
    const FlutterEngineDartObject* object);

//------------------------------------------------------------------------------
/// @brief      Posts a low memory notification to a running engine instance.
///             The engine will do its best to release non-critical resources in
///             response. It is not guaranteed that the resource would have been
///             collected by the time this call returns however. The
///             notification is posted to engine subsystems that may be
///             operating on other threads.
///
///             Flutter applications can respond to these notifications by
///             setting `WidgetsBindingObserver.didHaveMemoryPressure`
///             observers.
///
/// @param[in]  engine     A running engine instance.
///
/// @return     If the low memory notification was sent to the running engine
///             instance.
///
FLUTTER_EXPORT
FlutterEngineResult FlutterEngineNotifyLowMemoryWarning(
    FLUTTER_API_SYMBOL(FlutterEngine) engine);

//------------------------------------------------------------------------------
/// @brief      Schedule a callback to be run on all engine managed threads.
///             The engine will attempt to service this callback the next time
///             the message loop for each managed thread is idle. Since the
///             engine manages the entire lifecycle of multiple threads, there
///             is no opportunity for the embedders to finely tune the
///             priorities of threads directly, or, perform other thread
///             specific configuration (for example, setting thread names for
///             tracing). This callback gives embedders a chance to affect such
///             tuning.
///
/// @attention  This call is expensive and must be made as few times as
///             possible. The callback must also return immediately as not doing
///             so may risk performance issues (especially for callbacks of type
///             kFlutterNativeThreadTypeUI and kFlutterNativeThreadTypeRender).
///
/// @attention  Some callbacks (especially the ones of type
///             kFlutterNativeThreadTypeWorker) may be called after the
///             FlutterEngine instance has shut down. Embedders must be careful
///             in handling the lifecycle of objects associated with the user
///             data baton.
///
/// @attention  In case there are multiple running Flutter engine instances,
///             their workers are shared.
///
/// @param[in]  engine     A running engine instance.
/// @param[in]  callback   The callback that will get called multiple times on
///                        each engine managed thread.
/// @param[in]  user_data  A baton passed by the engine to the callback. This
///                        baton is not interpreted by the engine in any way.
///
/// @return     Returns if the callback was successfully posted to all threads.
///
FLUTTER_EXPORT
FlutterEngineResult FlutterEnginePostCallbackOnAllNativeThreads(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    FlutterNativeThreadCallback callback,
    void* user_data);

#endif  // !FLUTTER_ENGINE_NO_PROTOTYPES

// Typedefs for the function pointers in FlutterEngineProcTable.
typedef FlutterEngineResult (*FlutterEngineCreateAOTDataFnPtr)(
    const FlutterEngineAOTDataSource* source,
    FlutterEngineAOTData* data_out);
typedef FlutterEngineResult (*FlutterEngineCollectAOTDataFnPtr)(
    FlutterEngineAOTData data);
typedef FlutterEngineResult (*FlutterEngineRunFnPtr)(
    size_t version,
    const FlutterProjectArgs* args,
    void* user_data,
    FLUTTER_API_SYMBOL(FlutterEngine) * engine_out);
typedef FlutterEngineResult (*FlutterEngineShutdownFnPtr)(
    FLUTTER_API_SYMBOL(FlutterEngine) engine);
typedef FlutterEngineResult (*FlutterEngineInitializeFnPtr)(
    size_t version,
    const FlutterProjectArgs* args,
    void* user_data,
    FLUTTER_API_SYMBOL(FlutterEngine) * engine_out);
typedef FlutterEngineResult (*FlutterEngineDeinitializeFnPtr)(
    FLUTTER_API_SYMBOL(FlutterEngine) engine);
typedef FlutterEngineResult (*FlutterEngineRunInitializedFnPtr)(
    FLUTTER_API_SYMBOL(FlutterEngine) engine);
typedef FlutterEngineResult (*FlutterEngineSendPlatformMessageFnPtr)(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    const FlutterPlatformMessage* message);
typedef FlutterEngineResult (
    *FlutterEnginePlatformMessageCreateResponseHandleFnPtr)(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    FlutterDataCallback data_callback,
    void* user_data,
    FlutterPlatformMessageResponseHandle** response_out);
typedef FlutterEngineResult (
    *FlutterEnginePlatformMessageReleaseResponseHandleFnPtr)(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    FlutterPlatformMessageResponseHandle* response);
typedef FlutterEngineResult (*FlutterEngineSendPlatformMessageResponseFnPtr)(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    const FlutterPlatformMessageResponseHandle* handle,
    const uint8_t* data,
    size_t data_length);
typedef void (*FlutterEngineTraceEventDurationBeginFnPtr)(const char* name);
typedef void (*FlutterEngineTraceEventDurationEndFnPtr)(const char* name);
typedef void (*FlutterEngineTraceEventInstantFnPtr)(const char* name);
typedef uint64_t (*FlutterEngineGetCurrentTimeFnPtr)();
typedef FlutterEngineResult (*FlutterEngineRunTaskFnPtr)(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    const FlutterTask* task);
typedef FlutterEngineResult (*FlutterEngineUpdateLocalesFnPtr)(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    const FlutterLocale** locales,
    size_t locales_count);
typedef bool (*FlutterEngineRunsAOTCompiledDartCodeFnPtr)(void);
typedef FlutterEngineResult (*FlutterEnginePostDartObjectFnPtr)(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    FlutterEngineDartPort port,
    const FlutterEngineDartObject* object);
typedef FlutterEngineResult (*FlutterEngineNotifyLowMemoryWarningFnPtr)(
    FLUTTER_API_SYMBOL(FlutterEngine) engine);
typedef FlutterEngineResult (*FlutterEnginePostCallbackOnAllNativeThreadsFnPtr)(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    FlutterNativeThreadCallback callback,
    void* user_data);

/// Function-pointer-based versions of the APIs above.
typedef struct {
  /// The size of this struct. Must be sizeof(FlutterEngineProcs).
  size_t struct_size;

  FlutterEngineCreateAOTDataFnPtr CreateAOTData;
  FlutterEngineCollectAOTDataFnPtr CollectAOTData;
  FlutterEngineRunFnPtr Run;
  FlutterEngineShutdownFnPtr Shutdown;
  FlutterEngineInitializeFnPtr Initialize;
  FlutterEngineDeinitializeFnPtr Deinitialize;
  FlutterEngineRunInitializedFnPtr RunInitialized;
  FlutterEngineSendPlatformMessageFnPtr SendPlatformMessage;
  FlutterEnginePlatformMessageCreateResponseHandleFnPtr
      PlatformMessageCreateResponseHandle;
  FlutterEnginePlatformMessageReleaseResponseHandleFnPtr
      PlatformMessageReleaseResponseHandle;
  FlutterEngineSendPlatformMessageResponseFnPtr SendPlatformMessageResponse;
  FlutterEngineTraceEventDurationBeginFnPtr TraceEventDurationBegin;
  FlutterEngineTraceEventDurationEndFnPtr TraceEventDurationEnd;
  FlutterEngineTraceEventInstantFnPtr TraceEventInstant;
  FlutterEngineGetCurrentTimeFnPtr GetCurrentTime;
  FlutterEngineRunTaskFnPtr RunTask;
  FlutterEngineUpdateLocalesFnPtr UpdateLocales;
  FlutterEngineRunsAOTCompiledDartCodeFnPtr RunsAOTCompiledDartCode;
  FlutterEnginePostDartObjectFnPtr PostDartObject;
  FlutterEngineNotifyLowMemoryWarningFnPtr NotifyLowMemoryWarning;
  FlutterEnginePostCallbackOnAllNativeThreadsFnPtr
      PostCallbackOnAllNativeThreads;
} FlutterEngineProcTable;

//------------------------------------------------------------------------------
/// @brief      Gets the table of engine function pointers.
///
/// @param[out] table   The table to fill with pointers. This should be
///                     zero-initialized, except for struct_size.
///
/// @return     Returns whether the table was successfully populated.
///
FLUTTER_EXPORT
FlutterEngineResult FlutterEngineGetProcAddresses(
    FlutterEngineProcTable* table);

// NOLINTEND(google-objc-function-naming)

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_H_
