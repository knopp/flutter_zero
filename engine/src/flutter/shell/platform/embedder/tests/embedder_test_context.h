// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_TESTS_EMBEDDER_TEST_CONTEXT_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_TESTS_EMBEDDER_TEST_CONTEXT_H_

#include <memory>
#include <string>
#include <vector>

#include "flutter/fml/closure.h"
#include "flutter/fml/macros.h"
#include "flutter/fml/mapping.h"
#include "flutter/shell/platform/embedder/embedder.h"
#include "flutter/testing/elf_loader.h"
#include "flutter/testing/test_dart_native_resolver.h"

namespace flutter {
namespace testing {

using LogMessageCallback =
    std::function<void(const char* tag, const char* message)>;
using ChannelUpdateCallback = std::function<void(const FlutterChannelUpdate*)>;

struct AOTDataDeleter {
  void operator()(FlutterEngineAOTData aot_data) {
    if (aot_data) {
      FlutterEngineCollectAOTData(aot_data);
    }
  }
};

using UniqueAOTData = std::unique_ptr<_FlutterEngineAOTData, AOTDataDeleter>;

class EmbedderTestContext {
 public:
  explicit EmbedderTestContext(std::string assets_path = "");

  virtual ~EmbedderTestContext();

  const std::string& GetAssetsPath() const;

  const fml::Mapping* GetVMSnapshotData() const;

  const fml::Mapping* GetVMSnapshotInstructions() const;

  const fml::Mapping* GetIsolateSnapshotData() const;

  const fml::Mapping* GetIsolateSnapshotInstructions() const;

  FlutterEngineAOTData GetAOTData() const;

  void AddIsolateCreateCallback(const fml::closure& closure);

  void AddNativeCallback(const char* name, Dart_NativeFunction function);

  void SetPlatformMessageCallback(
      const std::function<void(const FlutterPlatformMessage*)>& callback);

  void SetLogMessageCallback(const LogMessageCallback& log_message_callback);

  void SetChannelUpdateCallback(const ChannelUpdateCallback& callback);

  // TODO(gw280): encapsulate these properly for subclasses to use
 protected:
  // This allows the builder to access the hooks.
  friend class EmbedderConfigBuilder;

  std::string assets_path_;
  ELFAOTSymbols aot_symbols_;
  std::unique_ptr<fml::Mapping> vm_snapshot_data_;
  std::unique_ptr<fml::Mapping> vm_snapshot_instructions_;
  std::unique_ptr<fml::Mapping> isolate_snapshot_data_;
  std::unique_ptr<fml::Mapping> isolate_snapshot_instructions_;
  UniqueAOTData aot_data_;
  std::vector<fml::closure> isolate_create_callbacks_;
  std::shared_ptr<TestDartNativeResolver> native_resolver_;
  ChannelUpdateCallback channel_update_callback_;
  std::function<void(const FlutterPlatformMessage*)> platform_message_callback_;
  LogMessageCallback log_message_callback_;
  std::function<void(intptr_t)> vsync_callback_ = nullptr;

  static VoidCallback GetIsolateCreateCallbackHook();

  static FlutterLogMessageCallback GetLogMessageCallbackHook();

  static FlutterComputePlatformResolvedLocaleCallback
  GetComputePlatformResolvedLocaleCallbackHook();

  FlutterChannelUpdateCallback GetChannelUpdateCallbackHook();

  void SetupAOTMappingsIfNecessary();

  void SetupAOTDataIfNecessary();

  void FireIsolateCreateCallbacks();

  void SetNativeResolver();

  void PlatformMessageCallback(const FlutterPlatformMessage* message);

  FML_DISALLOW_COPY_AND_ASSIGN(EmbedderTestContext);
};

}  // namespace testing
}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_TESTS_EMBEDDER_TEST_CONTEXT_H_
