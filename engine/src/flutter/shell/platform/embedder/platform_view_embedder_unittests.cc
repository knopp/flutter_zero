// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/platform_view_embedder.h"

#include "flutter/fml/synchronization/waitable_event.h"
#include "flutter/shell/common/thread_host.h"
#include "flutter/testing/testing.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {
namespace {
class MockDelegate : public PlatformView::Delegate {
  MOCK_METHOD(void, OnPlatformViewCreated, ());
  MOCK_METHOD(void, OnPlatformViewDestroyed, (), (override));
  MOCK_METHOD(void,
              OnPlatformViewDispatchPlatformMessage,
              (std::unique_ptr<PlatformMessage> message),
              (override));
  MOCK_METHOD(void,
              LoadDartDeferredLibrary,
              (intptr_t loading_unit_id,
               std::unique_ptr<const fml::Mapping> snapshot_data,
               std::unique_ptr<const fml::Mapping> snapshot_instructions),
              (override));
  MOCK_METHOD(void,
              LoadDartDeferredLibraryError,
              (intptr_t loading_unit_id,
               const std::string error_message,
               bool transient),
              (override));
  MOCK_METHOD(void,
              UpdateAssetResolverByType,
              (std::unique_ptr<AssetResolver> updated_asset_resolver,
               AssetResolver::AssetResolverType type),
              (override));
  MOCK_METHOD(const Settings&,
              OnPlatformViewGetSettings,
              (),
              (const, override));
};

class MockResponse : public PlatformMessageResponse {
 public:
  MOCK_METHOD(void, Complete, (std::unique_ptr<fml::Mapping> data), (override));
  MOCK_METHOD(void, CompleteEmpty, (), (override));
};
}  // namespace

TEST(PlatformViewEmbedderTest, HasPlatformMessageHandler) {
  ThreadHost thread_host("io.flutter.test." + GetCurrentTestName() + ".",
                         ThreadHost::Type::kPlatform);
  flutter::TaskRunners task_runners = flutter::TaskRunners(
      "HasPlatformMessageHandler", thread_host.platform_thread->GetTaskRunner(),
      nullptr);
  fml::AutoResetWaitableEvent latch;
  task_runners.GetPlatformTaskRunner()->PostTask([&latch, task_runners] {
    MockDelegate delegate;
    PlatformViewEmbedder::PlatformDispatchTable platform_dispatch_table;
    auto embedder = std::make_unique<PlatformViewEmbedder>(
        delegate, task_runners, platform_dispatch_table);

    ASSERT_TRUE(embedder->GetPlatformMessageHandler());
    latch.Signal();
  });
  latch.Wait();
}

TEST(PlatformViewEmbedderTest, Dispatches) {
  ThreadHost thread_host("io.flutter.test." + GetCurrentTestName() + ".",
                         ThreadHost::Type::kPlatform);
  flutter::TaskRunners task_runners = flutter::TaskRunners(
      "HasPlatformMessageHandler", thread_host.platform_thread->GetTaskRunner(),
      nullptr);
  bool did_call = false;
  std::unique_ptr<PlatformViewEmbedder> embedder;
  {
    fml::AutoResetWaitableEvent latch;
    task_runners.GetPlatformTaskRunner()->PostTask(
        [&latch, task_runners, &did_call, &embedder] {
          MockDelegate delegate;
          PlatformViewEmbedder::PlatformDispatchTable platform_dispatch_table;
          platform_dispatch_table.platform_message_response_callback =
              [&did_call](std::unique_ptr<PlatformMessage> message) {
                did_call = true;
              };
          embedder = std::make_unique<PlatformViewEmbedder>(
              delegate, task_runners, platform_dispatch_table);
          auto platform_message_handler = embedder->GetPlatformMessageHandler();
          fml::RefPtr<PlatformMessageResponse> response =
              fml::MakeRefCounted<MockResponse>();
          std::unique_ptr<PlatformMessage> message =
              std::make_unique<PlatformMessage>("foo", response);
          platform_message_handler->HandlePlatformMessage(std::move(message));
          latch.Signal();
        });
    latch.Wait();
  }
  {
    fml::AutoResetWaitableEvent latch;
    thread_host.platform_thread->GetTaskRunner()->PostTask([&latch, &embedder] {
      embedder.reset();
      latch.Signal();
    });
    latch.Wait();
  }

  EXPECT_TRUE(did_call);
}

TEST(PlatformViewEmbedderTest, DeletionDisabledDispatch) {
  ThreadHost thread_host("io.flutter.test." + GetCurrentTestName() + ".",
                         ThreadHost::Type::kPlatform);
  flutter::TaskRunners task_runners = flutter::TaskRunners(
      "HasPlatformMessageHandler", thread_host.platform_thread->GetTaskRunner(),
      nullptr);
  bool did_call = false;
  {
    fml::AutoResetWaitableEvent latch;
    task_runners.GetPlatformTaskRunner()->PostTask(
        [&latch, task_runners, &did_call] {
          MockDelegate delegate;
          PlatformViewEmbedder::PlatformDispatchTable platform_dispatch_table;
          platform_dispatch_table.platform_message_response_callback =
              [&did_call](std::unique_ptr<PlatformMessage> message) {
                did_call = true;
              };
          auto embedder = std::make_unique<PlatformViewEmbedder>(
              delegate, task_runners, platform_dispatch_table);
          auto platform_message_handler = embedder->GetPlatformMessageHandler();
          fml::RefPtr<PlatformMessageResponse> response =
              fml::MakeRefCounted<MockResponse>();
          std::unique_ptr<PlatformMessage> message =
              std::make_unique<PlatformMessage>("foo", response);
          platform_message_handler->HandlePlatformMessage(std::move(message));
          embedder.reset();
          latch.Signal();
        });
    latch.Wait();
  }
  {
    fml::AutoResetWaitableEvent latch;
    thread_host.platform_thread->GetTaskRunner()->PostTask(
        [&latch] { latch.Signal(); });
    latch.Wait();
  }

  EXPECT_FALSE(did_call);
}

}  // namespace testing
}  // namespace flutter
