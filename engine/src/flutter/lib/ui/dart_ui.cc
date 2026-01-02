// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/dart_ui.h"

#include <mutex>
#include <string_view>

#include "flutter/common/constants.h"
#include "flutter/common/settings.h"
#include "flutter/fml/build_config.h"
#include "flutter/lib/ui/dart_runtime_hooks.h"
#include "flutter/lib/ui/isolate_name_server/isolate_name_server_natives.h"
#include "flutter/lib/ui/painting/immutable_buffer.h"
#include "flutter/lib/ui/window/platform_configuration.h"
#include "third_party/tonic/converter/dart_converter.h"
#include "third_party/tonic/dart_args.h"
#include "third_party/tonic/logging/dart_error.h"

using tonic::ToDart;

namespace flutter {

// List of native static functions used as @Native functions.
// Items are tuples of ('function_name', 'parameter_count'), where:
//   'function_name' is the fully qualified name of the native function.
//   'parameter_count' is the number of parameters the function has.
//
// These are used to:
// - Instantiate FfiDispatcher templates to automatically create FFI Native
//   bindings.
//   If the name does not match a native function, the template will fail to
//   instatiate, resulting in a compile time error.
// - Resolve the native function pointer associated with an @Native function.
//   If there is a mismatch between name or parameter count an @Native is
//   trying to resolve, an exception will be thrown.
#define FFI_FUNCTION_LIST(V)                                       \
  /* Constructors */                                               \
  V(ImmutableBuffer::init)                                         \
  V(ImmutableBuffer::initFromAsset)                                \
  V(ImmutableBuffer::initFromFile)                                 \
  V(IsolateNameServerNatives::LookupPortByName)                    \
  V(IsolateNameServerNatives::RegisterPortWithName)                \
  V(IsolateNameServerNatives::RemovePortNameMapping)               \
  V(PlatformConfigurationNativeApi::SetIsolateDebugName)           \
  V(PlatformConfigurationNativeApi::RequestDartPerformanceMode)    \
  V(PlatformConfigurationNativeApi::GetPersistentIsolateData)      \
  V(PlatformConfigurationNativeApi::ComputePlatformResolvedLocale) \
  V(PlatformConfigurationNativeApi::SendPlatformMessage)           \
  V(PlatformConfigurationNativeApi::RespondToPlatformMessage)      \
  V(PlatformConfigurationNativeApi::GetRootIsolateToken)           \
  V(PlatformConfigurationNativeApi::RegisterBackgroundIsolate)     \
  V(PlatformConfigurationNativeApi::SendPortPlatformMessage)       \
  V(PlatformConfigurationNativeApi::SendChannelUpdate)             \
  V(DartRuntimeHooks::Logger_PrintDebugString)                     \
  V(DartRuntimeHooks::Logger_PrintString)                          \
  V(DartRuntimeHooks::ScheduleMicrotask)                           \
  V(DartRuntimeHooks::GetCallbackHandle)                           \
  V(DartRuntimeHooks::GetCallbackFromHandle)                       \
  V(DartPluginRegistrant_EnsureInitialized)

// List of native instance methods used as @Native functions.
// Items are tuples of ('class_name', 'method_name', 'parameter_count'), where:
//   'class_name' is the name of the class containing the method.
//   'method_name' is the name of the method.
//   'parameter_count' is the number of parameters the method has including the
//                     implicit `this` parameter.
//
// These are used to:
// - Instantiate FfiDispatcher templates to automatically create FFI Native
//   bindings.
//   If the name does not match a native function, the template will fail to
//   instatiate, resulting in a compile time error.
// - Resolve the native function pointer associated with an @Native function.
//   If there is a mismatch between names or parameter count an @Native is
//   trying to resolve, an exception will be thrown.
#define FFI_METHOD_LIST(V)    \
  V(ImmutableBuffer, dispose) \
  V(ImmutableBuffer, length)

#define FFI_FUNCTION_INSERT(FUNCTION)           \
  g_function_dispatchers.insert(std::make_pair( \
      std::string_view(#FUNCTION),              \
      reinterpret_cast<void*>(                  \
          tonic::FfiDispatcher<void, decltype(&FUNCTION), &FUNCTION>::Call)));

#define FFI_METHOD_INSERT(CLASS, METHOD)                                       \
  g_function_dispatchers.insert(                                               \
      std::make_pair(std::string_view(#CLASS "::" #METHOD),                    \
                     reinterpret_cast<void*>(                                  \
                         tonic::FfiDispatcher<CLASS, decltype(&CLASS::METHOD), \
                                              &CLASS::METHOD>::Call)));

namespace {

std::once_flag g_dispatchers_init_flag;
std::unordered_map<std::string_view, void*> g_function_dispatchers;

void* ResolveFfiNativeFunction(const char* name, uintptr_t args) {
  auto it = g_function_dispatchers.find(name);
  return (it != g_function_dispatchers.end()) ? it->second : nullptr;
}

void InitDispatcherMap() {
  FFI_FUNCTION_LIST(FFI_FUNCTION_INSERT)
  FFI_METHOD_LIST(FFI_METHOD_INSERT)
}

}  // anonymous namespace

void DartUI::InitForIsolate(const Settings& settings) {
  std::call_once(g_dispatchers_init_flag, InitDispatcherMap);

  auto dart_ui = Dart_LookupLibrary(ToDart("dart:ui"));
  if (Dart_IsError(dart_ui)) {
    Dart_PropagateError(dart_ui);
  }

  // Set up FFI Native resolver for dart:ui.
  Dart_Handle result =
      Dart_SetFfiNativeResolver(dart_ui, ResolveFfiNativeFunction);
  if (Dart_IsError(result)) {
    Dart_PropagateError(result);
  }
}

}  // namespace flutter
