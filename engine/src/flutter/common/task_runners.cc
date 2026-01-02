// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/common/task_runners.h"

#include <utility>

namespace flutter {

TaskRunners::TaskRunners(std::string label,
                         fml::RefPtr<fml::TaskRunner> platform,
                         fml::RefPtr<fml::TaskRunner> ui)
    : label_(std::move(label)),
      platform_(std::move(platform)),
      ui_(std::move(ui)) {}

TaskRunners::TaskRunners(const TaskRunners& other) = default;

TaskRunners::~TaskRunners() = default;

const std::string& TaskRunners::GetLabel() const {
  return label_;
}

fml::RefPtr<fml::TaskRunner> TaskRunners::GetPlatformTaskRunner() const {
  return platform_;
}

fml::RefPtr<fml::TaskRunner> TaskRunners::GetUITaskRunner() const {
  return ui_;
}

bool TaskRunners::IsValid() const {
  return platform_ && ui_;
}

}  // namespace flutter
