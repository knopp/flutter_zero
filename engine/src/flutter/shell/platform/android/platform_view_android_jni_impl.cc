// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/android/platform_view_android_jni_impl.h"

#include <android/hardware_buffer_jni.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <jni.h>
#include <memory>
#include <utility>

#include "unicode/uchar.h"

#include "flutter/common/constants.h"
#include "flutter/fml/mapping.h"
#include "flutter/fml/native_library.h"
#include "flutter/fml/platform/android/jni_util.h"
#include "flutter/fml/platform/android/jni_weak_ref.h"
#include "flutter/fml/platform/android/scoped_java_ref.h"
#include "flutter/lib/ui/plugins/callback_cache.h"
#include "flutter/shell/platform/android/android_shell_holder.h"
#include "flutter/shell/platform/android/apk_asset_provider.h"
#include "flutter/shell/platform/android/flutter_main.h"
#include "flutter/shell/platform/android/jni/platform_view_android_jni.h"
#include "flutter/shell/platform/android/platform_view_android.h"

#define ANDROID_SHELL_HOLDER \
  (reinterpret_cast<AndroidShellHolder*>(shell_holder))

namespace flutter {

static fml::jni::ScopedJavaGlobalRef<jclass>* g_flutter_callback_info_class =
    nullptr;

static fml::jni::ScopedJavaGlobalRef<jclass>* g_flutter_jni_class = nullptr;

static fml::jni::ScopedJavaGlobalRef<jclass>* g_java_weak_reference_class =
    nullptr;

static fml::jni::ScopedJavaGlobalRef<jclass>* g_java_long_class = nullptr;

static jmethodID g_compute_platform_resolved_locale_method = nullptr;

static jmethodID g_request_dart_deferred_library_method = nullptr;

// Called By Native

static jmethodID g_flutter_callback_info_constructor = nullptr;

static jfieldID g_jni_shell_holder_field = nullptr;

#define FLUTTER_FOR_EACH_JNI_METHOD(V)                                        \
  V(g_handle_platform_message_method, handlePlatformMessage,                  \
    "(Ljava/lang/String;Ljava/nio/ByteBuffer;IJ)V")                           \
  V(g_handle_platform_message_response_method, handlePlatformMessageResponse, \
    "(ILjava/nio/ByteBuffer;)V")                                              \
  V(g_on_engine_restart_method, onPreEngineRestart, "()V")                    \
  //

#define FLUTTER_DECLARE_JNI(global_field, jni_name, jni_arg) \
  static jmethodID global_field = nullptr;

#define FLUTTER_BIND_JNI(global_field, jni_name, jni_arg)               \
  global_field =                                                        \
      env->GetMethodID(g_flutter_jni_class->obj(), #jni_name, jni_arg); \
  if (global_field == nullptr) {                                        \
    FML_LOG(ERROR) << "Could not locate " << #jni_name << " method.";   \
    return false;                                                       \
  }

static jmethodID g_jni_constructor = nullptr;

static jmethodID g_long_constructor = nullptr;

FLUTTER_FOR_EACH_JNI_METHOD(FLUTTER_DECLARE_JNI)

static jmethodID g_java_weak_reference_get_method = nullptr;

// Called By Java
static jlong AttachJNI(JNIEnv* env, jclass clazz, jobject flutterJNI) {
  fml::jni::JavaObjectWeakGlobalRef java_object(env, flutterJNI);
  std::shared_ptr<PlatformViewAndroidJNI> jni_facade =
      std::make_shared<PlatformViewAndroidJNIImpl>(java_object);
  auto shell_holder = std::make_unique<AndroidShellHolder>(
      FlutterMain::Get().GetSettings(), jni_facade);
  if (shell_holder->IsValid()) {
    return reinterpret_cast<jlong>(shell_holder.release());
  } else {
    return 0;
  }
}

static void DestroyJNI(JNIEnv* env, jobject jcaller, jlong shell_holder) {
  delete ANDROID_SHELL_HOLDER;
}

// Signature is similar to RunBundleAndSnapshotFromLibrary but it can't change
// the bundle path or asset manager since we can only spawn with the same
// AOT.
//
// The shell_holder instance must be a pointer address to the current
// AndroidShellHolder whose Shell will be used to spawn a new Shell.
//
// This creates a Java Long that points to the newly created
// AndroidShellHolder's raw pointer, connects that Long to a newly created
// FlutterJNI instance, then returns the FlutterJNI instance.
static jobject SpawnJNI(JNIEnv* env,
                        jobject jcaller,
                        jlong shell_holder,
                        jstring jEntrypoint,
                        jstring jLibraryUrl,
                        jobject jEntrypointArgs,
                        jlong engineId) {
  jobject jni = env->NewObject(g_flutter_jni_class->obj(), g_jni_constructor);
  if (jni == nullptr) {
    FML_LOG(ERROR) << "Could not create a FlutterJNI instance";
    return nullptr;
  }

  fml::jni::JavaObjectWeakGlobalRef java_jni(env, jni);
  std::shared_ptr<PlatformViewAndroidJNI> jni_facade =
      std::make_shared<PlatformViewAndroidJNIImpl>(java_jni);

  auto entrypoint = fml::jni::JavaStringToString(env, jEntrypoint);
  auto libraryUrl = fml::jni::JavaStringToString(env, jLibraryUrl);
  auto entrypoint_args = fml::jni::StringListToVector(env, jEntrypointArgs);

  auto spawned_shell_holder = ANDROID_SHELL_HOLDER->Spawn(
      jni_facade, entrypoint, libraryUrl, entrypoint_args, engineId);

  if (spawned_shell_holder == nullptr || !spawned_shell_holder->IsValid()) {
    FML_LOG(ERROR) << "Could not spawn Shell";
    return nullptr;
  }

  jobject javaLong = env->CallStaticObjectMethod(
      g_java_long_class->obj(), g_long_constructor,
      reinterpret_cast<jlong>(spawned_shell_holder.release()));
  if (javaLong == nullptr) {
    FML_LOG(ERROR) << "Could not create a Long instance";
    return nullptr;
  }

  env->SetObjectField(jni, g_jni_shell_holder_field, javaLong);

  return jni;
}

static void RunBundleAndSnapshotFromLibrary(JNIEnv* env,
                                            jobject jcaller,
                                            jlong shell_holder,
                                            jstring jBundlePath,
                                            jstring jEntrypoint,
                                            jstring jLibraryUrl,
                                            jobject jAssetManager,
                                            jobject jEntrypointArgs,
                                            jlong engineId) {
  auto apk_asset_provider = std::make_unique<flutter::APKAssetProvider>(
      env,                                            // jni environment
      jAssetManager,                                  // asset manager
      fml::jni::JavaStringToString(env, jBundlePath)  // apk asset dir
  );
  auto entrypoint = fml::jni::JavaStringToString(env, jEntrypoint);
  auto libraryUrl = fml::jni::JavaStringToString(env, jLibraryUrl);
  auto entrypoint_args = fml::jni::StringListToVector(env, jEntrypointArgs);

  ANDROID_SHELL_HOLDER->Launch(std::move(apk_asset_provider), entrypoint,
                               libraryUrl, entrypoint_args, engineId);
}

static jobject LookupCallbackInformation(JNIEnv* env,
                                         /* unused */ jobject,
                                         jlong handle) {
  auto cbInfo = flutter::DartCallbackCache::GetCallbackInformation(handle);
  if (cbInfo == nullptr) {
    return nullptr;
  }
  return env->NewObject(g_flutter_callback_info_class->obj(),
                        g_flutter_callback_info_constructor,
                        env->NewStringUTF(cbInfo->name.c_str()),
                        env->NewStringUTF(cbInfo->class_name.c_str()),
                        env->NewStringUTF(cbInfo->library_path.c_str()));
}

static void DispatchPlatformMessage(JNIEnv* env,
                                    jobject jcaller,
                                    jlong shell_holder,
                                    jstring channel,
                                    jobject message,
                                    jint position,
                                    jint responseId) {
  ANDROID_SHELL_HOLDER->GetPlatformView()->DispatchPlatformMessage(
      env,                                         //
      fml::jni::JavaStringToString(env, channel),  //
      message,                                     //
      position,                                    //
      responseId                                   //
  );
}

static void DispatchEmptyPlatformMessage(JNIEnv* env,
                                         jobject jcaller,
                                         jlong shell_holder,
                                         jstring channel,
                                         jint responseId) {
  ANDROID_SHELL_HOLDER->GetPlatformView()->DispatchEmptyPlatformMessage(
      env,                                         //
      fml::jni::JavaStringToString(env, channel),  //
      responseId                                   //
  );
}

static void CleanupMessageData(JNIEnv* env,
                               jobject jcaller,
                               jlong message_data) {
  // Called from any thread.
  free(reinterpret_cast<void*>(message_data));
}

static void InvokePlatformMessageResponseCallback(JNIEnv* env,
                                                  jobject jcaller,
                                                  jlong shell_holder,
                                                  jint responseId,
                                                  jobject message,
                                                  jint position) {
  uint8_t* response_data =
      static_cast<uint8_t*>(env->GetDirectBufferAddress(message));
  FML_DCHECK(response_data != nullptr);
  auto mapping = std::make_unique<fml::MallocMapping>(
      fml::MallocMapping::Copy(response_data, response_data + position));
  ANDROID_SHELL_HOLDER->GetPlatformMessageHandler()
      ->InvokePlatformMessageResponseCallback(responseId, std::move(mapping));
}

static void InvokePlatformMessageEmptyResponseCallback(JNIEnv* env,
                                                       jobject jcaller,
                                                       jlong shell_holder,
                                                       jint responseId) {
  ANDROID_SHELL_HOLDER->GetPlatformMessageHandler()
      ->InvokePlatformMessageEmptyResponseCallback(responseId);
}

static void NotifyLowMemoryWarning(JNIEnv* env,
                                   jobject obj,
                                   jlong shell_holder) {
  ANDROID_SHELL_HOLDER->NotifyLowMemoryWarning();
}

static void LoadLoadingUnitFailure(intptr_t loading_unit_id,
                                   const std::string& message,
                                   bool transient) {
  // TODO(garyq): Implement
}

static void DeferredComponentInstallFailure(JNIEnv* env,
                                            jobject obj,
                                            jint jLoadingUnitId,
                                            jstring jError,
                                            jboolean jTransient) {
  LoadLoadingUnitFailure(static_cast<intptr_t>(jLoadingUnitId),
                         fml::jni::JavaStringToString(env, jError),
                         static_cast<bool>(jTransient));
}

static void LoadDartDeferredLibrary(JNIEnv* env,
                                    jobject obj,
                                    jlong shell_holder,
                                    jint jLoadingUnitId,
                                    jobjectArray jSearchPaths) {
  // Convert java->c++
  intptr_t loading_unit_id = static_cast<intptr_t>(jLoadingUnitId);
  std::vector<std::string> search_paths =
      fml::jni::StringArrayToVector(env, jSearchPaths);

  // Use dlopen here to directly check if handle is nullptr before creating a
  // NativeLibrary.
  void* handle = nullptr;
  while (handle == nullptr && !search_paths.empty()) {
    std::string path = search_paths.back();
    handle = ::dlopen(path.c_str(), RTLD_NOW);
    search_paths.pop_back();
  }
  if (handle == nullptr) {
    LoadLoadingUnitFailure(loading_unit_id,
                           "No lib .so found for provided search paths.", true);
    return;
  }
  fml::RefPtr<fml::NativeLibrary> native_lib =
      fml::NativeLibrary::CreateWithHandle(handle, false);

  // Resolve symbols.
  std::unique_ptr<const fml::SymbolMapping> data_mapping =
      std::make_unique<const fml::SymbolMapping>(
          native_lib, DartSnapshot::kIsolateDataSymbol);
  std::unique_ptr<const fml::SymbolMapping> instructions_mapping =
      std::make_unique<const fml::SymbolMapping>(
          native_lib, DartSnapshot::kIsolateInstructionsSymbol);

  ANDROID_SHELL_HOLDER->GetPlatformView()->LoadDartDeferredLibrary(
      loading_unit_id, std::move(data_mapping),
      std::move(instructions_mapping));
}

static void UpdateJavaAssetManager(JNIEnv* env,
                                   jobject obj,
                                   jlong shell_holder,
                                   jobject jAssetManager,
                                   jstring jAssetBundlePath) {
  auto asset_resolver = std::make_unique<flutter::APKAssetProvider>(
      env,                                                   // jni environment
      jAssetManager,                                         // asset manager
      fml::jni::JavaStringToString(env, jAssetBundlePath));  // apk asset dir

  ANDROID_SHELL_HOLDER->GetPlatformView()->UpdateAssetResolverByType(
      std::move(asset_resolver),
      AssetResolver::AssetResolverType::kApkAssetProvider);
}

bool RegisterApi(JNIEnv* env) {
  static const JNINativeMethod flutter_jni_methods[] = {
      // Start of methods from FlutterJNI
      {
          .name = "nativeAttach",
          .signature = "(Lio/flutter/embedding/engine/FlutterJNI;)J",
          .fnPtr = reinterpret_cast<void*>(&AttachJNI),
      },
      {
          .name = "nativeDestroy",
          .signature = "(J)V",
          .fnPtr = reinterpret_cast<void*>(&DestroyJNI),
      },
      {
          .name = "nativeSpawn",
          .signature = "(JLjava/lang/String;Ljava/lang/String;Ljava/util/"
                       "List;J)Lio/flutter/"
                       "embedding/engine/FlutterJNI;",
          .fnPtr = reinterpret_cast<void*>(&SpawnJNI),
      },
      {
          .name = "nativeRunBundleAndSnapshotFromLibrary",
          .signature = "(JLjava/lang/String;Ljava/lang/String;"
                       "Ljava/lang/String;Landroid/content/res/"
                       "AssetManager;Ljava/util/List;J)V",
          .fnPtr = reinterpret_cast<void*>(&RunBundleAndSnapshotFromLibrary),
      },
      {
          .name = "nativeDispatchEmptyPlatformMessage",
          .signature = "(JLjava/lang/String;I)V",
          .fnPtr = reinterpret_cast<void*>(&DispatchEmptyPlatformMessage),
      },
      {
          .name = "nativeCleanupMessageData",
          .signature = "(J)V",
          .fnPtr = reinterpret_cast<void*>(&CleanupMessageData),
      },
      {
          .name = "nativeDispatchPlatformMessage",
          .signature = "(JLjava/lang/String;Ljava/nio/ByteBuffer;II)V",
          .fnPtr = reinterpret_cast<void*>(&DispatchPlatformMessage),
      },
      {
          .name = "nativeInvokePlatformMessageResponseCallback",
          .signature = "(JILjava/nio/ByteBuffer;I)V",
          .fnPtr =
              reinterpret_cast<void*>(&InvokePlatformMessageResponseCallback),
      },
      {
          .name = "nativeInvokePlatformMessageEmptyResponseCallback",
          .signature = "(JI)V",
          .fnPtr = reinterpret_cast<void*>(
              &InvokePlatformMessageEmptyResponseCallback),
      },
      {
          .name = "nativeNotifyLowMemoryWarning",
          .signature = "(J)V",
          .fnPtr = reinterpret_cast<void*>(&NotifyLowMemoryWarning),
      },

      // Methods for Dart callback functionality.
      {
          .name = "nativeLookupCallbackInformation",
          .signature = "(J)Lio/flutter/view/FlutterCallbackInformation;",
          .fnPtr = reinterpret_cast<void*>(&LookupCallbackInformation),
      },

      {
          .name = "nativeLoadDartDeferredLibrary",
          .signature = "(JI[Ljava/lang/String;)V",
          .fnPtr = reinterpret_cast<void*>(&LoadDartDeferredLibrary),
      },
      {
          .name = "nativeUpdateJavaAssetManager",
          .signature =
              "(JLandroid/content/res/AssetManager;Ljava/lang/String;)V",
          .fnPtr = reinterpret_cast<void*>(&UpdateJavaAssetManager),
      },
      {
          .name = "nativeDeferredComponentInstallFailure",
          .signature = "(ILjava/lang/String;Z)V",
          .fnPtr = reinterpret_cast<void*>(&DeferredComponentInstallFailure),
      },
  };

  if (env->RegisterNatives(g_flutter_jni_class->obj(), flutter_jni_methods,
                           std::size(flutter_jni_methods)) != 0) {
    FML_LOG(ERROR) << "Failed to RegisterNatives with FlutterJNI";
    return false;
  }

  g_jni_shell_holder_field = env->GetFieldID(
      g_flutter_jni_class->obj(), "nativeShellHolderId", "Ljava/lang/Long;");

  if (g_jni_shell_holder_field == nullptr) {
    FML_LOG(ERROR) << "Could not locate FlutterJNI's nativeShellHolderId field";
    return false;
  }

  g_jni_constructor =
      env->GetMethodID(g_flutter_jni_class->obj(), "<init>", "()V");

  if (g_jni_constructor == nullptr) {
    FML_LOG(ERROR) << "Could not locate FlutterJNI's constructor";
    return false;
  }

  g_long_constructor = env->GetStaticMethodID(g_java_long_class->obj(),
                                              "valueOf", "(J)Ljava/lang/Long;");
  if (g_long_constructor == nullptr) {
    FML_LOG(ERROR) << "Could not locate Long's constructor";
    return false;
  }

  FLUTTER_FOR_EACH_JNI_METHOD(FLUTTER_BIND_JNI)

  return true;
}

bool PlatformViewAndroid::Register(JNIEnv* env) {
  if (env == nullptr) {
    FML_LOG(ERROR) << "No JNIEnv provided";
    return false;
  }

  g_flutter_callback_info_class = new fml::jni::ScopedJavaGlobalRef<jclass>(
      env, env->FindClass("io/flutter/view/FlutterCallbackInformation"));
  if (g_flutter_callback_info_class->is_null()) {
    FML_LOG(ERROR) << "Could not locate FlutterCallbackInformation class";
    return false;
  }

  g_flutter_callback_info_constructor = env->GetMethodID(
      g_flutter_callback_info_class->obj(), "<init>",
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
  if (g_flutter_callback_info_constructor == nullptr) {
    FML_LOG(ERROR) << "Could not locate FlutterCallbackInformation constructor";
    return false;
  }

  g_flutter_jni_class = new fml::jni::ScopedJavaGlobalRef<jclass>(
      env, env->FindClass("io/flutter/embedding/engine/FlutterJNI"));
  if (g_flutter_jni_class->is_null()) {
    FML_LOG(ERROR) << "Failed to find FlutterJNI Class.";
    return false;
  }

  g_java_weak_reference_class = new fml::jni::ScopedJavaGlobalRef<jclass>(
      env, env->FindClass("java/lang/ref/WeakReference"));
  if (g_java_weak_reference_class->is_null()) {
    FML_LOG(ERROR) << "Could not locate WeakReference class";
    return false;
  }

  g_java_weak_reference_get_method = env->GetMethodID(
      g_java_weak_reference_class->obj(), "get", "()Ljava/lang/Object;");
  if (g_java_weak_reference_get_method == nullptr) {
    FML_LOG(ERROR) << "Could not locate WeakReference.get method";
    return false;
  }

  // Ensure we don't have any pending exceptions.
  FML_CHECK(fml::jni::CheckException(env));

  g_compute_platform_resolved_locale_method = env->GetMethodID(
      g_flutter_jni_class->obj(), "computePlatformResolvedLocale",
      "([Ljava/lang/String;)[Ljava/lang/String;");

  if (g_compute_platform_resolved_locale_method == nullptr) {
    FML_LOG(ERROR) << "Could not locate computePlatformResolvedLocale method";
    return false;
  }

  g_request_dart_deferred_library_method = env->GetMethodID(
      g_flutter_jni_class->obj(), "requestDartDeferredLibrary", "(I)V");

  if (g_request_dart_deferred_library_method == nullptr) {
    FML_LOG(ERROR) << "Could not locate requestDartDeferredLibrary method";
    return false;
  }

  g_java_long_class = new fml::jni::ScopedJavaGlobalRef<jclass>(
      env, env->FindClass("java/lang/Long"));
  if (g_java_long_class->is_null()) {
    FML_LOG(ERROR) << "Could not locate java.lang.Long class";
    return false;
  }

  // Ensure we don't have any pending exceptions.
  FML_CHECK(fml::jni::CheckException(env));

  return RegisterApi(env);
}

PlatformViewAndroidJNIImpl::PlatformViewAndroidJNIImpl(
    const fml::jni::JavaObjectWeakGlobalRef& java_object)
    : java_object_(java_object) {}

PlatformViewAndroidJNIImpl::~PlatformViewAndroidJNIImpl() = default;

void PlatformViewAndroidJNIImpl::FlutterViewHandlePlatformMessage(
    std::unique_ptr<flutter::PlatformMessage> message,
    int responseId) {
  // Called from any thread.
  JNIEnv* env = fml::jni::AttachCurrentThread();

  auto java_object = java_object_.get(env);
  if (java_object.is_null()) {
    return;
  }

  fml::jni::ScopedJavaLocalRef<jstring> java_channel =
      fml::jni::StringToJavaString(env, message->channel());

  if (message->hasData()) {
    fml::jni::ScopedJavaLocalRef<jobject> message_array(
        env, env->NewDirectByteBuffer(
                 const_cast<uint8_t*>(message->data().GetMapping()),
                 message->data().GetSize()));
    // Message data is deleted in CleanupMessageData.
    fml::MallocMapping mapping = message->releaseData();
    env->CallVoidMethod(java_object.obj(), g_handle_platform_message_method,
                        java_channel.obj(), message_array.obj(), responseId,
                        reinterpret_cast<jlong>(mapping.Release()));
  } else {
    env->CallVoidMethod(java_object.obj(), g_handle_platform_message_method,
                        java_channel.obj(), nullptr, responseId, nullptr);
  }

  FML_CHECK(fml::jni::CheckException(env));
}

void PlatformViewAndroidJNIImpl::FlutterViewHandlePlatformMessageResponse(
    int responseId,
    std::unique_ptr<fml::Mapping> data) {
  // We are on the platform thread. Attempt to get the strong reference to
  // the Java object.
  JNIEnv* env = fml::jni::AttachCurrentThread();

  auto java_object = java_object_.get(env);
  if (java_object.is_null()) {
    // The Java object was collected before this message response got to
    // it. Drop the response on the floor.
    return;
  }
  if (data == nullptr) {  // Empty response.
    env->CallVoidMethod(java_object.obj(),
                        g_handle_platform_message_response_method, responseId,
                        nullptr);
  } else {
    // Convert the vector to a Java byte array.
    fml::jni::ScopedJavaLocalRef<jobject> data_array(
        env, env->NewDirectByteBuffer(const_cast<uint8_t*>(data->GetMapping()),
                                      data->GetSize()));

    env->CallVoidMethod(java_object.obj(),
                        g_handle_platform_message_response_method, responseId,
                        data_array.obj());
  }

  FML_CHECK(fml::jni::CheckException(env));
}

void PlatformViewAndroidJNIImpl::FlutterViewOnPreEngineRestart() {
  JNIEnv* env = fml::jni::AttachCurrentThread();

  auto java_object = java_object_.get(env);
  if (java_object.is_null()) {
    return;
  }

  env->CallVoidMethod(java_object.obj(), g_on_engine_restart_method);

  FML_CHECK(fml::jni::CheckException(env));
}

std::unique_ptr<std::vector<std::string>>
PlatformViewAndroidJNIImpl::FlutterViewComputePlatformResolvedLocale(
    std::vector<std::string> supported_locales_data) {
  JNIEnv* env = fml::jni::AttachCurrentThread();

  std::unique_ptr<std::vector<std::string>> out =
      std::make_unique<std::vector<std::string>>();

  auto java_object = java_object_.get(env);
  if (java_object.is_null()) {
    return out;
  }
  fml::jni::ScopedJavaLocalRef<jobjectArray> j_locales_data =
      fml::jni::VectorToStringArray(env, supported_locales_data);
  jobjectArray result = static_cast<jobjectArray>(env->CallObjectMethod(
      java_object.obj(), g_compute_platform_resolved_locale_method,
      j_locales_data.obj()));

  FML_CHECK(fml::jni::CheckException(env));

  int length = env->GetArrayLength(result);
  for (int i = 0; i < length; i++) {
    out->emplace_back(fml::jni::JavaStringToString(
        env, static_cast<jstring>(env->GetObjectArrayElement(result, i))));
  }
  return out;
}

bool PlatformViewAndroidJNIImpl::RequestDartDeferredLibrary(
    int loading_unit_id) {
  JNIEnv* env = fml::jni::AttachCurrentThread();

  auto java_object = java_object_.get(env);
  if (java_object.is_null()) {
    return true;
  }

  env->CallVoidMethod(java_object.obj(), g_request_dart_deferred_library_method,
                      loading_unit_id);

  FML_CHECK(fml::jni::CheckException(env));
  return true;
}

}  // namespace flutter
