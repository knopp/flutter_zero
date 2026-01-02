// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/window/platform_configuration.h"

#include <cstring>

#include "flutter/lib/ui/ui_dart_state.h"
#include "flutter/lib/ui/window/platform_message.h"
#include "flutter/lib/ui/window/platform_message_response_dart.h"
#include "flutter/lib/ui/window/platform_message_response_dart_port.h"
#include "third_party/tonic/converter/dart_converter.h"
#include "third_party/tonic/logging/dart_invoke.h"
#include "third_party/tonic/typed_data/dart_byte_data.h"

namespace flutter {
namespace {

Dart_Handle ToByteData(const fml::Mapping& buffer) {
  return tonic::DartByteData::Create(buffer.GetMapping(), buffer.GetSize());
}

}  // namespace

PlatformConfigurationClient::~PlatformConfigurationClient() {}

PlatformConfiguration::PlatformConfiguration(
    PlatformConfigurationClient* client)
    : client_(client) {}

PlatformConfiguration::~PlatformConfiguration() {}

void PlatformConfiguration::DidCreateIsolate() {
  Dart_Handle library = Dart_LookupLibrary(tonic::ToDart("dart:ui"));

  on_error_.Set(tonic::DartState::Current(),
                Dart_GetField(library, tonic::ToDart("_onError")));
  set_engine_id_.Set(tonic::DartState::Current(),
                     Dart_GetField(library, tonic::ToDart("_setEngineId")));
  update_locales_.Set(tonic::DartState::Current(),
                      Dart_GetField(library, tonic::ToDart("_updateLocales")));
  dispatch_platform_message_.Set(
      tonic::DartState::Current(),
      Dart_GetField(library, tonic::ToDart("_dispatchPlatformMessage")));
}

bool PlatformConfiguration::SetEngineId(int64_t engine_id) {
  std::shared_ptr<tonic::DartState> dart_state =
      set_engine_id_.dart_state().lock();
  if (!dart_state) {
    return false;
  }
  tonic::DartState::Scope scope(dart_state);
  tonic::CheckAndHandleError(
      tonic::DartInvoke(set_engine_id_.Get(), {
                                                  tonic::ToDart(engine_id),
                                              }));
  return true;
}

void PlatformConfiguration::UpdateLocales(
    const std::vector<std::string>& locales) {
  std::shared_ptr<tonic::DartState> dart_state =
      update_locales_.dart_state().lock();
  if (!dart_state) {
    return;
  }

  tonic::DartState::Scope scope(dart_state);
  tonic::CheckAndHandleError(
      tonic::DartInvoke(update_locales_.Get(),
                        {
                            tonic::ToDart<std::vector<std::string>>(locales),
                        }));
}

void PlatformConfiguration::DispatchPlatformMessage(
    std::unique_ptr<PlatformMessage> message) {
  std::shared_ptr<tonic::DartState> dart_state =
      dispatch_platform_message_.dart_state().lock();
  if (!dart_state) {
    FML_DLOG(WARNING)
        << "Dropping platform message for lack of DartState on channel: "
        << message->channel();
    return;
  }
  tonic::DartState::Scope scope(dart_state);
  Dart_Handle data_handle =
      (message->hasData()) ? ToByteData(message->data()) : Dart_Null();
  if (Dart_IsError(data_handle)) {
    FML_DLOG(WARNING)
        << "Dropping platform message because of a Dart error on channel: "
        << message->channel();
    return;
  }

  int response_id = 0;
  if (auto response = message->response()) {
    response_id = next_response_id_++;
    pending_responses_[response_id] = response;
  }

  tonic::CheckAndHandleError(
      tonic::DartInvoke(dispatch_platform_message_.Get(),
                        {tonic::ToDart(message->channel()), data_handle,
                         tonic::ToDart(response_id)}));
}

void PlatformConfiguration::CompletePlatformMessageEmptyResponse(
    int response_id) {
  if (!response_id) {
    return;
  }
  auto it = pending_responses_.find(response_id);
  if (it == pending_responses_.end()) {
    return;
  }
  auto response = std::move(it->second);
  pending_responses_.erase(it);
  response->CompleteEmpty();
}

void PlatformConfiguration::CompletePlatformMessageResponse(
    int response_id,
    std::vector<uint8_t> data) {
  if (!response_id) {
    return;
  }
  auto it = pending_responses_.find(response_id);
  if (it == pending_responses_.end()) {
    return;
  }
  auto response = std::move(it->second);
  pending_responses_.erase(it);
  response->Complete(std::make_unique<fml::DataMapping>(std::move(data)));
}

namespace {
Dart_Handle HandlePlatformMessage(
    UIDartState* dart_state,
    const std::string& name,
    Dart_Handle data_handle,
    const fml::RefPtr<PlatformMessageResponse>& response) {
  if (Dart_IsNull(data_handle)) {
    return dart_state->HandlePlatformMessage(
        std::make_unique<PlatformMessage>(name, response));
  } else {
    tonic::DartByteData data(data_handle);
    const uint8_t* buffer = static_cast<const uint8_t*>(data.data());
    return dart_state->HandlePlatformMessage(std::make_unique<PlatformMessage>(
        name, fml::MallocMapping::Copy(buffer, data.length_in_bytes()),
        response));
  }
}
}  // namespace

Dart_Handle PlatformConfigurationNativeApi::SendPlatformMessage(
    const std::string& name,
    Dart_Handle callback,
    Dart_Handle data_handle) {
  UIDartState* dart_state = UIDartState::Current();

  if (!dart_state->platform_configuration()) {
    return tonic::ToDart(
        "SendPlatformMessage only works on the root isolate, see "
        "SendPortPlatformMessage.");
  }

  fml::RefPtr<PlatformMessageResponse> response;
  if (!Dart_IsNull(callback)) {
    response = fml::MakeRefCounted<PlatformMessageResponseDart>(
        tonic::DartPersistentValue(dart_state, callback),
        dart_state->GetTaskRunners().GetUITaskRunner(), name);
  }

  return HandlePlatformMessage(dart_state, name, data_handle, response);
}

Dart_Handle PlatformConfigurationNativeApi::SendPortPlatformMessage(
    const std::string& name,
    Dart_Handle identifier,
    Dart_Handle send_port,
    Dart_Handle data_handle) {
  // This can be executed on any isolate.
  UIDartState* dart_state = UIDartState::Current();

  int64_t c_send_port = tonic::DartConverter<int64_t>::FromDart(send_port);
  if (c_send_port == ILLEGAL_PORT) {
    return tonic::ToDart("Invalid port specified");
  }

  fml::RefPtr<PlatformMessageResponse> response =
      fml::MakeRefCounted<PlatformMessageResponseDartPort>(
          c_send_port, tonic::DartConverter<int64_t>::FromDart(identifier),
          name);

  return HandlePlatformMessage(dart_state, name, data_handle, response);
}

void PlatformConfigurationNativeApi::RespondToPlatformMessage(
    int response_id,
    const tonic::DartByteData& data) {
  if (Dart_IsNull(data.dart_handle())) {
    UIDartState::Current()
        ->platform_configuration()
        ->CompletePlatformMessageEmptyResponse(response_id);
  } else {
    // TODO(engine): Avoid this copy.
    const uint8_t* buffer = static_cast<const uint8_t*>(data.data());
    UIDartState::Current()
        ->platform_configuration()
        ->CompletePlatformMessageResponse(
            response_id,
            std::vector<uint8_t>(buffer, buffer + data.length_in_bytes()));
  }
}

void PlatformConfigurationNativeApi::SetIsolateDebugName(
    const std::string& name) {
  UIDartState::ThrowIfUIOperationsProhibited();
  UIDartState::Current()->SetDebugName(name);
}

Dart_PerformanceMode PlatformConfigurationNativeApi::current_performance_mode_ =
    Dart_PerformanceMode_Default;

Dart_PerformanceMode PlatformConfigurationNativeApi::GetDartPerformanceMode() {
  return current_performance_mode_;
}

int PlatformConfigurationNativeApi::RequestDartPerformanceMode(int mode) {
  UIDartState::ThrowIfUIOperationsProhibited();
  current_performance_mode_ = static_cast<Dart_PerformanceMode>(mode);
  return Dart_SetPerformanceMode(current_performance_mode_);
}

Dart_Handle PlatformConfigurationNativeApi::GetPersistentIsolateData() {
  UIDartState::ThrowIfUIOperationsProhibited();

  auto persistent_isolate_data = UIDartState::Current()
                                     ->platform_configuration()
                                     ->client()
                                     ->GetPersistentIsolateData();

  if (!persistent_isolate_data) {
    return Dart_Null();
  }

  return tonic::DartByteData::Create(persistent_isolate_data->GetMapping(),
                                     persistent_isolate_data->GetSize());
}

Dart_Handle PlatformConfigurationNativeApi::ComputePlatformResolvedLocale(
    Dart_Handle supportedLocalesHandle) {
  UIDartState::ThrowIfUIOperationsProhibited();
  std::vector<std::string> supportedLocales =
      tonic::DartConverter<std::vector<std::string>>::FromDart(
          supportedLocalesHandle);

  std::vector<std::string> results =
      *UIDartState::Current()
           ->platform_configuration()
           ->client()
           ->ComputePlatformResolvedLocale(supportedLocales);

  return tonic::DartConverter<std::vector<std::string>>::ToDart(results);
}

int64_t PlatformConfigurationNativeApi::GetRootIsolateToken() {
  UIDartState* dart_state = UIDartState::Current();
  FML_DCHECK(dart_state);
  return dart_state->GetRootIsolateToken();
}

void PlatformConfigurationNativeApi::RegisterBackgroundIsolate(
    int64_t root_isolate_token) {
  UIDartState* dart_state = UIDartState::Current();
  FML_DCHECK(dart_state && !dart_state->IsRootIsolate());
  auto platform_message_handler =
      (*static_cast<std::shared_ptr<PlatformMessageHandlerStorage>*>(
          Dart_CurrentIsolateGroupData()));
  FML_DCHECK(platform_message_handler);
  auto weak_platform_message_handler =
      platform_message_handler->GetPlatformMessageHandler(root_isolate_token);
  dart_state->SetPlatformMessageHandler(weak_platform_message_handler);
}

void PlatformConfigurationNativeApi::SendChannelUpdate(const std::string& name,
                                                       bool listening) {
  UIDartState::Current()->platform_configuration()->client()->SendChannelUpdate(
      name, listening);
}

}  // namespace flutter
