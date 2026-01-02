// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>

#include "flutter/shell/platform/windows/client_wrapper/include/flutter/plugin_registrar_windows.h"
#include "flutter/shell/platform/windows/client_wrapper/testing/stub_flutter_windows_api.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace flutter {

namespace {

using ::testing::Return;

// Stub implementation to validate calls to the API.
class TestWindowsApi : public testing::StubFlutterWindowsApi {
 public:
  MOCK_METHOD(FlutterDesktopMessengerRef,
              PluginRegistrarGetMessenger,
              (),
              (override));
};

// A test plugin that tries to access registrar state during destruction and
// reports it out via a flag provided at construction.
class TestPlugin : public Plugin {
 public:
  // registrar_valid_at_destruction will be set at destruction to indicate
  // whether or not |registrar->GetView()| was non-null.
  TestPlugin(PluginRegistrarWindows* registrar,
             bool* registrar_valid_at_destruction)
      : registrar_(registrar),
        registrar_valid_at_destruction_(registrar_valid_at_destruction) {}
  virtual ~TestPlugin() {
    *registrar_valid_at_destruction_ = registrar_->messenger() != nullptr;
  }

 private:
  PluginRegistrarWindows* registrar_;
  bool* registrar_valid_at_destruction_;
};

}  // namespace

// Tests that the registrar runs plugin destructors before its own teardown.
TEST(PluginRegistrarWindowsTest, PluginDestroyedBeforeRegistrar) {
  auto windows_api = std::make_unique<TestWindowsApi>();
  EXPECT_CALL(*windows_api, PluginRegistrarGetMessenger)
      .WillRepeatedly(Return(reinterpret_cast<FlutterDesktopMessengerRef>(1)));
  testing::ScopedStubFlutterWindowsApi scoped_api_stub(std::move(windows_api));
  auto test_api = static_cast<TestWindowsApi*>(scoped_api_stub.stub());
  PluginRegistrarWindows registrar(
      reinterpret_cast<FlutterDesktopPluginRegistrarRef>(1));

  auto dummy_registrar_handle =
      reinterpret_cast<FlutterDesktopPluginRegistrarRef>(1);
  bool registrar_valid_at_destruction = false;
  {
    PluginRegistrarWindows registrar(dummy_registrar_handle);

    auto plugin = std::make_unique<TestPlugin>(&registrar,
                                               &registrar_valid_at_destruction);
    registrar.AddPlugin(std::move(plugin));
  }
  EXPECT_TRUE(registrar_valid_at_destruction);
}

}  // namespace flutter
