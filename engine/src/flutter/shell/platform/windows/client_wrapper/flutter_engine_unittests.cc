// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>

#include "flutter/shell/platform/windows/client_wrapper/include/flutter/flutter_engine.h"
#include "flutter/shell/platform/windows/client_wrapper/testing/stub_flutter_windows_api.h"
#include "gtest/gtest.h"

namespace flutter {

namespace {

// Stub implementation to validate calls to the API.
class TestFlutterWindowsApi : public testing::StubFlutterWindowsApi {
 public:
  // |flutter::testing::StubFlutterWindowsApi|
  FlutterDesktopEngineRef EngineCreate(
      const FlutterDesktopEngineProperties& engine_properties) {
    create_called_ = true;

    // dart_entrypoint_argv is only guaranteed to exist until this method
    // returns, so copy it here for unit test validation
    dart_entrypoint_arguments_.clear();
    for (int i = 0; i < engine_properties.dart_entrypoint_argc; i++) {
      dart_entrypoint_arguments_.push_back(
          std::string(engine_properties.dart_entrypoint_argv[i]));
    }
    return reinterpret_cast<FlutterDesktopEngineRef>(1);
  }

  // |flutter::testing::StubFlutterWindowsApi|
  bool EngineRun(const char* entry_point) override {
    run_called_ = true;
    return true;
  }

  // |flutter::testing::StubFlutterWindowsApi|
  bool EngineDestroy() override {
    destroy_called_ = true;
    return true;
  }

  bool create_called() { return create_called_; }

  bool run_called() { return run_called_; }

  bool destroy_called() { return destroy_called_; }

  const std::vector<std::string>& dart_entrypoint_arguments() {
    return dart_entrypoint_arguments_;
  }

 private:
  bool create_called_ = false;
  bool run_called_ = false;
  bool destroy_called_ = false;
  std::vector<std::string> dart_entrypoint_arguments_;
};

}  // namespace

TEST(FlutterEngineTest, CreateDestroy) {
  testing::ScopedStubFlutterWindowsApi scoped_api_stub(
      std::make_unique<TestFlutterWindowsApi>());
  auto test_api = static_cast<TestFlutterWindowsApi*>(scoped_api_stub.stub());
  {
    FlutterEngine engine(DartProject(L"fake/project/path"));
    engine.Run();
    EXPECT_EQ(test_api->create_called(), true);
    EXPECT_EQ(test_api->run_called(), true);
    EXPECT_EQ(test_api->destroy_called(), false);
  }
  // Destroying should implicitly shut down if it hasn't been done manually.
  EXPECT_EQ(test_api->destroy_called(), true);
}

TEST(FlutterEngineTest, CreateDestroyWithCustomEntrypoint) {
  testing::ScopedStubFlutterWindowsApi scoped_api_stub(
      std::make_unique<TestFlutterWindowsApi>());
  auto test_api = static_cast<TestFlutterWindowsApi*>(scoped_api_stub.stub());
  {
    DartProject project(L"fake/project/path");
    project.set_dart_entrypoint("customEntrypoint");
    FlutterEngine engine(project);
    engine.Run();
    EXPECT_EQ(test_api->create_called(), true);
    EXPECT_EQ(test_api->run_called(), true);
    EXPECT_EQ(test_api->destroy_called(), false);
  }
  // Destroying should implicitly shut down if it hasn't been done manually.
  EXPECT_EQ(test_api->destroy_called(), true);
}

TEST(FlutterEngineTest, ExplicitShutDown) {
  testing::ScopedStubFlutterWindowsApi scoped_api_stub(
      std::make_unique<TestFlutterWindowsApi>());
  auto test_api = static_cast<TestFlutterWindowsApi*>(scoped_api_stub.stub());

  FlutterEngine engine(DartProject(L"fake/project/path"));
  engine.Run();
  EXPECT_EQ(test_api->create_called(), true);
  EXPECT_EQ(test_api->run_called(), true);
  EXPECT_EQ(test_api->destroy_called(), false);
  engine.ShutDown();
  EXPECT_EQ(test_api->destroy_called(), true);
}

TEST(FlutterEngineTest, GetMessenger) {
  DartProject project(L"data");
  testing::ScopedStubFlutterWindowsApi scoped_api_stub(
      std::make_unique<TestFlutterWindowsApi>());
  auto test_api = static_cast<TestFlutterWindowsApi*>(scoped_api_stub.stub());

  FlutterEngine engine(DartProject(L"fake/project/path"));
  EXPECT_NE(engine.messenger(), nullptr);
}

TEST(FlutterEngineTest, DartEntrypointArgs) {
  testing::ScopedStubFlutterWindowsApi scoped_api_stub(
      std::make_unique<TestFlutterWindowsApi>());
  auto test_api = static_cast<TestFlutterWindowsApi*>(scoped_api_stub.stub());

  DartProject project(L"data");
  std::vector<std::string> arguments = {"one", "two"};
  project.set_dart_entrypoint_arguments(arguments);

  FlutterEngine engine(project);
  const std::vector<std::string>& arguments_ref =
      test_api->dart_entrypoint_arguments();
  ASSERT_EQ(2, arguments_ref.size());
  EXPECT_TRUE(arguments[0] == arguments_ref[0]);
  EXPECT_TRUE(arguments[1] == arguments_ref[1]);
}

}  // namespace flutter
