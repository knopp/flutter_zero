// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/runtime/runtime_controller.h"

#include <utility>

#include "flutter/common/settings.h"
#include "flutter/fml/trace_event.h"
#include "flutter/lib/ui/ui_dart_state.h"
#include "flutter/lib/ui/window/platform_configuration.h"
#include "flutter/runtime/dart_isolate_group_data.h"
#include "flutter/runtime/isolate_configuration.h"
#include "flutter/runtime/runtime_delegate.h"
#include "third_party/tonic/dart_message_handler.h"

namespace flutter {

RuntimeController::RuntimeController(RuntimeDelegate& p_client,
                                     const TaskRunners& task_runners)
    : client_(p_client), vm_(nullptr), context_(task_runners) {}

RuntimeController::RuntimeController(
    RuntimeDelegate& p_client,
    DartVM* p_vm,
    fml::RefPtr<const DartSnapshot> p_isolate_snapshot,
    const std::function<void(int64_t)>& p_idle_notification_callback,
    const PlatformData& p_platform_data,
    const fml::closure& p_isolate_create_callback,
    const fml::closure& p_isolate_shutdown_callback,
    std::shared_ptr<const fml::Mapping> p_persistent_isolate_data,
    const UIDartState::Context& p_context)
    : client_(p_client),
      vm_(p_vm),
      isolate_snapshot_(std::move(p_isolate_snapshot)),
      idle_notification_callback_(p_idle_notification_callback),
      platform_data_(p_platform_data),
      isolate_create_callback_(p_isolate_create_callback),
      isolate_shutdown_callback_(p_isolate_shutdown_callback),
      persistent_isolate_data_(std::move(p_persistent_isolate_data)),
      context_(p_context) {}

std::unique_ptr<RuntimeController> RuntimeController::Spawn(
    RuntimeDelegate& p_client,
    const std::string& advisory_script_uri,
    const std::string& advisory_script_entrypoint,
    const std::function<void(int64_t)>& p_idle_notification_callback,
    const fml::closure& p_isolate_create_callback,
    const fml::closure& p_isolate_shutdown_callback,
    const std::shared_ptr<const fml::Mapping>& p_persistent_isolate_data)
    const {
  UIDartState::Context spawned_context{
      context_.task_runners,
      advisory_script_uri,
      advisory_script_entrypoint,
      context_.concurrent_task_runner,
  };
  auto result =
      std::make_unique<RuntimeController>(p_client,                      //
                                          vm_,                           //
                                          isolate_snapshot_,             //
                                          p_idle_notification_callback,  //
                                          platform_data_,                //
                                          p_isolate_create_callback,     //
                                          p_isolate_shutdown_callback,   //
                                          p_persistent_isolate_data,     //
                                          spawned_context);              //
  result->spawning_isolate_ = root_isolate_;
  return result;
}

RuntimeController::~RuntimeController() {
  FML_DCHECK(Dart_CurrentIsolate() == nullptr);
  std::shared_ptr<DartIsolate> root_isolate = root_isolate_.lock();
  if (root_isolate) {
    root_isolate->SetReturnCodeCallback(nullptr);
    auto result = root_isolate->Shutdown();
    if (!result) {
      FML_DLOG(ERROR) << "Could not shutdown the root isolate.";
    }
    root_isolate_ = {};
  }
}

bool RuntimeController::IsRootIsolateRunning() const {
  std::shared_ptr<DartIsolate> root_isolate = root_isolate_.lock();
  if (root_isolate) {
    return root_isolate->GetPhase() == DartIsolate::Phase::Running;
  }
  return false;
}

std::unique_ptr<RuntimeController> RuntimeController::Clone() const {
  return std::make_unique<RuntimeController>(client_,                      //
                                             vm_,                          //
                                             isolate_snapshot_,            //
                                             idle_notification_callback_,  //
                                             platform_data_,               //
                                             isolate_create_callback_,     //
                                             isolate_shutdown_callback_,   //
                                             persistent_isolate_data_,     //
                                             context_                      //
  );
}

bool RuntimeController::FlushRuntimeStateToIsolate() {
  FML_DCHECK(!has_flushed_runtime_state_)
      << "FlushRuntimeStateToIsolate is called more than once somehow.";
  has_flushed_runtime_state_ = true;

  auto platform_configuration = GetPlatformConfigurationIfAvailable();
  if (!platform_configuration) {
    return false;
  }

  FML_DCHECK(pending_add_view_callbacks_.empty());
  return SetLocales(platform_data_.locale_data);
}

bool RuntimeController::SetLocales(
    const std::vector<std::string>& locale_data) {
  platform_data_.locale_data = locale_data;

  if (auto* platform_configuration = GetPlatformConfigurationIfAvailable()) {
    platform_configuration->UpdateLocales(locale_data);
    return true;
  }

  return false;
}

bool RuntimeController::NotifyIdle(fml::TimeDelta deadline) {
  if (deadline - fml::TimeDelta::FromMicroseconds(Dart_TimelineGetMicros()) <
      fml::TimeDelta::FromMilliseconds(1)) {
    // There's less than 1ms left before the deadline. Upstream callers do not
    // check to see if the deadline is in the past, and work after this point
    // will be in vain.
    return false;
  }

  std::shared_ptr<DartIsolate> root_isolate = root_isolate_.lock();
  if (!root_isolate) {
    return false;
  }

  tonic::DartState::Scope scope(root_isolate);

  Dart_PerformanceMode performance_mode =
      PlatformConfigurationNativeApi::GetDartPerformanceMode();
  if (performance_mode == Dart_PerformanceMode::Dart_PerformanceMode_Latency) {
    return false;
  }

  Dart_NotifyIdle(deadline.ToMicroseconds());

  // Idle notifications being in isolate scope are part of the contract.
  if (idle_notification_callback_) {
    TRACE_EVENT0("flutter", "EmbedderIdleNotification");
    idle_notification_callback_(deadline.ToMicroseconds());
  }
  return true;
}

bool RuntimeController::DispatchPlatformMessage(
    std::unique_ptr<PlatformMessage> message) {
  if (auto* platform_configuration = GetPlatformConfigurationIfAvailable()) {
    TRACE_EVENT0("flutter", "RuntimeController::DispatchPlatformMessage");
    platform_configuration->DispatchPlatformMessage(std::move(message));
    return true;
  }

  return false;
}

PlatformConfiguration*
RuntimeController::GetPlatformConfigurationIfAvailable() {
  std::shared_ptr<DartIsolate> root_isolate = root_isolate_.lock();
  return root_isolate ? root_isolate->platform_configuration() : nullptr;
}

// |PlatformConfigurationClient|
void RuntimeController::HandlePlatformMessage(
    std::unique_ptr<PlatformMessage> message) {
  client_.HandlePlatformMessage(std::move(message));
}

// |PlatfromConfigurationClient|
std::shared_ptr<AssetManager> RuntimeController::GetAssetManager() {
  return client_.GetAssetManager();
}

// |PlatformConfigurationClient|
void RuntimeController::UpdateIsolateDescription(const std::string isolate_name,
                                                 int64_t isolate_port) {
  client_.UpdateIsolateDescription(isolate_name, isolate_port);
}

// |PlatformConfigurationClient|
std::shared_ptr<const fml::Mapping>
RuntimeController::GetPersistentIsolateData() {
  return persistent_isolate_data_;
}

// |PlatformConfigurationClient|
std::unique_ptr<std::vector<std::string>>
RuntimeController::ComputePlatformResolvedLocale(
    const std::vector<std::string>& supported_locale_data) {
  return client_.ComputePlatformResolvedLocale(supported_locale_data);
}

// |PlatformConfigurationClient|
void RuntimeController::SendChannelUpdate(std::string name, bool listening) {
  client_.SendChannelUpdate(std::move(name), listening);
}

Dart_Port RuntimeController::GetMainPort() {
  std::shared_ptr<DartIsolate> root_isolate = root_isolate_.lock();
  return root_isolate ? root_isolate->main_port() : ILLEGAL_PORT;
}

std::string RuntimeController::GetIsolateName() {
  std::shared_ptr<DartIsolate> root_isolate = root_isolate_.lock();
  return root_isolate ? root_isolate->debug_name() : "";
}

bool RuntimeController::HasLivePorts() {
  std::shared_ptr<DartIsolate> root_isolate = root_isolate_.lock();
  if (!root_isolate) {
    return false;
  }
  tonic::DartState::Scope scope(root_isolate);
  return Dart_HasLivePorts();
}

bool RuntimeController::HasPendingMicrotasks() {
  std::shared_ptr<DartIsolate> root_isolate = root_isolate_.lock();
  if (!root_isolate) {
    return false;
  }
  return root_isolate->HasPendingMicrotasks();
}

tonic::DartErrorHandleType RuntimeController::GetLastError() {
  std::shared_ptr<DartIsolate> root_isolate = root_isolate_.lock();
  return root_isolate ? root_isolate->GetLastError() : tonic::kNoError;
}

bool RuntimeController::LaunchRootIsolate(
    const Settings& settings,
    const fml::closure& root_isolate_create_callback,
    std::optional<std::string> dart_entrypoint,
    std::optional<std::string> dart_entrypoint_library,
    const std::vector<std::string>& dart_entrypoint_args,
    std::unique_ptr<IsolateConfiguration> isolate_configuration,
    std::shared_ptr<NativeAssetsManager> native_assets_manager,
    std::optional<int64_t> engine_id) {
  if (root_isolate_.lock()) {
    FML_LOG(ERROR) << "Root isolate was already running.";
    return false;
  }

  auto strong_root_isolate =
      DartIsolate::CreateRunningRootIsolate(
          settings,                                       //
          isolate_snapshot_,                              //
          std::make_unique<PlatformConfiguration>(this),  //
          DartIsolate::Flags{},                           //
          root_isolate_create_callback,                   //
          isolate_create_callback_,                       //
          isolate_shutdown_callback_,                     //
          std::move(dart_entrypoint),                     //
          std::move(dart_entrypoint_library),             //
          dart_entrypoint_args,                           //
          std::move(isolate_configuration),               //
          context_,                                       //
          spawning_isolate_.lock().get(),
          std::move(native_assets_manager))  //
          .lock();

  if (!strong_root_isolate) {
    FML_LOG(ERROR) << "Could not create root isolate.";
    return false;
  }

  // Enable platform channels for background isolates.
  strong_root_isolate->GetIsolateGroupData().SetPlatformMessageHandler(
      strong_root_isolate->GetRootIsolateToken(),
      client_.GetPlatformMessageHandler());

  // The root isolate ivar is weak.
  root_isolate_ = strong_root_isolate;

  // Capture by `this` here is safe because the callback is made by the dart
  // state itself. The isolate (and its Dart state) is owned by this object and
  // it will be collected before this object.
  strong_root_isolate->SetReturnCodeCallback(
      [this](uint32_t code) { root_isolate_return_code_ = code; });

  if (auto* platform_configuration = GetPlatformConfigurationIfAvailable()) {
    tonic::DartState::Scope scope(strong_root_isolate);
    platform_configuration->DidCreateIsolate();
    if (!FlushRuntimeStateToIsolate()) {
      FML_DLOG(ERROR) << "Could not set up initial isolate state.";
    }
    if (engine_id) {
      if (!platform_configuration->SetEngineId(*engine_id)) {
        FML_DLOG(ERROR) << "Could not set engine identifier.";
      }
    }
  } else {
    FML_DCHECK(false) << "RuntimeController created without window binding.";
  }

  FML_DCHECK(Dart_CurrentIsolate() == nullptr);

  client_.OnRootIsolateCreated();

  return true;
}

std::optional<std::string> RuntimeController::GetRootIsolateServiceID() const {
  if (auto isolate = root_isolate_.lock()) {
    return isolate->GetServiceId();
  }
  return std::nullopt;
}

std::optional<uint32_t> RuntimeController::GetRootIsolateReturnCode() {
  return root_isolate_return_code_;
}

uint64_t RuntimeController::GetRootIsolateGroup() const {
  auto isolate = root_isolate_.lock();
  if (isolate) {
    auto isolate_scope = tonic::DartIsolateScope(isolate->isolate());
    Dart_IsolateGroup isolate_group = Dart_CurrentIsolateGroup();
    return reinterpret_cast<uint64_t>(isolate_group);
  } else {
    return 0;
  }
}

void RuntimeController::LoadDartDeferredLibrary(
    intptr_t loading_unit_id,
    std::unique_ptr<const fml::Mapping> snapshot_data,
    std::unique_ptr<const fml::Mapping> snapshot_instructions) {
  root_isolate_.lock()->LoadLoadingUnit(loading_unit_id,
                                        std::move(snapshot_data),
                                        std::move(snapshot_instructions));
}

void RuntimeController::LoadDartDeferredLibraryError(
    intptr_t loading_unit_id,
    const std::string
        error_message,  // NOLINT(performance-unnecessary-value-param)
    bool transient) {
  root_isolate_.lock()->LoadLoadingUnitError(loading_unit_id, error_message,
                                             transient);
}

void RuntimeController::RequestDartDeferredLibrary(intptr_t loading_unit_id) {
  return client_.RequestDartDeferredLibrary(loading_unit_id);
}

void RuntimeController::ShutdownPlatformIsolates() {
  platform_isolate_manager_->ShutdownPlatformIsolates();
}

void RuntimeController::SetRootIsolateOwnerToCurrentThread() {
  std::shared_ptr<DartIsolate> root_isolate = root_isolate_.lock();
  if (root_isolate) {
    root_isolate->SetOwnerToCurrentThread();
  }
}

RuntimeController::Locale::Locale(std::string language_code_,
                                  std::string country_code_,
                                  std::string script_code_,
                                  std::string variant_code_)
    : language_code(std::move(language_code_)),
      country_code(std::move(country_code_)),
      script_code(std::move(script_code_)),
      variant_code(std::move(variant_code_)) {}

RuntimeController::Locale::~Locale() = default;

}  // namespace flutter
