// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/public/flutter_windows.h"

#include <dxgi.h>
#include <wrl/client.h>
#include <thread>

#include "flutter/fml/synchronization/count_down_latch.h"
#include "flutter/fml/synchronization/waitable_event.h"
#include "flutter/shell/platform/embedder/test_utils/proc_table_replacement.h"
#include "flutter/shell/platform/windows/flutter_windows_internal.h"
#include "flutter/shell/platform/windows/testing/engine_modifier.h"
#include "flutter/shell/platform/windows/testing/windows_test.h"
#include "flutter/shell/platform/windows/testing/windows_test_config_builder.h"
#include "flutter/shell/platform/windows/testing/windows_test_context.h"
#include "flutter/testing/stream_capture.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/tonic/converter/dart_converter.h"

namespace flutter {
namespace testing {

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

// Verify that we can fetch a binary messenger.
TEST(WindowsNoFixtureTest, GetBinaryMessenger) {
  FlutterDesktopEngineProperties properties = {};
  properties.assets_path = L"";
  properties.icu_data_path = L"icudtl.dat";
  auto engine = FlutterDesktopEngineCreate(&properties);
  ASSERT_NE(engine, nullptr);
  auto messenger = FlutterDesktopEngineGetMessenger(engine);
  EXPECT_NE(messenger, nullptr);
  FlutterDesktopEngineDestroy(engine);
}

// Verify we can successfully launch main().
TEST_F(WindowsTest, LaunchMain) {
  auto& context = GetContext();
  WindowsConfigBuilder builder(context);
  auto engine = builder.RunHeadless();
  ASSERT_NE(engine.get(), nullptr);
}

// Verify there is no unexpected output from launching main.
TEST_F(WindowsTest, LaunchMainHasNoOutput) {
  // Replace stderr stream buffer with our own.  (stdout may contain expected
  // output printed by Dart, such as the Dart VM service startup message)
  StreamCapture stderr_capture(&std::cerr);

  auto& context = GetContext();
  WindowsConfigBuilder builder(context);
  auto engine = builder.RunHeadless();
  ASSERT_NE(engine.get(), nullptr);

  stderr_capture.Stop();

  // Verify stderr has no output.
  EXPECT_TRUE(stderr_capture.GetOutput().empty());
}

// Verify we can successfully launch a custom entry point.
TEST_F(WindowsTest, LaunchCustomEntrypoint) {
  auto& context = GetContext();
  WindowsConfigBuilder builder(context);
  builder.SetDartEntrypoint("customEntrypoint");
  auto engine = builder.RunHeadless();
  ASSERT_NE(engine.get(), nullptr);
}

// Verify that engine launches with the custom entrypoint specified in the
// FlutterDesktopEngineRun parameter when no entrypoint is specified in
// FlutterDesktopEngineProperties.dart_entrypoint.
//
// TODO(cbracken): https://github.com/flutter/flutter/issues/109285
TEST_F(WindowsTest, LaunchCustomEntrypointInEngineRunInvocation) {
  auto& context = GetContext();
  WindowsConfigBuilder builder(context);
  EnginePtr engine{builder.InitializeEngine()};
  ASSERT_NE(engine, nullptr);

  ASSERT_TRUE(FlutterDesktopEngineRun(engine.get(), "customEntrypoint"));
}

// Verify that engine fails to launch when a conflicting entrypoint in
// FlutterDesktopEngineProperties.dart_entrypoint and the
// FlutterDesktopEngineRun parameter.
//
// TODO(cbracken): https://github.com/flutter/flutter/issues/109285
TEST_F(WindowsTest, LaunchConflictingCustomEntrypoints) {
  auto& context = GetContext();
  WindowsConfigBuilder builder(context);
  builder.SetDartEntrypoint("customEntrypoint");
  EnginePtr engine{builder.InitializeEngine()};
  ASSERT_NE(engine, nullptr);

  ASSERT_FALSE(FlutterDesktopEngineRun(engine.get(), "conflictingEntrypoint"));
}

// Verify that native functions can be registered and resolved.
TEST_F(WindowsTest, VerifyNativeFunction) {
  auto& context = GetContext();
  WindowsConfigBuilder builder(context);
  builder.SetDartEntrypoint("verifyNativeFunction");

  bool signaled = false;
  auto native_entry =
      CREATE_NATIVE_ENTRY([&](Dart_NativeArguments args) { signaled = true; });
  context.AddNativeFunction("Signal", native_entry);

  auto engine = builder.RunHeadless();
  ASSERT_NE(engine.get(), nullptr);

  // Wait until signal has been called.
  while (!signaled) {
    PumpMessage();
  }
}

// Verify that native functions that pass parameters can be registered and
// resolved.
TEST_F(WindowsTest, VerifyNativeFunctionWithParameters) {
  auto& context = GetContext();
  WindowsConfigBuilder builder(context);
  builder.SetDartEntrypoint("verifyNativeFunctionWithParameters");

  bool bool_value = false;
  bool signaled = false;
  auto native_entry = CREATE_NATIVE_ENTRY([&](Dart_NativeArguments args) {
    auto handle = Dart_GetNativeBooleanArgument(args, 0, &bool_value);
    ASSERT_FALSE(Dart_IsError(handle));
    signaled = true;
  });
  context.AddNativeFunction("SignalBoolValue", native_entry);

  auto engine = builder.RunHeadless();
  ASSERT_NE(engine.get(), nullptr);

  // Wait until signalBoolValue has been called.
  while (!signaled) {
    PumpMessage();
  }
  EXPECT_TRUE(bool_value);
}

// Verify that Platform.executable returns the executable name.
TEST_F(WindowsTest, PlatformExecutable) {
  auto& context = GetContext();
  WindowsConfigBuilder builder(context);
  builder.SetDartEntrypoint("readPlatformExecutable");

  std::string executable_name;
  bool signaled = false;
  auto native_entry = CREATE_NATIVE_ENTRY([&](Dart_NativeArguments args) {
    auto handle = Dart_GetNativeArgument(args, 0);
    ASSERT_FALSE(Dart_IsError(handle));
    executable_name = tonic::DartConverter<std::string>::FromDart(handle);
    signaled = true;
  });
  context.AddNativeFunction("SignalStringValue", native_entry);

  auto engine = builder.RunHeadless();
  ASSERT_NE(engine.get(), nullptr);

  // Wait until signalStringValue has been called.
  while (!signaled) {
    PumpMessage();
  }
  EXPECT_EQ(executable_name, "flutter_windows_unittests.exe");
}

// Verify that native functions that return values can be registered and
// resolved.
TEST_F(WindowsTest, VerifyNativeFunctionWithReturn) {
  auto& context = GetContext();
  WindowsConfigBuilder builder(context);
  builder.SetDartEntrypoint("verifyNativeFunctionWithReturn");

  bool bool_value_to_return = true;
  int count = 2;
  auto bool_return_entry = CREATE_NATIVE_ENTRY([&](Dart_NativeArguments args) {
    Dart_SetBooleanReturnValue(args, bool_value_to_return);
    --count;
  });
  context.AddNativeFunction("SignalBoolReturn", bool_return_entry);

  bool bool_value_passed = false;
  auto bool_pass_entry = CREATE_NATIVE_ENTRY([&](Dart_NativeArguments args) {
    auto handle = Dart_GetNativeBooleanArgument(args, 0, &bool_value_passed);
    ASSERT_FALSE(Dart_IsError(handle));
    --count;
  });
  context.AddNativeFunction("SignalBoolValue", bool_pass_entry);

  auto engine = builder.RunHeadless();
  ASSERT_NE(engine.get(), nullptr);

  // Wait until signalBoolReturn and signalBoolValue have been called.
  while (count > 0) {
    PumpMessage();
  }
  EXPECT_TRUE(bool_value_passed);
}

TEST_F(WindowsTest, EngineId) {
  auto& context = GetContext();
  WindowsConfigBuilder builder(context);
  builder.SetDartEntrypoint("testEngineId");

  std::optional<int64_t> engineId;
  context.AddNativeFunction(
      "NotifyEngineId", CREATE_NATIVE_ENTRY([&](Dart_NativeArguments args) {
        const auto argument = Dart_GetNativeArgument(args, 0);
        if (!Dart_IsNull(argument)) {
          const auto handle = tonic::DartConverter<int64_t>::FromDart(argument);
          engineId = handle;
        }
      }));

  auto engine = builder.RunHeadless();
  ASSERT_NE(engine.get(), nullptr);

  while (!engineId.has_value()) {
    PumpMessage();
  }

  EXPECT_EQ(engine.get(), FlutterDesktopEngineForId(*engineId));
}

}  // namespace testing
}  // namespace flutter
