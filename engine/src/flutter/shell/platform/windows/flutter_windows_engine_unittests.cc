// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <thread>
#include "flutter/shell/platform/windows/flutter_windows_engine.h"

#include "flutter/shell/platform/embedder/embedder.h"
#include "flutter/shell/platform/embedder/test_utils/proc_table_replacement.h"
#include "flutter/shell/platform/windows/testing/engine_modifier.h"
#include "flutter/shell/platform/windows/testing/flutter_windows_engine_builder.h"
#include "flutter/shell/platform/windows/testing/mock_windows_proc_table.h"
#include "flutter/shell/platform/windows/testing/windows_test.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

// winbase.h defines GetCurrentTime as a macro.
#undef GetCurrentTime

namespace {
// Process the next win32 message if there is one. This can be used to
// pump the Windows platform thread task runner.
void PumpMessage() {
  ::MSG msg;
  if (::GetMessage(&msg, nullptr, 0, 0)) {
    ::TranslateMessage(&msg);
    ::DispatchMessage(&msg);
  }
}
}  // namespace

namespace flutter {
namespace testing {

using ::testing::_;
using ::testing::DoAll;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::SetArgPointee;

class FlutterWindowsEngineTest : public WindowsTest {};

// The engine can be run without any views.
TEST_F(FlutterWindowsEngineTest, RunHeadless) {
  FlutterWindowsEngineBuilder builder{GetContext()};
  std::unique_ptr<FlutterWindowsEngine> engine = builder.Build();

  EngineModifier modifier(engine.get());
  modifier.embedder_api().RunsAOTCompiledDartCode = []() { return false; };

  ASSERT_TRUE(engine->Run());
  ASSERT_NE(engine->messenger(), nullptr);
}

TEST_F(FlutterWindowsEngineTest, TaskRunnerDelayedTask) {
  bool finished = false;
  auto runner = std::make_unique<TaskRunner>(
      [] {
        return static_cast<uint64_t>(
            fml::TimePoint::Now().ToEpochDelta().ToNanoseconds());
      },
      [&](const FlutterTask*) { finished = true; });
  runner->PostFlutterTask(
      FlutterTask{},
      static_cast<uint64_t>((fml::TimePoint::Now().ToEpochDelta() +
                             fml::TimeDelta::FromMilliseconds(50))
                                .ToNanoseconds()));
  auto start = fml::TimePoint::Now();
  while (!finished) {
    PumpMessage();
  }
  auto duration = fml::TimePoint::Now() - start;
  EXPECT_GE(duration, fml::TimeDelta::FromMilliseconds(50));
}

// https://github.com/flutter/flutter/issues/173843)
TEST_F(FlutterWindowsEngineTest, TaskRunnerDoesNotDeadlock) {
  auto runner = std::make_unique<TaskRunner>(
      [] {
        return static_cast<uint64_t>(
            fml::TimePoint::Now().ToEpochDelta().ToNanoseconds());
      },
      [&](const FlutterTask*) {});

  struct RunnerHolder {
    void PostTaskLoop() {
      runner->PostTask([this] { PostTaskLoop(); });
    }
    std::unique_ptr<TaskRunner> runner;
  };

  RunnerHolder container{.runner = std::move(runner)};
  // Spam flutter tasks.
  container.PostTaskLoop();

  const LPCWSTR class_name = L"FlutterTestWindowClass";
  WNDCLASS wc = {0};
  wc.lpfnWndProc = DefWindowProc;
  wc.lpszClassName = class_name;
  RegisterClass(&wc);

  HWND window;
  container.runner->PostTask([&] {
    window = CreateWindowEx(0, class_name, L"Empty Window", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, nullptr,
                            nullptr, nullptr, nullptr);
    ShowWindow(window, SW_SHOW);
  });

  while (true) {
    ::MSG msg;
    if (::GetMessage(&msg, nullptr, 0, 0)) {
      if (msg.message == WM_PAINT) {
        break;
      }
      ::TranslateMessage(&msg);
      ::DispatchMessage(&msg);
    }
  }

  DestroyWindow(window);
  UnregisterClassW(class_name, nullptr);
}

TEST_F(FlutterWindowsEngineTest, RunDoesExpectedInitialization) {
  FlutterWindowsEngineBuilder builder{GetContext()};
  builder.AddDartEntrypointArgument("arg1");
  builder.AddDartEntrypointArgument("arg2");

  auto windows_proc_table = std::make_shared<MockWindowsProcTable>();

  // Mock locale information
  EXPECT_CALL(*windows_proc_table, GetThreadPreferredUILanguages(_, _, _, _))
      .WillRepeatedly(
          [](DWORD flags, PULONG count, PZZWSTR languages, PULONG length) {
            // We need to mock the locale information twice because the first
            // call is to get the size and the second call is to fill the
            // buffer.
            if (languages == nullptr) {
              // First call is to get the size
              *count = 1;    // One language
              *length = 10;  // "fr-FR\0\0" (double null-terminated)
              return TRUE;
            } else {
              // Second call is to fill the buffer
              *count = 1;
              // Fill with "fr-FR\0\0" (double null-terminated)
              wchar_t* lang_buffer = languages;
              wcscpy(lang_buffer, L"fr-FR");
              // Move past the first null terminator to add the second
              lang_buffer += wcslen(L"fr-FR") + 1;
              *lang_buffer = L'\0';
              return TRUE;
            }
          });

  builder.SetWindowsProcTable(windows_proc_table);

  std::unique_ptr<FlutterWindowsEngine> engine = builder.Build();
  EngineModifier modifier(engine.get());

  // The engine should be run with expected configuration values.
  bool run_called = false;
  modifier.embedder_api().Run = MOCK_ENGINE_PROC(
      Run, ([&run_called, engine_instance = engine.get()](
                size_t version, const FlutterProjectArgs* args, void* user_data,
                FLUTTER_API_SYMBOL(FlutterEngine) * engine_out) {
        run_called = true;
        *engine_out = reinterpret_cast<FLUTTER_API_SYMBOL(FlutterEngine)>(1);

        EXPECT_EQ(version, FLUTTER_ENGINE_VERSION);
        // We have an EGL manager, so this should be using OpenGL.
        EXPECT_EQ(user_data, engine_instance);
        // Spot-check arguments.
        EXPECT_NE(args->assets_path, nullptr);
        EXPECT_NE(args->icu_data_path, nullptr);
        EXPECT_EQ(args->dart_entrypoint_argc, 2U);
        EXPECT_EQ(strcmp(args->dart_entrypoint_argv[0], "arg1"), 0);
        EXPECT_EQ(strcmp(args->dart_entrypoint_argv[1], "arg2"), 0);
        EXPECT_NE(args->platform_message_callback, nullptr);
        EXPECT_NE(args->custom_task_runners, nullptr);
        EXPECT_NE(args->custom_task_runners->thread_priority_setter, nullptr);
        EXPECT_EQ(args->custom_dart_entrypoint, nullptr);

        return kSuccess;
      }));

  // It should send locale info.
  bool update_locales_called = false;
  modifier.embedder_api().UpdateLocales = MOCK_ENGINE_PROC(
      UpdateLocales,
      ([&update_locales_called](auto engine, const FlutterLocale** locales,
                                size_t locales_count) {
        update_locales_called = true;

        EXPECT_GT(locales_count, 0);
        EXPECT_NE(locales, nullptr);

        return kSuccess;
      }));

  engine->Run();

  EXPECT_TRUE(run_called);
  EXPECT_TRUE(update_locales_called);

  // Ensure that deallocation doesn't call the actual Shutdown with the bogus
  // engine pointer that the overridden Run returned.
  modifier.embedder_api().Shutdown = [](auto engine) { return kSuccess; };
}

TEST_F(FlutterWindowsEngineTest, SendPlatformMessageWithoutResponse) {
  FlutterWindowsEngineBuilder builder{GetContext()};
  std::unique_ptr<FlutterWindowsEngine> engine = builder.Build();
  EngineModifier modifier(engine.get());

  const char* channel = "test";
  const std::vector<uint8_t> test_message = {1, 2, 3, 4};

  // Without a response, SendPlatformMessage should be a simple pass-through.
  bool called = false;
  modifier.embedder_api().SendPlatformMessage = MOCK_ENGINE_PROC(
      SendPlatformMessage, ([&called, test_message](auto engine, auto message) {
        called = true;
        EXPECT_STREQ(message->channel, "test");
        EXPECT_EQ(message->message_size, test_message.size());
        EXPECT_EQ(memcmp(message->message, test_message.data(),
                         message->message_size),
                  0);
        EXPECT_EQ(message->response_handle, nullptr);
        return kSuccess;
      }));

  engine->SendPlatformMessage(channel, test_message.data(), test_message.size(),
                              nullptr, nullptr);
  EXPECT_TRUE(called);
}

TEST_F(FlutterWindowsEngineTest, PlatformMessageRoundTrip) {
  FlutterWindowsEngineBuilder builder{GetContext()};
  builder.SetDartEntrypoint("hiPlatformChannels");

  std::unique_ptr<FlutterWindowsEngine> engine = builder.Build();
  EngineModifier modifier(engine.get());
  modifier.embedder_api().RunsAOTCompiledDartCode = []() { return false; };

  auto binary_messenger =
      std::make_unique<BinaryMessengerImpl>(engine->messenger());

  engine->Run();
  bool did_call_callback = false;
  bool did_call_reply = false;
  bool did_call_dart_reply = false;
  std::string channel = "hi";
  binary_messenger->SetMessageHandler(
      channel,
      [&did_call_callback, &did_call_dart_reply](
          const uint8_t* message, size_t message_size, BinaryReply reply) {
        if (message_size == 5) {
          EXPECT_EQ(message[0], static_cast<uint8_t>('h'));
          char response[] = {'b', 'y', 'e'};
          reply(reinterpret_cast<uint8_t*>(response), 3);
          did_call_callback = true;
        } else {
          EXPECT_EQ(message_size, 3);
          EXPECT_EQ(message[0], static_cast<uint8_t>('b'));
          did_call_dart_reply = true;
        }
      });
  char payload[] = {'h', 'e', 'l', 'l', 'o'};
  binary_messenger->Send(
      channel, reinterpret_cast<uint8_t*>(payload), 5,
      [&did_call_reply](const uint8_t* reply, size_t reply_size) {
        EXPECT_EQ(reply_size, 5);
        EXPECT_EQ(reply[0], static_cast<uint8_t>('h'));
        did_call_reply = true;
      });
  // Rely on timeout mechanism in CI.
  while (!did_call_callback || !did_call_reply || !did_call_dart_reply) {
    engine->task_runner()->ProcessTasks();
  }
}

TEST_F(FlutterWindowsEngineTest, PlatformMessageRespondOnDifferentThread) {
  FlutterWindowsEngineBuilder builder{GetContext()};
  builder.SetDartEntrypoint("hiPlatformChannels");

  std::unique_ptr<FlutterWindowsEngine> engine = builder.Build();

  EngineModifier modifier(engine.get());
  modifier.embedder_api().RunsAOTCompiledDartCode = []() { return false; };

  auto binary_messenger =
      std::make_unique<BinaryMessengerImpl>(engine->messenger());

  engine->Run();
  bool did_call_callback = false;
  bool did_call_reply = false;
  bool did_call_dart_reply = false;
  std::string channel = "hi";
  std::unique_ptr<std::thread> reply_thread;
  binary_messenger->SetMessageHandler(
      channel,
      [&did_call_callback, &did_call_dart_reply, &reply_thread](
          const uint8_t* message, size_t message_size, BinaryReply reply) {
        if (message_size == 5) {
          EXPECT_EQ(message[0], static_cast<uint8_t>('h'));
          reply_thread.reset(new std::thread([reply = std::move(reply)]() {
            char response[] = {'b', 'y', 'e'};
            reply(reinterpret_cast<uint8_t*>(response), 3);
          }));
          did_call_callback = true;
        } else {
          EXPECT_EQ(message_size, 3);
          EXPECT_EQ(message[0], static_cast<uint8_t>('b'));
          did_call_dart_reply = true;
        }
      });
  char payload[] = {'h', 'e', 'l', 'l', 'o'};
  binary_messenger->Send(
      channel, reinterpret_cast<uint8_t*>(payload), 5,
      [&did_call_reply](const uint8_t* reply, size_t reply_size) {
        EXPECT_EQ(reply_size, 5);
        EXPECT_EQ(reply[0], static_cast<uint8_t>('h'));
        did_call_reply = true;
      });
  // Rely on timeout mechanism in CI.
  while (!did_call_callback || !did_call_reply || !did_call_dart_reply) {
    engine->task_runner()->ProcessTasks();
  }
  ASSERT_TRUE(reply_thread);
  reply_thread->join();
}

TEST_F(FlutterWindowsEngineTest, SendPlatformMessageWithResponse) {
  FlutterWindowsEngineBuilder builder{GetContext()};
  std::unique_ptr<FlutterWindowsEngine> engine = builder.Build();
  EngineModifier modifier(engine.get());

  const char* channel = "test";
  const std::vector<uint8_t> test_message = {1, 2, 3, 4};
  auto* dummy_response_handle =
      reinterpret_cast<FlutterPlatformMessageResponseHandle*>(5);
  const FlutterDesktopBinaryReply reply_handler = [](auto... args) {};
  void* reply_user_data = reinterpret_cast<void*>(6);

  // When a response is requested, a handle should be created, passed as part
  // of the message, and then released.
  bool create_response_handle_called = false;
  modifier.embedder_api().PlatformMessageCreateResponseHandle =
      MOCK_ENGINE_PROC(
          PlatformMessageCreateResponseHandle,
          ([&create_response_handle_called, &reply_handler, reply_user_data,
            dummy_response_handle](auto engine, auto reply, auto user_data,
                                   auto response_handle) {
            create_response_handle_called = true;
            EXPECT_EQ(reply, reply_handler);
            EXPECT_EQ(user_data, reply_user_data);
            EXPECT_NE(response_handle, nullptr);
            *response_handle = dummy_response_handle;
            return kSuccess;
          }));
  bool release_response_handle_called = false;
  modifier.embedder_api().PlatformMessageReleaseResponseHandle =
      MOCK_ENGINE_PROC(
          PlatformMessageReleaseResponseHandle,
          ([&release_response_handle_called, dummy_response_handle](
               auto engine, auto response_handle) {
            release_response_handle_called = true;
            EXPECT_EQ(response_handle, dummy_response_handle);
            return kSuccess;
          }));
  bool send_message_called = false;
  modifier.embedder_api().SendPlatformMessage = MOCK_ENGINE_PROC(
      SendPlatformMessage, ([&send_message_called, test_message,
                             dummy_response_handle](auto engine, auto message) {
        send_message_called = true;
        EXPECT_STREQ(message->channel, "test");
        EXPECT_EQ(message->message_size, test_message.size());
        EXPECT_EQ(memcmp(message->message, test_message.data(),
                         message->message_size),
                  0);
        EXPECT_EQ(message->response_handle, dummy_response_handle);
        return kSuccess;
      }));

  engine->SendPlatformMessage(channel, test_message.data(), test_message.size(),
                              reply_handler, reply_user_data);
  EXPECT_TRUE(create_response_handle_called);
  EXPECT_TRUE(release_response_handle_called);
  EXPECT_TRUE(send_message_called);
}

TEST_F(FlutterWindowsEngineTest, SetsThreadPriority) {
  WindowsPlatformThreadPrioritySetter(FlutterThreadPriority::kBackground);
  EXPECT_EQ(GetThreadPriority(GetCurrentThread()),
            THREAD_PRIORITY_BELOW_NORMAL);

  WindowsPlatformThreadPrioritySetter(FlutterThreadPriority::kDisplay);
  EXPECT_EQ(GetThreadPriority(GetCurrentThread()),
            THREAD_PRIORITY_ABOVE_NORMAL);

  // FlutterThreadPriority::kNormal does not change thread priority, reset to 0
  // here.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);

  WindowsPlatformThreadPrioritySetter(FlutterThreadPriority::kNormal);
  EXPECT_EQ(GetThreadPriority(GetCurrentThread()), THREAD_PRIORITY_NORMAL);
}

TEST_F(FlutterWindowsEngineTest, GetExecutableName) {
  FlutterWindowsEngineBuilder builder{GetContext()};
  std::unique_ptr<FlutterWindowsEngine> engine = builder.Build();
  EXPECT_EQ(engine->GetExecutableName(), "flutter_windows_unittests.exe");
}

}  // namespace testing
}  // namespace flutter
