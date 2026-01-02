// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/tests/embedder_test_context.h"

#include <utility>

#include "flutter/fml/make_copyable.h"
#include "flutter/fml/paths.h"
#include "flutter/runtime/dart_vm.h"
#include "flutter/testing/testing.h"
#include "third_party/dart/runtime/bin/elf_loader.h"

namespace flutter {
namespace testing {

EmbedderTestContext::EmbedderTestContext(std::string assets_path)
    : assets_path_(std::move(assets_path)),
      aot_symbols_(
          LoadELFSymbolFromFixturesIfNeccessary(kDefaultAOTAppELFFileName)),
      native_resolver_(std::make_shared<TestDartNativeResolver>()) {
  SetupAOTMappingsIfNecessary();
  SetupAOTDataIfNecessary();
  isolate_create_callbacks_.push_back(
      [weak_resolver =
           std::weak_ptr<TestDartNativeResolver>{native_resolver_}]() {
        if (auto resolver = weak_resolver.lock()) {
          resolver->SetNativeResolverForIsolate();
        }
      });
}

EmbedderTestContext::~EmbedderTestContext() = default;

void EmbedderTestContext::SetupAOTMappingsIfNecessary() {
  if (!DartVM::IsRunningPrecompiledCode()) {
    return;
  }
  vm_snapshot_data_ =
      std::make_unique<fml::NonOwnedMapping>(aot_symbols_.vm_snapshot_data, 0u);
  vm_snapshot_instructions_ = std::make_unique<fml::NonOwnedMapping>(
      aot_symbols_.vm_snapshot_instrs, 0u);
  isolate_snapshot_data_ =
      std::make_unique<fml::NonOwnedMapping>(aot_symbols_.vm_isolate_data, 0u);
  isolate_snapshot_instructions_ = std::make_unique<fml::NonOwnedMapping>(
      aot_symbols_.vm_isolate_instrs, 0u);
}

void EmbedderTestContext::SetupAOTDataIfNecessary() {
  if (!DartVM::IsRunningPrecompiledCode()) {
    return;
  }
  FlutterEngineAOTDataSource data_in = {};
  FlutterEngineAOTData data_out = nullptr;

  const auto elf_path = fml::paths::JoinPaths(
      {GetFixturesPath(), testing::kDefaultAOTAppELFFileName});

  data_in.type = kFlutterEngineAOTDataSourceTypeElfPath;
  data_in.elf_path = elf_path.c_str();

  ASSERT_EQ(FlutterEngineCreateAOTData(&data_in, &data_out), kSuccess);

  aot_data_.reset(data_out);
}

const std::string& EmbedderTestContext::GetAssetsPath() const {
  return assets_path_;
}

const fml::Mapping* EmbedderTestContext::GetVMSnapshotData() const {
  return vm_snapshot_data_.get();
}

const fml::Mapping* EmbedderTestContext::GetVMSnapshotInstructions() const {
  return vm_snapshot_instructions_.get();
}

const fml::Mapping* EmbedderTestContext::GetIsolateSnapshotData() const {
  return isolate_snapshot_data_.get();
}

const fml::Mapping* EmbedderTestContext::GetIsolateSnapshotInstructions()
    const {
  return isolate_snapshot_instructions_.get();
}

FlutterEngineAOTData EmbedderTestContext::GetAOTData() const {
  return aot_data_.get();
}

void EmbedderTestContext::AddIsolateCreateCallback(
    const fml::closure& closure) {
  if (closure) {
    isolate_create_callbacks_.push_back(closure);
  }
}

VoidCallback EmbedderTestContext::GetIsolateCreateCallbackHook() {
  return [](void* user_data) {
    reinterpret_cast<EmbedderTestContext*>(user_data)
        ->FireIsolateCreateCallbacks();
  };
}

void EmbedderTestContext::FireIsolateCreateCallbacks() {
  for (const auto& closure : isolate_create_callbacks_) {
    closure();
  }
}

void EmbedderTestContext::AddNativeCallback(const char* name,
                                            Dart_NativeFunction function) {
  native_resolver_->AddNativeCallback({name}, function);
}

void EmbedderTestContext::SetPlatformMessageCallback(
    const std::function<void(const FlutterPlatformMessage*)>& callback) {
  platform_message_callback_ = callback;
}

void EmbedderTestContext::SetChannelUpdateCallback(
    const ChannelUpdateCallback& callback) {
  channel_update_callback_ = callback;
}

void EmbedderTestContext::PlatformMessageCallback(
    const FlutterPlatformMessage* message) {
  if (platform_message_callback_) {
    platform_message_callback_(message);
  }
}

void EmbedderTestContext::SetLogMessageCallback(
    const LogMessageCallback& callback) {
  log_message_callback_ = callback;
}

FlutterLogMessageCallback EmbedderTestContext::GetLogMessageCallbackHook() {
  return [](const char* tag, const char* message, void* user_data) {
    auto context = reinterpret_cast<EmbedderTestContext*>(user_data);
    if (context->log_message_callback_) {
      context->log_message_callback_(tag, message);
    }
  };
}

FlutterComputePlatformResolvedLocaleCallback
EmbedderTestContext::GetComputePlatformResolvedLocaleCallbackHook() {
  return [](const FlutterLocale** supported_locales,
            size_t length) -> const FlutterLocale* {
    return supported_locales[0];
  };
}

FlutterChannelUpdateCallback
EmbedderTestContext::GetChannelUpdateCallbackHook() {
  if (channel_update_callback_ == nullptr) {
    return nullptr;
  }

  return [](const FlutterChannelUpdate* update, void* user_data) {
    auto context = reinterpret_cast<EmbedderTestContext*>(user_data);
    if (context->channel_update_callback_) {
      context->channel_update_callback_(update);
    }
  };
}

}  // namespace testing
}  // namespace flutter
