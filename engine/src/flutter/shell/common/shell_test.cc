// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER

#include "flutter/shell/common/shell_test.h"

#include "flutter/fml/make_copyable.h"
#include "flutter/fml/synchronization/waitable_event.h"
#include "flutter/shell/common/shell_test_platform_view.h"
#include "flutter/testing/testing.h"

namespace flutter {
namespace testing {

ShellTest::ShellTest()
    : thread_host_("io.flutter.test." + GetCurrentTestName() + ".",
                   ThreadHost::Type::kPlatform | ThreadHost::Type::kUi) {}

void ShellTest::SendPlatformMessage(Shell* shell,
                                    std::unique_ptr<PlatformMessage> message) {
  shell->OnPlatformViewDispatchPlatformMessage(std::move(message));
}

void ShellTest::SendEnginePlatformMessage(
    Shell* shell,
    std::unique_ptr<PlatformMessage> message) {
  fml::AutoResetWaitableEvent latch;
  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetPlatformTaskRunner(),
      fml::MakeCopyable(
          [shell, &latch, message = std::move(message)]() mutable {
            if (auto engine = shell->weak_engine_) {
              engine->HandlePlatformMessage(std::move(message));
            }
            latch.Signal();
          }));
  latch.Wait();
}

void ShellTest::PlatformViewNotifyCreated(Shell* shell) {
  fml::AutoResetWaitableEvent latch;
  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetPlatformTaskRunner(), [shell, &latch]() {
        shell->GetPlatformView()->NotifyCreated();
        latch.Signal();
      });
  latch.Wait();
}

void ShellTest::PlatformViewNotifyDestroyed(Shell* shell) {
  fml::AutoResetWaitableEvent latch;
  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetPlatformTaskRunner(), [shell, &latch]() {
        shell->GetPlatformView()->NotifyDestroyed();
        latch.Signal();
      });
  latch.Wait();
}

void ShellTest::RunEngine(Shell* shell, RunConfiguration configuration) {
  fml::AutoResetWaitableEvent latch;
  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetPlatformTaskRunner(),
      [shell, &latch, &configuration]() {
        shell->RunEngine(std::move(configuration),
                         [&latch](Engine::RunStatus run_status) {
                           ASSERT_EQ(run_status, Engine::RunStatus::Success);
                           latch.Signal();
                         });
      });
  latch.Wait();
}

void ShellTest::RestartEngine(Shell* shell, RunConfiguration configuration) {
  std::promise<bool> restarted;
  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetUITaskRunner(),
      [shell, &restarted, &configuration]() {
        restarted.set_value(shell->engine_->Restart(std::move(configuration)));
      });
  ASSERT_TRUE(restarted.get_future().get());
}

void ShellTest::OnServiceProtocol(
    Shell* shell,
    ServiceProtocolEnum some_protocol,
    const fml::RefPtr<fml::TaskRunner>& task_runner,
    const ServiceProtocol::Handler::ServiceProtocolMap& params,
    rapidjson::Document* response) {
  std::promise<bool> finished;
  fml::TaskRunner::RunNowOrPostTask(
      task_runner, [shell, some_protocol, params, response, &finished]() {
        switch (some_protocol) {
          case ServiceProtocolEnum::kGetSkSLs:
            shell->OnServiceProtocolGetSkSLs(params, response);
            break;
          case ServiceProtocolEnum::kEstimateRasterCacheMemory:
            shell->OnServiceProtocolEstimateRasterCacheMemory(params, response);
            break;
          case ServiceProtocolEnum::kSetAssetBundlePath:
            shell->OnServiceProtocolSetAssetBundlePath(params, response);
            break;
          case ServiceProtocolEnum::kRunInView:
            shell->OnServiceProtocolRunInView(params, response);
            break;
        }
        finished.set_value(true);
      });
  finished.get_future().wait();
}

Settings ShellTest::CreateSettingsForFixture() {
  Settings settings;
  settings.leak_vm = false;
  settings.task_observer_add = [](intptr_t key, const fml::closure& handler) {
    fml::TaskQueueId queue_id = fml::MessageLoop::GetCurrentTaskQueueId();
    fml::MessageLoopTaskQueues::GetInstance()->AddTaskObserver(queue_id, key,
                                                               handler);
    return queue_id;
  };
  settings.task_observer_remove = [](fml::TaskQueueId queue_id, intptr_t key) {
    fml::MessageLoopTaskQueues::GetInstance()->RemoveTaskObserver(queue_id,
                                                                  key);
  };
  settings.isolate_create_callback = [this]() {
    native_resolver_->SetNativeResolverForIsolate();
  };
#if OS_FUCHSIA
  settings.verbose_logging = true;
#endif
  SetSnapshotsAndAssets(settings);
  return settings;
}

TaskRunners ShellTest::GetTaskRunnersForFixture() {
  return {
      "test",
      thread_host_.platform_thread->GetTaskRunner(),  // platform
      thread_host_.ui_thread->GetTaskRunner(),        // ui
  };
}

std::unique_ptr<Shell> ShellTest::CreateShell(
    const Settings& settings,
    std::optional<TaskRunners> task_runners) {
  return CreateShell({
      .settings = settings,
      .task_runners = std::move(task_runners),
  });
}

std::unique_ptr<Shell> ShellTest::CreateShell(const Config& config) {
  TaskRunners task_runners = config.task_runners.has_value()
                                 ? config.task_runners.value()
                                 : GetTaskRunnersForFixture();
  Shell::CreateCallback<PlatformView> platform_view_create_callback =
      config.platform_view_create_callback;
  if (!platform_view_create_callback) {
    platform_view_create_callback = ShellTestPlatformViewBuilder();
  }

  return Shell::Create(flutter::PlatformData(),  //
                       task_runners,             //
                       config.settings,          //
                       platform_view_create_callback);
}

void ShellTest::DestroyShell(std::unique_ptr<Shell> shell) {
  DestroyShell(std::move(shell), GetTaskRunnersForFixture());
}

void ShellTest::DestroyShell(std::unique_ptr<Shell> shell,
                             const TaskRunners& task_runners) {
  fml::AutoResetWaitableEvent latch;
  fml::TaskRunner::RunNowOrPostTask(task_runners.GetPlatformTaskRunner(),
                                    [&shell, &latch]() mutable {
                                      shell.reset();
                                      latch.Signal();
                                    });
  latch.Wait();
}

}  // namespace testing
}  // namespace flutter
