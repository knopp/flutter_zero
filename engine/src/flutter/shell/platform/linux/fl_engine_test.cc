// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Included first as it collides with the X11 headers.
#include "gtest/gtest.h"

#include "flutter/shell/platform/embedder/test_utils/proc_table_replacement.h"
#include "flutter/shell/platform/linux/fl_engine_private.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_engine.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_json_message_codec.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_string_codec.h"

// MOCK_ENGINE_PROC is leaky by design
// NOLINTBEGIN(clang-analyzer-core.StackAddressEscape)

// Checks sending platform messages works.
TEST(FlEngineTest, PlatformMessage) {
  g_autoptr(FlDartProject) project = fl_dart_project_new();
  g_autoptr(FlEngine) engine = fl_engine_new(project);

  bool called = false;
  FlutterEngineSendPlatformMessageFnPtr old_handler =
      fl_engine_get_embedder_api(engine)->SendPlatformMessage;
  fl_engine_get_embedder_api(engine)->SendPlatformMessage = MOCK_ENGINE_PROC(
      SendPlatformMessage,
      ([&called, old_handler](auto engine,
                              const FlutterPlatformMessage* message) {
        if (strcmp(message->channel, "test") != 0) {
          return old_handler(engine, message);
        }

        called = true;

        EXPECT_EQ(message->message_size, static_cast<size_t>(4));
        EXPECT_EQ(message->message[0], 't');
        EXPECT_EQ(message->message[1], 'e');
        EXPECT_EQ(message->message[2], 's');
        EXPECT_EQ(message->message[3], 't');

        return kSuccess;
      }));

  g_autoptr(GError) error = nullptr;
  EXPECT_TRUE(fl_engine_start(engine, &error));
  EXPECT_EQ(error, nullptr);
  g_autoptr(GBytes) message = g_bytes_new_static("test", 4);
  fl_engine_send_platform_message(engine, "test", message, nullptr, nullptr,
                                  nullptr);

  EXPECT_TRUE(called);
}

// Checks sending platform message responses works.
TEST(FlEngineTest, PlatformMessageResponse) {
  g_autoptr(FlDartProject) project = fl_dart_project_new();
  g_autoptr(FlEngine) engine = fl_engine_new(project);

  bool called = false;
  fl_engine_get_embedder_api(engine)->SendPlatformMessageResponse =
      MOCK_ENGINE_PROC(
          SendPlatformMessageResponse,
          ([&called](auto engine,
                     const FlutterPlatformMessageResponseHandle* handle,
                     const uint8_t* data, size_t data_length) {
            called = true;

            EXPECT_EQ(
                handle,
                reinterpret_cast<const FlutterPlatformMessageResponseHandle*>(
                    42));
            EXPECT_EQ(data_length, static_cast<size_t>(4));
            EXPECT_EQ(data[0], 't');
            EXPECT_EQ(data[1], 'e');
            EXPECT_EQ(data[2], 's');
            EXPECT_EQ(data[3], 't');

            return kSuccess;
          }));

  g_autoptr(GError) error = nullptr;
  EXPECT_TRUE(fl_engine_start(engine, &error));
  EXPECT_EQ(error, nullptr);
  g_autoptr(GBytes) response = g_bytes_new_static("test", 4);
  EXPECT_TRUE(fl_engine_send_platform_message_response(
      engine, reinterpret_cast<const FlutterPlatformMessageResponseHandle*>(42),
      response, &error));
  EXPECT_EQ(error, nullptr);

  EXPECT_TRUE(called);
}

void on_pre_engine_restart_cb(FlEngine* engine, gpointer user_data) {
  int* count = reinterpret_cast<int*>(user_data);
  *count += 1;
}

// Checks restarting the engine invokes the correct callback.
TEST(FlEngineTest, OnPreEngineRestart) {
  g_autoptr(FlDartProject) project = fl_dart_project_new();
  g_autoptr(FlEngine) engine = fl_engine_new(project);

  OnPreEngineRestartCallback callback;
  void* callback_user_data;

  bool called = false;
  fl_engine_get_embedder_api(engine)->Initialize = MOCK_ENGINE_PROC(
      Initialize,
      ([&callback, &callback_user_data, &called](
           size_t version, const FlutterProjectArgs* args, void* user_data,
           FLUTTER_API_SYMBOL(FlutterEngine) * engine_out) {
        called = true;
        callback = args->on_pre_engine_restart_callback;
        callback_user_data = user_data;

        return kSuccess;
      }));
  fl_engine_get_embedder_api(engine)->RunInitialized =
      MOCK_ENGINE_PROC(RunInitialized, ([](auto engine) { return kSuccess; }));

  g_autoptr(GError) error = nullptr;
  EXPECT_TRUE(fl_engine_start(engine, &error));
  EXPECT_EQ(error, nullptr);

  EXPECT_TRUE(called);
  EXPECT_NE(callback, nullptr);

  // The following call has no effect but should not crash.
  callback(callback_user_data);

  int count = 0;

  // Set handler so that:
  //
  //  * When the engine restarts, count += 1;
  //  * When the engine is freed, count += 10.
  g_signal_connect(engine, "on-pre-engine-restart",
                   G_CALLBACK(on_pre_engine_restart_cb), &count);

  callback(callback_user_data);
  EXPECT_EQ(count, 1);
}

TEST(FlEngineTest, DartEntrypointArgs) {
  GPtrArray* args_array = g_ptr_array_new();
  g_ptr_array_add(args_array, const_cast<char*>("arg_one"));
  g_ptr_array_add(args_array, const_cast<char*>("arg_two"));
  g_ptr_array_add(args_array, const_cast<char*>("arg_three"));
  g_ptr_array_add(args_array, nullptr);
  gchar** args = reinterpret_cast<gchar**>(g_ptr_array_free(args_array, false));

  g_autoptr(FlDartProject) project = fl_dart_project_new();
  fl_dart_project_set_dart_entrypoint_arguments(project, args);
  g_autoptr(FlEngine) engine = fl_engine_new(project);

  bool called = false;
  fl_engine_get_embedder_api(engine)->Initialize = MOCK_ENGINE_PROC(
      Initialize,
      ([&called, &set_args = args](
           size_t version, const FlutterProjectArgs* args, void* user_data,
           FLUTTER_API_SYMBOL(FlutterEngine) * engine_out) {
        called = true;
        EXPECT_NE(set_args, args->dart_entrypoint_argv);
        EXPECT_EQ(args->dart_entrypoint_argc, 3);

        return kSuccess;
      }));
  fl_engine_get_embedder_api(engine)->RunInitialized =
      MOCK_ENGINE_PROC(RunInitialized, ([](auto engine) { return kSuccess; }));

  g_autoptr(GError) error = nullptr;
  EXPECT_TRUE(fl_engine_start(engine, &error));
  EXPECT_EQ(error, nullptr);

  EXPECT_TRUE(called);
}

TEST(FlEngineTest, EngineId) {
  g_autoptr(FlDartProject) project = fl_dart_project_new();
  g_autoptr(FlEngine) engine = fl_engine_new(project);
  int64_t engine_id;
  fl_engine_get_embedder_api(engine)->Initialize = MOCK_ENGINE_PROC(
      Initialize,
      ([&engine_id](size_t version, const FlutterProjectArgs* args,
                    void* user_data,
                    FLUTTER_API_SYMBOL(FlutterEngine) * engine_out) {
        engine_id = args->engine_id;
        return kSuccess;
      }));
  fl_engine_get_embedder_api(engine)->RunInitialized =
      MOCK_ENGINE_PROC(RunInitialized, ([](auto engine) { return kSuccess; }));

  g_autoptr(GError) error = nullptr;
  EXPECT_TRUE(fl_engine_start(engine, &error));
  EXPECT_EQ(error, nullptr);
  EXPECT_TRUE(engine_id != 0);

  EXPECT_EQ(fl_engine_for_id(engine_id), engine);
}

TEST(FlEngineTest, UIIsolateDefaultThreadPolicy) {
  g_autoptr(FlDartProject) project = fl_dart_project_new();
  g_autoptr(FlEngine) engine = fl_engine_new(project);
  fl_dart_project_set_ui_thread_policy(project, FL_UI_THREAD_POLICY_DEFAULT);

  bool same_task_runner = false;

  fl_engine_get_embedder_api(engine)->Initialize = MOCK_ENGINE_PROC(
      Initialize,
      ([&same_task_runner](size_t version, const FlutterProjectArgs* args,
                           void* user_data,
                           FLUTTER_API_SYMBOL(FlutterEngine) * engine_out) {
        same_task_runner = args->custom_task_runners->platform_task_runner ==
                           args->custom_task_runners->ui_task_runner;
        return kSuccess;
      }));
  fl_engine_get_embedder_api(engine)->RunInitialized =
      MOCK_ENGINE_PROC(RunInitialized, ([](auto engine) { return kSuccess; }));

  g_autoptr(GError) error = nullptr;
  EXPECT_TRUE(fl_engine_start(engine, &error));
  EXPECT_EQ(error, nullptr);
  EXPECT_TRUE(same_task_runner);
}

TEST(FlEngineTest, UIIsolateOnPlatformTaskRunner) {
  g_autoptr(FlDartProject) project = fl_dart_project_new();
  g_autoptr(FlEngine) engine = fl_engine_new(project);
  fl_dart_project_set_ui_thread_policy(
      project, FL_UI_THREAD_POLICY_RUN_ON_PLATFORM_THREAD);

  bool same_task_runner = false;

  fl_engine_get_embedder_api(engine)->Initialize = MOCK_ENGINE_PROC(
      Initialize,
      ([&same_task_runner](size_t version, const FlutterProjectArgs* args,
                           void* user_data,
                           FLUTTER_API_SYMBOL(FlutterEngine) * engine_out) {
        same_task_runner = args->custom_task_runners->platform_task_runner ==
                           args->custom_task_runners->ui_task_runner;
        return kSuccess;
      }));
  fl_engine_get_embedder_api(engine)->RunInitialized =
      MOCK_ENGINE_PROC(RunInitialized, ([](auto engine) { return kSuccess; }));

  g_autoptr(GError) error = nullptr;
  EXPECT_TRUE(fl_engine_start(engine, &error));
  EXPECT_EQ(error, nullptr);
  EXPECT_TRUE(same_task_runner);
}

TEST(FlEngineTest, UIIsolateOnSeparateThread) {
  g_autoptr(FlDartProject) project = fl_dart_project_new();
  g_autoptr(FlEngine) engine = fl_engine_new(project);
  fl_dart_project_set_ui_thread_policy(
      project, FL_UI_THREAD_POLICY_RUN_ON_SEPARATE_THREAD);

  bool separate_thread = false;

  fl_engine_get_embedder_api(engine)->Initialize = MOCK_ENGINE_PROC(
      Initialize,
      ([&separate_thread](size_t version, const FlutterProjectArgs* args,
                          void* user_data,
                          FLUTTER_API_SYMBOL(FlutterEngine) * engine_out) {
        separate_thread = args->custom_task_runners->ui_task_runner == nullptr;
        return kSuccess;
      }));
  fl_engine_get_embedder_api(engine)->RunInitialized =
      MOCK_ENGINE_PROC(RunInitialized, ([](auto engine) { return kSuccess; }));

  g_autoptr(GError) error = nullptr;
  EXPECT_TRUE(fl_engine_start(engine, &error));
  EXPECT_EQ(error, nullptr);
  EXPECT_TRUE(separate_thread);
}

TEST(FlEngineTest, Locales) {
  g_autofree gchar* initial_language = g_strdup(g_getenv("LANGUAGE"));
  g_setenv("LANGUAGE", "de:en_US", TRUE);
  g_autoptr(FlDartProject) project = fl_dart_project_new();

  g_autoptr(FlEngine) engine = fl_engine_new(project);

  bool called = false;
  fl_engine_get_embedder_api(engine)->UpdateLocales = MOCK_ENGINE_PROC(
      UpdateLocales, ([&called](auto engine, const FlutterLocale** locales,
                                size_t locales_count) {
        called = true;

        EXPECT_EQ(locales_count, static_cast<size_t>(4));

        EXPECT_STREQ(locales[0]->language_code, "de");
        EXPECT_STREQ(locales[0]->country_code, nullptr);
        EXPECT_STREQ(locales[0]->script_code, nullptr);
        EXPECT_STREQ(locales[0]->variant_code, nullptr);

        EXPECT_STREQ(locales[1]->language_code, "en");
        EXPECT_STREQ(locales[1]->country_code, "US");
        EXPECT_STREQ(locales[1]->script_code, nullptr);
        EXPECT_STREQ(locales[1]->variant_code, nullptr);

        EXPECT_STREQ(locales[2]->language_code, "en");
        EXPECT_STREQ(locales[2]->country_code, nullptr);
        EXPECT_STREQ(locales[2]->script_code, nullptr);
        EXPECT_STREQ(locales[2]->variant_code, nullptr);

        EXPECT_STREQ(locales[3]->language_code, "C");
        EXPECT_STREQ(locales[3]->country_code, nullptr);
        EXPECT_STREQ(locales[3]->script_code, nullptr);
        EXPECT_STREQ(locales[3]->variant_code, nullptr);

        return kSuccess;
      }));

  g_autoptr(GError) error = nullptr;
  EXPECT_TRUE(fl_engine_start(engine, &error));
  EXPECT_EQ(error, nullptr);

  EXPECT_TRUE(called);

  if (initial_language) {
    g_setenv("LANGUAGE", initial_language, TRUE);
  } else {
    g_unsetenv("LANGUAGE");
  }
}

TEST(FlEngineTest, CLocale) {
  g_autofree gchar* initial_language = g_strdup(g_getenv("LANGUAGE"));
  g_setenv("LANGUAGE", "C", TRUE);
  g_autoptr(FlDartProject) project = fl_dart_project_new();

  g_autoptr(FlEngine) engine = fl_engine_new(project);

  bool called = false;
  fl_engine_get_embedder_api(engine)->UpdateLocales = MOCK_ENGINE_PROC(
      UpdateLocales, ([&called](auto engine, const FlutterLocale** locales,
                                size_t locales_count) {
        called = true;

        EXPECT_EQ(locales_count, static_cast<size_t>(1));

        EXPECT_STREQ(locales[0]->language_code, "C");
        EXPECT_STREQ(locales[0]->country_code, nullptr);
        EXPECT_STREQ(locales[0]->script_code, nullptr);
        EXPECT_STREQ(locales[0]->variant_code, nullptr);

        return kSuccess;
      }));

  g_autoptr(GError) error = nullptr;
  EXPECT_TRUE(fl_engine_start(engine, &error));
  EXPECT_EQ(error, nullptr);

  EXPECT_TRUE(called);

  if (initial_language) {
    g_setenv("LANGUAGE", initial_language, TRUE);
  } else {
    g_unsetenv("LANGUAGE");
  }
}

TEST(FlEngineTest, DuplicateLocale) {
  g_autofree gchar* initial_language = g_strdup(g_getenv("LANGUAGE"));
  g_setenv("LANGUAGE", "en:en", TRUE);
  g_autoptr(FlDartProject) project = fl_dart_project_new();

  g_autoptr(FlEngine) engine = fl_engine_new(project);

  bool called = false;
  fl_engine_get_embedder_api(engine)->UpdateLocales = MOCK_ENGINE_PROC(
      UpdateLocales, ([&called](auto engine, const FlutterLocale** locales,
                                size_t locales_count) {
        called = true;

        EXPECT_EQ(locales_count, static_cast<size_t>(2));

        EXPECT_STREQ(locales[0]->language_code, "en");
        EXPECT_STREQ(locales[0]->country_code, nullptr);
        EXPECT_STREQ(locales[0]->script_code, nullptr);
        EXPECT_STREQ(locales[0]->variant_code, nullptr);

        EXPECT_STREQ(locales[1]->language_code, "C");
        EXPECT_STREQ(locales[1]->country_code, nullptr);
        EXPECT_STREQ(locales[1]->script_code, nullptr);
        EXPECT_STREQ(locales[1]->variant_code, nullptr);

        return kSuccess;
      }));

  g_autoptr(GError) error = nullptr;
  EXPECT_TRUE(fl_engine_start(engine, &error));
  EXPECT_EQ(error, nullptr);

  EXPECT_TRUE(called);

  if (initial_language) {
    g_setenv("LANGUAGE", initial_language, TRUE);
  } else {
    g_unsetenv("LANGUAGE");
  }
}

TEST(FlEngineTest, EmptyLocales) {
  g_autofree gchar* initial_language = g_strdup(g_getenv("LANGUAGE"));
  g_setenv("LANGUAGE", "de:: :en_US", TRUE);
  g_autoptr(FlDartProject) project = fl_dart_project_new();

  g_autoptr(FlEngine) engine = fl_engine_new(project);

  bool called = false;
  fl_engine_get_embedder_api(engine)->UpdateLocales = MOCK_ENGINE_PROC(
      UpdateLocales, ([&called](auto engine, const FlutterLocale** locales,
                                size_t locales_count) {
        called = true;

        EXPECT_EQ(locales_count, static_cast<size_t>(4));

        EXPECT_STREQ(locales[0]->language_code, "de");
        EXPECT_STREQ(locales[0]->country_code, nullptr);
        EXPECT_STREQ(locales[0]->script_code, nullptr);
        EXPECT_STREQ(locales[0]->variant_code, nullptr);

        EXPECT_STREQ(locales[1]->language_code, "en");
        EXPECT_STREQ(locales[1]->country_code, "US");
        EXPECT_STREQ(locales[1]->script_code, nullptr);
        EXPECT_STREQ(locales[1]->variant_code, nullptr);

        EXPECT_STREQ(locales[2]->language_code, "en");
        EXPECT_STREQ(locales[2]->country_code, nullptr);
        EXPECT_STREQ(locales[2]->script_code, nullptr);
        EXPECT_STREQ(locales[2]->variant_code, nullptr);

        EXPECT_STREQ(locales[3]->language_code, "C");
        EXPECT_STREQ(locales[3]->country_code, nullptr);
        EXPECT_STREQ(locales[3]->script_code, nullptr);
        EXPECT_STREQ(locales[3]->variant_code, nullptr);

        return kSuccess;
      }));

  g_autoptr(GError) error = nullptr;
  EXPECT_TRUE(fl_engine_start(engine, &error));
  EXPECT_EQ(error, nullptr);

  EXPECT_TRUE(called);

  if (initial_language) {
    g_setenv("LANGUAGE", initial_language, TRUE);
  } else {
    g_unsetenv("LANGUAGE");
  }
}

TEST(FlEngineTest, ChildObjects) {
  g_autoptr(FlDartProject) project = fl_dart_project_new();
  g_autoptr(FlEngine) engine = fl_engine_new(project);

  // Check objects exist before engine started.
  EXPECT_NE(fl_engine_get_binary_messenger(engine), nullptr);
  EXPECT_NE(fl_engine_get_task_runner(engine), nullptr);
}

// NOLINTEND(clang-analyzer-core.StackAddressEscape)
