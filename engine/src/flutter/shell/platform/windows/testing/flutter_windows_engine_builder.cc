// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/testing/flutter_windows_engine_builder.h"

#include "flutter/fml/macros.h"
#include "flutter/shell/platform/windows/windows_proc_table.h"

namespace flutter {
namespace testing {

class TestFlutterWindowsEngine : public FlutterWindowsEngine {
 public:
  TestFlutterWindowsEngine(
      const FlutterProjectBundle& project,
      std::shared_ptr<WindowsProcTable> windows_proc_table = nullptr)
      : FlutterWindowsEngine(project, std::move(windows_proc_table)) {}

 protected:
 private:
  FML_DISALLOW_COPY_AND_ASSIGN(TestFlutterWindowsEngine);
};

FlutterWindowsEngineBuilder::FlutterWindowsEngineBuilder(
    WindowsTestContext& context)
    : context_(context) {
  properties_.assets_path = context.GetAssetsPath().c_str();
  properties_.icu_data_path = context.GetIcuDataPath().c_str();
  properties_.aot_library_path = context.GetAotLibraryPath().c_str();
}

FlutterWindowsEngineBuilder::~FlutterWindowsEngineBuilder() = default;

void FlutterWindowsEngineBuilder::SetDartEntrypoint(std::string entrypoint) {
  dart_entrypoint_ = std::move(entrypoint);
  properties_.dart_entrypoint = dart_entrypoint_.c_str();
}

void FlutterWindowsEngineBuilder::AddDartEntrypointArgument(std::string arg) {
  dart_entrypoint_arguments_.emplace_back(std::move(arg));
}

void FlutterWindowsEngineBuilder::SetSwitches(
    std::vector<std::string> switches) {
  switches_ = std::move(switches);
}

void FlutterWindowsEngineBuilder::SetWindowsProcTable(
    std::shared_ptr<WindowsProcTable> windows_proc_table) {
  windows_proc_table_ = std::move(windows_proc_table);
}

std::unique_ptr<FlutterWindowsEngine> FlutterWindowsEngineBuilder::Build() {
  std::vector<const char*> dart_args;
  dart_args.reserve(dart_entrypoint_arguments_.size());

  for (const auto& arg : dart_entrypoint_arguments_) {
    dart_args.push_back(arg.c_str());
  }

  if (!dart_args.empty()) {
    properties_.dart_entrypoint_argv = dart_args.data();
    properties_.dart_entrypoint_argc = dart_args.size();
  } else {
    properties_.dart_entrypoint_argv = nullptr;
    properties_.dart_entrypoint_argc = 0;
  }

  FlutterProjectBundle project(properties_);
  project.SetSwitches(switches_);

  return std::make_unique<TestFlutterWindowsEngine>(
      project, std::move(windows_proc_table_));
}

}  // namespace testing
}  // namespace flutter
