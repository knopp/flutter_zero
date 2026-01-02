// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/engine.h"

#include <cstring>

#include "flutter/runtime/dart_vm_lifecycle.h"
#include "flutter/shell/common/thread_host.h"
#include "flutter/testing/fixture_test.h"
#include "fml/mapping.h"
#include "fml/synchronization/waitable_event.h"
#include "gmock/gmock.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace flutter {

namespace {

class FontManifestAssetResolver : public AssetResolver {
 public:
  FontManifestAssetResolver() {}

  bool IsValid() const override { return true; }

  bool IsValidAfterAssetManagerChange() const override { return true; }

  AssetResolver::AssetResolverType GetType() const override {
    return AssetResolver::AssetResolverType::kApkAssetProvider;
  }

  mutable size_t mapping_call_count = 0u;
  std::unique_ptr<fml::Mapping> GetAsMapping(
      const std::string& asset_name) const override {
    mapping_call_count++;
    if (asset_name == "FontManifest.json") {
      return std::make_unique<fml::DataMapping>("[{},{},{}]");
    }
    return nullptr;
  }

  std::vector<std::unique_ptr<fml::Mapping>> GetAsMappings(
      const std::string& asset_pattern,
      const std::optional<std::string>& subdir) const override {
    return {};
  };

  bool operator==(const AssetResolver& other) const override {
    auto mapping = GetAsMapping("FontManifest.json");
    return memcmp(other.GetAsMapping("FontManifest.json")->GetMapping(),
                  mapping->GetMapping(), mapping->GetSize()) == 0;
  }
};

class MockDelegate : public Engine::Delegate {
 public:
  MOCK_METHOD(void,
              OnEngineHandlePlatformMessage,
              (std::unique_ptr<PlatformMessage>),
              (override));
  MOCK_METHOD(void, OnPreEngineRestart, (), (override));
  MOCK_METHOD(void, OnRootIsolateCreated, (), (override));
  MOCK_METHOD(void,
              UpdateIsolateDescription,
              (const std::string, int64_t),
              (override));
  MOCK_METHOD(std::unique_ptr<std::vector<std::string>>,
              ComputePlatformResolvedLocale,
              (const std::vector<std::string>&),
              (override));
  MOCK_METHOD(void, RequestDartDeferredLibrary, (intptr_t), (override));
  MOCK_METHOD(fml::TimePoint, GetCurrentTimePoint, (), (override));
  MOCK_METHOD(const std::shared_ptr<PlatformMessageHandler>&,
              GetPlatformMessageHandler,
              (),
              (const, override));
  MOCK_METHOD(void, OnEngineChannelUpdate, (std::string, bool), (override));
};

class MockResponse : public PlatformMessageResponse {
 public:
  MOCK_METHOD(void, Complete, (std::unique_ptr<fml::Mapping> data), (override));
  MOCK_METHOD(void, CompleteEmpty, (), (override));
};

class MockRuntimeDelegate : public RuntimeDelegate {
 public:
  MOCK_METHOD(void,
              HandlePlatformMessage,
              (std::unique_ptr<PlatformMessage>),
              (override));
  MOCK_METHOD(std::shared_ptr<AssetManager>, GetAssetManager, (), (override));
  MOCK_METHOD(void, OnRootIsolateCreated, (), (override));
  MOCK_METHOD(void,
              UpdateIsolateDescription,
              (const std::string, int64_t),
              (override));
  MOCK_METHOD(std::unique_ptr<std::vector<std::string>>,
              ComputePlatformResolvedLocale,
              (const std::vector<std::string>&),
              (override));
  MOCK_METHOD(void, RequestDartDeferredLibrary, (intptr_t), (override));
  MOCK_METHOD(std::weak_ptr<PlatformMessageHandler>,
              GetPlatformMessageHandler,
              (),
              (const, override));
  MOCK_METHOD(void, SendChannelUpdate, (std::string, bool), (override));
};

class MockRuntimeController : public RuntimeController {
 public:
  MockRuntimeController(RuntimeDelegate& client,
                        const TaskRunners& p_task_runners)
      : RuntimeController(client, p_task_runners) {}
  MOCK_METHOD(bool, IsRootIsolateRunning, (), (override, const));
  MOCK_METHOD(bool,
              DispatchPlatformMessage,
              (std::unique_ptr<PlatformMessage>),
              (override));
  MOCK_METHOD(void,
              LoadDartDeferredLibraryError,
              (intptr_t, const std::string, bool),
              (override));
  MOCK_METHOD(DartVM*, GetDartVM, (), (const, override));
  MOCK_METHOD(bool, NotifyIdle, (fml::TimeDelta), (override));
};

#if 0
std::unique_ptr<PlatformMessage> MakePlatformMessage(
    const std::string& channel,
    const std::map<std::string, std::string>& values,
    const fml::RefPtr<PlatformMessageResponse>& response) {
  rapidjson::Document document;
  auto& allocator = document.GetAllocator();
  document.SetObject();

  for (const auto& pair : values) {
    rapidjson::Value key(pair.first.c_str(), strlen(pair.first.c_str()),
                         allocator);
    rapidjson::Value value(pair.second.c_str(), strlen(pair.second.c_str()),
                           allocator);
    document.AddMember(key, value, allocator);
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  document.Accept(writer);
  const uint8_t* data = reinterpret_cast<const uint8_t*>(buffer.GetString());

  std::unique_ptr<PlatformMessage> message = std::make_unique<PlatformMessage>(
      channel, fml::MallocMapping::Copy(data, buffer.GetSize()), response);
  return message;
}
#endif

class EngineTest : public testing::FixtureTest {
 public:
  EngineTest()
      : thread_host_("EngineTest",
                     ThreadHost::Type::kPlatform | ThreadHost::Type::kUi),
        task_runners_({
            "EngineTest",
            thread_host_.platform_thread->GetTaskRunner(),  // platform
            thread_host_.ui_thread->GetTaskRunner(),        // ui
        }) {}

  void PostUITaskSync(const std::function<void()>& function) {
    fml::AutoResetWaitableEvent latch;
    task_runners_.GetUITaskRunner()->PostTask([&] {
      function();
      latch.Signal();
    });
    latch.Wait();
  }

 protected:
  void SetUp() override { settings_ = CreateSettingsForFixture(); }

  MockDelegate delegate_;
  ThreadHost thread_host_;
  TaskRunners task_runners_;
  Settings settings_;
  std::unique_ptr<RuntimeController> runtime_controller_;
};
}  // namespace

TEST_F(EngineTest, Create) {
  PostUITaskSync([this] {
    auto engine = std::make_unique<Engine>(
        /*delegate=*/delegate_,
        /*task_runners=*/task_runners_,
        /*settings=*/settings_,
        /*runtime_controller=*/std::move(runtime_controller_));
    EXPECT_TRUE(engine);
  });
}

TEST_F(EngineTest, DispatchPlatformMessageUnknown) {
  PostUITaskSync([this] {
    MockRuntimeDelegate client;
    auto mock_runtime_controller =
        std::make_unique<MockRuntimeController>(client, task_runners_);
    EXPECT_CALL(*mock_runtime_controller, IsRootIsolateRunning())
        .WillRepeatedly(::testing::Return(false));
    auto engine = std::make_unique<Engine>(
        /*delegate=*/delegate_,
        /*task_runners=*/task_runners_,
        /*settings=*/settings_,
        /*runtime_controller=*/std::move(mock_runtime_controller));

    fml::RefPtr<PlatformMessageResponse> response =
        fml::MakeRefCounted<MockResponse>();
    std::unique_ptr<PlatformMessage> message =
        std::make_unique<PlatformMessage>("foo", response);
    engine->DispatchPlatformMessage(std::move(message));
  });
}

TEST_F(EngineTest, SpawnWithCustomSettings) {
  PostUITaskSync([this] {
    MockRuntimeDelegate client;
    auto mock_runtime_controller =
        std::make_unique<MockRuntimeController>(client, task_runners_);
    auto vm_ref = DartVMRef::Create(settings_);
    EXPECT_CALL(*mock_runtime_controller, GetDartVM())
        .WillRepeatedly(::testing::Return(vm_ref.get()));
    auto engine = std::make_unique<Engine>(
        /*delegate=*/delegate_,
        /*task_runners=*/task_runners_,
        /*settings=*/settings_,
        /*runtime_controller=*/std::move(mock_runtime_controller));

    Settings custom_settings = settings_;
    custom_settings.persistent_isolate_data =
        std::make_shared<fml::DataMapping>("foo");
    auto spawn = engine->Spawn(delegate_, custom_settings);
    EXPECT_TRUE(spawn != nullptr);
    auto new_persistent_isolate_data =
        const_cast<RuntimeController*>(spawn->GetRuntimeController())
            ->GetPersistentIsolateData();
    EXPECT_EQ(custom_settings.persistent_isolate_data->GetMapping(),
              new_persistent_isolate_data->GetMapping());
    EXPECT_EQ(custom_settings.persistent_isolate_data->GetSize(),
              new_persistent_isolate_data->GetSize());
  });
}

TEST_F(EngineTest, PassesLoadDartDeferredLibraryErrorToRuntime) {
  PostUITaskSync([this] {
    intptr_t error_id = 123;
    const std::string error_message = "error message";
    MockRuntimeDelegate client;
    auto mock_runtime_controller =
        std::make_unique<MockRuntimeController>(client, task_runners_);
    EXPECT_CALL(*mock_runtime_controller, IsRootIsolateRunning())
        .WillRepeatedly(::testing::Return(true));
    EXPECT_CALL(*mock_runtime_controller,
                LoadDartDeferredLibraryError(error_id, error_message, true))
        .Times(1);
    auto engine = std::make_unique<Engine>(
        /*delegate=*/delegate_,
        /*task_runners=*/task_runners_,
        /*settings=*/settings_,
        /*runtime_controller=*/std::move(mock_runtime_controller));

    engine->LoadDartDeferredLibraryError(error_id, error_message, true);
  });
}

TEST_F(EngineTest, SpawnedEngineInheritsAssetManager) {
  PostUITaskSync([this] {
    MockRuntimeDelegate client;
    auto mock_runtime_controller =
        std::make_unique<MockRuntimeController>(client, task_runners_);
    auto vm_ref = DartVMRef::Create(settings_);
    EXPECT_CALL(*mock_runtime_controller, GetDartVM())
        .WillRepeatedly(::testing::Return(vm_ref.get()));

    // auto mock_font_collection = std::make_shared<MockFontCollection>();
    // EXPECT_CALL(*mock_font_collection, RegisterFonts(::testing::_))
    //     .WillOnce(::testing::Return());
    auto engine = std::make_unique<Engine>(
        /*delegate=*/delegate_,
        /*task_runners=*/task_runners_,
        /*settings=*/settings_,
        /*runtime_controller=*/std::move(mock_runtime_controller));

    EXPECT_EQ(engine->GetAssetManager(), nullptr);

    auto asset_manager = std::make_shared<AssetManager>();
    asset_manager->PushBack(std::make_unique<FontManifestAssetResolver>());
    engine->UpdateAssetManager(asset_manager);
    EXPECT_EQ(engine->GetAssetManager(), asset_manager);

    auto spawn = engine->Spawn(delegate_, settings_);
    EXPECT_TRUE(spawn != nullptr);
    EXPECT_EQ(engine->GetAssetManager(), spawn->GetAssetManager());
  });
}

TEST_F(EngineTest, UpdateAssetManagerWithEqualManagers) {
  PostUITaskSync([this] {
    MockRuntimeDelegate client;
    auto mock_runtime_controller =
        std::make_unique<MockRuntimeController>(client, task_runners_);
    auto vm_ref = DartVMRef::Create(settings_);
    EXPECT_CALL(*mock_runtime_controller, GetDartVM())
        .WillRepeatedly(::testing::Return(vm_ref.get()));

    auto engine = std::make_unique<Engine>(
        /*delegate=*/delegate_,
        /*task_runners=*/task_runners_,
        /*settings=*/settings_,
        /*runtime_controller=*/std::move(mock_runtime_controller));

    EXPECT_EQ(engine->GetAssetManager(), nullptr);

    auto asset_manager = std::make_shared<AssetManager>();
    asset_manager->PushBack(std::make_unique<FontManifestAssetResolver>());

    auto asset_manager_2 = std::make_shared<AssetManager>();
    asset_manager_2->PushBack(std::make_unique<FontManifestAssetResolver>());

    EXPECT_NE(asset_manager, asset_manager_2);
    EXPECT_TRUE(*asset_manager == *asset_manager_2);

    engine->UpdateAssetManager(asset_manager);
    EXPECT_EQ(engine->GetAssetManager(), asset_manager);

    engine->UpdateAssetManager(asset_manager_2);
    // Didn't change because they're equivalent.
    EXPECT_EQ(engine->GetAssetManager(), asset_manager);
  });
}

}  // namespace flutter
