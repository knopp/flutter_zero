// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER

#include <string>
#include <vector>

#include "embedder.h"
#include "embedder_engine.h"
#include "flutter/fml/make_copyable.h"
#include "flutter/fml/message_loop.h"
#include "flutter/fml/paths.h"
#include "flutter/fml/synchronization/count_down_latch.h"
#include "flutter/fml/synchronization/waitable_event.h"
#include "flutter/fml/task_runner.h"
#include "flutter/fml/thread.h"
#include "flutter/fml/time/time_delta.h"
#include "flutter/fml/time/time_point.h"
#include "flutter/runtime/dart_vm.h"
#include "flutter/shell/platform/embedder/tests/embedder_config_builder.h"
#include "flutter/shell/platform/embedder/tests/embedder_test.h"
#include "flutter/shell/platform/embedder/tests/embedder_unittests_util.h"
#include "flutter/testing/testing.h"
#include "third_party/tonic/converter/dart_converter.h"

#if defined(FML_OS_MACOSX)
#include <pthread.h>
#endif

// CREATE_NATIVE_ENTRY is leaky by design
// NOLINTBEGIN(clang-analyzer-core.StackAddressEscape)

namespace flutter {
namespace testing {

using EmbedderTest = testing::EmbedderTest;

TEST(EmbedderTestNoFixture, MustNotRunWithInvalidArgs) {
  EmbedderTestContext context;
  EmbedderConfigBuilder builder(
      context, EmbedderConfigBuilder::InitializationPreference::kNoInitialize);
  auto engine = builder.LaunchEngine();
  ASSERT_FALSE(engine.is_valid());
}

TEST_F(EmbedderTest, CanLaunchAndShutdownWithValidProjectArgs) {
  auto& context = GetEmbedderContext();
  fml::AutoResetWaitableEvent latch;
  context.AddIsolateCreateCallback([&latch]() { latch.Signal(); });
  EmbedderConfigBuilder builder(context);
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Wait for the root isolate to launch.
  latch.Wait();
  engine.reset();
}

// TODO(41999): Disabled because flaky.
TEST_F(EmbedderTest, DISABLED_CanLaunchAndShutdownMultipleTimes) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  for (size_t i = 0; i < 3; ++i) {
    auto engine = builder.LaunchEngine();
    ASSERT_TRUE(engine.is_valid());
    FML_LOG(INFO) << "Engine launch count: " << i + 1;
  }
}

TEST_F(EmbedderTest, CanInvokeCustomEntrypoint) {
  auto& context = GetEmbedderContext();
  static fml::AutoResetWaitableEvent latch;
  Dart_NativeFunction entrypoint = [](Dart_NativeArguments args) {
    latch.Signal();
  };
  context.AddNativeCallback("SayHiFromCustomEntrypoint", entrypoint);
  EmbedderConfigBuilder builder(context);
  builder.SetDartEntrypoint("customEntrypoint");
  auto engine = builder.LaunchEngine();
  latch.Wait();
  ASSERT_TRUE(engine.is_valid());
}

TEST_F(EmbedderTest, CanInvokeCustomEntrypointMacro) {
  auto& context = GetEmbedderContext();

  fml::AutoResetWaitableEvent latch1;
  fml::AutoResetWaitableEvent latch2;
  fml::AutoResetWaitableEvent latch3;

  // Can be defined separately.
  auto entry1 = [&latch1](Dart_NativeArguments args) {
    FML_LOG(INFO) << "In Callback 1";
    latch1.Signal();
  };
  auto native_entry1 = CREATE_NATIVE_ENTRY(entry1);
  context.AddNativeCallback("SayHiFromCustomEntrypoint1", native_entry1);

  // Can be wrapped in the args.
  auto entry2 = [&latch2](Dart_NativeArguments args) {
    FML_LOG(INFO) << "In Callback 2";
    latch2.Signal();
  };
  context.AddNativeCallback("SayHiFromCustomEntrypoint2",
                            CREATE_NATIVE_ENTRY(entry2));

  // Everything can be inline.
  context.AddNativeCallback(
      "SayHiFromCustomEntrypoint3",
      CREATE_NATIVE_ENTRY([&latch3](Dart_NativeArguments args) {
        FML_LOG(INFO) << "In Callback 3";
        latch3.Signal();
      }));

  EmbedderConfigBuilder builder(context);
  builder.SetDartEntrypoint("customEntrypoint1");
  auto engine = builder.LaunchEngine();
  latch1.Wait();
  latch2.Wait();
  latch3.Wait();
  ASSERT_TRUE(engine.is_valid());
}

TEST_F(EmbedderTest, CanTerminateCleanly) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  builder.SetDartEntrypoint("terminateExitCodeHandler");
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
}

TEST_F(EmbedderTest, ExecutableNameNotNull) {
  auto& context = GetEmbedderContext();

  // Supply a callback to Dart for the test fixture to pass Platform.executable
  // back to us.
  fml::AutoResetWaitableEvent latch;
  context.AddNativeCallback(
      "NotifyStringValue", CREATE_NATIVE_ENTRY([&](Dart_NativeArguments args) {
        const auto dart_string = tonic::DartConverter<std::string>::FromDart(
            Dart_GetNativeArgument(args, 0));
        EXPECT_EQ("/path/to/binary", dart_string);
        latch.Signal();
      }));

  EmbedderConfigBuilder builder(context);
  builder.SetDartEntrypoint("executableNameNotNull");
  builder.SetExecutableName("/path/to/binary");
  auto engine = builder.LaunchEngine();
  latch.Wait();
}

std::atomic_size_t EmbedderTestTaskRunner::sEmbedderTaskRunnerIdentifiers = {};

TEST_F(EmbedderTest, CanSpecifyCustomUITaskRunner) {
  auto& context = GetEmbedderContext();
  auto ui_task_runner = CreateNewThread("test_ui_thread");
  auto platform_task_runner = CreateNewThread("test_platform_thread");
  static std::mutex engine_mutex;
  UniqueEngine engine;

  EmbedderTestTaskRunner test_ui_task_runner(
      ui_task_runner, [&](FlutterTask task) {
        std::scoped_lock lock(engine_mutex);
        if (!engine.is_valid()) {
          return;
        }
        FlutterEngineRunTask(engine.get(), &task);
      });
  EmbedderTestTaskRunner test_platform_task_runner(
      platform_task_runner, [&](FlutterTask task) {
        std::scoped_lock lock(engine_mutex);
        if (!engine.is_valid()) {
          return;
        }
        FlutterEngineRunTask(engine.get(), &task);
      });

  fml::AutoResetWaitableEvent signal_latch_ui;
  fml::AutoResetWaitableEvent signal_latch_platform;

  context.AddNativeCallback(
      "SignalNativeTest", CREATE_NATIVE_ENTRY([&](Dart_NativeArguments args) {
        // Assert that the UI isolate is running on platform thread.
        ASSERT_TRUE(ui_task_runner->RunsTasksOnCurrentThread());
        signal_latch_ui.Signal();
      }));

  platform_task_runner->PostTask([&]() {
    EmbedderConfigBuilder builder(context);
    const auto ui_task_runner_description =
        test_ui_task_runner.GetFlutterTaskRunnerDescription();
    const auto platform_task_runner_description =
        test_platform_task_runner.GetFlutterTaskRunnerDescription();
    builder.SetUITaskRunner(&ui_task_runner_description);
    builder.SetPlatformTaskRunner(&platform_task_runner_description);
    builder.SetDartEntrypoint("canSpecifyCustomUITaskRunner");
    builder.SetPlatformMessageCallback(
        [&](const FlutterPlatformMessage* message) {
          ASSERT_TRUE(platform_task_runner->RunsTasksOnCurrentThread());
          signal_latch_platform.Signal();
        });
    {
      std::scoped_lock lock(engine_mutex);
      engine = builder.InitializeEngine();
    }
    ASSERT_EQ(FlutterEngineRunInitialized(engine.get()), kSuccess);
    ASSERT_TRUE(engine.is_valid());
  });
  signal_latch_ui.Wait();
  signal_latch_platform.Wait();

  fml::AutoResetWaitableEvent kill_latch;
  platform_task_runner->PostTask([&] {
    engine.reset();
    platform_task_runner->PostTask([&kill_latch] { kill_latch.Signal(); });
  });
  kill_latch.Wait();
}

TEST_F(EmbedderTest, IgnoresStaleTasks) {
  auto& context = GetEmbedderContext();
  auto ui_task_runner = CreateNewThread("test_ui_thread");
  auto platform_task_runner = CreateNewThread("test_platform_thread");
  static std::mutex engine_mutex;
  UniqueEngine engine;
  FlutterEngine engine_ptr;

  EmbedderTestTaskRunner test_ui_task_runner(
      ui_task_runner, [&](FlutterTask task) {
        // The check for engine.is_valid() is intentionally absent here.
        // FlutterEngineRunTask must be able to detect and ignore stale tasks
        // without crashing even if the engine pointer is not null.
        // Because the engine is destroyed on platform thread,
        // relying solely on engine.is_valid() in UI thread is not safe.
        FlutterEngineRunTask(engine_ptr, &task);
      });
  EmbedderTestTaskRunner test_platform_task_runner(
      platform_task_runner, [&](FlutterTask task) {
        std::scoped_lock lock(engine_mutex);
        if (!engine.is_valid()) {
          return;
        }
        FlutterEngineRunTask(engine.get(), &task);
      });

  fml::AutoResetWaitableEvent init_latch;

  platform_task_runner->PostTask([&]() {
    EmbedderConfigBuilder builder(context);
    const auto ui_task_runner_description =
        test_ui_task_runner.GetFlutterTaskRunnerDescription();
    const auto platform_task_runner_description =
        test_platform_task_runner.GetFlutterTaskRunnerDescription();
    builder.SetUITaskRunner(&ui_task_runner_description);
    builder.SetPlatformTaskRunner(&platform_task_runner_description);
    {
      std::scoped_lock lock(engine_mutex);
      engine = builder.InitializeEngine();
    }
    init_latch.Signal();
  });

  init_latch.Wait();
  engine_ptr = engine.get();

  auto flutter_engine = reinterpret_cast<EmbedderEngine*>(engine.get());

  // Schedule task on UI thread that will likely run after the engine has shut
  // down.
  flutter_engine->GetTaskRunners().GetUITaskRunner()->PostDelayedTask(
      []() {}, fml::TimeDelta::FromMilliseconds(50));

  fml::AutoResetWaitableEvent kill_latch;
  platform_task_runner->PostTask([&] {
    engine.reset();
    platform_task_runner->PostTask([&kill_latch] { kill_latch.Signal(); });
  });
  kill_latch.Wait();

  // Ensure that the schedule task indeed runs.
  kill_latch.Reset();
  ui_task_runner->PostDelayedTask([&]() { kill_latch.Signal(); },
                                  fml::TimeDelta::FromMilliseconds(50));
  kill_latch.Wait();
}

TEST_F(EmbedderTest, MergedPlatformUIThread) {
  auto& context = GetEmbedderContext();
  auto task_runner = CreateNewThread("test_thread");
  UniqueEngine engine;

  EmbedderTestTaskRunner test_task_runner(task_runner, [&](FlutterTask task) {
    if (!engine.is_valid()) {
      return;
    }
    FlutterEngineRunTask(engine.get(), &task);
  });

  fml::AutoResetWaitableEvent signal_latch_ui;
  fml::AutoResetWaitableEvent signal_latch_platform;

  context.AddNativeCallback(
      "SignalNativeTest", CREATE_NATIVE_ENTRY([&](Dart_NativeArguments args) {
        // Assert that the UI isolate is running on platform thread.
        ASSERT_TRUE(task_runner->RunsTasksOnCurrentThread());
        signal_latch_ui.Signal();
      }));

  task_runner->PostTask([&]() {
    EmbedderConfigBuilder builder(context);
    const auto task_runner_description =
        test_task_runner.GetFlutterTaskRunnerDescription();
    builder.SetUITaskRunner(&task_runner_description);
    builder.SetPlatformTaskRunner(&task_runner_description);
    builder.SetDartEntrypoint("mergedPlatformUIThread");
    builder.SetPlatformMessageCallback(
        [&](const FlutterPlatformMessage* message) {
          ASSERT_TRUE(task_runner->RunsTasksOnCurrentThread());
          signal_latch_platform.Signal();
        });
    engine = builder.LaunchEngine();
    ASSERT_TRUE(engine.is_valid());
  });
  signal_latch_ui.Wait();
  signal_latch_platform.Wait();

  fml::AutoResetWaitableEvent kill_latch;
  task_runner->PostTask([&] {
    engine.reset();
    task_runner->PostTask([&kill_latch] { kill_latch.Signal(); });
  });
  kill_latch.Wait();
}

TEST_F(EmbedderTest, UITaskRunnerFlushesMicrotasks) {
  auto& context = GetEmbedderContext();
  auto ui_task_runner = CreateNewThread("test_ui_thread");
  UniqueEngine engine;

  EmbedderTestTaskRunner test_task_runner(
      // Assert that the UI isolate is running on platform thread.
      ui_task_runner, [&](FlutterTask task) {
        if (!engine.is_valid()) {
          return;
        }
        FlutterEngineRunTask(engine.get(), &task);
      });

  fml::AutoResetWaitableEvent signal_latch;

  context.AddNativeCallback(
      "SignalNativeTest", CREATE_NATIVE_ENTRY([&](Dart_NativeArguments args) {
        ASSERT_TRUE(ui_task_runner->RunsTasksOnCurrentThread());
        signal_latch.Signal();
      }));

  ui_task_runner->PostTask([&]() {
    EmbedderConfigBuilder builder(context);
    const auto task_runner_description =
        test_task_runner.GetFlutterTaskRunnerDescription();
    builder.SetUITaskRunner(&task_runner_description);
    builder.SetDartEntrypoint("uiTaskRunnerFlushesMicrotasks");
    engine = builder.LaunchEngine();
    ASSERT_TRUE(engine.is_valid());
  });
  signal_latch.Wait();

  fml::AutoResetWaitableEvent kill_latch;
  ui_task_runner->PostTask([&] {
    engine.reset();
    ui_task_runner->PostTask([&kill_latch] { kill_latch.Signal(); });
  });
  kill_latch.Wait();
}

TEST_F(EmbedderTest, CanSpecifyCustomPlatformTaskRunner) {
  auto& context = GetEmbedderContext();
  fml::AutoResetWaitableEvent latch;

  // Run the test on its own thread with a message loop so that it can safely
  // pump its event loop while we wait for all the conditions to be checked.
  auto platform_task_runner = CreateNewThread("test_platform_thread");
  static std::mutex engine_mutex;
  static bool signaled_once = false;
  static std::atomic<bool> destruction_callback_called = false;
  UniqueEngine engine;

  EmbedderTestTaskRunner test_task_runner(
      platform_task_runner, [&](FlutterTask task) {
        std::scoped_lock lock(engine_mutex);
        if (!engine.is_valid()) {
          return;
        }
        // There may be multiple tasks posted but we only need to check
        // assertions once.
        if (signaled_once) {
          FlutterEngineRunTask(engine.get(), &task);
          return;
        }

        signaled_once = true;
        ASSERT_TRUE(engine.is_valid());
        ASSERT_EQ(FlutterEngineRunTask(engine.get(), &task), kSuccess);
        latch.Signal();
      });
  test_task_runner.SetDestructionCallback(
      [](void* user_data) { destruction_callback_called = true; });

  platform_task_runner->PostTask([&]() {
    EmbedderConfigBuilder builder(context);
    const auto task_runner_description =
        test_task_runner.GetFlutterTaskRunnerDescription();
    builder.SetPlatformTaskRunner(&task_runner_description);
    builder.SetDartEntrypoint("invokePlatformTaskRunner");
    std::scoped_lock lock(engine_mutex);
    engine = builder.LaunchEngine();
    ASSERT_TRUE(engine.is_valid());
  });

  // Signaled when all the assertions are checked.
  latch.Wait();
  ASSERT_TRUE(engine.is_valid());

  // Since the engine was started on its own thread, it must be killed there as
  // well.
  fml::AutoResetWaitableEvent kill_latch;
  platform_task_runner->PostTask(fml::MakeCopyable([&]() mutable {
    std::scoped_lock lock(engine_mutex);
    engine.reset();

    // There may still be pending tasks on the platform thread that were queued
    // by the test_task_runner.  Signal the latch after these tasks have been
    // consumed.
    platform_task_runner->PostTask([&kill_latch] { kill_latch.Signal(); });
  }));
  kill_latch.Wait();

  ASSERT_TRUE(signaled_once);
  signaled_once = false;

  ASSERT_TRUE(destruction_callback_called);
  destruction_callback_called = false;
}

TEST(EmbedderTestNoFixture, CanGetCurrentTimeInNanoseconds) {
  auto point1 = fml::TimePoint::FromEpochDelta(
      fml::TimeDelta::FromNanoseconds(FlutterEngineGetCurrentTime()));
  auto point2 = fml::TimePoint::Now();

  ASSERT_LT((point2 - point1), fml::TimeDelta::FromMilliseconds(1));
}

TEST_F(EmbedderTest, IsolateServiceIdSent) {
  auto& context = GetEmbedderContext();
  fml::AutoResetWaitableEvent latch;

  fml::Thread thread;
  UniqueEngine engine;
  std::string isolate_message;

  thread.GetTaskRunner()->PostTask([&]() {
    EmbedderConfigBuilder builder(context);
    builder.SetDartEntrypoint("main");
    builder.SetPlatformMessageCallback(
        [&](const FlutterPlatformMessage* message) {
          if (strcmp(message->channel, "flutter/isolate") == 0) {
            isolate_message = {reinterpret_cast<const char*>(message->message),
                               message->message_size};
            latch.Signal();
          }
        });
    engine = builder.LaunchEngine();
    ASSERT_TRUE(engine.is_valid());
  });

  // Wait for the isolate ID message and check its format.
  latch.Wait();
  ASSERT_EQ(isolate_message.find("isolates/"), 0ul);

  // Since the engine was started on its own thread, it must be killed there as
  // well.
  fml::AutoResetWaitableEvent kill_latch;
  thread.GetTaskRunner()->PostTask(
      fml::MakeCopyable([&engine, &kill_latch]() mutable {
        engine.reset();
        kill_latch.Signal();
      }));
  kill_latch.Wait();
}

//------------------------------------------------------------------------------
/// Creates a platform message response callbacks, does NOT send them, and
/// immediately collects the same.
///
TEST_F(EmbedderTest, CanCreateAndCollectCallbacks) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  builder.SetDartEntrypoint("platform_messages_response");
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY([](Dart_NativeArguments args) {}));

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  FlutterPlatformMessageResponseHandle* response_handle = nullptr;
  auto callback = [](const uint8_t* data, size_t size,
                     void* user_data) -> void {};
  auto result = FlutterPlatformMessageCreateResponseHandle(
      engine.get(), callback, nullptr, &response_handle);
  ASSERT_EQ(result, kSuccess);
  ASSERT_NE(response_handle, nullptr);

  result = FlutterPlatformMessageReleaseResponseHandle(engine.get(),
                                                       response_handle);
  ASSERT_EQ(result, kSuccess);
}

//------------------------------------------------------------------------------
/// Sends platform messages to Dart code than simply echoes the contents of the
/// message back to the embedder. The embedder registers a native callback to
/// intercept that message.
///
TEST_F(EmbedderTest, PlatformMessagesCanReceiveResponse) {
  struct Captures {
    fml::AutoResetWaitableEvent latch;
    std::thread::id thread_id;
  };
  Captures captures;

  CreateNewThread()->PostTask([&]() {
    captures.thread_id = std::this_thread::get_id();
    auto& context = GetEmbedderContext();
    EmbedderConfigBuilder builder(context);
    builder.SetDartEntrypoint("platform_messages_response");

    fml::AutoResetWaitableEvent ready;
    context.AddNativeCallback(
        "SignalNativeTest",
        CREATE_NATIVE_ENTRY(
            [&ready](Dart_NativeArguments args) { ready.Signal(); }));

    auto engine = builder.LaunchEngine();
    ASSERT_TRUE(engine.is_valid());

    static std::string kMessageData = "Hello from embedder.";

    FlutterPlatformMessageResponseHandle* response_handle = nullptr;
    auto callback = [](const uint8_t* data, size_t size,
                       void* user_data) -> void {
      ASSERT_EQ(size, kMessageData.size());
      ASSERT_EQ(strncmp(reinterpret_cast<const char*>(kMessageData.data()),
                        reinterpret_cast<const char*>(data), size),
                0);
      auto captures = reinterpret_cast<Captures*>(user_data);
      ASSERT_EQ(captures->thread_id, std::this_thread::get_id());
      captures->latch.Signal();
    };
    auto result = FlutterPlatformMessageCreateResponseHandle(
        engine.get(), callback, &captures, &response_handle);
    ASSERT_EQ(result, kSuccess);

    FlutterPlatformMessage message = {};
    message.struct_size = sizeof(FlutterPlatformMessage);
    message.channel = "test_channel";
    message.message = reinterpret_cast<const uint8_t*>(kMessageData.data());
    message.message_size = kMessageData.size();
    message.response_handle = response_handle;

    ready.Wait();
    result = FlutterEngineSendPlatformMessage(engine.get(), &message);
    ASSERT_EQ(result, kSuccess);

    result = FlutterPlatformMessageReleaseResponseHandle(engine.get(),
                                                         response_handle);
    ASSERT_EQ(result, kSuccess);
  });

  captures.latch.Wait();
}

//------------------------------------------------------------------------------
/// Tests that a platform message can be sent with no response handle. Instead
/// of the platform message integrity checked via a response handle, a native
/// callback with the response is invoked to assert integrity.
///
TEST_F(EmbedderTest, PlatformMessagesCanBeSentWithoutResponseHandles) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  builder.SetDartEntrypoint("platform_messages_no_response");

  const std::string message_data = "Hello but don't call me back.";

  fml::AutoResetWaitableEvent ready, message;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&ready](Dart_NativeArguments args) { ready.Signal(); }));
  context.AddNativeCallback(
      "SignalNativeMessage",
      CREATE_NATIVE_ENTRY(
          ([&message, &message_data](Dart_NativeArguments args) {
            auto received_message = tonic::DartConverter<std::string>::FromDart(
                Dart_GetNativeArgument(args, 0));
            ASSERT_EQ(received_message, message_data);
            message.Signal();
          })));

  auto engine = builder.LaunchEngine();

  ASSERT_TRUE(engine.is_valid());
  ready.Wait();

  FlutterPlatformMessage platform_message = {};
  platform_message.struct_size = sizeof(FlutterPlatformMessage);
  platform_message.channel = "test_channel";
  platform_message.message =
      reinterpret_cast<const uint8_t*>(message_data.data());
  platform_message.message_size = message_data.size();
  platform_message.response_handle = nullptr;  // No response needed.

  auto result =
      FlutterEngineSendPlatformMessage(engine.get(), &platform_message);
  ASSERT_EQ(result, kSuccess);
  message.Wait();
}

//------------------------------------------------------------------------------
/// Tests that a null platform message can be sent.
///
TEST_F(EmbedderTest, NullPlatformMessagesCanBeSent) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  builder.SetDartEntrypoint("null_platform_messages");

  fml::AutoResetWaitableEvent ready, message;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&ready](Dart_NativeArguments args) { ready.Signal(); }));
  context.AddNativeCallback(
      "SignalNativeMessage",
      CREATE_NATIVE_ENTRY(([&message](Dart_NativeArguments args) {
        auto received_message = tonic::DartConverter<std::string>::FromDart(
            Dart_GetNativeArgument(args, 0));
        ASSERT_EQ("true", received_message);
        message.Signal();
      })));

  auto engine = builder.LaunchEngine();

  ASSERT_TRUE(engine.is_valid());
  ready.Wait();

  FlutterPlatformMessage platform_message = {};
  platform_message.struct_size = sizeof(FlutterPlatformMessage);
  platform_message.channel = "test_channel";
  platform_message.message = nullptr;
  platform_message.message_size = 0;
  platform_message.response_handle = nullptr;  // No response needed.

  auto result =
      FlutterEngineSendPlatformMessage(engine.get(), &platform_message);
  ASSERT_EQ(result, kSuccess);
  message.Wait();
}

//------------------------------------------------------------------------------
/// Tests that a null platform message cannot be send if the message_size
/// isn't equals to 0.
///
TEST_F(EmbedderTest, InvalidPlatformMessages) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  auto engine = builder.LaunchEngine();

  ASSERT_TRUE(engine.is_valid());

  FlutterPlatformMessage platform_message = {};
  platform_message.struct_size = sizeof(FlutterPlatformMessage);
  platform_message.channel = "test_channel";
  platform_message.message = nullptr;
  platform_message.message_size = 1;
  platform_message.response_handle = nullptr;  // No response needed.

  auto result =
      FlutterEngineSendPlatformMessage(engine.get(), &platform_message);
  ASSERT_EQ(result, kInvalidArguments);
}

//------------------------------------------------------------------------------
/// Tests that setting a custom log callback works as expected and defaults to
/// using tag "flutter".
TEST_F(EmbedderTest, CanSetCustomLogMessageCallback) {
  fml::AutoResetWaitableEvent callback_latch;
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  builder.SetDartEntrypoint("custom_logger");
  context.SetLogMessageCallback(
      [&callback_latch](const char* tag, const char* message) {
        EXPECT_EQ(std::string(tag), "flutter");
        EXPECT_EQ(std::string(message), "hello world");
        callback_latch.Signal();
      });
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  callback_latch.Wait();
}

//------------------------------------------------------------------------------
/// Tests that setting a custom log tag works.
TEST_F(EmbedderTest, CanSetCustomLogTag) {
  fml::AutoResetWaitableEvent callback_latch;
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  builder.SetDartEntrypoint("custom_logger");
  builder.SetLogTag("butterfly");
  context.SetLogMessageCallback(
      [&callback_latch](const char* tag, const char* message) {
        EXPECT_EQ(std::string(tag), "butterfly");
        EXPECT_EQ(std::string(message), "hello world");
        callback_latch.Signal();
      });
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  callback_latch.Wait();
}

//------------------------------------------------------------------------------
/// Asserts behavior of FlutterProjectArgs::shutdown_dart_vm_when_done (which is
/// set to true by default in these unit-tests).
///
TEST_F(EmbedderTest, VMShutsDownWhenNoEnginesInProcess) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  const auto launch_count = DartVM::GetVMLaunchCount();

  {
    auto engine = builder.LaunchEngine();
    ASSERT_EQ(launch_count + 1u, DartVM::GetVMLaunchCount());
  }

  {
    auto engine = builder.LaunchEngine();
    ASSERT_EQ(launch_count + 2u, DartVM::GetVMLaunchCount());
  }
}

//------------------------------------------------------------------------------
///
TEST_F(EmbedderTest, DartEntrypointArgs) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  builder.AddDartEntrypointArgument("foo");
  builder.AddDartEntrypointArgument("bar");
  builder.SetDartEntrypoint("dart_entrypoint_args");
  fml::AutoResetWaitableEvent callback_latch;
  std::vector<std::string> callback_args;
  auto nativeArgumentsCallback = [&callback_args,
                                  &callback_latch](Dart_NativeArguments args) {
    Dart_Handle exception = nullptr;
    callback_args =
        tonic::DartConverter<std::vector<std::string>>::FromArguments(
            args, 0, exception);
    callback_latch.Signal();
  };
  context.AddNativeCallback("NativeArgumentsCallback",
                            CREATE_NATIVE_ENTRY(nativeArgumentsCallback));
  auto engine = builder.LaunchEngine();
  callback_latch.Wait();
  ASSERT_EQ(callback_args[0], "foo");
  ASSERT_EQ(callback_args[1], "bar");
}

//------------------------------------------------------------------------------
/// These snapshots may be materialized from symbols and the size field may not
/// be relevant. Since this information is redundant, engine launch should not
/// be gated on a non-zero buffer size.
///
TEST_F(EmbedderTest, VMAndIsolateSnapshotSizesAreRedundantInAOTMode) {
  if (!DartVM::IsRunningPrecompiledCode()) {
    GTEST_SKIP();
    return;
  }
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);

  // The fixture sets this up correctly. Intentionally mess up the args.
  builder.GetProjectArgs().vm_snapshot_data_size = 0;
  builder.GetProjectArgs().vm_snapshot_instructions_size = 0;
  builder.GetProjectArgs().isolate_snapshot_data_size = 0;
  builder.GetProjectArgs().isolate_snapshot_instructions_size = 0;

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
}

//------------------------------------------------------------------------------
/// Test that an engine can be initialized but not run.
///
TEST_F(EmbedderTest, CanCreateInitializedEngine) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  auto engine = builder.InitializeEngine();
  ASSERT_TRUE(engine.is_valid());
  engine.reset();
}

//------------------------------------------------------------------------------
/// Test that an initialized engine can be run exactly once.
///
TEST_F(EmbedderTest, CanRunInitializedEngine) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  auto engine = builder.InitializeEngine();
  ASSERT_TRUE(engine.is_valid());
  ASSERT_EQ(FlutterEngineRunInitialized(engine.get()), kSuccess);
  // Cannot re-run an already running engine.
  ASSERT_EQ(FlutterEngineRunInitialized(engine.get()), kInvalidArguments);
  engine.reset();
}

//------------------------------------------------------------------------------
/// Test that an engine can be deinitialized.
///
TEST_F(EmbedderTest, CanDeinitializeAnEngine) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  auto engine = builder.InitializeEngine();
  ASSERT_TRUE(engine.is_valid());
  ASSERT_EQ(FlutterEngineRunInitialized(engine.get()), kSuccess);
  // Cannot re-run an already running engine.
  ASSERT_EQ(FlutterEngineRunInitialized(engine.get()), kInvalidArguments);
  ASSERT_EQ(FlutterEngineDeinitialize(engine.get()), kSuccess);
  // It is ok to deinitialize an engine multiple times.
  ASSERT_EQ(FlutterEngineDeinitialize(engine.get()), kSuccess);

  engine.reset();
}

TEST_F(EmbedderTest, CanUpdateLocales) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  builder.SetDartEntrypoint("can_receive_locale_updates");
  fml::AutoResetWaitableEvent latch;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&latch](Dart_NativeArguments args) { latch.Signal(); }));

  fml::AutoResetWaitableEvent check_latch;
  context.AddNativeCallback(
      "SignalNativeCount",
      CREATE_NATIVE_ENTRY([&check_latch](Dart_NativeArguments args) {
        ASSERT_EQ(tonic::DartConverter<int>::FromDart(
                      Dart_GetNativeArgument(args, 0)),
                  2);
        check_latch.Signal();
      }));

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  // Wait for the application to attach the listener.
  latch.Wait();

  FlutterLocale locale1 = {};
  locale1.struct_size = sizeof(locale1);
  locale1.language_code = "";  // invalid
  locale1.country_code = "US";
  locale1.script_code = "";
  locale1.variant_code = nullptr;

  FlutterLocale locale2 = {};
  locale2.struct_size = sizeof(locale2);
  locale2.language_code = "zh";
  locale2.country_code = "CN";
  locale2.script_code = "Hans";
  locale2.variant_code = nullptr;

  std::vector<const FlutterLocale*> locales;
  locales.push_back(&locale1);
  locales.push_back(&locale2);

  ASSERT_EQ(
      FlutterEngineUpdateLocales(engine.get(), locales.data(), locales.size()),
      kInvalidArguments);

  // Fix the invalid code.
  locale1.language_code = "en";

  ASSERT_EQ(
      FlutterEngineUpdateLocales(engine.get(), locales.data(), locales.size()),
      kSuccess);

  check_latch.Wait();
}

inline flutter::EmbedderEngine* ToEmbedderEngine(const FlutterEngine& engine) {
  return reinterpret_cast<flutter::EmbedderEngine*>(engine);
}

TEST_F(EmbedderTest, LocalizationCallbacksCalled) {
  auto& context = GetEmbedderContext();
  fml::AutoResetWaitableEvent latch;
  context.AddIsolateCreateCallback([&latch]() { latch.Signal(); });
  EmbedderConfigBuilder builder(context);
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Wait for the root isolate to launch.
  latch.Wait();

  flutter::Shell& shell = ToEmbedderEngine(engine.get())->GetShell();
  std::vector<std::string> supported_locales;
  supported_locales.push_back("es");
  supported_locales.push_back("MX");
  supported_locales.push_back("");
  auto result = shell.GetPlatformView()->ComputePlatformResolvedLocales(
      supported_locales);

  ASSERT_EQ((*result).size(), supported_locales.size());  // 3
  ASSERT_EQ((*result)[0], supported_locales[0]);
  ASSERT_EQ((*result)[1], supported_locales[1]);
  ASSERT_EQ((*result)[2], supported_locales[2]);

  engine.reset();
}

TEST_F(EmbedderTest, CanQueryDartAOTMode) {
  ASSERT_EQ(FlutterEngineRunsAOTCompiledDartCode(),
            flutter::DartVM::IsRunningPrecompiledCode());
}

TEST_F(EmbedderTest, CanSendLowMemoryNotification) {
  auto& context = GetEmbedderContext();

  EmbedderConfigBuilder builder(context);

  auto engine = builder.LaunchEngine();

  ASSERT_TRUE(engine.is_valid());

  // TODO(chinmaygarde): The shell ought to have a mechanism for notification
  // dispatch that engine subsystems can register handlers to. This would allow
  // the raster cache and the secondary context caches to respond to
  // notifications. Once that is in place, this test can be updated to actually
  // ensure that the dispatched message is visible to engine subsystems.
  ASSERT_EQ(FlutterEngineNotifyLowMemoryWarning(engine.get()), kSuccess);
}

TEST_F(EmbedderTest, CanPostTaskToAllNativeThreads) {
  UniqueEngine engine;
  size_t worker_count = 0;
  fml::AutoResetWaitableEvent sync_latch;

  // One of the threads that the callback will be posted to is the platform
  // thread. So we cannot wait for assertions to complete on the platform
  // thread. Create a new thread to manage the engine instance and wait for
  // assertions on the test thread.
  auto platform_task_runner = CreateNewThread("platform_thread");

  platform_task_runner->PostTask([&]() {
    auto& context = GetEmbedderContext();
    EmbedderConfigBuilder builder(context);

    engine = builder.LaunchEngine();

    ASSERT_TRUE(engine.is_valid());

    worker_count = ToEmbedderEngine(engine.get())
                       ->GetShell()
                       .GetDartVM()
                       ->GetConcurrentMessageLoop()
                       ->GetWorkerCount();

    sync_latch.Signal();
  });

  sync_latch.Wait();

  const auto engine_threads_count = worker_count + 2u;

  struct Captures {
    // Waits the adequate number of callbacks to fire.
    fml::CountDownLatch latch;

    // This class will be accessed from multiple threads concurrently to track
    // thread specific information that is later checked. All updates to fields
    // in this struct must be made with this mutex acquired.

    std::mutex captures_mutex;
    // Ensures that the expect number of distinct threads were serviced.
    std::set<std::thread::id> thread_ids;

    size_t platform_threads_count = 0;
    size_t ui_threads_count = 0;
    size_t worker_threads_count = 0;

    explicit Captures(size_t count) : latch(count) {}
  };

  Captures captures(engine_threads_count);

  platform_task_runner->PostTask([&]() {
    ASSERT_EQ(FlutterEnginePostCallbackOnAllNativeThreads(
                  engine.get(),
                  [](FlutterNativeThreadType type, void* baton) {
                    auto captures = reinterpret_cast<Captures*>(baton);
                    {
                      std::scoped_lock lock(captures->captures_mutex);
                      switch (type) {
                        case kFlutterNativeThreadTypeWorker:
                          captures->worker_threads_count++;
                          break;
                        case kFlutterNativeThreadTypeUI:
                          captures->ui_threads_count++;
                          break;
                        case kFlutterNativeThreadTypePlatform:
                          captures->platform_threads_count++;
                          break;
                      }
                      captures->thread_ids.insert(std::this_thread::get_id());
                    }
                    captures->latch.CountDown();
                  },
                  &captures),
              kSuccess);
  });

  captures.latch.Wait();
  ASSERT_EQ(captures.thread_ids.size(), engine_threads_count);
  ASSERT_EQ(captures.platform_threads_count, 1u);
  ASSERT_EQ(captures.ui_threads_count, 1u);
  ASSERT_EQ(captures.worker_threads_count, worker_count);
  EXPECT_GE(captures.worker_threads_count - 1, 2u);
  EXPECT_LE(captures.worker_threads_count - 1, 4u);

  platform_task_runner->PostTask([&]() {
    engine.reset();
    sync_latch.Signal();
  });
  sync_latch.Wait();

  // The engine should have already been destroyed on the platform task runner.
  ASSERT_FALSE(engine.is_valid());
}

TEST_F(EmbedderTest, InvalidAOTDataSourcesMustReturnError) {
  if (!DartVM::IsRunningPrecompiledCode()) {
    GTEST_SKIP();
    return;
  }
  FlutterEngineAOTDataSource data_in = {};
  FlutterEngineAOTData data_out = nullptr;

  // Null source specified.
  ASSERT_EQ(FlutterEngineCreateAOTData(nullptr, &data_out), kInvalidArguments);
  ASSERT_EQ(data_out, nullptr);

  // Null data_out specified.
  ASSERT_EQ(FlutterEngineCreateAOTData(&data_in, nullptr), kInvalidArguments);

  // Invalid FlutterEngineAOTDataSourceType type specified.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  data_in.type = static_cast<FlutterEngineAOTDataSourceType>(-1);
  ASSERT_EQ(FlutterEngineCreateAOTData(&data_in, &data_out), kInvalidArguments);
  ASSERT_EQ(data_out, nullptr);

  // Invalid ELF path specified.
  data_in.type = kFlutterEngineAOTDataSourceTypeElfPath;
  data_in.elf_path = nullptr;
  ASSERT_EQ(FlutterEngineCreateAOTData(&data_in, &data_out), kInvalidArguments);
  ASSERT_EQ(data_in.type, kFlutterEngineAOTDataSourceTypeElfPath);
  ASSERT_EQ(data_in.elf_path, nullptr);
  ASSERT_EQ(data_out, nullptr);

  // Invalid ELF path specified.
  data_in.elf_path = "";
  ASSERT_EQ(FlutterEngineCreateAOTData(&data_in, &data_out), kInvalidArguments);
  ASSERT_EQ(data_in.type, kFlutterEngineAOTDataSourceTypeElfPath);
  ASSERT_EQ(data_in.elf_path, "");
  ASSERT_EQ(data_out, nullptr);

  // Could not find VM snapshot data.
  data_in.elf_path = "/bin/true";
  ASSERT_EQ(FlutterEngineCreateAOTData(&data_in, &data_out), kInvalidArguments);
  ASSERT_EQ(data_in.type, kFlutterEngineAOTDataSourceTypeElfPath);
  ASSERT_EQ(data_in.elf_path, "/bin/true");
  ASSERT_EQ(data_out, nullptr);
}

TEST_F(EmbedderTest, MustNotRunWithMultipleAOTSources) {
  if (!DartVM::IsRunningPrecompiledCode()) {
    GTEST_SKIP();
    return;
  }
  auto& context = GetEmbedderContext();

  EmbedderConfigBuilder builder(
      context,
      EmbedderConfigBuilder::InitializationPreference::kMultiAOTInitialize);

  auto engine = builder.LaunchEngine();
  ASSERT_FALSE(engine.is_valid());
}

TEST_F(EmbedderTest, CanCreateAndCollectAValidElfSource) {
  if (!DartVM::IsRunningPrecompiledCode()) {
    GTEST_SKIP();
    return;
  }
  FlutterEngineAOTDataSource data_in = {};
  FlutterEngineAOTData data_out = nullptr;

  // Collecting a null object should be allowed
  ASSERT_EQ(FlutterEngineCollectAOTData(data_out), kSuccess);

  const auto elf_path =
      fml::paths::JoinPaths({GetFixturesPath(), kDefaultAOTAppELFFileName});

  data_in.type = kFlutterEngineAOTDataSourceTypeElfPath;
  data_in.elf_path = elf_path.c_str();

  ASSERT_EQ(FlutterEngineCreateAOTData(&data_in, &data_out), kSuccess);
  ASSERT_EQ(data_in.type, kFlutterEngineAOTDataSourceTypeElfPath);
  ASSERT_EQ(data_in.elf_path, elf_path.c_str());
  ASSERT_NE(data_out, nullptr);

  ASSERT_EQ(FlutterEngineCollectAOTData(data_out), kSuccess);
}

TEST_F(EmbedderTest, CanLaunchAndShutdownWithAValidElfSource) {
  if (!DartVM::IsRunningPrecompiledCode()) {
    GTEST_SKIP();
    return;
  }
  auto& context = GetEmbedderContext();

  fml::AutoResetWaitableEvent latch;
  context.AddIsolateCreateCallback([&latch]() { latch.Signal(); });

  EmbedderConfigBuilder builder(
      context,
      EmbedderConfigBuilder::InitializationPreference::kAOTDataInitialize);

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  // Wait for the root isolate to launch.
  latch.Wait();
  engine.reset();
}

#if defined(__clang_analyzer__)
#define TEST_VM_SNAPSHOT_DATA "vm_data"
#define TEST_VM_SNAPSHOT_INSTRUCTIONS "vm_instructions"
#define TEST_ISOLATE_SNAPSHOT_DATA "isolate_data"
#define TEST_ISOLATE_SNAPSHOT_INSTRUCTIONS "isolate_instructions"
#endif

//------------------------------------------------------------------------------
/// PopulateJITSnapshotMappingCallbacks should successfully change the callbacks
/// of the snapshots in the engine's settings when JIT snapshots are explicitly
/// defined.
///
TEST_F(EmbedderTest, CanSuccessfullyPopulateSpecificJITSnapshotCallbacks) {
// TODO(#107263): Inconsistent snapshot paths in the Linux Fuchsia FEMU test.
#if defined(OS_FUCHSIA)
  GTEST_SKIP() << "Inconsistent paths in Fuchsia.";
#else

  // This test is only relevant in JIT mode.
  if (DartVM::IsRunningPrecompiledCode()) {
    GTEST_SKIP();
    return;
  }

  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);

  // Construct the location of valid JIT snapshots.
  const std::string src_path = GetSourcePath();
  const std::string vm_snapshot_data =
      fml::paths::JoinPaths({src_path, TEST_VM_SNAPSHOT_DATA});
  const std::string vm_snapshot_instructions =
      fml::paths::JoinPaths({src_path, TEST_VM_SNAPSHOT_INSTRUCTIONS});
  const std::string isolate_snapshot_data =
      fml::paths::JoinPaths({src_path, TEST_ISOLATE_SNAPSHOT_DATA});
  const std::string isolate_snapshot_instructions =
      fml::paths::JoinPaths({src_path, TEST_ISOLATE_SNAPSHOT_INSTRUCTIONS});

  // Explicitly define the locations of the JIT snapshots
  builder.GetProjectArgs().vm_snapshot_data =
      reinterpret_cast<const uint8_t*>(vm_snapshot_data.c_str());
  builder.GetProjectArgs().vm_snapshot_instructions =
      reinterpret_cast<const uint8_t*>(vm_snapshot_instructions.c_str());
  builder.GetProjectArgs().isolate_snapshot_data =
      reinterpret_cast<const uint8_t*>(isolate_snapshot_data.c_str());
  builder.GetProjectArgs().isolate_snapshot_instructions =
      reinterpret_cast<const uint8_t*>(isolate_snapshot_instructions.c_str());

  auto engine = builder.LaunchEngine();

  flutter::Shell& shell = ToEmbedderEngine(engine.get())->GetShell();
  const Settings settings = shell.GetSettings();

  ASSERT_NE(settings.vm_snapshot_data(), nullptr);
  ASSERT_NE(settings.vm_snapshot_instr(), nullptr);
  ASSERT_NE(settings.isolate_snapshot_data(), nullptr);
  ASSERT_NE(settings.isolate_snapshot_instr(), nullptr);
  ASSERT_NE(settings.dart_library_sources_kernel(), nullptr);
#endif  // OS_FUCHSIA
}

//------------------------------------------------------------------------------
/// PopulateJITSnapshotMappingCallbacks should still be able to successfully
/// change the callbacks of the snapshots in the engine's settings when JIT
/// snapshots are explicitly defined. However, if those snapshot locations are
/// invalid, the callbacks should return a nullptr.
///
TEST_F(EmbedderTest, JITSnapshotCallbacksFailWithInvalidLocation) {
// TODO(#107263): Inconsistent snapshot paths in the Linux Fuchsia FEMU test.
#if defined(OS_FUCHSIA)
  GTEST_SKIP() << "Inconsistent paths in Fuchsia.";
#else

  // This test is only relevant in JIT mode.
  if (DartVM::IsRunningPrecompiledCode()) {
    GTEST_SKIP();
    return;
  }

  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);

  // Explicitly define the locations of the invalid JIT snapshots
  builder.GetProjectArgs().vm_snapshot_data =
      reinterpret_cast<const uint8_t*>("invalid_vm_data");
  builder.GetProjectArgs().vm_snapshot_instructions =
      reinterpret_cast<const uint8_t*>("invalid_vm_instructions");
  builder.GetProjectArgs().isolate_snapshot_data =
      reinterpret_cast<const uint8_t*>("invalid_snapshot_data");
  builder.GetProjectArgs().isolate_snapshot_instructions =
      reinterpret_cast<const uint8_t*>("invalid_snapshot_instructions");

  auto engine = builder.LaunchEngine();

  flutter::Shell& shell = ToEmbedderEngine(engine.get())->GetShell();
  const Settings settings = shell.GetSettings();

  ASSERT_EQ(settings.vm_snapshot_data(), nullptr);
  ASSERT_EQ(settings.vm_snapshot_instr(), nullptr);
  ASSERT_EQ(settings.isolate_snapshot_data(), nullptr);
  ASSERT_EQ(settings.isolate_snapshot_instr(), nullptr);
#endif  // OS_FUCHSIA
}

//------------------------------------------------------------------------------
/// The embedder must be able to run explicitly specified snapshots in JIT mode
/// (i.e. when those are present in known locations).
///
TEST_F(EmbedderTest, CanLaunchEngineWithSpecifiedJITSnapshots) {
  // This test is only relevant in JIT mode.
  if (DartVM::IsRunningPrecompiledCode()) {
    GTEST_SKIP();
    return;
  }

  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);

  // Construct the location of valid JIT snapshots.
  const std::string src_path = GetSourcePath();
  const std::string vm_snapshot_data =
      fml::paths::JoinPaths({src_path, TEST_VM_SNAPSHOT_DATA});
  const std::string vm_snapshot_instructions =
      fml::paths::JoinPaths({src_path, TEST_VM_SNAPSHOT_INSTRUCTIONS});
  const std::string isolate_snapshot_data =
      fml::paths::JoinPaths({src_path, TEST_ISOLATE_SNAPSHOT_DATA});
  const std::string isolate_snapshot_instructions =
      fml::paths::JoinPaths({src_path, TEST_ISOLATE_SNAPSHOT_INSTRUCTIONS});

  // Explicitly define the locations of the JIT snapshots
  builder.GetProjectArgs().vm_snapshot_data =
      reinterpret_cast<const uint8_t*>(vm_snapshot_data.c_str());
  builder.GetProjectArgs().vm_snapshot_instructions =
      reinterpret_cast<const uint8_t*>(vm_snapshot_instructions.c_str());
  builder.GetProjectArgs().isolate_snapshot_data =
      reinterpret_cast<const uint8_t*>(isolate_snapshot_data.c_str());
  builder.GetProjectArgs().isolate_snapshot_instructions =
      reinterpret_cast<const uint8_t*>(isolate_snapshot_instructions.c_str());

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
}

//------------------------------------------------------------------------------
/// The embedder must be able to run in JIT mode when only some snapshots are
/// specified.
///
TEST_F(EmbedderTest, CanLaunchEngineWithSomeSpecifiedJITSnapshots) {
  // This test is only relevant in JIT mode.
  if (DartVM::IsRunningPrecompiledCode()) {
    GTEST_SKIP();
    return;
  }

  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);

  // Construct the location of valid JIT snapshots.
  const std::string src_path = GetSourcePath();
  const std::string vm_snapshot_data =
      fml::paths::JoinPaths({src_path, TEST_VM_SNAPSHOT_DATA});
  const std::string vm_snapshot_instructions =
      fml::paths::JoinPaths({src_path, TEST_VM_SNAPSHOT_INSTRUCTIONS});

  // Explicitly define the locations of the JIT snapshots
  builder.GetProjectArgs().vm_snapshot_data =
      reinterpret_cast<const uint8_t*>(vm_snapshot_data.c_str());
  builder.GetProjectArgs().vm_snapshot_instructions =
      reinterpret_cast<const uint8_t*>(vm_snapshot_instructions.c_str());

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
}

//------------------------------------------------------------------------------
/// The embedder must be able to run in JIT mode even when the specfied
/// snapshots are invalid. It should be able to resolve them as it would when
/// the snapshots are not specified.
///
TEST_F(EmbedderTest, CanLaunchEngineWithInvalidJITSnapshots) {
  // This test is only relevant in JIT mode.
  if (DartVM::IsRunningPrecompiledCode()) {
    GTEST_SKIP();
    return;
  }

  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);

  // Explicitly define the locations of the JIT snapshots
  builder.GetProjectArgs().isolate_snapshot_data =
      reinterpret_cast<const uint8_t*>("invalid_snapshot_data");
  builder.GetProjectArgs().isolate_snapshot_instructions =
      reinterpret_cast<const uint8_t*>("invalid_snapshot_instructions");

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  ASSERT_EQ(FlutterEngineRunInitialized(engine.get()), kInvalidArguments);
}

//------------------------------------------------------------------------------
/// The embedder must be able to launch even when the snapshots are not
/// explicitly defined in JIT mode. It must be able to resolve those snapshots.
///
TEST_F(EmbedderTest, CanLaunchEngineWithUnspecifiedJITSnapshots) {
  // This test is only relevant in JIT mode.
  if (DartVM::IsRunningPrecompiledCode()) {
    GTEST_SKIP();
    return;
  }

  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);

  ASSERT_EQ(builder.GetProjectArgs().vm_snapshot_data, nullptr);
  ASSERT_EQ(builder.GetProjectArgs().vm_snapshot_instructions, nullptr);
  ASSERT_EQ(builder.GetProjectArgs().isolate_snapshot_data, nullptr);
  ASSERT_EQ(builder.GetProjectArgs().isolate_snapshot_instructions, nullptr);

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
}

TEST_F(EmbedderTest, RegisterChannelListener) {
  auto& context = GetEmbedderContext();

  fml::AutoResetWaitableEvent latch;
  fml::AutoResetWaitableEvent latch2;
  bool listening = false;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY([&](Dart_NativeArguments) { latch.Signal(); }));
  context.SetChannelUpdateCallback([&](const FlutterChannelUpdate* update) {
    EXPECT_STREQ(update->channel, "test/listen");
    EXPECT_TRUE(update->listening);
    listening = true;
    latch2.Signal();
  });

  EmbedderConfigBuilder builder(context);
  builder.SetDartEntrypoint("channel_listener_response");

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  latch.Wait();
  // Drain tasks posted to platform thread task runner.
  fml::MessageLoop::GetCurrent().RunExpiredTasksNow();
  latch2.Wait();

  ASSERT_TRUE(listening);
}

}  // namespace testing
}  // namespace flutter

// NOLINTEND(clang-analyzer-core.StackAddressEscape)
