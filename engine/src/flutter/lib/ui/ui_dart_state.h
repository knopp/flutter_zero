// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_LIB_UI_UI_DART_STATE_H_
#define FLUTTER_LIB_UI_UI_DART_STATE_H_

#include <memory>
#include <string>

#include "flutter/common/settings.h"
#include "flutter/common/task_runners.h"
#include "flutter/fml/concurrent_message_loop.h"
#include "flutter/lib/ui/isolate_name_server/isolate_name_server.h"
#include "flutter/shell/common/platform_message_handler.h"
#include "third_party/dart/runtime/include/dart_api.h"
#include "third_party/tonic/dart_microtask_queue.h"
#include "third_party/tonic/dart_persistent_value.h"
#include "third_party/tonic/dart_state.h"

namespace flutter {
class FontSelector;
class ImageGeneratorRegistry;
class PlatformConfiguration;
class PlatformMessage;

class UIDartState : public tonic::DartState {
 public:
  static UIDartState* Current();

  /// @brief  The subset of state which is owned by the shell or engine
  ///         and passed through the RuntimeController into DartIsolates.
  ///         If a shell-owned resource needs to be exposed to the framework via
  ///         UIDartState, a pointer to the resource can be added to this
  ///         struct with appropriate default construction.
  struct Context {
    explicit Context(const TaskRunners& task_runners);

    Context(const TaskRunners& task_runners,
            std::string advisory_script_uri,
            std::string advisory_script_entrypoint,
            std::shared_ptr<fml::ConcurrentTaskRunner> concurrent_task_runner);

    /// The task runners used by the shell hosting this runtime controller. This
    /// may be used by the isolate to scheduled asynchronous texture uploads or
    /// post tasks to the platform task runner.
    const TaskRunners task_runners;

    /// The advisory script URI (only used for debugging). This does not affect
    /// the code being run in the isolate in any way.
    std::string advisory_script_uri;

    /// The advisory script entrypoint (only used for debugging). This does not
    /// affect the code being run in the isolate in any way. The isolate must be
    /// transitioned to the running state explicitly by the caller.
    std::string advisory_script_entrypoint;

    /// The task runner whose tasks may be executed concurrently on a pool
    /// of shared worker threads.
    std::shared_ptr<fml::ConcurrentTaskRunner> concurrent_task_runner;
  };

  Dart_Port main_port() const { return main_port_; }
  // Root isolate of the VM application
  bool IsRootIsolate() const { return is_root_isolate_; }
  static void ThrowIfUIOperationsProhibited();

  void SetDebugName(const std::string& name);

  const std::string& debug_name() const { return debug_name_; }

  const std::string& logger_prefix() const { return logger_prefix_; }

  PlatformConfiguration* platform_configuration() const {
    return platform_configuration_.get();
  }

  void SetPlatformMessageHandler(std::weak_ptr<PlatformMessageHandler> handler);

  Dart_Handle HandlePlatformMessage(std::unique_ptr<PlatformMessage> message);

  const TaskRunners& GetTaskRunners() const;

  void ScheduleMicrotask(Dart_Handle handle);

  void FlushMicrotasksNow();

  bool HasPendingMicrotasks();

  std::shared_ptr<fml::ConcurrentTaskRunner> GetConcurrentTaskRunner() const;

  std::shared_ptr<IsolateNameServer> GetIsolateNameServer() const;

  tonic::DartErrorHandleType GetLastError();

  // Logs `print` messages from the application via an embedder-specified
  // logging mechanism.
  //
  // @param[in]  tag      A component name or tag that identifies the logging
  //                      application.
  // @param[in]  message  The message to be logged.
  void LogMessage(const std::string& tag, const std::string& message) const;

  UnhandledExceptionCallback unhandled_exception_callback() const {
    return unhandled_exception_callback_;
  }

  /// Returns a enumeration that uniquely represents this root isolate.
  /// Returns `0` if called from a non-root isolate.
  int64_t GetRootIsolateToken() const;

 protected:
  UIDartState(TaskObserverAdd add_callback,
              TaskObserverRemove remove_callback,
              std::string logger_prefix,
              UnhandledExceptionCallback unhandled_exception_callback,
              LogMessageCallback log_message_callback,
              std::shared_ptr<IsolateNameServer> isolate_name_server,
              bool is_root_isolate_,
              const UIDartState::Context& context);

  ~UIDartState() override;

  void SetPlatformConfiguration(
      std::unique_ptr<PlatformConfiguration> platform_configuration);

  const std::string& GetAdvisoryScriptURI() const;

 private:
  void DidSetIsolate() override;

  const TaskObserverAdd add_callback_;
  const TaskObserverRemove remove_callback_;
  std::optional<fml::TaskQueueId> callback_queue_id_;
  const std::string logger_prefix_;
  Dart_Port main_port_ = ILLEGAL_PORT;
  const bool is_root_isolate_;
  std::string debug_name_;
  std::unique_ptr<PlatformConfiguration> platform_configuration_;
  std::weak_ptr<PlatformMessageHandler> platform_message_handler_;
  tonic::DartMicrotaskQueue microtask_queue_;
  UnhandledExceptionCallback unhandled_exception_callback_;
  LogMessageCallback log_message_callback_;
  const std::shared_ptr<IsolateNameServer> isolate_name_server_;
  UIDartState::Context context_;

  void AddOrRemoveTaskObserver(bool add);
};

}  // namespace flutter

#endif  // FLUTTER_LIB_UI_UI_DART_STATE_H_
