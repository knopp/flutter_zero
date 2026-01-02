// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_SHELL_TEST_H_
#define FLUTTER_SHELL_COMMON_SHELL_TEST_H_

#include "flutter/shell/common/shell.h"

#include <memory>

#include "flutter/common/settings.h"
#include "flutter/fml/macros.h"
#include "flutter/lib/ui/window/platform_message.h"
#include "flutter/shell/common/run_configuration.h"
#include "flutter/shell/common/thread_host.h"
#include "flutter/testing/fixture_test.h"

namespace flutter {
namespace testing {

class ShellTest : public FixtureTest {
 public:
  struct Config {
    // Required.
    const Settings& settings;
    // Defaults to GetTaskRunnersForFixture().
    std::optional<TaskRunners> task_runners = {};
    // Defaults to calling ShellTestPlatformView::Create with the provided
    // arguments.
    Shell::CreateCallback<PlatformView> platform_view_create_callback;
    std::optional<int64_t> engine_id;
  };

  ShellTest();

  Settings CreateSettingsForFixture() override;
  std::unique_ptr<Shell> CreateShell(
      const Settings& settings,
      std::optional<TaskRunners> task_runners = {});
  std::unique_ptr<Shell> CreateShell(const Config& config);
  void DestroyShell(std::unique_ptr<Shell> shell);
  void DestroyShell(std::unique_ptr<Shell> shell,
                    const TaskRunners& task_runners);
  TaskRunners GetTaskRunnersForFixture();

  static void PumpOneFrame(Shell* shell) {}

  void SendPlatformMessage(Shell* shell,
                           std::unique_ptr<PlatformMessage> message);

  void SendEnginePlatformMessage(Shell* shell,
                                 std::unique_ptr<PlatformMessage> message);

  static void PlatformViewNotifyCreated(
      Shell* shell);  // This creates the surface
  static void PlatformViewNotifyDestroyed(
      Shell* shell);  // This destroys the surface
  static void RunEngine(Shell* shell, RunConfiguration configuration);
  static void RestartEngine(Shell* shell, RunConfiguration configuration);

  enum ServiceProtocolEnum {
    kGetSkSLs,
    kEstimateRasterCacheMemory,
    kSetAssetBundlePath,
    kRunInView,
  };

  // Helper method to test private method Shell::OnServiceProtocolGetSkSLs.
  // (ShellTest is a friend class of Shell.) We'll also make sure that it is
  // running on the correct task_runner.
  static void OnServiceProtocol(
      Shell* shell,
      ServiceProtocolEnum some_protocol,
      const fml::RefPtr<fml::TaskRunner>& task_runner,
      const ServiceProtocol::Handler::ServiceProtocolMap& params,
      rapidjson::Document* response);

  // Do not assert |UnreportedTimingsCount| to be positive in any tests.
  // Otherwise those tests will be flaky as the clearing of unreported timings
  // is unpredictive.
  static int UnreportedTimingsCount(Shell* shell);

 private:
  ThreadHost thread_host_;

  FML_DISALLOW_COPY_AND_ASSIGN(ShellTest);
};

}  // namespace testing
}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHELL_TEST_H_
