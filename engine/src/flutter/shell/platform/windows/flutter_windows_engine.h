// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_WINDOWS_FLUTTER_WINDOWS_ENGINE_H_
#define FLUTTER_SHELL_PLATFORM_WINDOWS_FLUTTER_WINDOWS_ENGINE_H_

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "flutter/fml/closure.h"
#include "flutter/fml/macros.h"
#include "flutter/shell/platform/common/client_wrapper/binary_messenger_impl.h"
#include "flutter/shell/platform/common/incoming_message_dispatcher.h"
#include "flutter/shell/platform/embedder/embedder.h"
#include "flutter/shell/platform/windows/flutter_desktop_messenger.h"
#include "flutter/shell/platform/windows/flutter_project_bundle.h"
#include "flutter/shell/platform/windows/task_runner.h"
#include "flutter/shell/platform/windows/windows_proc_table.h"

namespace flutter {

class DisplayManagerWin32;

// Update the thread priority for the Windows engine.
static void WindowsPlatformThreadPrioritySetter(
    FlutterThreadPriority priority) {
  // TODO(99502): Add support for tracing to the windows embedding so we can
  // mark thread priorities and success/failure.
  switch (priority) {
    case FlutterThreadPriority::kBackground: {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
      break;
    }
    case FlutterThreadPriority::kDisplay: {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
      break;
    }
    case FlutterThreadPriority::kNormal: {
      // For normal or default priority we do not need to set the priority
      // class.
      break;
    }
  }
}

// Manages state associated with the underlying FlutterEngine that isn't
// related to its display.
//
// In most cases this will be associated with a FlutterView, but if not will
// run in headless mode.
class FlutterWindowsEngine {
 public:
  // Creates a new Flutter engine object configured to run |project|.
  explicit FlutterWindowsEngine(
      const FlutterProjectBundle& project,
      std::shared_ptr<WindowsProcTable> windows_proc_table = nullptr);

  virtual ~FlutterWindowsEngine();

  // Returns the engine associated with the given identifier.
  // The engine_id must be valid and for a running engine, otherwise
  // the behavior is undefined.
  // Must be called on the platform thread.
  static FlutterWindowsEngine* GetEngineForId(int64_t engine_id);

  // Starts running the entrypoint function specifed in the project bundle. If
  // unspecified, defaults to main().
  //
  // Returns false if the engine couldn't be started.
  bool Run();

  // Starts running the engine with the given entrypoint. If the empty string
  // is specified, defaults to the entrypoint function specified in the project
  // bundle, or main() if both are unspecified.
  //
  // Returns false if the engine couldn't be started or if conflicting,
  // non-default values are passed here and in the project bundle..
  //
  // DEPRECATED: Prefer setting the entrypoint in the FlutterProjectBundle
  // passed to the constructor and calling the no-parameter overload.
  bool Run(std::string_view entrypoint);

  // Returns true if the engine is currently running.
  virtual bool running() const { return engine_ != nullptr; }

  // Stops the engine. This invalidates the pointer returned by engine().
  //
  // Returns false if stopping the engine fails, or if it was not running.
  virtual bool Stop();

  // Returns the currently configured Plugin Registrar.
  FlutterDesktopPluginRegistrarRef GetRegistrar();

  // Registers |callback| to be called when the plugin registrar is destroyed.
  void AddPluginRegistrarDestructionCallback(
      FlutterDesktopOnPluginRegistrarDestroyed callback,
      FlutterDesktopPluginRegistrarRef registrar);

  // Sets switches member to the given switches.
  void SetSwitches(const std::vector<std::string>& switches);

  FlutterDesktopMessengerRef messenger() { return messenger_->ToRef(); }

  IncomingMessageDispatcher* message_dispatcher() {
    return message_dispatcher_.get();
  }

  TaskRunner* task_runner() { return task_runner_.get(); }

  BinaryMessenger* messenger_wrapper() { return messenger_wrapper_.get(); }

  // Sends the given message to the engine, calling |reply| with |user_data|
  // when a response is received from the engine if they are non-null.
  bool SendPlatformMessage(const char* channel,
                           const uint8_t* message,
                           const size_t message_size,
                           const FlutterDesktopBinaryReply reply,
                           void* user_data);

  // Sends the given data as the response to an earlier platform message.
  void SendPlatformMessageResponse(
      const FlutterDesktopMessageResponseHandle* handle,
      const uint8_t* data,
      size_t data_length);

  // Callback passed to Flutter engine for notifying window of platform
  // messages.
  void HandlePlatformMessage(const FlutterPlatformMessage*);

  // Register a root isolate create callback.
  //
  // The root isolate create callback is invoked at creation of the root Dart
  // isolate in the app. This may be used to be notified that execution of the
  // main Dart entrypoint is about to begin, and is used by test infrastructure
  // to register a native function resolver that can register and resolve
  // functions marked as native in the Dart code.
  //
  // This must be called before calling |Run|.
  void SetRootIsolateCreateCallback(const fml::closure& callback) {
    root_isolate_create_callback_ = callback;
  }

  // Returns the executable name for this process or "Flutter" if unknown.
  std::string GetExecutableName() const;

  std::shared_ptr<WindowsProcTable> windows_proc_table() {
    return windows_proc_table_;
  }

 protected:
  // Invoked by the engine right before the engine is restarted.
  //
  // This should reset necessary states to as if the engine has just been
  // created. This is typically caused by a hot restart (Shift-R in CLI.)
  void OnPreEngineRestart();

 private:
  // Allows swapping out embedder_api_ calls in tests.
  friend class EngineModifier;

  // Sends system locales to the engine.
  //
  // Should be called just after the engine is run, and after any relevant
  // system changes.
  void SendSystemLocales();

  // The handle to the embedder.h engine instance.
  FLUTTER_API_SYMBOL(FlutterEngine) engine_ = nullptr;

  FlutterEngineProcTable embedder_api_ = {};

  std::unique_ptr<FlutterProjectBundle> project_;

  // AOT data, if any.
  UniqueAotDataPtr aot_data_;

  // Task runner for tasks posted from the engine.
  std::unique_ptr<TaskRunner> task_runner_;

  // The plugin messenger handle given to API clients.
  fml::RefPtr<flutter::FlutterDesktopMessenger> messenger_;

  // A wrapper around messenger_ for interacting with client_wrapper-level APIs.
  std::unique_ptr<BinaryMessengerImpl> messenger_wrapper_;

  // Message dispatch manager for messages from engine_.
  std::unique_ptr<IncomingMessageDispatcher> message_dispatcher_;

  // The plugin registrar handle given to API clients.
  std::unique_ptr<FlutterDesktopPluginRegistrar> plugin_registrar_;

  // Callbacks to be called when the engine (and thus the plugin registrar) is
  // being destroyed.
  std::map<FlutterDesktopOnPluginRegistrarDestroyed,
           FlutterDesktopPluginRegistrarRef>
      plugin_registrar_destruction_callbacks_;

  // The root isolate creation callback.
  fml::closure root_isolate_create_callback_;

  std::shared_ptr<WindowsProcTable> windows_proc_table_;

  FML_DISALLOW_COPY_AND_ASSIGN(FlutterWindowsEngine);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_WINDOWS_FLUTTER_WINDOWS_ENGINE_H_
