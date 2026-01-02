// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER

#include <algorithm>
#include <ctime>
#include <future>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#if SHELL_ENABLE_GL
#include <EGL/egl.h>
#endif  // SHELL_ENABLE_GL

#include "assets/asset_resolver.h"
#include "assets/directory_asset_bundle.h"
#include "flutter/fml/backtrace.h"
#include "flutter/fml/command_line.h"
#include "flutter/fml/message_loop.h"
#include "flutter/fml/synchronization/count_down_latch.h"
#include "flutter/fml/synchronization/waitable_event.h"
#include "flutter/runtime/dart_vm.h"
#include "flutter/shell/common/platform_view.h"
#include "flutter/shell/common/shell_test.h"
#include "flutter/shell/common/shell_test_platform_view.h"
#include "flutter/shell/common/switches.h"
#include "flutter/shell/common/thread_host.h"
#include "flutter/testing/testing.h"
#include "fml/mapping.h"
#include "gmock/gmock.h"
#include "third_party/rapidjson/include/rapidjson/writer.h"

// CREATE_NATIVE_ENTRY is leaky by design
// NOLINTBEGIN(clang-analyzer-core.StackAddressEscape)

namespace flutter {
namespace testing {

using ::testing::_;
using ::testing::Return;

namespace {

#if 0
std::unique_ptr<PlatformMessage> MakePlatformMessage(
    const std::string& channel,
    const std::map<std::string, std::string>& values,
    const fml::RefPtr<PlatformMessageResponse>& response) {
  rapidjson::Document document;
  auto& allocator = document.GetAllocator();
  document.SetObject();

  for (const auto& pair : values) {
    rapidjson::Value key(pair.first.c_str(), strlen(pair.first.c_str()),
                         allocator);
    rapidjson::Value value(pair.second.c_str(), strlen(pair.second.c_str()),
                           allocator);
    document.AddMember(key, value, allocator);
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  document.Accept(writer);
  const uint8_t* data = reinterpret_cast<const uint8_t*>(buffer.GetString());

  std::unique_ptr<PlatformMessage> message = std::make_unique<PlatformMessage>(
      channel, fml::MallocMapping::Copy(data, buffer.GetSize()), response);
  return message;
}
#endif

class MockPlatformViewDelegate : public PlatformView::Delegate {
  MOCK_METHOD(void, OnPlatformViewCreated, (), (override));

  MOCK_METHOD(void, OnPlatformViewDestroyed, (), (override));

  MOCK_METHOD(void,
              OnPlatformViewDispatchPlatformMessage,
              (std::unique_ptr<PlatformMessage> message),
              (override));

  MOCK_METHOD(const Settings&,
              OnPlatformViewGetSettings,
              (),
              (const, override));

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
};

class MockPlatformView : public PlatformView {
 public:
  MockPlatformView(MockPlatformViewDelegate& delegate,
                   const TaskRunners& task_runners)
      : PlatformView(delegate, task_runners) {}
  MOCK_METHOD(std::shared_ptr<PlatformMessageHandler>,
              GetPlatformMessageHandler,
              (),
              (const, override));
};

class TestPlatformView : public PlatformView {
 public:
  TestPlatformView(Shell& shell, const TaskRunners& task_runners)
      : PlatformView(shell, task_runners) {}
};

class MockPlatformMessageHandler : public PlatformMessageHandler {
 public:
  MOCK_METHOD(void,
              HandlePlatformMessage,
              (std::unique_ptr<PlatformMessage> message),
              (override));
  MOCK_METHOD(bool,
              DoesHandlePlatformMessageOnPlatformThread,
              (),
              (const, override));
  MOCK_METHOD(void,
              InvokePlatformMessageResponseCallback,
              (int response_id, std::unique_ptr<fml::Mapping> mapping),
              (override));
  MOCK_METHOD(void,
              InvokePlatformMessageEmptyResponseCallback,
              (int response_id),
              (override));
};

class MockPlatformMessageResponse : public PlatformMessageResponse {
 public:
  static fml::RefPtr<MockPlatformMessageResponse> Create() {
    return fml::AdoptRef(new MockPlatformMessageResponse());
  }
  MOCK_METHOD(void, Complete, (std::unique_ptr<fml::Mapping> data), (override));
  MOCK_METHOD(void, CompleteEmpty, (), (override));
};
}  // namespace

class TestAssetResolver : public AssetResolver {
 public:
  TestAssetResolver(bool valid, AssetResolver::AssetResolverType type)
      : valid_(valid), type_(type) {}

  bool IsValid() const override { return true; }

  // This is used to identify if replacement was made or not.
  bool IsValidAfterAssetManagerChange() const override { return valid_; }

  AssetResolver::AssetResolverType GetType() const override { return type_; }

  std::unique_ptr<fml::Mapping> GetAsMapping(
      const std::string& asset_name) const override {
    return nullptr;
  }

  std::vector<std::unique_ptr<fml::Mapping>> GetAsMappings(
      const std::string& asset_pattern,
      const std::optional<std::string>& subdir) const override {
    return {};
  };

  bool operator==(const AssetResolver& other) const override {
    return this == &other;
  }

 private:
  bool valid_;
  AssetResolver::AssetResolverType type_;
};

class ThreadCheckingAssetResolver : public AssetResolver {
 public:
  explicit ThreadCheckingAssetResolver(
      std::shared_ptr<fml::ConcurrentMessageLoop> concurrent_loop)
      : concurrent_loop_(std::move(concurrent_loop)) {}

  // |AssetResolver|
  bool IsValid() const override { return true; }

  // |AssetResolver|
  bool IsValidAfterAssetManagerChange() const override { return true; }

  // |AssetResolver|
  AssetResolverType GetType() const {
    return AssetResolverType::kApkAssetProvider;
  }

  // |AssetResolver|
  std::unique_ptr<fml::Mapping> GetAsMapping(
      const std::string& asset_name) const override {
    if (asset_name == "FontManifest.json" ||
        asset_name == "NativeAssetsManifest.json") {
      // This file is loaded directly by the engine.
      return nullptr;
    }
    mapping_requests.push_back(asset_name);
    EXPECT_TRUE(concurrent_loop_->RunsTasksOnCurrentThread())
        << fml::BacktraceHere();
    return nullptr;
  }

  mutable std::vector<std::string> mapping_requests;

  bool operator==(const AssetResolver& other) const override {
    return this == &other;
  }

 private:
  std::shared_ptr<fml::ConcurrentMessageLoop> concurrent_loop_;
};

static bool ValidateShell(Shell* shell) {
  if (!shell) {
    return false;
  }

  if (!shell->IsSetup()) {
    return false;
  }

  ShellTest::PlatformViewNotifyCreated(shell);

  {
    fml::AutoResetWaitableEvent latch;
    fml::TaskRunner::RunNowOrPostTask(
        shell->GetTaskRunners().GetPlatformTaskRunner(), [shell, &latch]() {
          shell->GetPlatformView()->NotifyDestroyed();
          latch.Signal();
        });
    latch.Wait();
  }

  return true;
}

static std::string CreateFlagsString(std::vector<const char*>& flags) {
  if (flags.empty()) {
    return "";
  }
  std::string flags_string = flags[0];
  for (size_t i = 1; i < flags.size(); ++i) {
    flags_string += ",";
    flags_string += flags[i];
  }
  return flags_string;
}

static void TestDartVmFlags(std::vector<const char*>& flags) {
  std::string flags_string = CreateFlagsString(flags);
  const std::vector<fml::CommandLine::Option> options = {
      fml::CommandLine::Option("dart-flags", flags_string)};
  fml::CommandLine command_line("", options, std::vector<std::string>());
  flutter::Settings settings = flutter::SettingsFromCommandLine(command_line);
  EXPECT_EQ(settings.dart_flags.size(), flags.size());
  for (size_t i = 0; i < flags.size(); ++i) {
    EXPECT_EQ(settings.dart_flags[i], flags[i]);
  }
}

static void PostSync(const fml::RefPtr<fml::TaskRunner>& task_runner,
                     const fml::closure& task) {
  fml::AutoResetWaitableEvent latch;
  fml::TaskRunner::RunNowOrPostTask(task_runner, [&latch, &task] {
    task();
    latch.Signal();
  });
  latch.Wait();
}

TEST_F(ShellTest, InitializeWithInvalidThreads) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  TaskRunners task_runners("test", nullptr, nullptr);
  auto shell = CreateShell(settings, task_runners);
  ASSERT_FALSE(shell);
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, InitializeWithDifferentThreads) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  std::string name_prefix = "io.flutter.test." + GetCurrentTestName() + ".";
  ThreadHost thread_host(ThreadHost::ThreadHostConfig(
      name_prefix, ThreadHost::Type::kPlatform | ThreadHost::Type::kUi));
  ASSERT_EQ(thread_host.name_prefix, name_prefix);

  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner());
  auto shell = CreateShell(settings, task_runners);
  ASSERT_TRUE(ValidateShell(shell.get()));
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  DestroyShell(std::move(shell), task_runners);
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, InitializeWithSingleThread) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  ThreadHost thread_host("io.flutter.test." + GetCurrentTestName() + ".",
                         ThreadHost::Type::kPlatform);
  auto task_runner = thread_host.platform_thread->GetTaskRunner();
  TaskRunners task_runners("test", task_runner, task_runner);
  auto shell = CreateShell(settings, task_runners);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  ASSERT_TRUE(ValidateShell(shell.get()));
  DestroyShell(std::move(shell), task_runners);
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, InitializeWithSingleThreadWhichIsTheCallingThread) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  fml::MessageLoop::EnsureInitializedForCurrentThread();
  auto task_runner = fml::MessageLoop::GetCurrent().GetTaskRunner();
  TaskRunners task_runners("test", task_runner, task_runner);
  auto shell = CreateShell(settings, task_runners);
  ASSERT_TRUE(ValidateShell(shell.get()));
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  DestroyShell(std::move(shell), task_runners);
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest,
       InitializeWithMultipleThreadButCallingThreadAsPlatformThread) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  ThreadHost thread_host("io.flutter.test." + GetCurrentTestName() + ".",
                         ThreadHost::Type::kUi);
  fml::MessageLoop::EnsureInitializedForCurrentThread();
  TaskRunners task_runners("test",
                           fml::MessageLoop::GetCurrent().GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner());
  auto shell = Shell::Create(
      flutter::PlatformData(), task_runners, settings, [](Shell& shell) {
        return ShellTestPlatformView::Create(shell, shell.GetTaskRunners());
      });
  ASSERT_TRUE(ValidateShell(shell.get()));
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  DestroyShell(std::move(shell), task_runners);
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, InitializeWithGPUAndPlatformThreadsTheSame) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  ThreadHost thread_host("io.flutter.test." + GetCurrentTestName() + ".",
                         ThreadHost::Type::kPlatform | ThreadHost::Type::kUi);
  TaskRunners task_runners(
      "test",
      thread_host.platform_thread->GetTaskRunner(),  // platform
      thread_host.ui_thread->GetTaskRunner()         // ui
  );
  auto shell = CreateShell(settings, task_runners);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  ASSERT_TRUE(ValidateShell(shell.get()));
  DestroyShell(std::move(shell), task_runners);
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, FixturesAreFunctional) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  auto settings = CreateSettingsForFixture();
  auto shell = CreateShell(settings);
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(configuration.IsValid());
  configuration.SetEntrypoint("fixturesAreFunctionalMain");

  fml::AutoResetWaitableEvent main_latch;
  AddNativeCallback(
      "SayHiFromFixturesAreFunctionalMain",
      CREATE_NATIVE_ENTRY([&main_latch](auto args) { main_latch.Signal(); }));

  RunEngine(shell.get(), std::move(configuration));
  main_latch.Wait();
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  DestroyShell(std::move(shell));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, SecondaryIsolateBindingsAreSetupViaShellSettings) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  auto settings = CreateSettingsForFixture();
  auto shell = CreateShell(settings);
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(configuration.IsValid());
  configuration.SetEntrypoint("testCanLaunchSecondaryIsolate");

  fml::CountDownLatch latch(2);
  AddNativeCallback("NotifyNative", CREATE_NATIVE_ENTRY([&latch](auto args) {
                      latch.CountDown();
                    }));

  RunEngine(shell.get(), std::move(configuration));

  latch.Wait();

  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  DestroyShell(std::move(shell));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, LastEntrypoint) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  auto settings = CreateSettingsForFixture();
  auto shell = CreateShell(settings);
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(configuration.IsValid());
  std::string entry_point = "fixturesAreFunctionalMain";
  configuration.SetEntrypoint(entry_point);

  fml::AutoResetWaitableEvent main_latch;
  std::string last_entry_point;
  AddNativeCallback(
      "SayHiFromFixturesAreFunctionalMain", CREATE_NATIVE_ENTRY([&](auto args) {
        last_entry_point = shell->GetEngine()->GetLastEntrypoint();
        main_latch.Signal();
      }));

  RunEngine(shell.get(), std::move(configuration));
  main_latch.Wait();
  EXPECT_EQ(entry_point, last_entry_point);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  DestroyShell(std::move(shell));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, LastEntrypointArgs) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  auto settings = CreateSettingsForFixture();
  auto shell = CreateShell(settings);
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(configuration.IsValid());
  std::string entry_point = "fixturesAreFunctionalMain";
  std::vector<std::string> entry_point_args = {"arg1"};
  configuration.SetEntrypoint(entry_point);
  configuration.SetEntrypointArgs(entry_point_args);

  fml::AutoResetWaitableEvent main_latch;
  std::vector<std::string> last_entry_point_args;
  AddNativeCallback(
      "SayHiFromFixturesAreFunctionalMain", CREATE_NATIVE_ENTRY([&](auto args) {
        last_entry_point_args = shell->GetEngine()->GetLastEntrypointArgs();
        main_latch.Signal();
      }));

  RunEngine(shell.get(), std::move(configuration));
  main_latch.Wait();
#if (FLUTTER_RUNTIME_MODE == FLUTTER_RUNTIME_MODE_DEBUG)
  EXPECT_EQ(last_entry_point_args, entry_point_args);
#else
  ASSERT_TRUE(last_entry_point_args.empty());
#endif
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  DestroyShell(std::move(shell));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, DisallowedDartVMFlag) {
#if defined(OS_FUCHSIA)
  GTEST_SKIP() << "This test flakes on Fuchsia. https://fxbug.dev/110006 ";
#else

  // Run this test in a thread-safe manner, otherwise gtest will complain.
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";

  const std::vector<fml::CommandLine::Option> options = {
      fml::CommandLine::Option("dart-flags", "--verify_after_gc")};
  fml::CommandLine command_line("", options, std::vector<std::string>());

  // Upon encountering a disallowed Dart flag the process terminates.
  const char* expected =
      "Encountered disallowed Dart VM flag: --verify_after_gc";
  ASSERT_DEATH(flutter::SettingsFromCommandLine(command_line), expected);
#endif  // OS_FUCHSIA
}

TEST_F(ShellTest, AllowedDartVMFlag) {
  std::vector<const char*> flags = {
      "--enable-isolate-groups",
      "--no-enable-isolate-groups",
  };
#if !FLUTTER_RELEASE
  flags.push_back("--max_profile_depth 1");
  flags.push_back("--random_seed 42");
  flags.push_back("--max_subtype_cache_entries=22");
  if (!DartVM::IsRunningPrecompiledCode()) {
    flags.push_back("--enable_mirrors");
  }
#endif

  TestDartVmFlags(flags);
}

// Compares local times as seen by the dart isolate and as seen by this test
// fixture, to a resolution of 1 hour.
//
// This verifies that (1) the isolate is able to get a timezone (doesn't lock
// up for example), and (2) that the host and the isolate agree on what the
// timezone is.
TEST_F(ShellTest, LocaltimesMatch) {
  fml::AutoResetWaitableEvent latch;
  std::string dart_isolate_time_str;

  // See fixtures/shell_test.dart, the callback NotifyLocalTime is declared
  // there.
  AddNativeCallback("NotifyLocalTime", CREATE_NATIVE_ENTRY([&](auto args) {
                      dart_isolate_time_str =
                          tonic::DartConverter<std::string>::FromDart(
                              Dart_GetNativeArgument(args, 0));
                      latch.Signal();
                    }));

  auto settings = CreateSettingsForFixture();
  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("localtimesMatch");
  std::unique_ptr<Shell> shell = CreateShell(settings);
  ASSERT_NE(shell.get(), nullptr);
  RunEngine(shell.get(), std::move(configuration));
  latch.Wait();

  char timestr[200];
  const time_t timestamp = time(nullptr);
  const struct tm* local_time = localtime(&timestamp);
  ASSERT_NE(local_time, nullptr)
      << "Could not get local time: errno=" << errno << ": " << strerror(errno);
  // Example: "2020-02-26 14" for 2pm on February 26, 2020.
  const size_t format_size =
      strftime(timestr, sizeof(timestr), "%Y-%m-%d %H", local_time);
  ASSERT_NE(format_size, 0UL)
      << "strftime failed: host time: " << std::string(timestr)
      << " dart isolate time: " << dart_isolate_time_str;

  const std::string host_local_time_str = timestr;

  ASSERT_EQ(dart_isolate_time_str, host_local_time_str)
      << "Local times in the dart isolate and the local time seen by the test "
      << "differ by more than 1 hour, but are expected to be about equal";

  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, OnServiceProtocolSetAssetBundlePathWorks) {
  Settings settings = CreateSettingsForFixture();
  std::unique_ptr<Shell> shell = CreateShell(settings);
  RunConfiguration configuration =
      RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("canAccessResourceFromAssetDir");

  // Verify isolate can load a known resource with the
  // default asset directory - kernel_blob.bin
  fml::AutoResetWaitableEvent latch;

  // Callback used to signal whether the resource was loaded successfully.
  bool can_access_resource = false;
  auto native_can_access_resource = [&can_access_resource,
                                     &latch](Dart_NativeArguments args) {
    Dart_Handle exception = nullptr;
    can_access_resource =
        tonic::DartConverter<bool>::FromArguments(args, 0, exception);
    latch.Signal();
  };
  AddNativeCallback("NotifyCanAccessResource",
                    CREATE_NATIVE_ENTRY(native_can_access_resource));

  // Callback used to delay the asset load until after the service
  // protocol method has finished.
  auto native_notify_set_asset_bundle_path =
      [&shell](Dart_NativeArguments args) {
        // Update the asset directory to a bonus path.
        ServiceProtocol::Handler::ServiceProtocolMap params;
        params["assetDirectory"] = "assetDirectory";
        rapidjson::Document document;
        OnServiceProtocol(shell.get(), ServiceProtocolEnum::kSetAssetBundlePath,
                          shell->GetTaskRunners().GetUITaskRunner(), params,
                          &document);
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        document.Accept(writer);
      };
  AddNativeCallback("NotifySetAssetBundlePath",
                    CREATE_NATIVE_ENTRY(native_notify_set_asset_bundle_path));

  RunEngine(shell.get(), std::move(configuration));

  latch.Wait();
  ASSERT_TRUE(can_access_resource);

  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, EngineRootIsolateLaunchesDontTakeVMDataSettings) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  // Make sure the shell launch does not kick off the creation of the VM
  // instance by already creating one upfront.
  auto vm_settings = CreateSettingsForFixture();
  auto vm_ref = DartVMRef::Create(vm_settings);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());

  auto settings = vm_settings;
  fml::AutoResetWaitableEvent isolate_create_latch;
  settings.root_isolate_create_callback = [&](const auto& isolate) {
    isolate_create_latch.Signal();
  };
  auto shell = CreateShell(settings);
  ASSERT_TRUE(ValidateShell(shell.get()));
  auto configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(configuration.IsValid());
  RunEngine(shell.get(), std::move(configuration));
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  DestroyShell(std::move(shell));
  isolate_create_latch.Wait();
}

TEST_F(ShellTest, AssetManagerSingle) {
  fml::ScopedTemporaryDirectory asset_dir;
  fml::UniqueFD asset_dir_fd = fml::OpenDirectory(
      asset_dir.path().c_str(), false, fml::FilePermission::kRead);

  std::string filename = "test_name";
  std::string content = "test_content";

  bool success = fml::WriteAtomically(asset_dir_fd, filename.c_str(),
                                      fml::DataMapping(content));
  ASSERT_TRUE(success);

  AssetManager asset_manager;
  asset_manager.PushBack(
      std::make_unique<DirectoryAssetBundle>(std::move(asset_dir_fd), false));

  auto mapping = asset_manager.GetAsMapping(filename);
  ASSERT_TRUE(mapping != nullptr);

  std::string result(reinterpret_cast<const char*>(mapping->GetMapping()),
                     mapping->GetSize());

  ASSERT_TRUE(result == content);
}

TEST_F(ShellTest, AssetManagerMulti) {
  fml::ScopedTemporaryDirectory asset_dir;
  fml::UniqueFD asset_dir_fd = fml::OpenDirectory(
      asset_dir.path().c_str(), false, fml::FilePermission::kRead);

  std::vector<std::string> filenames = {
      "good0",
      "bad0",
      "good1",
      "bad1",
  };

  for (const auto& filename : filenames) {
    bool success = fml::WriteAtomically(asset_dir_fd, filename.c_str(),
                                        fml::DataMapping(filename));
    ASSERT_TRUE(success);
  }

  AssetManager asset_manager;
  asset_manager.PushBack(
      std::make_unique<DirectoryAssetBundle>(std::move(asset_dir_fd), false));

  auto mappings = asset_manager.GetAsMappings("(.*)", std::nullopt);
  EXPECT_EQ(mappings.size(), 4u);

  std::vector<std::string> expected_results = {
      "good0",
      "good1",
  };

  mappings = asset_manager.GetAsMappings("(.*)good(.*)", std::nullopt);
  ASSERT_EQ(mappings.size(), expected_results.size());

  for (auto& mapping : mappings) {
    std::string result(reinterpret_cast<const char*>(mapping->GetMapping()),
                       mapping->GetSize());
    EXPECT_NE(
        std::find(expected_results.begin(), expected_results.end(), result),
        expected_results.end());
  }
}

#if defined(OS_FUCHSIA)
TEST_F(ShellTest, AssetManagerMultiSubdir) {
  std::string subdir_path = "subdir";

  fml::ScopedTemporaryDirectory asset_dir;
  fml::UniqueFD asset_dir_fd = fml::OpenDirectory(
      asset_dir.path().c_str(), false, fml::FilePermission::kRead);
  fml::UniqueFD subdir_fd =
      fml::OpenDirectory((asset_dir.path() + "/" + subdir_path).c_str(), true,
                         fml::FilePermission::kReadWrite);

  std::vector<std::string> filenames = {
      "bad0",
      "notgood",  // this is to make sure the pattern (.*)good(.*) only
                  // matches things in the subdirectory
  };

  std::vector<std::string> subdir_filenames = {
      "good0",
      "good1",
      "bad1",
  };

  for (auto filename : filenames) {
    bool success = fml::WriteAtomically(asset_dir_fd, filename.c_str(),
                                        fml::DataMapping(filename));
    ASSERT_TRUE(success);
  }

  for (auto filename : subdir_filenames) {
    bool success = fml::WriteAtomically(subdir_fd, filename.c_str(),
                                        fml::DataMapping(filename));
    ASSERT_TRUE(success);
  }

  AssetManager asset_manager;
  asset_manager.PushBack(
      std::make_unique<DirectoryAssetBundle>(std::move(asset_dir_fd), false));

  auto mappings = asset_manager.GetAsMappings("(.*)", std::nullopt);
  EXPECT_EQ(mappings.size(), 5u);

  mappings = asset_manager.GetAsMappings("(.*)", subdir_path);
  EXPECT_EQ(mappings.size(), 3u);

  std::vector<std::string> expected_results = {
      "good0",
      "good1",
  };

  mappings = asset_manager.GetAsMappings("(.*)good(.*)", subdir_path);
  ASSERT_EQ(mappings.size(), expected_results.size());

  for (auto& mapping : mappings) {
    std::string result(reinterpret_cast<const char*>(mapping->GetMapping()),
                       mapping->GetSize());
    ASSERT_NE(
        std::find(expected_results.begin(), expected_results.end(), result),
        expected_results.end());
  }
}
#endif  // OS_FUCHSIA

TEST_F(ShellTest, Spawn) {
  auto settings = CreateSettingsForFixture();
  auto shell = CreateShell(settings);
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(configuration.IsValid());
  configuration.SetEntrypoint("fixturesAreFunctionalMain");

  auto second_configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(second_configuration.IsValid());
  second_configuration.SetEntrypoint("testCanLaunchSecondaryIsolate");

  const std::string initial_route("/foo");

  fml::AutoResetWaitableEvent main_latch;
  std::string last_entry_point;
  // Fulfill native function for the first Shell's entrypoint.
  AddNativeCallback(
      "SayHiFromFixturesAreFunctionalMain", CREATE_NATIVE_ENTRY([&](auto args) {
        last_entry_point = shell->GetEngine()->GetLastEntrypoint();
        main_latch.Signal();
      }));
  // Fulfill native function for the second Shell's entrypoint.
  fml::CountDownLatch second_latch(2);
  AddNativeCallback(
      // The Dart native function names aren't very consistent but this is
      // just the native function name of the second vm entrypoint in the
      // fixture.
      "NotifyNative",
      CREATE_NATIVE_ENTRY([&](auto args) { second_latch.CountDown(); }));

  RunEngine(shell.get(), std::move(configuration));
  main_latch.Wait();
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  // Check first Shell ran the first entrypoint.
  ASSERT_EQ("fixturesAreFunctionalMain", last_entry_point);

  PostSync(shell->GetTaskRunners().GetPlatformTaskRunner(),
           [this, &spawner = shell, &second_configuration, &second_latch,
            initial_route]() {
             MockPlatformViewDelegate platform_view_delegate;
             auto spawn = spawner->Spawn(
                 std::move(second_configuration),
                 [&platform_view_delegate](Shell& shell) {
                   auto result = std::make_unique<MockPlatformView>(
                       platform_view_delegate, shell.GetTaskRunners());
                   return result;
                 });
             ASSERT_NE(nullptr, spawn.get());
             ASSERT_TRUE(ValidateShell(spawn.get()));

             PostSync(spawner->GetTaskRunners().GetUITaskRunner(),
                      [&spawn, &spawner] {
                        // Check second shell ran the second entrypoint.
                        ASSERT_EQ("testCanLaunchSecondaryIsolate",
                                  spawn->GetEngine()->GetLastEntrypoint());

                        ASSERT_NE(spawner->GetEngine()
                                      ->GetRuntimeController()
                                      ->GetRootIsolateGroup(),
                                  0u);
                        ASSERT_EQ(spawner->GetEngine()
                                      ->GetRuntimeController()
                                      ->GetRootIsolateGroup(),
                                  spawn->GetEngine()
                                      ->GetRuntimeController()
                                      ->GetRootIsolateGroup());
                      });

             // Before destroying the shell, wait for expectations of the
             // spawned isolate to be met.
             second_latch.Wait();

             DestroyShell(std::move(spawn));
           });

  DestroyShell(std::move(shell));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, SpawnWithDartEntrypointArgs) {
  auto settings = CreateSettingsForFixture();
  auto shell = CreateShell(settings);
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(configuration.IsValid());
  configuration.SetEntrypoint("canReceiveArgumentsWhenEngineRun");
  const std::vector<std::string> entrypoint_args{"foo", "bar"};
  configuration.SetEntrypointArgs(entrypoint_args);

  auto second_configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(second_configuration.IsValid());
  second_configuration.SetEntrypoint("canReceiveArgumentsWhenEngineSpawn");
  const std::vector<std::string> second_entrypoint_args{"arg1", "arg2"};
  second_configuration.SetEntrypointArgs(second_entrypoint_args);

  const std::string initial_route("/foo");

  fml::AutoResetWaitableEvent main_latch;
  std::string last_entry_point;
  // Fulfill native function for the first Shell's entrypoint.
  AddNativeCallback("NotifyNativeWhenEngineRun",
                    CREATE_NATIVE_ENTRY(([&](Dart_NativeArguments args) {
                      ASSERT_TRUE(tonic::DartConverter<bool>::FromDart(
                          Dart_GetNativeArgument(args, 0)));
                      last_entry_point =
                          shell->GetEngine()->GetLastEntrypoint();
                      main_latch.Signal();
                    })));

  fml::AutoResetWaitableEvent second_latch;
  // Fulfill native function for the second Shell's entrypoint.
  AddNativeCallback("NotifyNativeWhenEngineSpawn",
                    CREATE_NATIVE_ENTRY(([&](Dart_NativeArguments args) {
                      ASSERT_TRUE(tonic::DartConverter<bool>::FromDart(
                          Dart_GetNativeArgument(args, 0)));
                      last_entry_point =
                          shell->GetEngine()->GetLastEntrypoint();
                      second_latch.Signal();
                    })));

  RunEngine(shell.get(), std::move(configuration));
  main_latch.Wait();
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  // Check first Shell ran the first entrypoint.
  ASSERT_EQ("canReceiveArgumentsWhenEngineRun", last_entry_point);

  PostSync(shell->GetTaskRunners().GetPlatformTaskRunner(),
           [this, &spawner = shell, &second_configuration, &second_latch,
            initial_route]() {
             MockPlatformViewDelegate platform_view_delegate;
             auto spawn = spawner->Spawn(
                 std::move(second_configuration),
                 [&platform_view_delegate](Shell& shell) {
                   auto result = std::make_unique<MockPlatformView>(
                       platform_view_delegate, shell.GetTaskRunners());
                   return result;
                 });
             ASSERT_NE(nullptr, spawn.get());
             ASSERT_TRUE(ValidateShell(spawn.get()));

             PostSync(spawner->GetTaskRunners().GetUITaskRunner(),
                      [&spawn, &spawner] {
                        // Check second shell ran the second entrypoint.
                        ASSERT_EQ("canReceiveArgumentsWhenEngineSpawn",
                                  spawn->GetEngine()->GetLastEntrypoint());

                        ASSERT_NE(spawner->GetEngine()
                                      ->GetRuntimeController()
                                      ->GetRootIsolateGroup(),
                                  0u);
                        ASSERT_EQ(spawner->GetEngine()
                                      ->GetRuntimeController()
                                      ->GetRootIsolateGroup(),
                                  spawn->GetEngine()
                                      ->GetRuntimeController()
                                      ->GetRootIsolateGroup());
                      });

             // Before destroying the shell, wait for expectations of the
             // spawned isolate to be met.
             second_latch.Wait();

             DestroyShell(std::move(spawn));
           });

  DestroyShell(std::move(shell));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, IOManagerIsSharedBetweenParentAndSpawnedShell) {
  auto settings = CreateSettingsForFixture();
  auto shell = CreateShell(settings);
  ASSERT_TRUE(ValidateShell(shell.get()));

  PostSync(shell->GetTaskRunners().GetPlatformTaskRunner(),
           [this, &spawner = shell, &settings] {
             auto second_configuration =
                 RunConfiguration::InferFromSettings(settings);
             ASSERT_TRUE(second_configuration.IsValid());
             second_configuration.SetEntrypoint("emptyMain");
             const std::string initial_route("/foo");
             MockPlatformViewDelegate platform_view_delegate;
             auto spawn = spawner->Spawn(
                 std::move(second_configuration),
                 [&platform_view_delegate](Shell& shell) {
                   auto result = std::make_unique<MockPlatformView>(
                       platform_view_delegate, shell.GetTaskRunners());
                   return result;
                 });
             ASSERT_TRUE(ValidateShell(spawn.get()));

             // Destroy the child shell.
             DestroyShell(std::move(spawn));
           });
  // Destroy the parent shell.
  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, UpdateAssetResolverByTypeReplaces) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();

  fml::MessageLoop::EnsureInitializedForCurrentThread();
  auto task_runner = fml::MessageLoop::GetCurrent().GetTaskRunner();
  TaskRunners task_runners("test", task_runner, task_runner);
  auto shell = CreateShell(settings, task_runners);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");
  auto asset_manager = configuration.GetAssetManager();

  shell->RunEngine(std::move(configuration), [&](auto result) {
    ASSERT_EQ(result, Engine::RunStatus::Success);
  });

  auto platform_view =
      std::make_unique<PlatformView>(*shell.get(), task_runners);

  auto old_resolver = std::make_unique<TestAssetResolver>(
      true, AssetResolver::AssetResolverType::kApkAssetProvider);
  ASSERT_TRUE(old_resolver->IsValid());
  asset_manager->PushBack(std::move(old_resolver));

  auto updated_resolver = std::make_unique<TestAssetResolver>(
      false, AssetResolver::AssetResolverType::kApkAssetProvider);
  ASSERT_FALSE(updated_resolver->IsValidAfterAssetManagerChange());
  platform_view->UpdateAssetResolverByType(
      std::move(updated_resolver),
      AssetResolver::AssetResolverType::kApkAssetProvider);

  auto resolvers = asset_manager->TakeResolvers();
  ASSERT_EQ(resolvers.size(), 2ull);
  ASSERT_TRUE(resolvers[0]->IsValidAfterAssetManagerChange());

  ASSERT_FALSE(resolvers[1]->IsValidAfterAssetManagerChange());

  DestroyShell(std::move(shell), task_runners);
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, UpdateAssetResolverByTypeAppends) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();

  fml::MessageLoop::EnsureInitializedForCurrentThread();
  auto task_runner = fml::MessageLoop::GetCurrent().GetTaskRunner();
  TaskRunners task_runners("test", task_runner, task_runner);
  auto shell = CreateShell(settings, task_runners);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");
  auto asset_manager = configuration.GetAssetManager();

  shell->RunEngine(std::move(configuration), [&](auto result) {
    ASSERT_EQ(result, Engine::RunStatus::Success);
  });

  auto platform_view =
      std::make_unique<PlatformView>(*shell.get(), task_runners);

  auto updated_resolver = std::make_unique<TestAssetResolver>(
      false, AssetResolver::AssetResolverType::kApkAssetProvider);
  ASSERT_FALSE(updated_resolver->IsValidAfterAssetManagerChange());
  platform_view->UpdateAssetResolverByType(
      std::move(updated_resolver),
      AssetResolver::AssetResolverType::kApkAssetProvider);

  auto resolvers = asset_manager->TakeResolvers();
  ASSERT_EQ(resolvers.size(), 2ull);
  ASSERT_TRUE(resolvers[0]->IsValidAfterAssetManagerChange());

  ASSERT_FALSE(resolvers[1]->IsValidAfterAssetManagerChange());

  DestroyShell(std::move(shell), task_runners);
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, UpdateAssetResolverByTypeNull) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  ThreadHost thread_host(ThreadHost::ThreadHostConfig(
      "io.flutter.test." + GetCurrentTestName() + ".",
      ThreadHost::Type::kPlatform));
  auto task_runner = thread_host.platform_thread->GetTaskRunner();
  TaskRunners task_runners("test", task_runner, task_runner);
  auto shell = CreateShell(settings, task_runners);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");
  auto asset_manager = configuration.GetAssetManager();
  RunEngine(shell.get(), std::move(configuration));

  auto platform_view =
      std::make_unique<PlatformView>(*shell.get(), task_runners);

  auto old_resolver = std::make_unique<TestAssetResolver>(
      true, AssetResolver::AssetResolverType::kApkAssetProvider);
  ASSERT_TRUE(old_resolver->IsValid());
  asset_manager->PushBack(std::move(old_resolver));

  platform_view->UpdateAssetResolverByType(
      nullptr, AssetResolver::AssetResolverType::kApkAssetProvider);

  auto resolvers = asset_manager->TakeResolvers();
  ASSERT_EQ(resolvers.size(), 2ull);
  ASSERT_TRUE(resolvers[0]->IsValidAfterAssetManagerChange());
  ASSERT_TRUE(resolvers[1]->IsValidAfterAssetManagerChange());

  DestroyShell(std::move(shell), task_runners);
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, UpdateAssetResolverByTypeDoesNotReplaceMismatchType) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();

  fml::MessageLoop::EnsureInitializedForCurrentThread();
  auto task_runner = fml::MessageLoop::GetCurrent().GetTaskRunner();
  TaskRunners task_runners("test", task_runner, task_runner);
  auto shell = CreateShell(settings, task_runners);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");
  auto asset_manager = configuration.GetAssetManager();

  shell->RunEngine(std::move(configuration), [&](auto result) {
    ASSERT_EQ(result, Engine::RunStatus::Success);
  });

  auto platform_view =
      std::make_unique<PlatformView>(*shell.get(), task_runners);

  auto old_resolver = std::make_unique<TestAssetResolver>(
      true, AssetResolver::AssetResolverType::kAssetManager);
  ASSERT_TRUE(old_resolver->IsValid());
  asset_manager->PushBack(std::move(old_resolver));

  auto updated_resolver = std::make_unique<TestAssetResolver>(
      false, AssetResolver::AssetResolverType::kApkAssetProvider);
  ASSERT_FALSE(updated_resolver->IsValidAfterAssetManagerChange());
  platform_view->UpdateAssetResolverByType(
      std::move(updated_resolver),
      AssetResolver::AssetResolverType::kApkAssetProvider);

  auto resolvers = asset_manager->TakeResolvers();
  ASSERT_EQ(resolvers.size(), 3ull);
  ASSERT_TRUE(resolvers[0]->IsValidAfterAssetManagerChange());

  ASSERT_TRUE(resolvers[1]->IsValidAfterAssetManagerChange());

  ASSERT_FALSE(resolvers[2]->IsValidAfterAssetManagerChange());

  DestroyShell(std::move(shell), task_runners);
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, UserTagSetOnStartup) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  // Make sure the shell launch does not kick off the creation of the VM
  // instance by already creating one upfront.
  auto vm_settings = CreateSettingsForFixture();
  auto vm_ref = DartVMRef::Create(vm_settings);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());

  auto settings = vm_settings;
  fml::AutoResetWaitableEvent isolate_create_latch;

  // ensure that "AppStartUpTag" is set during isolate creation.
  settings.root_isolate_create_callback = [&](const DartIsolate& isolate) {
    Dart_Handle current_tag = Dart_GetCurrentUserTag();
    Dart_Handle startup_tag = Dart_NewUserTag("AppStartUp");
    EXPECT_TRUE(Dart_IdentityEquals(current_tag, startup_tag));

    isolate_create_latch.Signal();
  };

  auto shell = CreateShell(settings);
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(configuration.IsValid());

  RunEngine(shell.get(), std::move(configuration));
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());

  DestroyShell(std::move(shell));
  isolate_create_latch.Wait();
}

TEST_F(ShellTest, OnPlatformViewCreatedWhenUIThreadIsBusy) {
  // This test will deadlock if the threading logic in
  // Shell::OnCreatePlatformView is wrong.
  auto settings = CreateSettingsForFixture();
  auto shell = CreateShell(settings);

  fml::AutoResetWaitableEvent latch;
  fml::TaskRunner::RunNowOrPostTask(shell->GetTaskRunners().GetUITaskRunner(),
                                    [&latch]() { latch.Wait(); });

  ShellTest::PlatformViewNotifyCreated(shell.get());
  latch.Signal();

  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, UsesPlatformMessageHandler) {
  TaskRunners task_runners = GetTaskRunnersForFixture();
  auto settings = CreateSettingsForFixture();
  MockPlatformViewDelegate platform_view_delegate;
  auto platform_message_handler =
      std::make_shared<MockPlatformMessageHandler>();
  int message_id = 1;
  EXPECT_CALL(*platform_message_handler, HandlePlatformMessage(_));
  EXPECT_CALL(*platform_message_handler,
              InvokePlatformMessageEmptyResponseCallback(message_id));
  Shell::CreateCallback<PlatformView> platform_view_create_callback =
      [&platform_view_delegate, task_runners,
       platform_message_handler](flutter::Shell& shell) {
        auto result = std::make_unique<MockPlatformView>(platform_view_delegate,
                                                         task_runners);
        EXPECT_CALL(*result, GetPlatformMessageHandler())
            .WillOnce(Return(platform_message_handler));
        return result;
      };
  auto shell = CreateShell({
      .settings = settings,
      .task_runners = task_runners,
      .platform_view_create_callback = platform_view_create_callback,
  });

  EXPECT_EQ(platform_message_handler, shell->GetPlatformMessageHandler());
  PostSync(task_runners.GetUITaskRunner(), [&shell]() {
    size_t data_size = 4;
    fml::MallocMapping bytes =
        fml::MallocMapping(static_cast<uint8_t*>(malloc(data_size)), data_size);
    fml::RefPtr<MockPlatformMessageResponse> response =
        MockPlatformMessageResponse::Create();
    auto message = std::make_unique<PlatformMessage>(
        /*channel=*/"foo", /*data=*/std::move(bytes), /*response=*/response);
    (static_cast<Engine::Delegate*>(shell.get()))
        ->OnEngineHandlePlatformMessage(std::move(message));
  });
  shell->GetPlatformMessageHandler()
      ->InvokePlatformMessageEmptyResponseCallback(message_id);
  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, SpawnWorksWithOnError) {
  auto settings = CreateSettingsForFixture();
  auto shell = CreateShell(settings);
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(configuration.IsValid());
  configuration.SetEntrypoint("onErrorA");

  auto second_configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(second_configuration.IsValid());
  second_configuration.SetEntrypoint("onErrorB");

  fml::CountDownLatch latch(2);

  AddNativeCallback(
      "NotifyErrorA", CREATE_NATIVE_ENTRY([&](Dart_NativeArguments args) {
        auto string_handle = Dart_GetNativeArgument(args, 0);
        const char* c_str;
        Dart_StringToCString(string_handle, &c_str);
        EXPECT_STREQ(c_str, "Exception: I should be coming from A");
        latch.CountDown();
      }));

  AddNativeCallback(
      "NotifyErrorB", CREATE_NATIVE_ENTRY([&](Dart_NativeArguments args) {
        auto string_handle = Dart_GetNativeArgument(args, 0);
        const char* c_str;
        Dart_StringToCString(string_handle, &c_str);
        EXPECT_STREQ(c_str, "Exception: I should be coming from B");
        latch.CountDown();
      }));

  RunEngine(shell.get(), std::move(configuration));

  ASSERT_TRUE(DartVMRef::IsInstanceRunning());

  PostSync(
      shell->GetTaskRunners().GetPlatformTaskRunner(),
      [this, &spawner = shell, &second_configuration, &latch]() {
        ::testing::NiceMock<MockPlatformViewDelegate> platform_view_delegate;
        auto spawn = spawner->Spawn(
            std::move(second_configuration),
            [&platform_view_delegate](Shell& shell) {
              auto result =
                  std::make_unique<::testing::NiceMock<MockPlatformView>>(
                      platform_view_delegate, shell.GetTaskRunners());

              return result;
            });
        ASSERT_NE(nullptr, spawn.get());
        ASSERT_TRUE(ValidateShell(spawn.get()));

        // Before destroying the shell, wait for expectations of the spawned
        // isolate to be met.
        latch.Wait();

        DestroyShell(std::move(spawn));
      });

  DestroyShell(std::move(shell));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, ImmutableBufferLoadsAssetOnBackgroundThread) {
  Settings settings = CreateSettingsForFixture();
  auto task_runner = CreateNewThread();
  TaskRunners task_runners("test", task_runner, task_runner);
  std::unique_ptr<Shell> shell = CreateShell(settings, task_runners);

  fml::CountDownLatch latch(1);
  AddNativeCallback("NotifyNative",
                    CREATE_NATIVE_ENTRY([&](auto args) { latch.CountDown(); }));

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("testThatAssetLoadingHappensOnWorkerThread");
  auto asset_manager = configuration.GetAssetManager();
  auto test_resolver = std::make_unique<ThreadCheckingAssetResolver>(
      shell->GetDartVM()->GetConcurrentMessageLoop());
  auto leaked_resolver = test_resolver.get();
  asset_manager->PushBack(std::move(test_resolver));

  RunEngine(shell.get(), std::move(configuration));

  latch.Wait();
  // std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_EQ(leaked_resolver->mapping_requests[0], "DoesNotExist");

  PlatformViewNotifyDestroyed(shell.get());
  DestroyShell(std::move(shell), task_runners);
}

TEST_F(ShellTest, PluginUtilitiesCallbackHandleErrorHandling) {
  auto settings = CreateSettingsForFixture();
  std::unique_ptr<Shell> shell = CreateShell(settings);

  fml::AutoResetWaitableEvent latch;
  bool test_passed;
  AddNativeCallback("NotifyNativeBool", CREATE_NATIVE_ENTRY([&](auto args) {
                      Dart_Handle exception = nullptr;
                      test_passed = tonic::DartConverter<bool>::FromArguments(
                          args, 0, exception);
                      latch.Signal();
                    }));

  ASSERT_NE(shell, nullptr);
  ASSERT_TRUE(shell->IsSetup());
  auto configuration = RunConfiguration::InferFromSettings(settings);
  PlatformViewNotifyCreated(shell.get());
  configuration.SetEntrypoint("testPluginUtilitiesCallbackHandle");
  RunEngine(shell.get(), std::move(configuration));
  PumpOneFrame(shell.get());

  latch.Wait();

  ASSERT_TRUE(test_passed);

  PlatformViewNotifyDestroyed(shell.get());
  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, NotifyIdleRejectsPastAndNearFuture) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  ThreadHost thread_host("io.flutter.test." + GetCurrentTestName() + ".",
                         ThreadHost::Type::kPlatform | ThreadHost::kUi);
  auto platform_task_runner = thread_host.platform_thread->GetTaskRunner();
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner());
  auto shell = CreateShell(settings, task_runners);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  ASSERT_TRUE(ValidateShell(shell.get()));

  fml::AutoResetWaitableEvent latch;

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");
  RunEngine(shell.get(), std::move(configuration));

  fml::TaskRunner::RunNowOrPostTask(
      task_runners.GetUITaskRunner(), [&latch, &shell]() {
        auto runtime_controller = const_cast<RuntimeController*>(
            shell->GetEngine()->GetRuntimeController());

        auto now = fml::TimeDelta::FromMicroseconds(Dart_TimelineGetMicros());

        EXPECT_FALSE(runtime_controller->NotifyIdle(
            now - fml::TimeDelta::FromMilliseconds(10)));
        EXPECT_FALSE(runtime_controller->NotifyIdle(now));
        EXPECT_FALSE(runtime_controller->NotifyIdle(
            now + fml::TimeDelta::FromNanoseconds(100)));

        EXPECT_TRUE(runtime_controller->NotifyIdle(
            now + fml::TimeDelta::FromMilliseconds(100)));
        latch.Signal();
      });

  latch.Wait();

  DestroyShell(std::move(shell), task_runners);
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, NotifyIdleNotCalledInLatencyMode) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  ThreadHost thread_host("io.flutter.test." + GetCurrentTestName() + ".",
                         ThreadHost::Type::kPlatform | ThreadHost::kUi);
  auto platform_task_runner = thread_host.platform_thread->GetTaskRunner();
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner());
  auto shell = CreateShell(settings, task_runners);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  ASSERT_TRUE(ValidateShell(shell.get()));

  // we start off in balanced mode, where we expect idle notifications to
  // succeed. After the first `NotifyNativeBool` we expect to be in latency
  // mode, where we expect idle notifications to fail.
  fml::CountDownLatch latch(2);
  AddNativeCallback(
      "NotifyNativeBool", CREATE_NATIVE_ENTRY([&](auto args) {
        Dart_Handle exception = nullptr;
        bool is_in_latency_mode =
            tonic::DartConverter<bool>::FromArguments(args, 0, exception);
        auto runtime_controller = const_cast<RuntimeController*>(
            shell->GetEngine()->GetRuntimeController());
        bool success =
            runtime_controller->NotifyIdle(fml::TimeDelta::FromMicroseconds(
                Dart_TimelineGetMicros() + 100000));
        EXPECT_EQ(success, !is_in_latency_mode);
        latch.CountDown();
      }));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("performanceModeImpactsNotifyIdle");
  RunEngine(shell.get(), std::move(configuration));

  latch.Wait();

  DestroyShell(std::move(shell), task_runners);
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, PrintsErrorWhenPlatformMessageSentFromWrongThread) {
#if FLUTTER_RUNTIME_MODE != FLUTTER_RUNTIME_MODE_DEBUG || OS_FUCHSIA
  GTEST_SKIP() << "Test is for debug mode only on non-fuchsia targets.";
#else
  Settings settings = CreateSettingsForFixture();
  ThreadHost thread_host("io.flutter.test." + GetCurrentTestName() + ".",
                         ThreadHost::Type::kPlatform);
  auto task_runner = thread_host.platform_thread->GetTaskRunner();
  TaskRunners task_runners("test", task_runner, task_runner);
  auto shell = CreateShell(settings, task_runners);

  {
    fml::testing::LogCapture log_capture;

    // The next call will result in a thread checker violation.
    fml::ThreadChecker::DisableNextThreadCheckFailure();
    SendPlatformMessage(shell.get(), std::make_unique<PlatformMessage>(
                                         "com.test.plugin", nullptr));

    EXPECT_THAT(
        log_capture.str(),
        ::testing::EndsWith(
            "The 'com.test.plugin' channel sent a message from native to "
            "Flutter on a non-platform thread. Platform channel messages "
            "must be sent on the platform thread. Failure to do so may "
            "result in data loss or crashes, and must be fixed in the "
            "plugin or application code creating that channel.\nSee "
            "https://docs.flutter.dev/platform-integration/"
            "platform-channels#channels-and-platform-threading for more "
            "information.\n"));
  }

  {
    fml::testing::LogCapture log_capture;

    // The next call will result in a thread checker violation.
    fml::ThreadChecker::DisableNextThreadCheckFailure();
    SendPlatformMessage(shell.get(), std::make_unique<PlatformMessage>(
                                         "com.test.plugin", nullptr));

    EXPECT_EQ(log_capture.str(), "");
  }

  DestroyShell(std::move(shell), task_runners);
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
#endif
}

TEST_F(ShellTest, ProvidesEngineId) {
  Settings settings = CreateSettingsForFixture();
  TaskRunners task_runners = GetTaskRunnersForFixture();
  fml::AutoResetWaitableEvent latch;

  std::optional<int> reported_handle = std::nullopt;

  AddNativeCallback(
      "ReportEngineId", CREATE_NATIVE_ENTRY([&](Dart_NativeArguments args) {
        Dart_Handle arg = Dart_GetNativeArgument(args, 0);
        if (Dart_IsNull(arg)) {
          reported_handle = std::nullopt;
        } else {
          reported_handle = tonic::DartConverter<int64_t>::FromDart(arg);
        }
        latch.Signal();
      }));
  fml::AutoResetWaitableEvent check_latch;

  std::unique_ptr<Shell> shell = CreateShell(settings, task_runners);
  ASSERT_TRUE(shell->IsSetup());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEngineId(99);
  configuration.SetEntrypoint("providesEngineId");
  RunEngine(shell.get(), std::move(configuration));

  latch.Wait();
  ASSERT_EQ(reported_handle, 99);

  latch.Reset();

  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetUITaskRunner(), [&]() {
        ASSERT_EQ(shell->GetEngine()->GetLastEngineId(), 99);
        latch.Signal();
      });
  latch.Wait();
  DestroyShell(std::move(shell), task_runners);
}

TEST_F(ShellTest, ProvidesNullEngineId) {
  Settings settings = CreateSettingsForFixture();
  TaskRunners task_runners = GetTaskRunnersForFixture();
  fml::AutoResetWaitableEvent latch;

  std::optional<int> reported_handle = std::nullopt;

  AddNativeCallback(
      "ReportEngineId", CREATE_NATIVE_ENTRY([&](Dart_NativeArguments args) {
        Dart_Handle arg = Dart_GetNativeArgument(args, 0);
        if (Dart_IsNull(arg)) {
          reported_handle = std::nullopt;
        } else {
          reported_handle = tonic::DartConverter<int64_t>::FromDart(arg);
        }
        latch.Signal();
      }));
  fml::AutoResetWaitableEvent check_latch;

  std::unique_ptr<Shell> shell = CreateShell(settings, task_runners);
  ASSERT_TRUE(shell->IsSetup());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("providesEngineId");
  RunEngine(shell.get(), std::move(configuration));

  latch.Wait();
  ASSERT_EQ(reported_handle, std::nullopt);
  DestroyShell(std::move(shell), task_runners);
}

TEST_F(ShellTest, MergeUIAndPlatformThreadsAfterLaunch) {
  Settings settings = CreateSettingsForFixture();
  settings.merged_platform_ui_thread =
      Settings::MergedPlatformUIThread::kMergeAfterLaunch;
  ThreadHost thread_host(ThreadHost::ThreadHostConfig(
      "io.flutter.test." + GetCurrentTestName() + ".",
      ThreadHost::Type::kPlatform | ThreadHost::Type::kUi));
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner());

  std::unique_ptr<Shell> shell = CreateShell(settings, task_runners);
  ASSERT_TRUE(shell);

  ASSERT_FALSE(fml::TaskRunnerChecker::RunsOnTheSameThread(
      task_runners.GetUITaskRunner()->GetTaskQueueId(),
      task_runners.GetPlatformTaskRunner()->GetTaskQueueId()));

  fml::AutoResetWaitableEvent latch;
  AddNativeCallback(
      "NotifyNative", CREATE_NATIVE_ENTRY([&](auto args) {
        ASSERT_TRUE(
            task_runners.GetPlatformTaskRunner()->RunsTasksOnCurrentThread());
        latch.Signal();
      }));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("mainNotifyNative");
  RunEngine(shell.get(), std::move(configuration));

  latch.Wait();

  ASSERT_TRUE(fml::TaskRunnerChecker::RunsOnTheSameThread(
      task_runners.GetUITaskRunner()->GetTaskQueueId(),
      task_runners.GetPlatformTaskRunner()->GetTaskQueueId()));

  DestroyShell(std::move(shell), task_runners);
}

}  // namespace testing
}  // namespace flutter

// NOLINTEND(clang-analyzer-core.StackAddressEscape)
