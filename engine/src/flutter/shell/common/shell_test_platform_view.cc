// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/shell_test_platform_view.h"

#include <memory>

namespace flutter::testing {

std::unique_ptr<ShellTestPlatformView> ShellTestPlatformView::Create(
    PlatformView::Delegate& delegate,
    const TaskRunners& task_runners) {
  return std::unique_ptr<ShellTestPlatformView>(
      new ShellTestPlatformView(delegate, task_runners));
}

ShellTestPlatformViewBuilder::ShellTestPlatformViewBuilder() {}

std::unique_ptr<PlatformView> ShellTestPlatformViewBuilder::operator()(
    Shell& shell) {
  const TaskRunners& task_runners = shell.GetTaskRunners();

  return ShellTestPlatformView::Create(shell,  //
                                       task_runners);
}

}  // namespace flutter::testing
