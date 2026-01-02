// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/engine.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "flutter/assets/native_assets.h"
#include "flutter/common/settings.h"
#include "flutter/fml/trace_event.h"
#include "rapidjson/document.h"

namespace flutter {

static constexpr char kAssetChannel[] = "flutter/assets";
static constexpr char kLocalizationChannel[] = "flutter/localization";
static constexpr char kIsolateChannel[] = "flutter/isolate";

namespace {
fml::MallocMapping MakeMapping(const std::string& str) {
  return fml::MallocMapping::Copy(str.c_str(), str.length());
}
}  // namespace

Engine::Engine(Delegate& delegate,
               const TaskRunners& task_runners,
               const Settings& settings,
               std::unique_ptr<RuntimeController> runtime_controller)
    : delegate_(delegate),
      settings_(settings),
      runtime_controller_(std::move(runtime_controller)),
      task_runners_(task_runners),
      weak_factory_(this) {}

Engine::Engine(Delegate& delegate,
               DartVM& vm,
               fml::RefPtr<const DartSnapshot> isolate_snapshot,
               const TaskRunners& task_runners,
               const PlatformData& platform_data,
               const Settings& settings)
    : Engine(delegate, task_runners, settings, nullptr) {
  runtime_controller_ = std::make_unique<RuntimeController>(
      *this,                                 // runtime delegate
      &vm,                                   // VM
      std::move(isolate_snapshot),           // isolate snapshot
      settings_.idle_notification_callback,  // idle notification callback
      platform_data,                         // platform data
      settings_.isolate_create_callback,     // isolate create callback
      settings_.isolate_shutdown_callback,   // isolate shutdown callback
      settings_.persistent_isolate_data,     // persistent isolate data
      UIDartState::Context{
          task_runners_,                         // task runners
          settings_.advisory_script_uri,         // advisory script uri
          settings_.advisory_script_entrypoint,  // advisory script entrypoint
          vm.GetConcurrentWorkerTaskRunner(),    // concurrent task runner
      });
}

std::unique_ptr<Engine> Engine::Spawn(Delegate& delegate,
                                      const Settings& settings) const {
  auto result = std::make_unique<Engine>(
      /*delegate=*/delegate,
      /*task_runners=*/task_runners_,
      /*settings=*/settings,
      /*runtime_controller=*/nullptr);
  result->runtime_controller_ = runtime_controller_->Spawn(
      /*p_client=*/*result,
      /*advisory_script_uri=*/settings.advisory_script_uri,
      /*advisory_script_entrypoint=*/settings.advisory_script_entrypoint,
      /*idle_notification_callback=*/settings.idle_notification_callback,
      /*isolate_create_callback=*/settings.isolate_create_callback,
      /*isolate_shutdown_callback=*/settings.isolate_shutdown_callback,
      /*persistent_isolate_data=*/settings.persistent_isolate_data);
  result->asset_manager_ = asset_manager_;
  return result;
}

Engine::~Engine() = default;

fml::TaskRunnerAffineWeakPtr<Engine> Engine::GetWeakPtr() const {
  return weak_factory_.GetWeakPtr();
}

std::shared_ptr<AssetManager> Engine::GetAssetManager() {
  return asset_manager_;
}

bool Engine::UpdateAssetManager(
    const std::shared_ptr<AssetManager>& new_asset_manager) {
  if (asset_manager_ && new_asset_manager &&
      *asset_manager_ == *new_asset_manager) {
    return false;
  }

  asset_manager_ = new_asset_manager;

  if (!asset_manager_) {
    return false;
  }

  if (native_assets_manager_ == nullptr) {
    native_assets_manager_ = std::make_shared<NativeAssetsManager>();
  }
  native_assets_manager_->RegisterNativeAssets(asset_manager_);

  return true;
}

bool Engine::Restart(RunConfiguration configuration) {
  TRACE_EVENT0("flutter", "Engine::Restart");
  if (!configuration.IsValid()) {
    FML_LOG(ERROR) << "Engine run configuration was invalid.";
    return false;
  }
  delegate_.OnPreEngineRestart();
  runtime_controller_ = runtime_controller_->Clone();
  UpdateAssetManager(nullptr);
  return Run(std::move(configuration)) == Engine::RunStatus::Success;
}

Engine::RunStatus Engine::Run(RunConfiguration configuration) {
  if (!configuration.IsValid()) {
    FML_LOG(ERROR) << "Engine run configuration was invalid.";
    return RunStatus::Failure;
  }

  last_entry_point_ = configuration.GetEntrypoint();
  last_entry_point_library_ = configuration.GetEntrypointLibrary();
#if (FLUTTER_RUNTIME_MODE == FLUTTER_RUNTIME_MODE_DEBUG)
  // This is only used to support restart.
  last_entry_point_args_ = configuration.GetEntrypointArgs();
#endif

  last_engine_id_ = configuration.GetEngineId();

  UpdateAssetManager(configuration.GetAssetManager());

  if (runtime_controller_->IsRootIsolateRunning()) {
    return RunStatus::FailureAlreadyRunning;
  }

  // If the embedding prefetched the default font manager, then set up the
  // font manager later in the engine launch process.  This makes it less
  // likely that the setup will need to wait for the prefetch to complete.
  auto root_isolate_create_callback = [&]() {};

  if (settings_.merged_platform_ui_thread ==
      Settings::MergedPlatformUIThread::kMergeAfterLaunch) {
    // Queue a task to the UI task runner that sets the owner of the root
    // isolate.  This task runs after the thread merge and will therefore be
    // executed on the platform thread.  The task will run before any tasks
    // queued by LaunchRootIsolate that execute the app's Dart code.
    task_runners_.GetUITaskRunner()->PostTask([engine = GetWeakPtr()]() {
      if (engine) {
        engine->runtime_controller_->SetRootIsolateOwnerToCurrentThread();
      }
    });
  }

  if (!runtime_controller_->LaunchRootIsolate(
          settings_,                                 //
          root_isolate_create_callback,              //
          configuration.GetEntrypoint(),             //
          configuration.GetEntrypointLibrary(),      //
          configuration.GetEntrypointArgs(),         //
          configuration.TakeIsolateConfiguration(),  //
          native_assets_manager_,                    //
          configuration.GetEngineId()))              //
  {
    return RunStatus::Failure;
  }

  auto service_id = runtime_controller_->GetRootIsolateServiceID();
  if (service_id.has_value()) {
    std::unique_ptr<PlatformMessage> service_id_message =
        std::make_unique<flutter::PlatformMessage>(
            kIsolateChannel, MakeMapping(service_id.value()), nullptr);
    HandlePlatformMessage(std::move(service_id_message));
  }

  if (settings_.merged_platform_ui_thread ==
      Settings::MergedPlatformUIThread::kMergeAfterLaunch) {
    // Move the UI task runner to the platform thread.
    bool success = fml::MessageLoopTaskQueues::GetInstance()->Merge(
        task_runners_.GetPlatformTaskRunner()->GetTaskQueueId(),
        task_runners_.GetUITaskRunner()->GetTaskQueueId());
    if (!success) {
      FML_LOG(ERROR)
          << "Unable to move the UI task runner to the platform thread";
    }
  }

  return Engine::RunStatus::Success;
}

void Engine::NotifyIdle(fml::TimeDelta deadline) {
  runtime_controller_->NotifyIdle(deadline);
}

std::optional<uint32_t> Engine::GetUIIsolateReturnCode() {
  return runtime_controller_->GetRootIsolateReturnCode();
}

Dart_Port Engine::GetUIIsolateMainPort() {
  return runtime_controller_->GetMainPort();
}

std::string Engine::GetUIIsolateName() {
  return runtime_controller_->GetIsolateName();
}

bool Engine::UIIsolateHasLivePorts() {
  return runtime_controller_->HasLivePorts();
}

bool Engine::UIIsolateHasPendingMicrotasks() {
  return runtime_controller_->HasPendingMicrotasks();
}

tonic::DartErrorHandleType Engine::GetUIIsolateLastError() {
  return runtime_controller_->GetLastError();
}

void Engine::DispatchPlatformMessage(std::unique_ptr<PlatformMessage> message) {
  std::string channel = message->channel();
  if (channel == kLocalizationChannel) {
    if (HandleLocalizationPlatformMessage(message.get())) {
      return;
    }
  }

  if (runtime_controller_->IsRootIsolateRunning() &&
      runtime_controller_->DispatchPlatformMessage(std::move(message))) {
    return;
  }

  FML_DLOG(WARNING) << "Dropping platform message on channel: " << channel;
}

bool Engine::HandleLocalizationPlatformMessage(PlatformMessage* message) {
  const auto& data = message->data();

  rapidjson::Document document;
  document.Parse(reinterpret_cast<const char*>(data.GetMapping()),
                 data.GetSize());
  if (document.HasParseError() || !document.IsObject()) {
    return false;
  }
  auto root = document.GetObject();
  auto method = root.FindMember("method");
  if (method == root.MemberEnd()) {
    return false;
  }
  const size_t strings_per_locale = 4;
  if (method->value == "setLocale") {
    // Decode and pass the list of locale data onwards to dart.
    auto args = root.FindMember("args");
    if (args == root.MemberEnd() || !args->value.IsArray()) {
      return false;
    }

    if (args->value.Size() % strings_per_locale != 0) {
      return false;
    }
    std::vector<std::string> locale_data;
    for (size_t locale_index = 0; locale_index < args->value.Size();
         locale_index += strings_per_locale) {
      if (!args->value[locale_index].IsString() ||
          !args->value[locale_index + 1].IsString()) {
        return false;
      }
      locale_data.push_back(args->value[locale_index].GetString());
      locale_data.push_back(args->value[locale_index + 1].GetString());
      locale_data.push_back(args->value[locale_index + 2].GetString());
      locale_data.push_back(args->value[locale_index + 3].GetString());
    }

    return runtime_controller_->SetLocales(locale_data);
  }
  return false;
}

void Engine::HandlePlatformMessage(std::unique_ptr<PlatformMessage> message) {
  if (message->channel() == kAssetChannel) {
    HandleAssetPlatformMessage(std::move(message));
  } else {
    delegate_.OnEngineHandlePlatformMessage(std::move(message));
  }
}

void Engine::OnRootIsolateCreated() {
  delegate_.OnRootIsolateCreated();
}

void Engine::UpdateIsolateDescription(const std::string isolate_name,
                                      int64_t isolate_port) {
  delegate_.UpdateIsolateDescription(isolate_name, isolate_port);
}

std::unique_ptr<std::vector<std::string>> Engine::ComputePlatformResolvedLocale(
    const std::vector<std::string>& supported_locale_data) {
  return delegate_.ComputePlatformResolvedLocale(supported_locale_data);
}

void Engine::HandleAssetPlatformMessage(
    std::unique_ptr<PlatformMessage> message) {
  fml::RefPtr<PlatformMessageResponse> response = message->response();
  if (!response) {
    return;
  }
  const auto& data = message->data();
  std::string asset_name(reinterpret_cast<const char*>(data.GetMapping()),
                         data.GetSize());

  if (asset_manager_) {
    std::unique_ptr<fml::Mapping> asset_mapping =
        asset_manager_->GetAsMapping(asset_name);
    if (asset_mapping) {
      response->Complete(std::move(asset_mapping));
      return;
    }
  }

  response->CompleteEmpty();
}

const std::string& Engine::GetLastEntrypoint() const {
  return last_entry_point_;
}

const std::string& Engine::GetLastEntrypointLibrary() const {
  return last_entry_point_library_;
}

const std::vector<std::string>& Engine::GetLastEntrypointArgs() const {
  return last_entry_point_args_;
}

std::optional<int64_t> Engine::GetLastEngineId() const {
  return last_engine_id_;
}

// |RuntimeDelegate|
void Engine::RequestDartDeferredLibrary(intptr_t loading_unit_id) {
  return delegate_.RequestDartDeferredLibrary(loading_unit_id);
}

std::weak_ptr<PlatformMessageHandler> Engine::GetPlatformMessageHandler()
    const {
  return delegate_.GetPlatformMessageHandler();
}

void Engine::SendChannelUpdate(std::string name, bool listening) {
  delegate_.OnEngineChannelUpdate(std::move(name), listening);
}

void Engine::LoadDartDeferredLibrary(
    intptr_t loading_unit_id,
    std::unique_ptr<const fml::Mapping> snapshot_data,
    std::unique_ptr<const fml::Mapping> snapshot_instructions) {
  if (runtime_controller_->IsRootIsolateRunning()) {
    runtime_controller_->LoadDartDeferredLibrary(
        loading_unit_id, std::move(snapshot_data),
        std::move(snapshot_instructions));
  } else {
    LoadDartDeferredLibraryError(loading_unit_id, "No running root isolate.",
                                 true);
  }
}

void Engine::LoadDartDeferredLibraryError(intptr_t loading_unit_id,
                                          const std::string& error_message,
                                          bool transient) {
  if (runtime_controller_->IsRootIsolateRunning()) {
    runtime_controller_->LoadDartDeferredLibraryError(loading_unit_id,
                                                      error_message, transient);
  }
}

void Engine::ShutdownPlatformIsolates() {
  runtime_controller_->ShutdownPlatformIsolates();
}

void Engine::FlushMicrotaskQueue() {
  runtime_controller_->FlushMicrotaskQueue();
}

}  // namespace flutter
