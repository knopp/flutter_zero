// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/platform_view.h"

#include <utility>

namespace flutter {

PlatformView::PlatformView(Delegate& delegate, const TaskRunners& task_runners)
    : delegate_(delegate), task_runners_(task_runners), weak_factory_(this) {}

PlatformView::~PlatformView() = default;

void PlatformView::DispatchPlatformMessage(
    std::unique_ptr<PlatformMessage> message) {
  delegate_.OnPlatformViewDispatchPlatformMessage(std::move(message));
}

void PlatformView::NotifyCreated() {
  delegate_.OnPlatformViewCreated();
}

void PlatformView::NotifyDestroyed() {
  delegate_.OnPlatformViewDestroyed();
}

fml::WeakPtr<PlatformView> PlatformView::GetWeakPtr() const {
  return weak_factory_.GetWeakPtr();
}

void PlatformView::SendChannelUpdate(const std::string& name, bool listening) {}

void PlatformView::HandlePlatformMessage(
    std::unique_ptr<PlatformMessage> message) {
  if (auto response = message->response()) {
    response->CompleteEmpty();
  }
}

void PlatformView::OnPreEngineRestart() const {}

std::unique_ptr<std::vector<std::string>>
PlatformView::ComputePlatformResolvedLocales(
    const std::vector<std::string>& supported_locale_data) {
  std::unique_ptr<std::vector<std::string>> out =
      std::make_unique<std::vector<std::string>>();
  return out;
}

void PlatformView::RequestDartDeferredLibrary(intptr_t loading_unit_id) {}

void PlatformView::LoadDartDeferredLibrary(
    intptr_t loading_unit_id,
    std::unique_ptr<const fml::Mapping> snapshot_data,
    std::unique_ptr<const fml::Mapping> snapshot_instructions) {}

void PlatformView::LoadDartDeferredLibraryError(
    intptr_t loading_unit_id,
    const std::string
        error_message,  // NOLINT(performance-unnecessary-value-param)
    bool transient) {}

void PlatformView::UpdateAssetResolverByType(
    std::unique_ptr<AssetResolver> updated_asset_resolver,
    AssetResolver::AssetResolverType type) {
  delegate_.UpdateAssetResolverByType(std::move(updated_asset_resolver), type);
}

std::shared_ptr<PlatformMessageHandler>
PlatformView::GetPlatformMessageHandler() const {
  return nullptr;
}

const Settings& PlatformView::GetSettings() const {
  return delegate_.OnPlatformViewGetSettings();
}

}  // namespace flutter
