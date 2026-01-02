// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/public/flutter_linux/fl_engine.h"

#include <epoxy/egl.h>
#include <gmodule.h>

#include <cstring>

#include "flutter/shell/platform/common/engine_switches.h"
#include "flutter/shell/platform/embedder/embedder.h"
#include "flutter/shell/platform/linux/fl_binary_messenger_private.h"
#include "flutter/shell/platform/linux/fl_engine_private.h"
#include "flutter/shell/platform/linux/fl_plugin_registrar_private.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_plugin_registry.h"

// Unique number associated with platform tasks.
static constexpr size_t kPlatformTaskRunnerIdentifier = 1;

struct _FlEngine {
  GObject parent_instance;

  // Thread the GLib main loop is running on.
  GThread* thread;

  // The project this engine is running.
  FlDartProject* project;

  // Messenger used to send and receive platform messages.
  FlBinaryMessenger* binary_messenger;

  // Schedules tasks to be run on the appropriate thread.
  FlTaskRunner* task_runner;

  // Ahead of time data used to make engine run faster.
  FlutterEngineAOTData aot_data;

  // The Flutter engine.
  FLUTTER_API_SYMBOL(FlutterEngine) engine;

  // Function table for engine API, used to intercept engine calls for testing
  // purposes.
  FlutterEngineProcTable embedder_api;

  // Function to call when a platform message is received.
  FlEnginePlatformMessageHandler platform_message_handler;
  gpointer platform_message_handler_data;
  GDestroyNotify platform_message_handler_destroy_notify;
};

G_DEFINE_QUARK(fl_engine_error_quark, fl_engine_error)

static void fl_engine_plugin_registry_iface_init(
    FlPluginRegistryInterface* iface);

enum { SIGNAL_ON_PRE_ENGINE_RESTART, SIGNAL_UPDATE_SEMANTICS, LAST_SIGNAL };

static guint fl_engine_signals[LAST_SIGNAL];

G_DEFINE_TYPE_WITH_CODE(
    FlEngine,
    fl_engine,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(fl_plugin_registry_get_type(),
                          fl_engine_plugin_registry_iface_init))

enum { PROP_0, PROP_BINARY_MESSENGER, PROP_LAST };

// Parse a locale into its components.
static void parse_locale(const gchar* locale,
                         gchar** language,
                         gchar** territory,
                         gchar** codeset,
                         gchar** modifier) {
  gchar* l = g_strdup(locale);

  // Locales are in the form "language[_territory][.codeset][@modifier]"
  gchar* match = strrchr(l, '@');
  if (match != nullptr) {
    if (modifier != nullptr) {
      *modifier = g_strdup(match + 1);
    }
    *match = '\0';
  } else if (modifier != nullptr) {
    *modifier = nullptr;
  }

  match = strrchr(l, '.');
  if (match != nullptr) {
    if (codeset != nullptr) {
      *codeset = g_strdup(match + 1);
    }
    *match = '\0';
  } else if (codeset != nullptr) {
    *codeset = nullptr;
  }

  match = strrchr(l, '_');
  if (match != nullptr) {
    if (territory != nullptr) {
      *territory = g_strdup(match + 1);
    }
    *match = '\0';
  } else if (territory != nullptr) {
    *territory = nullptr;
  }

  if (language != nullptr) {
    *language = l;
  }
}

static void free_locale(FlutterLocale* locale) {
  free(const_cast<gchar*>(locale->language_code));
  free(const_cast<gchar*>(locale->country_code));
  free(locale);
}

// Passes locale information to the Flutter engine.
static void setup_locales(FlEngine* self) {
  const gchar* const* languages = g_get_language_names();
  g_autoptr(GPtrArray) locales_array = g_ptr_array_new_with_free_func(
      reinterpret_cast<GDestroyNotify>(free_locale));
  for (int i = 0; languages[i] != nullptr; i++) {
    g_autofree gchar* locale_string = g_strstrip(g_strdup(languages[i]));

    // Ignore empty locales, caused by settings like `LANGUAGE=pt_BR:`
    if (strcmp(locale_string, "") == 0) {
      continue;
    }

    g_autofree gchar* language = nullptr;
    g_autofree gchar* territory = nullptr;
    parse_locale(locale_string, &language, &territory, nullptr, nullptr);

    // Ignore duplicate locales, caused by settings like `LANGUAGE=C` (returns
    // two "C") or `LANGUAGE=en:en`
    gboolean has_locale = FALSE;
    for (guint j = 0; !has_locale && j < locales_array->len; j++) {
      FlutterLocale* locale =
          reinterpret_cast<FlutterLocale*>(g_ptr_array_index(locales_array, j));
      has_locale = g_strcmp0(locale->language_code, language) == 0 &&
                   g_strcmp0(locale->country_code, territory) == 0;
    }
    if (has_locale) {
      continue;
    }

    FlutterLocale* locale =
        static_cast<FlutterLocale*>(g_malloc0(sizeof(FlutterLocale)));
    g_ptr_array_add(locales_array, locale);
    locale->struct_size = sizeof(FlutterLocale);
    locale->language_code =
        reinterpret_cast<const gchar*>(g_steal_pointer(&language));
    locale->country_code =
        reinterpret_cast<const gchar*>(g_steal_pointer(&territory));
    locale->script_code = nullptr;
    locale->variant_code = nullptr;
  }
  FlutterLocale** locales =
      reinterpret_cast<FlutterLocale**>(locales_array->pdata);
  FlutterEngineResult result = self->embedder_api.UpdateLocales(
      self->engine, const_cast<const FlutterLocale**>(locales),
      locales_array->len);
  if (result != kSuccess) {
    g_warning("Failed to set up Flutter locales");
  }
}

// Called by the engine to determine if it is on the GTK thread.
static bool fl_engine_runs_task_on_current_thread(void* user_data) {
  FlEngine* self = static_cast<FlEngine*>(user_data);
  return self->thread == g_thread_self();
}

// Called when the engine has a task to perform in the GTK thread.
static void fl_engine_post_task(FlutterTask task,
                                uint64_t target_time_nanos,
                                void* user_data) {
  FlEngine* self = static_cast<FlEngine*>(user_data);

  fl_task_runner_post_flutter_task(self->task_runner, task, target_time_nanos);
}

// Called when a platform message is received from the engine.
static void fl_engine_platform_message_cb(const FlutterPlatformMessage* message,
                                          void* user_data) {
  FlEngine* self = FL_ENGINE(user_data);

  gboolean handled = FALSE;
  if (self->platform_message_handler != nullptr) {
    g_autoptr(GBytes) data =
        g_bytes_new(message->message, message->message_size);
    handled = self->platform_message_handler(
        self, message->channel, data, message->response_handle,
        self->platform_message_handler_data);
  }

  if (!handled) {
    fl_engine_send_platform_message_response(self, message->response_handle,
                                             nullptr, nullptr);
  }
}

// Called right before the engine is restarted.
//
// This method should reset states to as if the engine has just been started,
// which usually indicates the user has requested a hot restart (Shift-R in the
// Flutter CLI.)
static void fl_engine_on_pre_engine_restart_cb(void* user_data) {
  FlEngine* self = FL_ENGINE(user_data);

  g_signal_emit(self, fl_engine_signals[SIGNAL_ON_PRE_ENGINE_RESTART], 0);
}

// Called when a response to a sent platform message is received from the
// engine.
static void fl_engine_platform_message_response_cb(const uint8_t* data,
                                                   size_t data_length,
                                                   void* user_data) {
  g_autoptr(GTask) task = G_TASK(user_data);
  g_task_return_pointer(task, g_bytes_new(data, data_length),
                        reinterpret_cast<GDestroyNotify>(g_bytes_unref));
}

// Implements FlPluginRegistry::get_registrar_for_plugin.
static FlPluginRegistrar* fl_engine_get_registrar_for_plugin(
    FlPluginRegistry* registry,
    const gchar* name) {
  FlEngine* self = FL_ENGINE(registry);

  return fl_plugin_registrar_new(self->binary_messenger);
}

static void fl_engine_plugin_registry_iface_init(
    FlPluginRegistryInterface* iface) {
  iface->get_registrar_for_plugin = fl_engine_get_registrar_for_plugin;
}

static void fl_engine_set_property(GObject* object,
                                   guint prop_id,
                                   const GValue* value,
                                   GParamSpec* pspec) {
  FlEngine* self = FL_ENGINE(object);
  switch (prop_id) {
    case PROP_BINARY_MESSENGER:
      g_set_object(&self->binary_messenger,
                   FL_BINARY_MESSENGER(g_value_get_object(value)));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void fl_engine_dispose(GObject* object) {
  FlEngine* self = FL_ENGINE(object);

  if (self->engine != nullptr) {
    if (self->embedder_api.Shutdown(self->engine) != kSuccess) {
      g_warning("Failed to shutdown Flutter engine");
    }
    self->engine = nullptr;
  }

  if (self->aot_data != nullptr) {
    if (self->embedder_api.CollectAOTData(self->aot_data) != kSuccess) {
      g_warning("Failed to send collect AOT data");
    }
    self->aot_data = nullptr;
  }

  fl_binary_messenger_shutdown(self->binary_messenger);

  g_clear_object(&self->project);
  g_clear_object(&self->binary_messenger);
  g_clear_object(&self->task_runner);

  if (self->platform_message_handler_destroy_notify) {
    self->platform_message_handler_destroy_notify(
        self->platform_message_handler_data);
  }
  self->platform_message_handler_data = nullptr;
  self->platform_message_handler_destroy_notify = nullptr;

  G_OBJECT_CLASS(fl_engine_parent_class)->dispose(object);
}

static void fl_engine_class_init(FlEngineClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_engine_dispose;
  G_OBJECT_CLASS(klass)->set_property = fl_engine_set_property;

  g_object_class_install_property(
      G_OBJECT_CLASS(klass), PROP_BINARY_MESSENGER,
      g_param_spec_object(
          "binary-messenger", "messenger", "Binary messenger",
          fl_binary_messenger_get_type(),
          static_cast<GParamFlags>(G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY |
                                   G_PARAM_STATIC_STRINGS)));

  fl_engine_signals[SIGNAL_ON_PRE_ENGINE_RESTART] = g_signal_new(
      "on-pre-engine-restart", fl_engine_get_type(), G_SIGNAL_RUN_LAST, 0,
      nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
  fl_engine_signals[SIGNAL_UPDATE_SEMANTICS] = g_signal_new(
      "update-semantics", fl_engine_get_type(), G_SIGNAL_RUN_LAST, 0, nullptr,
      nullptr, nullptr, G_TYPE_NONE, 1, G_TYPE_POINTER);
}

static void fl_engine_init(FlEngine* self) {
  self->thread = g_thread_self();

  self->embedder_api.struct_size = sizeof(FlutterEngineProcTable);
  if (FlutterEngineGetProcAddresses(&self->embedder_api) != kSuccess) {
    g_warning("Failed get get engine function pointers");
  }

  self->task_runner = fl_task_runner_new(self);
}

static FlEngine* fl_engine_new_full(FlDartProject* project,
                                    FlBinaryMessenger* binary_messenger) {
  g_return_val_if_fail(FL_IS_DART_PROJECT(project), nullptr);

  FlEngine* self = FL_ENGINE(g_object_new(fl_engine_get_type(), nullptr));

  self->project = FL_DART_PROJECT(g_object_ref(project));

  if (binary_messenger != nullptr) {
    self->binary_messenger =
        FL_BINARY_MESSENGER(g_object_ref(binary_messenger));
  } else {
    self->binary_messenger = fl_binary_messenger_new(self);
  }

  return self;
}

FlEngine* fl_engine_for_id(int64_t id) {
  void* engine = reinterpret_cast<void*>(id);
  g_return_val_if_fail(FL_IS_ENGINE(engine), nullptr);
  return FL_ENGINE(engine);
}

G_MODULE_EXPORT FlEngine* fl_engine_new(FlDartProject* project) {
  return fl_engine_new_full(project, nullptr);
}

FlEngine* fl_engine_new_with_binary_messenger(
    FlBinaryMessenger* binary_messenger) {
  g_autoptr(FlDartProject) project = fl_dart_project_new();
  return fl_engine_new_full(project, binary_messenger);
}

G_MODULE_EXPORT FlEngine* fl_engine_new_headless(FlDartProject* project) {
  return fl_engine_new(project);
}

G_MODULE_EXPORT gboolean fl_engine_start(FlEngine* self, GError** error) {
  g_return_val_if_fail(FL_IS_ENGINE(self), FALSE);

  FlutterTaskRunnerDescription platform_task_runner = {};
  platform_task_runner.struct_size = sizeof(FlutterTaskRunnerDescription);
  platform_task_runner.user_data = self;
  platform_task_runner.runs_task_on_current_thread_callback =
      fl_engine_runs_task_on_current_thread;
  platform_task_runner.post_task_callback = fl_engine_post_task;
  platform_task_runner.identifier = kPlatformTaskRunnerIdentifier;

  FlutterCustomTaskRunners custom_task_runners = {};
  custom_task_runners.struct_size = sizeof(FlutterCustomTaskRunners);
  custom_task_runners.platform_task_runner = &platform_task_runner;

  switch (fl_dart_project_get_ui_thread_policy(self->project)) {
    case FL_UI_THREAD_POLICY_RUN_ON_SEPARATE_THREAD:
      break;
    case FL_UI_THREAD_POLICY_DEFAULT:
    case FL_UI_THREAD_POLICY_RUN_ON_PLATFORM_THREAD:
      custom_task_runners.ui_task_runner = &platform_task_runner;
      break;
  }

  g_autoptr(GPtrArray) command_line_args =
      g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_insert(command_line_args, 0, g_strdup("flutter"));
  for (const auto& env_switch : flutter::GetSwitchesFromEnvironment()) {
    g_ptr_array_add(command_line_args, g_strdup(env_switch.c_str()));
  }

  gchar** dart_entrypoint_args =
      fl_dart_project_get_dart_entrypoint_arguments(self->project);

  FlutterProjectArgs args = {};
  args.struct_size = sizeof(FlutterProjectArgs);
  args.assets_path = fl_dart_project_get_assets_path(self->project);
  args.icu_data_path = fl_dart_project_get_icu_data_path(self->project);
  args.command_line_argc = command_line_args->len;
  args.command_line_argv =
      reinterpret_cast<const char* const*>(command_line_args->pdata);
  args.platform_message_callback = fl_engine_platform_message_cb;
  args.custom_task_runners = &custom_task_runners;
  args.shutdown_dart_vm_when_done = true;
  args.on_pre_engine_restart_callback = fl_engine_on_pre_engine_restart_cb;
  args.dart_entrypoint_argc =
      dart_entrypoint_args != nullptr ? g_strv_length(dart_entrypoint_args) : 0;
  args.dart_entrypoint_argv =
      reinterpret_cast<const char* const*>(dart_entrypoint_args);
  args.engine_id = reinterpret_cast<int64_t>(self);

  if (self->embedder_api.RunsAOTCompiledDartCode()) {
    FlutterEngineAOTDataSource source = {};
    source.type = kFlutterEngineAOTDataSourceTypeElfPath;
    source.elf_path = fl_dart_project_get_aot_library_path(self->project);
    if (self->embedder_api.CreateAOTData(&source, &self->aot_data) !=
        kSuccess) {
      g_set_error(error, fl_engine_error_quark(), FL_ENGINE_ERROR_FAILED,
                  "Failed to create AOT data");
      return FALSE;
    }
    args.aot_data = self->aot_data;
  }

  FlutterEngineResult result = self->embedder_api.Initialize(
      FLUTTER_ENGINE_VERSION, &args, self, &self->engine);
  if (result != kSuccess) {
    g_set_error(error, fl_engine_error_quark(), FL_ENGINE_ERROR_FAILED,
                "Failed to initialize Flutter engine");
    return FALSE;
  }

  result = self->embedder_api.RunInitialized(self->engine);
  if (result != kSuccess) {
    g_set_error(error, fl_engine_error_quark(), FL_ENGINE_ERROR_FAILED,
                "Failed to run Flutter engine");
    return FALSE;
  }

  setup_locales(self);

  return TRUE;
}

FlutterEngineProcTable* fl_engine_get_embedder_api(FlEngine* self) {
  return &(self->embedder_api);
}

void fl_engine_set_platform_message_handler(
    FlEngine* self,
    FlEnginePlatformMessageHandler handler,
    gpointer user_data,
    GDestroyNotify destroy_notify) {
  g_return_if_fail(FL_IS_ENGINE(self));
  g_return_if_fail(handler != nullptr);

  if (self->platform_message_handler_destroy_notify) {
    self->platform_message_handler_destroy_notify(
        self->platform_message_handler_data);
  }

  self->platform_message_handler = handler;
  self->platform_message_handler_data = user_data;
  self->platform_message_handler_destroy_notify = destroy_notify;
}

// Note: This function can be called from any thread.
gboolean fl_engine_send_platform_message_response(
    FlEngine* self,
    const FlutterPlatformMessageResponseHandle* handle,
    GBytes* response,
    GError** error) {
  g_return_val_if_fail(FL_IS_ENGINE(self), FALSE);
  g_return_val_if_fail(handle != nullptr, FALSE);

  if (self->engine == nullptr) {
    g_set_error(error, fl_engine_error_quark(), FL_ENGINE_ERROR_FAILED,
                "No engine to send response to");
    return FALSE;
  }

  gsize data_length = 0;
  const uint8_t* data = nullptr;
  if (response != nullptr) {
    data =
        static_cast<const uint8_t*>(g_bytes_get_data(response, &data_length));
  }
  FlutterEngineResult result = self->embedder_api.SendPlatformMessageResponse(
      self->engine, handle, data, data_length);

  if (result != kSuccess) {
    g_set_error(error, fl_engine_error_quark(), FL_ENGINE_ERROR_FAILED,
                "Failed to send platform message response");
    return FALSE;
  }

  return TRUE;
}

void fl_engine_send_platform_message(FlEngine* self,
                                     const gchar* channel,
                                     GBytes* message,
                                     GCancellable* cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data) {
  g_return_if_fail(FL_IS_ENGINE(self));

  GTask* task = nullptr;
  FlutterPlatformMessageResponseHandle* response_handle = nullptr;
  if (callback != nullptr) {
    task = g_task_new(self, cancellable, callback, user_data);

    if (self->engine == nullptr) {
      g_task_return_new_error(task, fl_engine_error_quark(),
                              FL_ENGINE_ERROR_FAILED, "No engine to send to");
      return;
    }

    FlutterEngineResult result =
        self->embedder_api.PlatformMessageCreateResponseHandle(
            self->engine, fl_engine_platform_message_response_cb, task,
            &response_handle);
    if (result != kSuccess) {
      g_task_return_new_error(task, fl_engine_error_quark(),
                              FL_ENGINE_ERROR_FAILED,
                              "Failed to create response handle");
      g_object_unref(task);
      return;
    }
  } else if (self->engine == nullptr) {
    return;
  }

  FlutterPlatformMessage fl_message = {};
  fl_message.struct_size = sizeof(fl_message);
  fl_message.channel = channel;
  fl_message.message =
      message != nullptr
          ? static_cast<const uint8_t*>(g_bytes_get_data(message, nullptr))
          : nullptr;
  fl_message.message_size = message != nullptr ? g_bytes_get_size(message) : 0;
  fl_message.response_handle = response_handle;
  FlutterEngineResult result =
      self->embedder_api.SendPlatformMessage(self->engine, &fl_message);

  if (result != kSuccess && task != nullptr) {
    g_task_return_new_error(task, fl_engine_error_quark(),
                            FL_ENGINE_ERROR_FAILED,
                            "Failed to send platform messages");
    g_object_unref(task);
  }

  if (response_handle != nullptr) {
    if (self->embedder_api.PlatformMessageReleaseResponseHandle(
            self->engine, response_handle) != kSuccess) {
      g_warning("Failed to release response handle");
    }
  }
}

GBytes* fl_engine_send_platform_message_finish(FlEngine* self,
                                               GAsyncResult* result,
                                               GError** error) {
  g_return_val_if_fail(FL_IS_ENGINE(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

  return static_cast<GBytes*>(g_task_propagate_pointer(G_TASK(result), error));
}

G_MODULE_EXPORT FlBinaryMessenger* fl_engine_get_binary_messenger(
    FlEngine* self) {
  g_return_val_if_fail(FL_IS_ENGINE(self), nullptr);
  return self->binary_messenger;
}

FlTaskRunner* fl_engine_get_task_runner(FlEngine* self) {
  g_return_val_if_fail(FL_IS_ENGINE(self), nullptr);
  return self->task_runner;
}

void fl_engine_execute_task(FlEngine* self, FlutterTask* task) {
  g_return_if_fail(FL_IS_ENGINE(self));
  if (self->embedder_api.RunTask(self->engine, task) != kSuccess) {
    g_warning("Failed to run task");
  }
}
