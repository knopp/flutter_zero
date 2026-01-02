// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_PLATFORM_VIEW_EMBEDDER_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_PLATFORM_VIEW_EMBEDDER_H_

#include <functional>

#include "flutter/fml/macros.h"
#include "flutter/shell/common/platform_view.h"

namespace flutter {

class PlatformViewEmbedder final : public PlatformView {
 public:
  using PlatformMessageResponseCallback =
      std::function<void(std::unique_ptr<PlatformMessage>)>;
  using ComputePlatformResolvedLocaleCallback =
      std::function<std::unique_ptr<std::vector<std::string>>(
          const std::vector<std::string>& supported_locale_data)>;
  using OnPreEngineRestartCallback = std::function<void()>;
  using ChanneUpdateCallback = std::function<void(const std::string&, bool)>;

  struct PlatformDispatchTable {
    PlatformMessageResponseCallback
        platform_message_response_callback;  // optional
    ComputePlatformResolvedLocaleCallback
        compute_platform_resolved_locale_callback;
    OnPreEngineRestartCallback on_pre_engine_restart_callback;  // optional
    ChanneUpdateCallback on_channel_update;                     // optional
  };

  // Create a platform view that sets up a software rasterizer.
  PlatformViewEmbedder(PlatformView::Delegate& delegate,
                       const flutter::TaskRunners& task_runners,
                       PlatformDispatchTable platform_dispatch_table);

  ~PlatformViewEmbedder() override;

  // |PlatformView|
  void HandlePlatformMessage(std::unique_ptr<PlatformMessage> message) override;

  // |PlatformView|
  std::shared_ptr<PlatformMessageHandler> GetPlatformMessageHandler()
      const override;

 private:
  class EmbedderPlatformMessageHandler;
  std::shared_ptr<EmbedderPlatformMessageHandler> platform_message_handler_;
  PlatformDispatchTable platform_dispatch_table_;

  // |PlatformView|
  void OnPreEngineRestart() const override;

  // |PlatformView|
  std::unique_ptr<std::vector<std::string>> ComputePlatformResolvedLocales(
      const std::vector<std::string>& supported_locale_data) override;

  // |PlatformView|
  void SendChannelUpdate(const std::string& name, bool listening) override;

  FML_DISALLOW_COPY_AND_ASSIGN(PlatformViewEmbedder);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_PLATFORM_VIEW_EMBEDDER_H_
