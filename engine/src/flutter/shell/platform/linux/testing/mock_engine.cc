// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file is a historical legacy, predating the proc table API. It has been
// updated to continue to work with the proc table, but new tests should not
// rely on replacements set up here, but instead use test-local replacements
// for any functions relevant to that test.
//
// Over time existing tests should be migrated and this file should be removed.

#include <cstring>

#include "flutter/shell/platform/embedder/embedder.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_json_message_codec.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_method_response.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_standard_method_codec.h"

struct _FlutterEngine {
  _FlutterEngine() {}
};

namespace {

FlutterEngineResult FlutterEngineCreateAOTData(
    const FlutterEngineAOTDataSource* source,
    FlutterEngineAOTData* data_out) {
  return kSuccess;
}

FlutterEngineResult FlutterEngineCollectAOTData(FlutterEngineAOTData data) {
  return kSuccess;
}

FlutterEngineResult FlutterEngineInitialize(size_t version,
                                            const FlutterProjectArgs* args,
                                            void* user_data,
                                            FLUTTER_API_SYMBOL(FlutterEngine) *
                                                engine_out) {
  *engine_out = new _FlutterEngine();
  return kSuccess;
}

FlutterEngineResult FlutterEngineRunInitialized(
    FLUTTER_API_SYMBOL(FlutterEngine) engine) {
  return kSuccess;
}

FlutterEngineResult FlutterEngineRun(size_t version,
                                     const FlutterProjectArgs* args,
                                     void* user_data,
                                     FLUTTER_API_SYMBOL(FlutterEngine) *
                                         engine_out) {
  return kSuccess;
}

FlutterEngineResult FlutterEngineShutdown(FLUTTER_API_SYMBOL(FlutterEngine)
                                              engine) {
  delete engine;
  return kSuccess;
}

FlutterEngineResult FlutterEngineDeinitialize(FLUTTER_API_SYMBOL(FlutterEngine)
                                                  engine) {
  return kSuccess;
}

FLUTTER_EXPORT
FlutterEngineResult FlutterEngineSendPlatformMessage(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    const FlutterPlatformMessage* message) {
  return kSuccess;
}

FlutterEngineResult FlutterPlatformMessageCreateResponseHandle(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    FlutterDataCallback data_callback,
    void* user_data,
    FlutterPlatformMessageResponseHandle** response_out) {
  return kSuccess;
}

FlutterEngineResult FlutterPlatformMessageReleaseResponseHandle(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    FlutterPlatformMessageResponseHandle* response) {
  return kSuccess;
}

FlutterEngineResult FlutterEngineSendPlatformMessageResponse(
    FLUTTER_API_SYMBOL(FlutterEngine) engine,
    const FlutterPlatformMessageResponseHandle* handle,
    const uint8_t* data,
    size_t data_length) {
  return kSuccess;
}

FlutterEngineResult FlutterEngineRunTask(FLUTTER_API_SYMBOL(FlutterEngine)
                                             engine,
                                         const FlutterTask* task) {
  return kSuccess;
}

bool FlutterEngineRunsAOTCompiledDartCode() {
  return false;
}

FlutterEngineResult FlutterEngineUpdateLocales(FLUTTER_API_SYMBOL(FlutterEngine)
                                                   engine,
                                               const FlutterLocale** locales,
                                               size_t locales_count) {
  return kSuccess;
}

}  // namespace

FlutterEngineResult FlutterEngineGetProcAddresses(
    FlutterEngineProcTable* table) {
  if (!table) {
    return kInvalidArguments;
  }

  FlutterEngineProcTable empty_table = {};
  *table = empty_table;

  table->CreateAOTData = &FlutterEngineCreateAOTData;
  table->CollectAOTData = &FlutterEngineCollectAOTData;
  table->Run = &FlutterEngineRun;
  table->Shutdown = &FlutterEngineShutdown;
  table->Initialize = &FlutterEngineInitialize;
  table->Deinitialize = &FlutterEngineDeinitialize;
  table->RunInitialized = &FlutterEngineRunInitialized;
  table->SendPlatformMessage = &FlutterEngineSendPlatformMessage;
  table->PlatformMessageCreateResponseHandle =
      &FlutterPlatformMessageCreateResponseHandle;
  table->PlatformMessageReleaseResponseHandle =
      &FlutterPlatformMessageReleaseResponseHandle;
  table->SendPlatformMessageResponse =
      &FlutterEngineSendPlatformMessageResponse;
  table->RunTask = &FlutterEngineRunTask;
  table->UpdateLocales = &FlutterEngineUpdateLocales;
  table->RunsAOTCompiledDartCode = &FlutterEngineRunsAOTCompiledDartCode;
  return kSuccess;
}
