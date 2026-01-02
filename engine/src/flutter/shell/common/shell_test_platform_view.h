// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_SHELL_TEST_PLATFORM_VIEW_H_
#define FLUTTER_SHELL_COMMON_SHELL_TEST_PLATFORM_VIEW_H_

#include "flutter/shell/common/platform_view.h"
#include "flutter/shell/common/shell.h"

namespace flutter::testing {

class ShellTestPlatformView : public PlatformView {
 public:
  static std::unique_ptr<ShellTestPlatformView> Create(
      PlatformView::Delegate& delegate,
      const TaskRunners& task_runners);

 protected:
  ShellTestPlatformView(PlatformView::Delegate& delegate,
                        const TaskRunners& task_runners)
      : PlatformView(delegate, task_runners) {}

  FML_DISALLOW_COPY_AND_ASSIGN(ShellTestPlatformView);
};

// Create a ShellTestPlatformView from configuration struct.
class ShellTestPlatformViewBuilder {
 public:
  explicit ShellTestPlatformViewBuilder();
  ~ShellTestPlatformViewBuilder() = default;

  // Override operator () to make this class assignable to std::function.
  std::unique_ptr<PlatformView> operator()(Shell& shell);
};

}  // namespace flutter::testing

#endif  // FLUTTER_SHELL_COMMON_SHELL_TEST_PLATFORM_VIEW_H_
