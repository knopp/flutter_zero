// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER
#define RAPIDJSON_HAS_STDSTRING 1

#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "flutter/fml/closure.h"
#include "flutter/fml/make_copyable.h"
#include "flutter/fml/thread.h"
#include "third_party/dart/runtime/bin/elf_loader.h"
#include "third_party/dart/runtime/include/dart_native_api.h"

#if !defined(FLUTTER_NO_EXPORT)
#if FML_OS_WIN
#define FLUTTER_EXPORT __declspec(dllexport)
#else  // FML_OS_WIN
#define FLUTTER_EXPORT __attribute__((visibility("default")))
#endif  // FML_OS_WIN
#endif  // !FLUTTER_NO_EXPORT

extern "C" {
#if FLUTTER_RUNTIME_MODE == FLUTTER_RUNTIME_MODE_DEBUG
// Used for debugging dart:* sources.
extern const uint8_t kPlatformStrongDill[];
extern const intptr_t kPlatformStrongDillSize;
#endif  // FLUTTER_RUNTIME_MODE == FLUTTER_RUNTIME_MODE_DEBUG
}

#include "flutter/common/task_runners.h"
#include "flutter/fml/command_line.h"
#include "flutter/fml/file.h"
#include "flutter/fml/make_copyable.h"
#include "flutter/fml/message_loop.h"
#include "flutter/fml/paths.h"
#include "flutter/fml/trace_event.h"
#include "flutter/shell/common/switches.h"
#include "flutter/shell/platform/embedder/embedder.h"
#include "flutter/shell/platform/embedder/embedder_engine.h"
#include "flutter/shell/platform/embedder/embedder_platform_message_response.h"
#include "flutter/shell/platform/embedder/embedder_struct_macros.h"
#include "flutter/shell/platform/embedder/embedder_thread_host.h"
#include "flutter/shell/platform/embedder/platform_view_embedder.h"
#include "rapidjson/rapidjson.h"
#include "rapidjson/writer.h"

static FlutterEngineResult LogEmbedderError(FlutterEngineResult code,
                                            const char* reason,
                                            const char* code_name,
                                            const char* function,
                                            const char* file,
                                            int line) {
#if FML_OS_WIN
  constexpr char kSeparator = '\\';
#else
  constexpr char kSeparator = '/';
#endif
  const auto file_base =
      (::strrchr(file, kSeparator) ? strrchr(file, kSeparator) + 1 : file);
  char error[256] = {};
  snprintf(error, (sizeof(error) / sizeof(char)),
           "%s (%d): '%s' returned '%s'. %s", file_base, line, function,
           code_name, reason);
  std::cerr << error << std::endl;
  return code;
}

#define LOG_EMBEDDER_ERROR(code, reason) \
  LogEmbedderError(code, reason, #code, __FUNCTION__, __FILE__, __LINE__)

struct _FlutterPlatformMessageResponseHandle {
  std::unique_ptr<flutter::PlatformMessage> message;
};

struct LoadedElfDeleter {
  void operator()(Dart_LoadedElf* elf) {
    if (elf) {
      ::Dart_UnloadELF(elf);
    }
  }
};

using UniqueLoadedElf = std::unique_ptr<Dart_LoadedElf, LoadedElfDeleter>;

struct _FlutterEngineAOTData {
  UniqueLoadedElf loaded_elf = nullptr;
  const uint8_t* vm_snapshot_data = nullptr;
  const uint8_t* vm_snapshot_instrs = nullptr;
  const uint8_t* vm_isolate_data = nullptr;
  const uint8_t* vm_isolate_instrs = nullptr;
};

FlutterEngineResult FlutterEngineCreateAOTData(
    const FlutterEngineAOTDataSource* source,
    FlutterEngineAOTData* data_out) {
  if (!flutter::DartVM::IsRunningPrecompiledCode()) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments,
                              "AOT data can only be created in AOT mode.");
  } else if (!source) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments, "Null source specified.");
  } else if (!data_out) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments, "Null data_out specified.");
  }

  switch (source->type) {
    case kFlutterEngineAOTDataSourceTypeElfPath: {
      if (!source->elf_path || !fml::IsFile(source->elf_path)) {
        return LOG_EMBEDDER_ERROR(kInvalidArguments,
                                  "Invalid ELF path specified.");
      }

      auto aot_data = std::make_unique<_FlutterEngineAOTData>();
      const char* error = nullptr;

#if OS_FUCHSIA
      // TODO(gw280): https://github.com/flutter/flutter/issues/50285
      // Dart doesn't implement Dart_LoadELF on Fuchsia
      Dart_LoadedElf* loaded_elf = nullptr;
#else
      Dart_LoadedElf* loaded_elf = Dart_LoadELF(
          source->elf_path,               // file path
          0,                              // file offset
          &error,                         // error (out)
          &aot_data->vm_snapshot_data,    // vm snapshot data (out)
          &aot_data->vm_snapshot_instrs,  // vm snapshot instr (out)
          &aot_data->vm_isolate_data,     // vm isolate data (out)
          &aot_data->vm_isolate_instrs    // vm isolate instr (out)
      );
#endif

      if (loaded_elf == nullptr) {
        return LOG_EMBEDDER_ERROR(kInvalidArguments, error);
      }

      aot_data->loaded_elf.reset(loaded_elf);

      *data_out = aot_data.release();
      return kSuccess;
    }
  }

  return LOG_EMBEDDER_ERROR(
      kInvalidArguments,
      "Invalid FlutterEngineAOTDataSourceType type specified.");
}

FlutterEngineResult FlutterEngineCollectAOTData(FlutterEngineAOTData data) {
  if (!data) {
    // Deleting a null object should be a no-op.
    return kSuccess;
  }

  // Created in a unique pointer in `FlutterEngineCreateAOTData`.
  delete data;
  return kSuccess;
}

// Constructs appropriate mapping callbacks if JIT snapshot locations have
// been explictly specified.
void PopulateJITSnapshotMappingCallbacks(const FlutterProjectArgs* args,
                                         flutter::Settings& settings) {
  auto make_mapping_callback = [](const char* path, bool executable) {
    return [path, executable]() {
      if (executable) {
        return fml::FileMapping::CreateReadExecute(path);
      } else {
        return fml::FileMapping::CreateReadOnly(path);
      }
    };
  };

  // Users are allowed to specify only certain snapshots if they so desire.
  if (SAFE_ACCESS(args, vm_snapshot_data, nullptr) != nullptr) {
    settings.vm_snapshot_data = make_mapping_callback(
        reinterpret_cast<const char*>(args->vm_snapshot_data), false);
  }

  if (SAFE_ACCESS(args, vm_snapshot_instructions, nullptr) != nullptr) {
    settings.vm_snapshot_instr = make_mapping_callback(
        reinterpret_cast<const char*>(args->vm_snapshot_instructions), true);
  }

  if (SAFE_ACCESS(args, isolate_snapshot_data, nullptr) != nullptr) {
    settings.isolate_snapshot_data = make_mapping_callback(
        reinterpret_cast<const char*>(args->isolate_snapshot_data), false);
  }

  if (SAFE_ACCESS(args, isolate_snapshot_instructions, nullptr) != nullptr) {
    settings.isolate_snapshot_instr = make_mapping_callback(
        reinterpret_cast<const char*>(args->isolate_snapshot_instructions),
        true);
  }

#if !OS_FUCHSIA && (FLUTTER_RUNTIME_MODE == FLUTTER_RUNTIME_MODE_DEBUG)
  settings.dart_library_sources_kernel = []() {
    return std::make_unique<fml::NonOwnedMapping>(kPlatformStrongDill,
                                                  kPlatformStrongDillSize);
  };
#endif  // !OS_FUCHSIA && (FLUTTER_RUNTIME_MODE ==
        // FLUTTER_RUNTIME_MODE_DEBUG)
}

void PopulateAOTSnapshotMappingCallbacks(
    const FlutterProjectArgs* args,
    flutter::Settings& settings) {  // NOLINT(google-runtime-references)
  // There are no ownership concerns here as all mappings are owned by the
  // embedder and not the engine.
  auto make_mapping_callback = [](const uint8_t* mapping, size_t size) {
    return [mapping, size]() {
      return std::make_unique<fml::NonOwnedMapping>(mapping, size);
    };
  };

  if (SAFE_ACCESS(args, aot_data, nullptr) != nullptr) {
    settings.vm_snapshot_data =
        make_mapping_callback(args->aot_data->vm_snapshot_data, 0);

    settings.vm_snapshot_instr =
        make_mapping_callback(args->aot_data->vm_snapshot_instrs, 0);

    settings.isolate_snapshot_data =
        make_mapping_callback(args->aot_data->vm_isolate_data, 0);

    settings.isolate_snapshot_instr =
        make_mapping_callback(args->aot_data->vm_isolate_instrs, 0);
  }

  if (SAFE_ACCESS(args, vm_snapshot_data, nullptr) != nullptr) {
    settings.vm_snapshot_data = make_mapping_callback(
        args->vm_snapshot_data, SAFE_ACCESS(args, vm_snapshot_data_size, 0));
  }

  if (SAFE_ACCESS(args, vm_snapshot_instructions, nullptr) != nullptr) {
    settings.vm_snapshot_instr = make_mapping_callback(
        args->vm_snapshot_instructions,
        SAFE_ACCESS(args, vm_snapshot_instructions_size, 0));
  }

  if (SAFE_ACCESS(args, isolate_snapshot_data, nullptr) != nullptr) {
    settings.isolate_snapshot_data =
        make_mapping_callback(args->isolate_snapshot_data,
                              SAFE_ACCESS(args, isolate_snapshot_data_size, 0));
  }

  if (SAFE_ACCESS(args, isolate_snapshot_instructions, nullptr) != nullptr) {
    settings.isolate_snapshot_instr = make_mapping_callback(
        args->isolate_snapshot_instructions,
        SAFE_ACCESS(args, isolate_snapshot_instructions_size, 0));
  }
}

FlutterEngineResult FlutterEngineRun(size_t version,
                                     const FlutterProjectArgs* args,
                                     void* user_data,
                                     FLUTTER_API_SYMBOL(FlutterEngine) *
                                         engine_out) {
  auto result = FlutterEngineInitialize(version, args, user_data, engine_out);

  if (result != kSuccess) {
    return result;
  }

  return FlutterEngineRunInitialized(*engine_out);
}

static flutter::Shell::CreateCallback<flutter::PlatformView>
InferPlatformViewCreationCallback(
    void* user_data,
    const flutter::PlatformViewEmbedder::PlatformDispatchTable&
        platform_dispatch_table) {
  // The static leak checker gets confused by the use of fml::MakeCopyable.
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
  return fml::MakeCopyable(
      [platform_dispatch_table](flutter::Shell& shell) mutable {
        return std::make_unique<flutter::PlatformViewEmbedder>(
            shell,                   // delegate
            shell.GetTaskRunners(),  // task runners
            platform_dispatch_table  // platform dispatch table)
        );
      });
}

FlutterEngineResult FlutterEngineInitialize(size_t version,
                                            const FlutterProjectArgs* args,
                                            void* user_data,
                                            FLUTTER_API_SYMBOL(FlutterEngine) *
                                                engine_out) {
  // Step 0: Figure out arguments for shell creation.
  if (version != FLUTTER_ENGINE_VERSION) {
    return LOG_EMBEDDER_ERROR(
        kInvalidLibraryVersion,
        "Flutter embedder version mismatch. There has been a breaking "
        "change. "
        "Please consult the changelog and update the embedder.");
  }

  if (engine_out == nullptr) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments,
                              "The engine out parameter was missing.");
  }

  if (args == nullptr) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments,
                              "The Flutter project arguments were missing.");
  }

  if (SAFE_ACCESS(args, assets_path, nullptr) == nullptr) {
    return LOG_EMBEDDER_ERROR(
        kInvalidArguments,
        "The assets path in the Flutter project arguments was missing.");
  }

  if (SAFE_ACCESS(args, main_path__unused__, nullptr) != nullptr) {
    FML_LOG(WARNING) << "FlutterProjectArgs.main_path is deprecated and "
                        "should be set null.";
  }

  if (SAFE_ACCESS(args, packages_path__unused__, nullptr) != nullptr) {
    FML_LOG(WARNING) << "FlutterProjectArgs.packages_path is deprecated and "
                        "should be set null.";
  }

  std::string icu_data_path;
  if (SAFE_ACCESS(args, icu_data_path, nullptr) != nullptr) {
    icu_data_path = SAFE_ACCESS(args, icu_data_path, nullptr);
  }

  fml::CommandLine command_line;
  if (SAFE_ACCESS(args, command_line_argc, 0) != 0 &&
      SAFE_ACCESS(args, command_line_argv, nullptr) != nullptr) {
    command_line = fml::CommandLineFromArgcArgv(
        SAFE_ACCESS(args, command_line_argc, 0),
        SAFE_ACCESS(args, command_line_argv, nullptr));
  }

  flutter::Settings settings = flutter::SettingsFromCommandLine(command_line);

  if (SAFE_ACCESS(args, aot_data, nullptr)) {
    if (SAFE_ACCESS(args, vm_snapshot_data, nullptr) ||
        SAFE_ACCESS(args, vm_snapshot_instructions, nullptr) ||
        SAFE_ACCESS(args, isolate_snapshot_data, nullptr) ||
        SAFE_ACCESS(args, isolate_snapshot_instructions, nullptr)) {
      return LOG_EMBEDDER_ERROR(
          kInvalidArguments,
          "Multiple AOT sources specified. Embedders should provide either "
          "*_snapshot_* buffers or aot_data, not both.");
    }
  }

  if (flutter::DartVM::IsRunningPrecompiledCode()) {
    PopulateAOTSnapshotMappingCallbacks(args, settings);
  } else {
    PopulateJITSnapshotMappingCallbacks(args, settings);
  }

  settings.icu_data_path = icu_data_path;
  settings.assets_path = args->assets_path;
  settings.leak_vm = !SAFE_ACCESS(args, shutdown_dart_vm_when_done, false);
  settings.old_gen_heap_size = SAFE_ACCESS(args, dart_old_gen_heap_size, -1);

  if (!flutter::DartVM::IsRunningPrecompiledCode()) {
    // Verify the assets path contains Dart 2 kernel assets.
    const std::string kApplicationKernelSnapshotFileName = "kernel_blob.bin";
    std::string application_kernel_path = fml::paths::JoinPaths(
        {settings.assets_path, kApplicationKernelSnapshotFileName});
    if (!fml::IsFile(application_kernel_path)) {
      return LOG_EMBEDDER_ERROR(
          kInvalidArguments,
          "Not running in AOT mode but could not resolve the kernel binary.");
    }
    settings.application_kernel_asset = kApplicationKernelSnapshotFileName;
  }

  if (SAFE_ACCESS(args, root_isolate_create_callback, nullptr) != nullptr) {
    VoidCallback callback =
        SAFE_ACCESS(args, root_isolate_create_callback, nullptr);
    settings.root_isolate_create_callback =
        [callback, user_data](const auto& isolate) { callback(user_data); };
  }

  // Wire up callback for engine and print logging.
  if (SAFE_ACCESS(args, log_message_callback, nullptr) != nullptr) {
    FlutterLogMessageCallback callback =
        SAFE_ACCESS(args, log_message_callback, nullptr);
    settings.log_message_callback = [callback, user_data](
                                        const std::string& tag,
                                        const std::string& message) {
      callback(tag.c_str(), message.c_str(), user_data);
    };
  } else {
    settings.log_message_callback = [](const std::string& tag,
                                       const std::string& message) {
      // Fall back to logging to stdout if unspecified.
      if (tag.empty()) {
        std::cout << tag << ": ";
      }
      std::cout << message << std::endl;
    };
  }

  if (SAFE_ACCESS(args, log_tag, nullptr) != nullptr) {
    settings.log_tag = SAFE_ACCESS(args, log_tag, nullptr);
  }

  flutter::PlatformViewEmbedder::PlatformMessageResponseCallback
      platform_message_response_callback = nullptr;
  if (SAFE_ACCESS(args, platform_message_callback, nullptr) != nullptr) {
    platform_message_response_callback =
        [ptr = args->platform_message_callback,
         user_data](std::unique_ptr<flutter::PlatformMessage> message) {
          auto handle = new FlutterPlatformMessageResponseHandle();
          const FlutterPlatformMessage incoming_message = {
              sizeof(FlutterPlatformMessage),  // struct_size
              message->channel().c_str(),      // channel
              message->data().GetMapping(),    // message
              message->data().GetSize(),       // message_size
              handle,                          // response_handle
          };
          handle->message = std::move(message);
          return ptr(&incoming_message, user_data);
        };
  }

  flutter::PlatformViewEmbedder::ComputePlatformResolvedLocaleCallback
      compute_platform_resolved_locale_callback = nullptr;
  if (SAFE_ACCESS(args, compute_platform_resolved_locale_callback, nullptr) !=
      nullptr) {
    compute_platform_resolved_locale_callback =
        [ptr = args->compute_platform_resolved_locale_callback](
            const std::vector<std::string>& supported_locales_data) {
          const size_t number_of_strings_per_locale = 3;
          size_t locale_count =
              supported_locales_data.size() / number_of_strings_per_locale;
          std::vector<FlutterLocale> supported_locales;
          std::vector<const FlutterLocale*> supported_locales_ptr;
          for (size_t i = 0; i < locale_count; ++i) {
            supported_locales.push_back(
                {.struct_size = sizeof(FlutterLocale),
                 .language_code =
                     supported_locales_data[i * number_of_strings_per_locale +
                                            0]
                         .c_str(),
                 .country_code =
                     supported_locales_data[i * number_of_strings_per_locale +
                                            1]
                         .c_str(),
                 .script_code =
                     supported_locales_data[i * number_of_strings_per_locale +
                                            2]
                         .c_str(),
                 .variant_code = nullptr});
            supported_locales_ptr.push_back(&supported_locales[i]);
          }

          const FlutterLocale* result =
              ptr(supported_locales_ptr.data(), locale_count);

          std::unique_ptr<std::vector<std::string>> out =
              std::make_unique<std::vector<std::string>>();
          if (result) {
            std::string language_code(SAFE_ACCESS(result, language_code, ""));
            if (language_code != "") {
              out->push_back(language_code);
              out->emplace_back(SAFE_ACCESS(result, country_code, ""));
              out->emplace_back(SAFE_ACCESS(result, script_code, ""));
            }
          }
          return out;
        };
  }

  flutter::PlatformViewEmbedder::OnPreEngineRestartCallback
      on_pre_engine_restart_callback = nullptr;
  if (SAFE_ACCESS(args, on_pre_engine_restart_callback, nullptr) != nullptr) {
    on_pre_engine_restart_callback = [ptr =
                                          args->on_pre_engine_restart_callback,
                                      user_data]() { return ptr(user_data); };
  }

  flutter::PlatformViewEmbedder::ChanneUpdateCallback channel_update_callback =
      nullptr;
  if (SAFE_ACCESS(args, channel_update_callback, nullptr) != nullptr) {
    channel_update_callback = [ptr = args->channel_update_callback, user_data](
                                  const std::string& name, bool listening) {
      FlutterChannelUpdate update{sizeof(FlutterChannelUpdate), name.c_str(),
                                  listening};
      ptr(&update, user_data);
    };
  }

  flutter::PlatformViewEmbedder::PlatformDispatchTable platform_dispatch_table =
      {
          platform_message_response_callback,         //
          compute_platform_resolved_locale_callback,  //
          on_pre_engine_restart_callback,             //
          channel_update_callback,                    //
      };

  auto on_create_platform_view =
      InferPlatformViewCreationCallback(user_data, platform_dispatch_table);

  if (!on_create_platform_view) {
    return LOG_EMBEDDER_ERROR(
        kInternalInconsistency,
        "Could not infer platform view creation callback.");
  }

  auto custom_task_runners = SAFE_ACCESS(args, custom_task_runners, nullptr);
  auto thread_config_callback = [&custom_task_runners](
                                    const fml::Thread::ThreadConfig& config) {
    fml::Thread::SetCurrentThreadName(config);
    if (!custom_task_runners || !custom_task_runners->thread_priority_setter) {
      return;
    }
    FlutterThreadPriority priority = FlutterThreadPriority::kNormal;
    switch (config.priority) {
      case fml::Thread::ThreadPriority::kBackground:
        priority = FlutterThreadPriority::kBackground;
        break;
      case fml::Thread::ThreadPriority::kNormal:
        priority = FlutterThreadPriority::kNormal;
        break;
      case fml::Thread::ThreadPriority::kDisplay:
        priority = FlutterThreadPriority::kDisplay;
        break;
    }
    custom_task_runners->thread_priority_setter(priority);
  };
  auto thread_host =
      flutter::EmbedderThreadHost::CreateEmbedderOrEngineManagedThreadHost(
          custom_task_runners, thread_config_callback);

  if (!thread_host || !thread_host->IsValid()) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments,
                              "Could not set up or infer thread configuration "
                              "to run the Flutter engine on.");
  }

  auto task_runners = thread_host->GetTaskRunners();

  if (!task_runners.IsValid()) {
    return LOG_EMBEDDER_ERROR(kInternalInconsistency,
                              "Task runner configuration was invalid.");
  }

  // Embedder supplied UI task runner runner does not have a message loop.
  bool has_ui_thread_message_loop =
      task_runners.GetUITaskRunner()->GetTaskQueueId().is_valid();
  // Message loop observers are used to flush the microtask queue.
  // If there is no message loop the queue is flushed from
  // EmbedderEngine::RunTask.
  settings.task_observer_add = [has_ui_thread_message_loop](
                                   intptr_t key, const fml::closure& callback) {
    if (has_ui_thread_message_loop) {
      fml::MessageLoop::GetCurrent().AddTaskObserver(key, callback);
    }
    return fml::TaskQueueId::Invalid();
  };
  settings.task_observer_remove = [has_ui_thread_message_loop](
                                      fml::TaskQueueId queue_id, intptr_t key) {
    if (has_ui_thread_message_loop) {
      fml::MessageLoop::GetCurrent().RemoveTaskObserver(key);
    }
  };

  auto run_configuration =
      flutter::RunConfiguration::InferFromSettings(settings);

  if (SAFE_ACCESS(args, custom_dart_entrypoint, nullptr) != nullptr) {
    auto dart_entrypoint = std::string{args->custom_dart_entrypoint};
    if (!dart_entrypoint.empty()) {
      run_configuration.SetEntrypoint(std::move(dart_entrypoint));
    }
  }

  if (SAFE_ACCESS(args, dart_entrypoint_argc, 0) > 0) {
    if (SAFE_ACCESS(args, dart_entrypoint_argv, nullptr) == nullptr) {
      return LOG_EMBEDDER_ERROR(kInvalidArguments,
                                "Could not determine Dart entrypoint arguments "
                                "as dart_entrypoint_argc "
                                "was set, but dart_entrypoint_argv was null.");
    }
    std::vector<std::string> arguments(args->dart_entrypoint_argc);
    for (int i = 0; i < args->dart_entrypoint_argc; ++i) {
      arguments[i] = std::string{args->dart_entrypoint_argv[i]};
    }
    run_configuration.SetEntrypointArgs(std::move(arguments));
  }

  if (SAFE_ACCESS(args, engine_id, 0) != 0) {
    run_configuration.SetEngineId(args->engine_id);
  }

  if (!run_configuration.IsValid()) {
    return LOG_EMBEDDER_ERROR(
        kInvalidArguments,
        "Could not infer the Flutter project to run from given arguments.");
  }

  // Create the engine but don't launch the shell or run the root isolate.
  auto embedder_engine = std::make_unique<flutter::EmbedderEngine>(
      std::move(thread_host),        //
      std::move(task_runners),       //
      std::move(settings),           //
      std::move(run_configuration),  //
      on_create_platform_view        //
  );

  // Release the ownership of the embedder engine to the caller.
  *engine_out = reinterpret_cast<FLUTTER_API_SYMBOL(FlutterEngine)>(
      embedder_engine.release());
  return kSuccess;
}

FlutterEngineResult FlutterEngineRunInitialized(
    FLUTTER_API_SYMBOL(FlutterEngine) engine) {
  if (!engine) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments, "Engine handle was invalid.");
  }

  auto embedder_engine = reinterpret_cast<flutter::EmbedderEngine*>(engine);

  // The engine must not already be running. Initialize may only be called
  // once on an engine instance.
  if (embedder_engine->IsValid()) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments, "Engine handle was invalid.");
  }

  // Step 1: Launch the shell.
  if (!embedder_engine->LaunchShell()) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments,
                              "Could not launch the engine using supplied "
                              "initialization arguments.");
  }

  // Step 2: Tell the platform view to initialize itself.
  if (!embedder_engine->NotifyCreated()) {
    return LOG_EMBEDDER_ERROR(kInternalInconsistency,
                              "Could not create platform view components.");
  }

  // Step 3: Launch the root isolate.
  if (!embedder_engine->RunRootIsolate()) {
    return LOG_EMBEDDER_ERROR(
        kInvalidArguments,
        "Could not run the root isolate of the Flutter application using the "
        "project arguments specified.");
  }

  return kSuccess;
}

FLUTTER_EXPORT
FlutterEngineResult FlutterEngineDeinitialize(FLUTTER_API_SYMBOL(FlutterEngine)
                                                  engine) {
  if (engine == nullptr) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments, "Engine handle was invalid.");
  }

  auto embedder_engine = reinterpret_cast<flutter::EmbedderEngine*>(engine);
  embedder_engine->NotifyDestroyed();
  embedder_engine->CollectShell();
  embedder_engine->CollectThreadHost();
  return kSuccess;
}

FlutterEngineResult FlutterEngineShutdown(FLUTTER_API_SYMBOL(FlutterEngine)
                                              engine) {
  auto result = FlutterEngineDeinitialize(engine);
  if (result != kSuccess) {
    return result;
  }
  auto embedder_engine = reinterpret_cast<flutter::EmbedderEngine*>(engine);
  delete embedder_engine;
  return kSuccess;
}

FlutterEngineResult FlutterEngineSendPlatformMessage(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    const FlutterPlatformMessage* flutter_message) {
  if (engine == nullptr) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments, "Invalid engine handle.");
  }

  if (flutter_message == nullptr) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments, "Invalid message argument.");
  }

  if (SAFE_ACCESS(flutter_message, channel, nullptr) == nullptr) {
    return LOG_EMBEDDER_ERROR(
        kInvalidArguments, "Message argument did not specify a valid channel.");
  }

  size_t message_size = SAFE_ACCESS(flutter_message, message_size, 0);
  const uint8_t* message_data = SAFE_ACCESS(flutter_message, message, nullptr);

  if (message_size != 0 && message_data == nullptr) {
    return LOG_EMBEDDER_ERROR(
        kInvalidArguments,
        "Message size was non-zero but the message data was nullptr.");
  }

  const FlutterPlatformMessageResponseHandle* response_handle =
      SAFE_ACCESS(flutter_message, response_handle, nullptr);

  fml::RefPtr<flutter::PlatformMessageResponse> response;
  if (response_handle && response_handle->message) {
    response = response_handle->message->response();
  }

  std::unique_ptr<flutter::PlatformMessage> message;
  if (message_size == 0) {
    message = std::make_unique<flutter::PlatformMessage>(
        flutter_message->channel, response);
  } else {
    message = std::make_unique<flutter::PlatformMessage>(
        flutter_message->channel,
        fml::MallocMapping::Copy(message_data, message_size), response);
  }

  return reinterpret_cast<flutter::EmbedderEngine*>(engine)
                 ->SendPlatformMessage(std::move(message))
             ? kSuccess
             : LOG_EMBEDDER_ERROR(kInternalInconsistency,
                                  "Could not send a message to the running "
                                  "Flutter application.");
}

FlutterEngineResult FlutterPlatformMessageCreateResponseHandle(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    FlutterDataCallback data_callback,
    void* user_data,
    FlutterPlatformMessageResponseHandle** response_out) {
  if (engine == nullptr) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments, "Engine handle was invalid.");
  }

  if (data_callback == nullptr || response_out == nullptr) {
    return LOG_EMBEDDER_ERROR(
        kInvalidArguments, "Data callback or the response handle was invalid.");
  }

  flutter::EmbedderPlatformMessageResponse::Callback response_callback =
      [user_data, data_callback](const uint8_t* data, size_t size) {
        data_callback(data, size, user_data);
      };

  auto platform_task_runner = reinterpret_cast<flutter::EmbedderEngine*>(engine)
                                  ->GetTaskRunners()
                                  .GetPlatformTaskRunner();

  auto handle = new FlutterPlatformMessageResponseHandle();

  handle->message = std::make_unique<flutter::PlatformMessage>(
      "",  // The channel is empty and unused as the response handle is going
           // to referenced directly in the |FlutterEngineSendPlatformMessage|
           // with the container message discarded.
      fml::MakeRefCounted<flutter::EmbedderPlatformMessageResponse>(
          std::move(platform_task_runner), response_callback));
  *response_out = handle;
  return kSuccess;
}

FlutterEngineResult FlutterPlatformMessageReleaseResponseHandle(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    FlutterPlatformMessageResponseHandle* response) {
  if (engine == nullptr) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments, "Invalid engine handle.");
  }

  if (response == nullptr) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments, "Invalid response handle.");
  }
  delete response;
  return kSuccess;
}

// Note: This can execute on any thread.
FlutterEngineResult FlutterEngineSendPlatformMessageResponse(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    const FlutterPlatformMessageResponseHandle* handle,
    const uint8_t* data,
    size_t data_length) {
  if (data_length != 0 && data == nullptr) {
    return LOG_EMBEDDER_ERROR(
        kInvalidArguments,
        "Data size was non zero but the pointer to the data was null.");
  }

  auto response = handle->message->response();

  if (response) {
    if (data_length == 0) {
      response->CompleteEmpty();
    } else {
      response->Complete(std::make_unique<fml::DataMapping>(
          std::vector<uint8_t>({data, data + data_length})));
    }
  }

  delete handle;

  return kSuccess;
}

FlutterEngineResult __FlutterEngineFlushPendingTasksNow() {
  fml::MessageLoop::GetCurrent().RunExpiredTasksNow();
  return kSuccess;
}

void FlutterEngineTraceEventDurationBegin(const char* name) {
  fml::tracing::TraceEvent0("flutter", name, /*flow_id_count=*/0,
                            /*flow_ids=*/nullptr);
}

void FlutterEngineTraceEventDurationEnd(const char* name) {
  fml::tracing::TraceEventEnd(name);
}

void FlutterEngineTraceEventInstant(const char* name) {
  fml::tracing::TraceEventInstant0("flutter", name, /*flow_id_count=*/0,
                                   /*flow_ids=*/nullptr);
}

uint64_t FlutterEngineGetCurrentTime() {
  return fml::TimePoint::Now().ToEpochDelta().ToNanoseconds();
}

FlutterEngineResult FlutterEngineRunTask(FLUTTER_API_SYMBOL(FlutterEngine)
                                             engine,
                                         const FlutterTask* task) {
  if (engine == nullptr) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments, "Invalid engine handle.");
  }

  if (!flutter::EmbedderThreadHost::RunnerIsValid(
          reinterpret_cast<intptr_t>(task->runner))) {
    // This task came too late, the embedder has already been destroyed.
    // This is not an error, just ignore the task.
    return kSuccess;
  }

  return reinterpret_cast<flutter::EmbedderEngine*>(engine)->RunTask(task)
             ? kSuccess
             : LOG_EMBEDDER_ERROR(kInvalidArguments,
                                  "Could not run the specified task.");
}

static bool DispatchJSONPlatformMessage(FLUTTER_API_SYMBOL(FlutterEngine)
                                            engine,
                                        const rapidjson::Document& document,
                                        const std::string& channel_name) {
  if (channel_name.empty()) {
    return false;
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

  if (!document.Accept(writer)) {
    return false;
  }

  const char* message = buffer.GetString();

  if (message == nullptr || buffer.GetSize() == 0) {
    return false;
  }

  auto platform_message = std::make_unique<flutter::PlatformMessage>(
      channel_name.c_str(),  // channel
      fml::MallocMapping::Copy(message,
                               buffer.GetSize()),  // message
      nullptr                                      // response
  );

  return reinterpret_cast<flutter::EmbedderEngine*>(engine)
      ->SendPlatformMessage(std::move(platform_message));
}

FlutterEngineResult FlutterEngineUpdateLocales(FLUTTER_API_SYMBOL(FlutterEngine)
                                                   engine,
                                               const FlutterLocale** locales,
                                               size_t locales_count) {
  if (engine == nullptr) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments, "Invalid engine handle.");
  }

  if (locales_count == 0) {
    return kSuccess;
  }

  if (locales == nullptr) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments, "No locales were specified.");
  }

  rapidjson::Document document;
  auto& allocator = document.GetAllocator();

  document.SetObject();
  document.AddMember("method", "setLocale", allocator);

  rapidjson::Value args(rapidjson::kArrayType);
  args.Reserve(locales_count * 4, allocator);
  for (size_t i = 0; i < locales_count; ++i) {
    const FlutterLocale* locale = locales[i];
    const char* language_code_str = SAFE_ACCESS(locale, language_code, nullptr);
    if (language_code_str == nullptr || ::strlen(language_code_str) == 0) {
      return LOG_EMBEDDER_ERROR(
          kInvalidArguments,
          "Language code is required but not present in FlutterLocale.");
    }

    const char* country_code_str = SAFE_ACCESS(locale, country_code, "");
    const char* script_code_str = SAFE_ACCESS(locale, script_code, "");
    const char* variant_code_str = SAFE_ACCESS(locale, variant_code, "");

    rapidjson::Value language_code, country_code, script_code, variant_code;

    language_code.SetString(language_code_str, allocator);
    country_code.SetString(country_code_str ? country_code_str : "", allocator);
    script_code.SetString(script_code_str ? script_code_str : "", allocator);
    variant_code.SetString(variant_code_str ? variant_code_str : "", allocator);

    // Required.
    args.PushBack(language_code, allocator);
    args.PushBack(country_code, allocator);
    args.PushBack(script_code, allocator);
    args.PushBack(variant_code, allocator);
  }
  document.AddMember("args", args, allocator);

  return DispatchJSONPlatformMessage(engine, document, "flutter/localization")
             ? kSuccess
             : LOG_EMBEDDER_ERROR(kInternalInconsistency,
                                  "Could not send message to update locale of "
                                  "a running Flutter application.");
}

bool FlutterEngineRunsAOTCompiledDartCode(void) {
  return flutter::DartVM::IsRunningPrecompiledCode();
}

FlutterEngineResult FlutterEnginePostDartObject(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    FlutterEngineDartPort port,
    const FlutterEngineDartObject* object) {
  if (engine == nullptr) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments, "Invalid engine handle.");
  }

  if (!reinterpret_cast<flutter::EmbedderEngine*>(engine)->IsValid()) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments, "Engine not running.");
  }

  if (port == ILLEGAL_PORT) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments,
                              "Attempted to post to an illegal port.");
  }

  if (object == nullptr) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments,
                              "Invalid Dart object to post.");
  }

  Dart_CObject dart_object = {};
  fml::ScopedCleanupClosure typed_data_finalizer;

  switch (object->type) {
    case kFlutterEngineDartObjectTypeNull:
      dart_object.type = Dart_CObject_kNull;
      break;
    case kFlutterEngineDartObjectTypeBool:
      dart_object.type = Dart_CObject_kBool;
      dart_object.value.as_bool = object->bool_value;
      break;
    case kFlutterEngineDartObjectTypeInt32:
      dart_object.type = Dart_CObject_kInt32;
      dart_object.value.as_int32 = object->int32_value;
      break;
    case kFlutterEngineDartObjectTypeInt64:
      dart_object.type = Dart_CObject_kInt64;
      dart_object.value.as_int64 = object->int64_value;
      break;
    case kFlutterEngineDartObjectTypeDouble:
      dart_object.type = Dart_CObject_kDouble;
      dart_object.value.as_double = object->double_value;
      break;
    case kFlutterEngineDartObjectTypeString:
      if (object->string_value == nullptr) {
        return LOG_EMBEDDER_ERROR(kInvalidArguments,
                                  "kFlutterEngineDartObjectTypeString must be "
                                  "a null terminated string but was null.");
      }
      dart_object.type = Dart_CObject_kString;
      dart_object.value.as_string = const_cast<char*>(object->string_value);
      break;
    case kFlutterEngineDartObjectTypeBuffer: {
      auto* buffer = SAFE_ACCESS(object->buffer_value, buffer, nullptr);
      if (buffer == nullptr) {
        return LOG_EMBEDDER_ERROR(kInvalidArguments,
                                  "kFlutterEngineDartObjectTypeBuffer must "
                                  "specify a buffer but found nullptr.");
      }
      auto buffer_size = SAFE_ACCESS(object->buffer_value, buffer_size, 0);
      auto callback =
          SAFE_ACCESS(object->buffer_value, buffer_collect_callback, nullptr);
      auto user_data = SAFE_ACCESS(object->buffer_value, user_data, nullptr);

      // The user has provided a callback, let them manage the lifecycle of
      // the underlying data. If not, copy it out from the provided buffer.

      if (callback == nullptr) {
        dart_object.type = Dart_CObject_kTypedData;
        dart_object.value.as_typed_data.type = Dart_TypedData_kUint8;
        dart_object.value.as_typed_data.length = buffer_size;
        dart_object.value.as_typed_data.values = buffer;
      } else {
        struct ExternalTypedDataPeer {
          void* user_data = nullptr;
          VoidCallback trampoline = nullptr;
        };
        auto peer = new ExternalTypedDataPeer();
        peer->user_data = user_data;
        peer->trampoline = callback;
        // This finalizer is set so that in case of failure of the
        // Dart_PostCObject below, we collect the peer. The embedder is still
        // responsible for collecting the buffer in case of non-kSuccess
        // returns from this method. This finalizer must be released in case
        // of kSuccess returns from this method.
        typed_data_finalizer.SetClosure([peer]() {
          // This is the tiny object we use as the peer to the Dart call so
          // that we can attach the a trampoline to the embedder supplied
          // callback. In case of error, we need to collect this object lest
          // we introduce a tiny leak.
          delete peer;
        });
        dart_object.type = Dart_CObject_kExternalTypedData;
        dart_object.value.as_external_typed_data.type = Dart_TypedData_kUint8;
        dart_object.value.as_external_typed_data.length = buffer_size;
        dart_object.value.as_external_typed_data.data = buffer;
        dart_object.value.as_external_typed_data.peer = peer;
        dart_object.value.as_external_typed_data.callback =
            +[](void* unused_isolate_callback_data, void* peer) {
              auto typed_peer = reinterpret_cast<ExternalTypedDataPeer*>(peer);
              typed_peer->trampoline(typed_peer->user_data);
              delete typed_peer;
            };
      }
    } break;
    default:
      return LOG_EMBEDDER_ERROR(
          kInvalidArguments,
          "Invalid FlutterEngineDartObjectType type specified.");
  }

  if (!Dart_PostCObject(port, &dart_object)) {
    return LOG_EMBEDDER_ERROR(kInternalInconsistency,
                              "Could not post the object to the Dart VM.");
  }

  // On a successful call, the VM takes ownership of and is responsible for
  // invoking the finalizer.
  typed_data_finalizer.Release();
  return kSuccess;
}

FlutterEngineResult FlutterEngineNotifyLowMemoryWarning(
    FLUTTER_API_SYMBOL(FlutterEngine) raw_engine) {
  auto engine = reinterpret_cast<flutter::EmbedderEngine*>(raw_engine);
  if (engine == nullptr || !engine->IsValid()) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments, "Engine was invalid.");
  }

  engine->GetShell().NotifyLowMemoryWarning();

  rapidjson::Document document;
  auto& allocator = document.GetAllocator();

  document.SetObject();
  document.AddMember("type", "memoryPressure", allocator);

  return DispatchJSONPlatformMessage(raw_engine, document, "flutter/system")
             ? kSuccess
             : LOG_EMBEDDER_ERROR(
                   kInternalInconsistency,
                   "Could not dispatch the low memory notification message.");
}

FlutterEngineResult FlutterEnginePostCallbackOnAllNativeThreads(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    FlutterNativeThreadCallback callback,
    void* user_data) {
  if (engine == nullptr) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments, "Invalid engine handle.");
  }

  if (callback == nullptr) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments,
                              "Invalid native thread callback.");
  }

  return reinterpret_cast<flutter::EmbedderEngine*>(engine)
                 ->PostTaskOnEngineManagedNativeThreads(
                     [callback, user_data](FlutterNativeThreadType type) {
                       callback(type, user_data);
                     })
             ? kSuccess
             : LOG_EMBEDDER_ERROR(kInvalidArguments,
                                  "Internal error while attempting to post "
                                  "tasks to all threads.");
}

FlutterEngineResult FlutterEngineGetProcAddresses(
    FlutterEngineProcTable* table) {
  if (!table) {
    return LOG_EMBEDDER_ERROR(kInvalidArguments, "Null table specified.");
  }
#define SET_PROC(member, function)        \
  if (STRUCT_HAS_MEMBER(table, member)) { \
    table->member = &function;            \
  }

  SET_PROC(CreateAOTData, FlutterEngineCreateAOTData);
  SET_PROC(CollectAOTData, FlutterEngineCollectAOTData);
  SET_PROC(Run, FlutterEngineRun);
  SET_PROC(Shutdown, FlutterEngineShutdown);
  SET_PROC(Initialize, FlutterEngineInitialize);
  SET_PROC(Deinitialize, FlutterEngineDeinitialize);
  SET_PROC(RunInitialized, FlutterEngineRunInitialized);
  SET_PROC(SendPlatformMessage, FlutterEngineSendPlatformMessage);
  SET_PROC(PlatformMessageCreateResponseHandle,
           FlutterPlatformMessageCreateResponseHandle);
  SET_PROC(PlatformMessageReleaseResponseHandle,
           FlutterPlatformMessageReleaseResponseHandle);
  SET_PROC(SendPlatformMessageResponse,
           FlutterEngineSendPlatformMessageResponse);
  SET_PROC(TraceEventDurationBegin, FlutterEngineTraceEventDurationBegin);
  SET_PROC(TraceEventDurationEnd, FlutterEngineTraceEventDurationEnd);
  SET_PROC(TraceEventInstant, FlutterEngineTraceEventInstant);
  SET_PROC(GetCurrentTime, FlutterEngineGetCurrentTime);
  SET_PROC(RunTask, FlutterEngineRunTask);
  SET_PROC(UpdateLocales, FlutterEngineUpdateLocales);
  SET_PROC(RunsAOTCompiledDartCode, FlutterEngineRunsAOTCompiledDartCode);
  SET_PROC(PostDartObject, FlutterEnginePostDartObject);
  SET_PROC(NotifyLowMemoryWarning, FlutterEngineNotifyLowMemoryWarning);
  SET_PROC(PostCallbackOnAllNativeThreads,
           FlutterEnginePostCallbackOnAllNativeThreads);
#undef SET_PROC

  return kSuccess;
}
