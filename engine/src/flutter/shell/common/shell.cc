// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <future>
#define RAPIDJSON_HAS_STDSTRING 1
#include "flutter/shell/common/shell.h"

#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include "flutter/assets/directory_asset_bundle.h"
#include "flutter/fml/file.h"
#include "flutter/fml/icu_util.h"
#include "flutter/fml/log_settings.h"
#include "flutter/fml/logging.h"
#include "flutter/fml/make_copyable.h"
#include "flutter/fml/message_loop.h"
#include "flutter/fml/paths.h"
#include "flutter/fml/synchronization/waitable_event.h"
#include "flutter/fml/task_runner.h"
#include "flutter/fml/trace_event.h"
#include "flutter/runtime/dart_vm.h"
#include "flutter/shell/common/engine.h"
#include "flutter/shell/common/switches.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "third_party/tonic/common/log.h"

namespace flutter {

constexpr char kSystemChannel[] = "flutter/system";
constexpr char kTypeKey[] = "type";
constexpr char kFontChange[] = "fontsChange";

namespace {

std::unique_ptr<Engine> CreateEngine(
    Engine::Delegate& delegate,
    DartVM& vm,
    const fml::RefPtr<const DartSnapshot>& isolate_snapshot,
    const TaskRunners& task_runners,
    const PlatformData& platform_data,
    const Settings& settings) {
  return std::make_unique<Engine>(delegate,          //
                                  vm,                //
                                  isolate_snapshot,  //
                                  task_runners,      //
                                  platform_data,     //
                                  settings);
}

// Though there can be multiple shells, some settings apply to all components in
// the process. These have to be set up before the shell or any of its
// sub-components can be initialized. In a perfect world, this would be empty.
// TODO(chinmaygarde): The unfortunate side effect of this call is that settings
// that cause shell initialization failures will still lead to some of their
// settings being applied.
void PerformInitializationTasks(Settings& settings) {
  {
    fml::LogSettings log_settings;
    log_settings.min_log_level =
        settings.verbose_logging ? fml::kLogInfo : fml::kLogError;
    fml::SetLogSettings(log_settings);
  }

  static std::once_flag gShellSettingsInitialization = {};
  std::call_once(gShellSettingsInitialization, [&settings] {
    tonic::SetLogHandler(
        [](const char* message) { FML_LOG(ERROR) << message; });

    if (!settings.trace_allowlist.empty()) {
      fml::tracing::TraceSetAllowlist(settings.trace_allowlist);
    }

    if (settings.icu_initialization_required) {
      if (!settings.icu_data_path.empty()) {
        fml::icu::InitializeICU(settings.icu_data_path);
      } else if (settings.icu_mapper) {
        fml::icu::InitializeICUFromMapping(settings.icu_mapper());
      } else {
        FML_DLOG(WARNING) << "Skipping ICU initialization in the shell.";
      }
    }
  });
}

}  // namespace

std::pair<DartVMRef, fml::RefPtr<const DartSnapshot>>
Shell::InferVmInitDataFromSettings(Settings& settings) {
  // Always use the `vm_snapshot` and `isolate_snapshot` provided by the
  // settings to launch the VM.  If the VM is already running, the snapshot
  // arguments are ignored.
  auto vm_snapshot = DartSnapshot::VMSnapshotFromSettings(settings);
  auto isolate_snapshot = DartSnapshot::IsolateSnapshotFromSettings(settings);
  auto vm = DartVMRef::Create(settings, vm_snapshot, isolate_snapshot);

  // If the settings did not specify an `isolate_snapshot`, fall back to the
  // one the VM was launched with.
  if (!isolate_snapshot) {
    isolate_snapshot = vm->GetVMData()->GetIsolateSnapshot();
  }
  return {std::move(vm), isolate_snapshot};
}

std::unique_ptr<Shell> Shell::Create(
    const PlatformData& platform_data,
    const TaskRunners& task_runners,
    Settings settings,
    const Shell::CreateCallback<PlatformView>& on_create_platform_view) {
  // This must come first as it initializes tracing.
  PerformInitializationTasks(settings);

  TRACE_EVENT0("flutter", "Shell::Create");

  auto [vm, isolate_snapshot] = InferVmInitDataFromSettings(settings);

  return CreateWithSnapshot(platform_data,                //
                            task_runners,                 //
                            settings,                     //
                            std::move(vm),                //
                            std::move(isolate_snapshot),  //
                            on_create_platform_view,      //
                            CreateEngine);
}

std::unique_ptr<Shell> Shell::CreateShellOnPlatformThread(
    DartVMRef vm,
    const TaskRunners& task_runners,
    const PlatformData& platform_data,
    const Settings& settings,
    fml::RefPtr<const DartSnapshot> isolate_snapshot,
    const Shell::CreateCallback<PlatformView>& on_create_platform_view,
    const Shell::EngineCreateCallback& on_create_engine) {
  if (!task_runners.IsValid()) {
    FML_LOG(ERROR) << "Task runners to run the shell were invalid.";
    return nullptr;
  }

  auto shell =
      std::unique_ptr<Shell>(new Shell(std::move(vm), task_runners, settings));

  // Create the platform view on the platform thread (this thread).
  auto platform_view = on_create_platform_view(*shell.get());
  if (!platform_view || !platform_view->GetWeakPtr()) {
    return nullptr;
  }

  // Create the engine on the UI thread.
  std::promise<std::unique_ptr<Engine>> engine_promise;
  auto engine_future = engine_promise.get_future();
  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetUITaskRunner(),
      fml::MakeCopyable([&engine_promise,                                 //
                         shell = shell.get(),                             //
                         &platform_data,                                  //
                         isolate_snapshot = std::move(isolate_snapshot),  //
                         &on_create_engine]() mutable {
        TRACE_EVENT0("flutter", "ShellSetupUISubsystem");
        const auto& task_runners = shell->GetTaskRunners();

        engine_promise.set_value(
            on_create_engine(*shell,                       //
                             *shell->GetDartVM(),          //
                             std::move(isolate_snapshot),  //
                             task_runners,                 //
                             platform_data,                //
                             shell->GetSettings()));
      }));

  if (!shell->Setup(std::move(platform_view),  //
                    engine_future.get())       //
  ) {
    return nullptr;
  }

  return shell;
}

std::unique_ptr<Shell> Shell::CreateWithSnapshot(
    const PlatformData& platform_data,
    const TaskRunners& task_runners,
    Settings settings,
    DartVMRef vm,
    fml::RefPtr<const DartSnapshot> isolate_snapshot,
    const Shell::CreateCallback<PlatformView>& on_create_platform_view,
    const Shell::EngineCreateCallback& on_create_engine) {
  // This must come first as it initializes tracing.
  PerformInitializationTasks(settings);

  TRACE_EVENT0("flutter", "Shell::CreateWithSnapshot");

  const bool callbacks_valid = on_create_platform_view && on_create_engine;
  if (!task_runners.IsValid() || !callbacks_valid) {
    return nullptr;
  }

  fml::AutoResetWaitableEvent latch;
  std::unique_ptr<Shell> shell;
  auto platform_task_runner = task_runners.GetPlatformTaskRunner();
  fml::TaskRunner::RunNowOrPostTask(
      platform_task_runner,
      fml::MakeCopyable([&latch,                                             //
                         &shell,                                             //
                         task_runners = task_runners,                        //
                         platform_data = platform_data,                      //
                         settings = settings,                                //
                         vm = std::move(vm),                                 //
                         isolate_snapshot = std::move(isolate_snapshot),     //
                         on_create_platform_view = on_create_platform_view,  //
                         on_create_engine = on_create_engine]() mutable {
        shell = CreateShellOnPlatformThread(std::move(vm),                //
                                            task_runners,                 //
                                            platform_data,                //
                                            settings,                     //
                                            std::move(isolate_snapshot),  //
                                            on_create_platform_view,      //
                                            on_create_engine);
        latch.Signal();
      }));
  latch.Wait();
  return shell;
}

Shell::Shell(DartVMRef vm,
             const TaskRunners& task_runners,
             const Settings& settings)
    : task_runners_(task_runners),
      settings_(settings),
      vm_(std::move(vm)),
      weak_factory_(this) {
  FML_CHECK(vm_) << "Must have access to VM to create a shell.";
  FML_DCHECK(task_runners_.IsValid());
  FML_DCHECK(task_runners_.GetPlatformTaskRunner()->RunsTasksOnCurrentThread());

  // Install service protocol handlers.

  service_protocol_handlers_[ServiceProtocol::kScreenshotExtensionName] = {
      task_runners_.GetUITaskRunner(),
      std::bind(&Shell::OnServiceProtocolScreenshot, this,
                std::placeholders::_1, std::placeholders::_2)};
  service_protocol_handlers_[ServiceProtocol::kScreenshotSkpExtensionName] = {
      task_runners_.GetUITaskRunner(),
      std::bind(&Shell::OnServiceProtocolScreenshotSKP, this,
                std::placeholders::_1, std::placeholders::_2)};
  service_protocol_handlers_[ServiceProtocol::kRunInViewExtensionName] = {
      task_runners_.GetUITaskRunner(),
      std::bind(&Shell::OnServiceProtocolRunInView, this, std::placeholders::_1,
                std::placeholders::_2)};
  service_protocol_handlers_
      [ServiceProtocol::kFlushUIThreadTasksExtensionName] = {
          task_runners_.GetUITaskRunner(),
          std::bind(&Shell::OnServiceProtocolFlushUIThreadTasks, this,
                    std::placeholders::_1, std::placeholders::_2)};
  service_protocol_handlers_
      [ServiceProtocol::kSetAssetBundlePathExtensionName] = {
          task_runners_.GetUITaskRunner(),
          std::bind(&Shell::OnServiceProtocolSetAssetBundlePath, this,
                    std::placeholders::_1, std::placeholders::_2)};
  service_protocol_handlers_
      [ServiceProtocol::kGetDisplayRefreshRateExtensionName] = {
          task_runners_.GetUITaskRunner(),
          std::bind(&Shell::OnServiceProtocolGetDisplayRefreshRate, this,
                    std::placeholders::_1, std::placeholders::_2)};
  service_protocol_handlers_[ServiceProtocol::kGetSkSLsExtensionName] = {
      task_runners_.GetUITaskRunner(),
      std::bind(&Shell::OnServiceProtocolGetSkSLs, this, std::placeholders::_1,
                std::placeholders::_2)};
  service_protocol_handlers_
      [ServiceProtocol::kEstimateRasterCacheMemoryExtensionName] = {
          task_runners_.GetUITaskRunner(),
          std::bind(&Shell::OnServiceProtocolEstimateRasterCacheMemory, this,
                    std::placeholders::_1, std::placeholders::_2)};
  service_protocol_handlers_[ServiceProtocol::kReloadAssetFonts] = {
      task_runners_.GetPlatformTaskRunner(),
      std::bind(&Shell::OnServiceProtocolReloadAssetFonts, this,
                std::placeholders::_1, std::placeholders::_2)};
  service_protocol_handlers_[ServiceProtocol::kGetPipelineUsageExtensionName] =
      {task_runners_.GetUITaskRunner(),
       std::bind(&Shell::OnServiceProtocolGetPipelineUsage, this,
                 std::placeholders::_1, std::placeholders::_2)};
}

Shell::~Shell() {
  vm_->GetServiceProtocol()->RemoveHandler(this);

  fml::AutoResetWaitableEvent platiso_latch, ui_latch, platform_latch;

  fml::TaskRunner::RunNowOrPostTask(
      task_runners_.GetPlatformTaskRunner(),
      fml::MakeCopyable([this, &platiso_latch]() mutable {
        engine_->ShutdownPlatformIsolates();
        platiso_latch.Signal();
      }));
  platiso_latch.Wait();

  fml::TaskRunner::RunNowOrPostTask(
      task_runners_.GetUITaskRunner(),
      fml::MakeCopyable([this, &ui_latch]() mutable {
        engine_.reset();
        ui_latch.Signal();
      }));
  ui_latch.Wait();

  // The platform view must go last because it may be holding onto platform side
  // counterparts to resources owned by subsystems running on other threads. For
  // example, the NSOpenGLContext on the Mac.
  fml::TaskRunner::RunNowOrPostTask(
      task_runners_.GetPlatformTaskRunner(),
      fml::MakeCopyable([platform_view = std::move(platform_view_),
                         &platform_latch]() mutable {
        platform_view.reset();
        platform_latch.Signal();
      }));
  platform_latch.Wait();

  if (settings_.merged_platform_ui_thread ==
      Settings::MergedPlatformUIThread::kMergeAfterLaunch) {
    // Move the UI task runner back to its original thread to enable shutdown of
    // that thread.
    auto task_queues = fml::MessageLoopTaskQueues::GetInstance();
    auto platform_queue_id =
        task_runners_.GetPlatformTaskRunner()->GetTaskQueueId();
    auto ui_queue_id = task_runners_.GetUITaskRunner()->GetTaskQueueId();
    if (task_queues->Owns(platform_queue_id, ui_queue_id)) {
      task_queues->Unmerge(platform_queue_id, ui_queue_id);
    }
  }
}

std::unique_ptr<Shell> Shell::Spawn(
    RunConfiguration run_configuration,
    const CreateCallback<PlatformView>& on_create_platform_view) const {
  FML_DCHECK(task_runners_.IsValid());

  if (settings_.merged_platform_ui_thread ==
      Settings::MergedPlatformUIThread::kMergeAfterLaunch) {
    // Spawning engines that share the same task runners can result in
    // deadlocks when the UI task runner is moved to the platform thread.
    FML_LOG(ERROR) << "MergedPlatformUIThread::kMergeAfterLaunch does not "
                      "support spawning";
    return nullptr;
  }

  std::unique_ptr<Shell> result = CreateWithSnapshot(
      PlatformData{}, task_runners_, GetSettings(), vm_,
      vm_->GetVMData()->GetIsolateSnapshot(), on_create_platform_view,
      [engine = this->engine_.get()](
          Engine::Delegate& delegate, DartVM& vm,
          const fml::RefPtr<const DartSnapshot>& isolate_snapshot,
          const TaskRunners& task_runners, const PlatformData& platform_data,
          const Settings& settings) {
        return engine->Spawn(
            /*delegate=*/delegate,
            /*settings=*/settings);
      });
  result->RunEngine(std::move(run_configuration));
  return result;
}

void Shell::NotifyLowMemoryWarning() const {
  auto trace_id = fml::tracing::TraceNonce();
  TRACE_EVENT_ASYNC_BEGIN0("flutter", "Shell::NotifyLowMemoryWarning",
                           trace_id);
  // This does not require a current isolate but does require a running VM.
  // Since a valid shell will not be returned to the embedder without a valid
  // DartVMRef, we can be certain that this is a safe spot to assume a VM is
  // running.
  ::Dart_NotifyLowMemory();

  // The IO Manager uses resource cache limits of 0, so it is not necessary
  // to purge them.
}

void Shell::FlushMicrotaskQueue() const {
  if (engine_) {
    engine_->FlushMicrotaskQueue();
  }
}

void Shell::RunEngine(RunConfiguration run_configuration) {
  RunEngine(std::move(run_configuration), nullptr);
}

void Shell::RunEngine(
    RunConfiguration run_configuration,
    const std::function<void(Engine::RunStatus)>& result_callback) {
  auto result = [platform_runner = task_runners_.GetPlatformTaskRunner(),
                 result_callback](Engine::RunStatus run_result) {
    if (!result_callback) {
      return;
    }
    platform_runner->PostTask(
        [result_callback, run_result]() { result_callback(run_result); });
  };
  FML_DCHECK(is_set_up_);
  FML_DCHECK(task_runners_.GetPlatformTaskRunner()->RunsTasksOnCurrentThread());

  fml::TaskRunner::RunNowOrPostTask(
      task_runners_.GetUITaskRunner(),
      fml::MakeCopyable(
          [run_configuration = std::move(run_configuration),
           weak_engine = weak_engine_, result]() mutable {
            if (!weak_engine) {
              FML_LOG(ERROR)
                  << "Could not launch engine with configuration - no engine.";
              result(Engine::RunStatus::Failure);
              return;
            }
            auto run_result = weak_engine->Run(std::move(run_configuration));
            if (run_result == flutter::Engine::RunStatus::Failure) {
              FML_LOG(ERROR) << "Could not launch engine with configuration.";
            }

            result(run_result);
          }));
}

std::optional<DartErrorCode> Shell::GetUIIsolateLastError() const {
  FML_DCHECK(is_set_up_);
  FML_DCHECK(task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread());

  if (!weak_engine_) {
    return std::nullopt;
  }
  switch (weak_engine_->GetUIIsolateLastError()) {
    case tonic::kCompilationErrorType:
      return DartErrorCode::CompilationError;
    case tonic::kApiErrorType:
      return DartErrorCode::ApiError;
    case tonic::kUnknownErrorType:
      return DartErrorCode::UnknownError;
    case tonic::kNoError:
      return DartErrorCode::NoError;
  }
  return DartErrorCode::UnknownError;
}

bool Shell::EngineHasLivePorts() const {
  FML_DCHECK(is_set_up_);
  FML_DCHECK(task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread());

  if (!weak_engine_) {
    return false;
  }

  return weak_engine_->UIIsolateHasLivePorts();
}

bool Shell::EngineHasPendingMicrotasks() const {
  FML_DCHECK(is_set_up_);
  FML_DCHECK(task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread());

  if (!weak_engine_) {
    return false;
  }

  return weak_engine_->UIIsolateHasPendingMicrotasks();
}

bool Shell::IsSetup() const {
  return is_set_up_;
}

bool Shell::Setup(std::unique_ptr<PlatformView> platform_view,
                  std::unique_ptr<Engine> engine) {
  if (is_set_up_) {
    return false;
  }

  if (!platform_view || !engine) {
    return false;
  }

  platform_view_ = std::move(platform_view);
  platform_message_handler_ = platform_view_->GetPlatformMessageHandler();
  route_messages_through_platform_thread_.store(true);
  task_runners_.GetPlatformTaskRunner()->PostTask(
      [self = weak_factory_.GetWeakPtr()] {
        if (self) {
          self->route_messages_through_platform_thread_.store(false);
        }
      });
  engine_ = std::move(engine);

  // Set the external view embedder for the rasterizer.
  // The weak ptr must be generated in the platform thread which owns the unique
  // ptr.
  weak_engine_ = engine_->GetWeakPtr();
  weak_platform_view_ = platform_view_->GetWeakPtr();

  is_set_up_ = true;

  return true;
}

const Settings& Shell::GetSettings() const {
  return settings_;
}

const TaskRunners& Shell::GetTaskRunners() const {
  return task_runners_;
}

fml::TaskRunnerAffineWeakPtr<Engine> Shell::GetEngine() {
  FML_DCHECK(is_set_up_);
  return weak_engine_;
}

fml::WeakPtr<PlatformView> Shell::GetPlatformView() {
  FML_DCHECK(is_set_up_);
  return weak_platform_view_;
}

DartVM* Shell::GetDartVM() {
  return &vm_;
}

// |PlatformView::Delegate|
void Shell::OnPlatformViewCreated() {
  TRACE_EVENT0("flutter", "Shell::OnPlatformViewCreated");
  FML_DCHECK(is_set_up_);
  FML_DCHECK(task_runners_.GetPlatformTaskRunner()->RunsTasksOnCurrentThread());
}

// |PlatformView::Delegate|
void Shell::OnPlatformViewDestroyed() {
  TRACE_EVENT0("flutter", "Shell::OnPlatformViewDestroyed");
  FML_DCHECK(is_set_up_);
  FML_DCHECK(task_runners_.GetPlatformTaskRunner()->RunsTasksOnCurrentThread());
}

// |PlatformView::Delegate|
void Shell::OnPlatformViewDispatchPlatformMessage(
    std::unique_ptr<PlatformMessage> message) {
  FML_DCHECK(is_set_up_);
#if FLUTTER_RUNTIME_MODE == FLUTTER_RUNTIME_MODE_DEBUG
  if (!task_runners_.GetPlatformTaskRunner()->RunsTasksOnCurrentThread()) {
    std::scoped_lock lock(misbehaving_message_channels_mutex_);
    auto inserted = misbehaving_message_channels_.insert(message->channel());
    if (inserted.second) {
      FML_LOG(ERROR)
          << "The '" << message->channel()
          << "' channel sent a message from native to Flutter on a "
             "non-platform thread. Platform channel messages must be sent on "
             "the platform thread. Failure to do so may result in data loss or "
             "crashes, and must be fixed in the plugin or application code "
             "creating that channel.\n"
             "See https://docs.flutter.dev/platform-integration/"
             "platform-channels#channels-and-platform-threading for more "
             "information.";
    }
  }
#endif  // FLUTTER_RUNTIME_MODE == FLUTTER_RUNTIME_MODE_DEBUG

  // The static leak checker gets confused by the use of fml::MakeCopyable.
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
  fml::TaskRunner::RunNowAndFlushMessages(
      task_runners_.GetUITaskRunner(),
      fml::MakeCopyable(
          [engine = weak_engine_, message = std::move(message)]() mutable {
            if (engine) {
              engine->DispatchPlatformMessage(std::move(message));
            }
          }));
}

// |PlatformView::Delegate|
const Settings& Shell::OnPlatformViewGetSettings() const {
  return settings_;
}

// |Engine::Delegate|
void Shell::OnEngineHandlePlatformMessage(
    std::unique_ptr<PlatformMessage> message) {
  FML_DCHECK(is_set_up_);
  FML_DCHECK(task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread());

  if (platform_message_handler_) {
    if (route_messages_through_platform_thread_ &&
        !platform_message_handler_
             ->DoesHandlePlatformMessageOnPlatformThread()) {
#if _WIN32
      // On Windows capturing a TaskRunner with a TaskRunner will cause an
      // uncaught exception in process shutdown because of the deletion order of
      // global variables. See also
      // https://github.com/flutter/flutter/issues/111575.
      // This won't be an issue until Windows supports background platform
      // channels (https://github.com/flutter/flutter/issues/93945). Then this
      // can potentially be addressed by capturing a weak_ptr to an object that
      // retains the ui TaskRunner, instead of the TaskRunner directly.
      FML_DCHECK(false);
#endif
      // We route messages through the platform thread temporarily when the
      // shell is being initialized to be backwards compatible with setting
      // message handlers in the same event as starting the isolate, but after
      // it is started.
      auto ui_task_runner = task_runners_.GetUITaskRunner();
      task_runners_.GetPlatformTaskRunner()->PostTask(fml::MakeCopyable(
          [weak_platform_message_handler =
               std::weak_ptr<PlatformMessageHandler>(platform_message_handler_),
           message = std::move(message), ui_task_runner]() mutable {
            ui_task_runner->PostTask(
                fml::MakeCopyable([weak_platform_message_handler,
                                   message = std::move(message)]() mutable {
                  auto platform_message_handler =
                      weak_platform_message_handler.lock();
                  if (platform_message_handler) {
                    platform_message_handler->HandlePlatformMessage(
                        std::move(message));
                  }
                }));
          }));
    } else {
      platform_message_handler_->HandlePlatformMessage(std::move(message));
    }
  } else {
    task_runners_.GetPlatformTaskRunner()->PostTask(
        fml::MakeCopyable([view = platform_view_->GetWeakPtr(),
                           message = std::move(message)]() mutable {
          if (view) {
            view->HandlePlatformMessage(std::move(message));
          }
        }));
  }
}

void Shell::OnEngineChannelUpdate(std::string name, bool listening) {
  FML_DCHECK(is_set_up_);

  task_runners_.GetPlatformTaskRunner()->PostTask(
      [view = platform_view_->GetWeakPtr(), name = std::move(name), listening] {
        if (view) {
          view->SendChannelUpdate(name, listening);
        }
      });
}

// |Engine::Delegate|
void Shell::OnPreEngineRestart() {
  FML_DCHECK(is_set_up_);
  FML_DCHECK(task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread());

  fml::AutoResetWaitableEvent latch;
  fml::TaskRunner::RunNowOrPostTask(
      task_runners_.GetPlatformTaskRunner(),
      [view = platform_view_->GetWeakPtr(), &latch]() {
        if (view) {
          view->OnPreEngineRestart();
        }
        latch.Signal();
      });
  // This is blocking as any embedded platform views has to be flushed before
  // we re-run the Dart code.
  latch.Wait();
}

// |Engine::Delegate|
void Shell::OnRootIsolateCreated() {
  if (is_added_to_service_protocol_) {
    return;
  }
  auto description = GetServiceProtocolDescription();
  fml::TaskRunner::RunNowOrPostTask(
      task_runners_.GetPlatformTaskRunner(),
      [self = weak_factory_.GetWeakPtr(),
       description = std::move(description)]() {
        if (self) {
          self->vm_->GetServiceProtocol()->AddHandler(self.get(), description);
        }
      });
  is_added_to_service_protocol_ = true;
}

// |Engine::Delegate|
void Shell::UpdateIsolateDescription(const std::string isolate_name,
                                     int64_t isolate_port) {
  Handler::Description description(isolate_port, isolate_name);
  vm_->GetServiceProtocol()->SetHandlerDescription(this, description);
}

// |Engine::Delegate|
std::unique_ptr<std::vector<std::string>> Shell::ComputePlatformResolvedLocale(
    const std::vector<std::string>& supported_locale_data) {
  return platform_view_->ComputePlatformResolvedLocales(supported_locale_data);
}

void Shell::LoadDartDeferredLibrary(
    intptr_t loading_unit_id,
    std::unique_ptr<const fml::Mapping> snapshot_data,
    std::unique_ptr<const fml::Mapping> snapshot_instructions) {
  task_runners_.GetUITaskRunner()->PostTask(fml::MakeCopyable(
      [engine = engine_->GetWeakPtr(), loading_unit_id,
       data = std::move(snapshot_data),
       instructions = std::move(snapshot_instructions)]() mutable {
        if (engine) {
          engine->LoadDartDeferredLibrary(loading_unit_id, std::move(data),
                                          std::move(instructions));
        }
      }));
}

void Shell::LoadDartDeferredLibraryError(intptr_t loading_unit_id,
                                         const std::string error_message,
                                         bool transient) {
  fml::TaskRunner::RunNowOrPostTask(
      task_runners_.GetUITaskRunner(),
      [engine = weak_engine_, loading_unit_id, error_message, transient] {
        if (engine) {
          engine->LoadDartDeferredLibraryError(loading_unit_id, error_message,
                                               transient);
        }
      });
}

void Shell::UpdateAssetResolverByType(
    std::unique_ptr<AssetResolver> updated_asset_resolver,
    AssetResolver::AssetResolverType type) {
  fml::TaskRunner::RunNowOrPostTask(
      task_runners_.GetUITaskRunner(),
      fml::MakeCopyable(
          [engine = weak_engine_, type,
           asset_resolver = std::move(updated_asset_resolver)]() mutable {
            if (engine) {
              engine->GetAssetManager()->UpdateResolverByType(
                  std::move(asset_resolver), type);
            }
          }));
}

// |Engine::Delegate|
void Shell::RequestDartDeferredLibrary(intptr_t loading_unit_id) {
  task_runners_.GetPlatformTaskRunner()->PostTask(
      [view = platform_view_->GetWeakPtr(), loading_unit_id] {
        if (view) {
          view->RequestDartDeferredLibrary(loading_unit_id);
        }
      });
}

// |ServiceProtocol::Handler|
fml::RefPtr<fml::TaskRunner> Shell::GetServiceProtocolHandlerTaskRunner(
    std::string_view method) const {
  FML_DCHECK(is_set_up_);
  auto found = service_protocol_handlers_.find(method);
  if (found != service_protocol_handlers_.end()) {
    return found->second.first;
  }
  return task_runners_.GetUITaskRunner();
}

// |ServiceProtocol::Handler|
bool Shell::HandleServiceProtocolMessage(
    std::string_view method,  // one if the extension names specified above.
    const ServiceProtocolMap& params,
    rapidjson::Document* response) {
  auto found = service_protocol_handlers_.find(method);
  if (found != service_protocol_handlers_.end()) {
    return found->second.second(params, response);
  }
  return false;
}

// |ServiceProtocol::Handler|
ServiceProtocol::Handler::Description Shell::GetServiceProtocolDescription()
    const {
  FML_DCHECK(task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread());

  if (!weak_engine_) {
    return ServiceProtocol::Handler::Description();
  }

  return {
      weak_engine_->GetUIIsolateMainPort(),
      weak_engine_->GetUIIsolateName(),
  };
}

static void ServiceProtocolParameterError(rapidjson::Document* response,
                                          std::string error_details) {
  auto& allocator = response->GetAllocator();
  response->SetObject();
  const int64_t kInvalidParams = -32602;
  response->AddMember("code", kInvalidParams, allocator);
  response->AddMember("message", "Invalid params", allocator);
  {
    rapidjson::Value details(rapidjson::kObjectType);
    details.AddMember("details", std::move(error_details), allocator);
    response->AddMember("data", details, allocator);
  }
}

static void ServiceProtocolFailureError(rapidjson::Document* response,
                                        std::string message) {
  auto& allocator = response->GetAllocator();
  response->SetObject();
  const int64_t kJsonServerError = -32000;
  response->AddMember("code", kJsonServerError, allocator);
  response->AddMember("message", std::move(message), allocator);
}

// Service protocol handler
bool Shell::OnServiceProtocolScreenshot(
    const ServiceProtocol::Handler::ServiceProtocolMap& params,
    rapidjson::Document* response) {
#if 0  // TODO(knopp)
  FML_DCHECK(task_runners_.GetRasterTaskRunner()->RunsTasksOnCurrentThread());
  auto screenshot = rasterizer_->ScreenshotLastLayerTree(
      Rasterizer::ScreenshotType::CompressedImage, true);
  if (screenshot.data) {
    response->SetObject();
    auto& allocator = response->GetAllocator();
    response->AddMember("type", "Screenshot", allocator);
    rapidjson::Value image;
    image.SetString(static_cast<const char*>(screenshot.data->data()),
                    screenshot.data->size(), allocator);
    response->AddMember("screenshot", image, allocator);
    return true;
  }
#endif
  ServiceProtocolFailureError(response, "Could not capture image screenshot.");
  return false;
}

// Service protocol handler
bool Shell::OnServiceProtocolScreenshotSKP(
    const ServiceProtocol::Handler::ServiceProtocolMap& params,
    rapidjson::Document* response) {
#if 0  // TODO(knopp)
  FML_DCHECK(task_runners_.GetRasterTaskRunner()->RunsTasksOnCurrentThread());
  if (settings_.enable_impeller) {
    ServiceProtocolFailureError(
        response, "Cannot capture SKP screenshot with Impeller enabled.");
    return false;
  }
  auto screenshot = rasterizer_->ScreenshotLastLayerTree(
      Rasterizer::ScreenshotType::SkiaPicture, true);
  if (screenshot.data) {
    response->SetObject();
    auto& allocator = response->GetAllocator();
    response->AddMember("type", "ScreenshotSkp", allocator);
    rapidjson::Value skp;
    skp.SetString(static_cast<const char*>(screenshot.data->data()),
                  screenshot.data->size(), allocator);
    response->AddMember("skp", skp, allocator);
    return true;
  }
#endif
  ServiceProtocolFailureError(response, "Could not capture SKP screenshot.");
  return false;
}

// Service protocol handler
bool Shell::OnServiceProtocolRunInView(
    const ServiceProtocol::Handler::ServiceProtocolMap& params,
    rapidjson::Document* response) {
  FML_DCHECK(task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread());

  if (params.count("mainScript") == 0) {
    ServiceProtocolParameterError(response,
                                  "'mainScript' parameter is missing.");
    return false;
  }

  if (params.count("assetDirectory") == 0) {
    ServiceProtocolParameterError(response,
                                  "'assetDirectory' parameter is missing.");
    return false;
  }

  std::string main_script_path =
      fml::paths::FromURI(params.at("mainScript").data());
  std::string asset_directory_path =
      fml::paths::FromURI(params.at("assetDirectory").data());

  auto main_script_file_mapping =
      std::make_unique<fml::FileMapping>(fml::OpenFile(
          main_script_path.c_str(), false, fml::FilePermission::kRead));

  auto isolate_configuration = IsolateConfiguration::CreateForKernel(
      std::move(main_script_file_mapping));

  RunConfiguration configuration(std::move(isolate_configuration));

  configuration.SetEntrypointAndLibrary(engine_->GetLastEntrypoint(),
                                        engine_->GetLastEntrypointLibrary());
  configuration.SetEntrypointArgs(engine_->GetLastEntrypointArgs());

  configuration.SetEngineId(engine_->GetLastEngineId());

  configuration.AddAssetResolver(std::make_unique<DirectoryAssetBundle>(
      fml::OpenDirectory(asset_directory_path.c_str(), false,
                         fml::FilePermission::kRead),
      false));

  // Preserve any original asset resolvers to avoid syncing unchanged assets
  // over the DevFS connection.
  auto old_asset_manager = engine_->GetAssetManager();
  if (old_asset_manager != nullptr) {
    for (auto& old_resolver : old_asset_manager->TakeResolvers()) {
      if (old_resolver->IsValidAfterAssetManagerChange()) {
        configuration.AddAssetResolver(std::move(old_resolver));
      }
    }
  }

  auto& allocator = response->GetAllocator();
  response->SetObject();
  if (engine_->Restart(std::move(configuration))) {
    response->AddMember("type", "Success", allocator);
    auto new_description = GetServiceProtocolDescription();
    rapidjson::Value view(rapidjson::kObjectType);
    new_description.Write(this, view, allocator);
    response->AddMember("view", view, allocator);
    return true;
  } else {
    FML_DLOG(ERROR) << "Could not run configuration in engine.";
    ServiceProtocolFailureError(response,
                                "Could not run configuration in engine.");
    return false;
  }

  FML_DCHECK(false);
  return false;
}

// Service protocol handler
bool Shell::OnServiceProtocolFlushUIThreadTasks(
    const ServiceProtocol::Handler::ServiceProtocolMap& params,
    rapidjson::Document* response) {
  FML_DCHECK(task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread());
  // This API should not be invoked by production code.
  // It can potentially starve the service isolate if the main isolate pauses
  // at a breakpoint or is in an infinite loop.
  //
  // It should be invoked from the VM Service and blocks it until UI thread
  // tasks are processed.
  response->SetObject();
  response->AddMember("type", "Success", response->GetAllocator());
  return true;
}

bool Shell::OnServiceProtocolGetDisplayRefreshRate(
    const ServiceProtocol::Handler::ServiceProtocolMap& params,
    rapidjson::Document* response) {
  FML_DCHECK(task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread());
  response->SetObject();
  response->AddMember("type", "DisplayRefreshRate", response->GetAllocator());
  // TODO(knopp)
  response->AddMember("fps", 0.0, response->GetAllocator());
  return true;
}

bool Shell::OnServiceProtocolGetSkSLs(
    const ServiceProtocol::Handler::ServiceProtocolMap& params,
    rapidjson::Document* response) {
  FML_DCHECK(task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread());
  response->SetObject();
  response->AddMember("type", "GetSkSLs", response->GetAllocator());

  rapidjson::Value shaders_json(rapidjson::kObjectType);
// TODO(knopp)
#if 0
#if !SLIMPELLER
  PersistentCache* persistent_cache = PersistentCache::GetCacheForProcess();
  std::vector<PersistentCache::SkSLCache> sksls = persistent_cache->LoadSkSLs();
  for (const auto& sksl : sksls) {
    size_t b64_size = Base64::EncodedSize(sksl.value->size());
    sk_sp<SkData> b64_data = SkData::MakeUninitialized(b64_size + 1);
    char* b64_char = static_cast<char*>(b64_data->writable_data());
    Base64::Encode(sksl.value->data(), sksl.value->size(), b64_char);
    b64_char[b64_size] = 0;  // make it null terminated for printing
    rapidjson::Value shader_value(b64_char, response->GetAllocator());
    std::string_view key_view(reinterpret_cast<const char*>(sksl.key->data()),
                              sksl.key->size());
    auto encode_result = fml::Base32Encode(key_view);
    if (!encode_result.first) {
      continue;
    }
    rapidjson::Value shader_key(encode_result.second, response->GetAllocator());
    shaders_json.AddMember(shader_key, shader_value, response->GetAllocator());
  }
#endif  //  !SLIMPELLER
#endif
  response->AddMember("SkSLs", shaders_json, response->GetAllocator());
  return true;
}

bool Shell::OnServiceProtocolEstimateRasterCacheMemory(
    const ServiceProtocol::Handler::ServiceProtocolMap& params,
    rapidjson::Document* response) {
  FML_DCHECK(task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread());

  uint64_t layer_cache_byte_size = 0u;
  uint64_t picture_cache_byte_size = 0u;

  // TODO(knopp)
#if 0
#if !SLIMPELLER
  const auto& raster_cache = rasterizer_->compositor_context()->raster_cache();
  layer_cache_byte_size = raster_cache.EstimateLayerCacheByteSize();
  picture_cache_byte_size = raster_cache.EstimatePictureCacheByteSize();
#endif  //  !SLIMPELLER
#endif

  response->SetObject();
  response->AddMember("type", "EstimateRasterCacheMemory",
                      response->GetAllocator());
  response->AddMember<uint64_t>("layerBytes", layer_cache_byte_size,
                                response->GetAllocator());
  response->AddMember<uint64_t>("pictureBytes", picture_cache_byte_size,
                                response->GetAllocator());
  return true;
}

// Service protocol handler
bool Shell::OnServiceProtocolSetAssetBundlePath(
    const ServiceProtocol::Handler::ServiceProtocolMap& params,
    rapidjson::Document* response) {
  FML_DCHECK(task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread());

  if (params.count("assetDirectory") == 0) {
    ServiceProtocolParameterError(response,
                                  "'assetDirectory' parameter is missing.");
    return false;
  }

  auto& allocator = response->GetAllocator();
  response->SetObject();

  auto asset_manager = std::make_shared<AssetManager>();

  if (!asset_manager->PushFront(std::make_unique<DirectoryAssetBundle>(
          fml::OpenDirectory(params.at("assetDirectory").data(), false,
                             fml::FilePermission::kRead),
          false))) {
    // The new asset directory path was invalid.
    FML_DLOG(ERROR) << "Could not update asset directory.";
    ServiceProtocolFailureError(response, "Could not update asset directory.");
    return false;
  }

  // Preserve any original asset resolvers to avoid syncing unchanged assets
  // over the DevFS connection.
  auto old_asset_manager = engine_->GetAssetManager();
  if (old_asset_manager != nullptr) {
    for (auto& old_resolver : old_asset_manager->TakeResolvers()) {
      if (old_resolver->IsValidAfterAssetManagerChange()) {
        asset_manager->PushBack(std::move(old_resolver));
      }
    }
  }

  if (engine_->UpdateAssetManager(asset_manager)) {
    response->AddMember("type", "Success", allocator);
    auto new_description = GetServiceProtocolDescription();
    rapidjson::Value view(rapidjson::kObjectType);
    new_description.Write(this, view, allocator);
    response->AddMember("view", view, allocator);
    return true;
  } else {
    FML_DLOG(ERROR) << "Could not update asset directory.";
    ServiceProtocolFailureError(response, "Could not update asset directory.");
    return false;
  }

  FML_DCHECK(false);
  return false;
}

bool Shell::OnServiceProtocolGetPipelineUsage(
    const ServiceProtocol::Handler::ServiceProtocolMap& params,
    rapidjson::Document* response) {
  FML_DCHECK(task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread());

  response->SetObject();

  rapidjson::Value pipelines_json(rapidjson::kObjectType);

// TODO(knopp)
#if 0

  for (const auto& pipelineCount : use_counts) {
    std::string_view pipeline_name = pipelineCount.first.GetLabel();
    rapidjson::Value pipeline_key(pipeline_name.data(), pipeline_name.length(),
                                  response->GetAllocator());

    pipelines_json.AddMember(pipeline_key, pipelineCount.second,
                             response->GetAllocator());
  }
#endif

  response->AddMember("Usages", pipelines_json, response->GetAllocator());
  return true;
}

void Shell::SendFontChangeNotification() {
  // After system fonts are reloaded, we send a system channel message
  // to notify flutter framework.
  rapidjson::Document document;
  document.SetObject();
  auto& allocator = document.GetAllocator();
  rapidjson::Value message_value;
  message_value.SetString(kFontChange, allocator);
  document.AddMember(kTypeKey, message_value, allocator);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  document.Accept(writer);
  std::string message = buffer.GetString();
  std::unique_ptr<PlatformMessage> fontsChangeMessage =
      std::make_unique<flutter::PlatformMessage>(
          kSystemChannel,
          fml::MallocMapping::Copy(message.c_str(), message.length()), nullptr);
  OnPlatformViewDispatchPlatformMessage(std::move(fontsChangeMessage));
}

bool Shell::OnServiceProtocolReloadAssetFonts(
    const ServiceProtocol::Handler::ServiceProtocolMap& params,
    rapidjson::Document* response) {
  FML_DCHECK(task_runners_.GetPlatformTaskRunner()->RunsTasksOnCurrentThread());
  if (!engine_) {
    return false;
  }
#if 0  // TODO(knopp)
  engine_->GetFontCollection().RegisterFonts(engine_->GetAssetManager());
  engine_->GetFontCollection().GetFontCollection()->ClearFontFamilyCache();
#endif
  SendFontChangeNotification();

  auto& allocator = response->GetAllocator();
  response->SetObject();
  response->AddMember("type", "Success", allocator);

  return true;
}

fml::TimePoint Shell::GetCurrentTimePoint() {
  return fml::TimePoint::Now();
}

const std::shared_ptr<PlatformMessageHandler>&
Shell::GetPlatformMessageHandler() const {
  return platform_message_handler_;
}

const std::shared_ptr<fml::ConcurrentTaskRunner>
Shell::GetConcurrentWorkerTaskRunner() const {
  FML_DCHECK(vm_);
  if (!vm_) {
    return nullptr;
  }
  return vm_->GetConcurrentWorkerTaskRunner();
}

}  // namespace flutter
