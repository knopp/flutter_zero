// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_TESTS_EMBEDDER_TEST_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_TESTS_EMBEDDER_TEST_H_

#include <memory>

#include "flutter/fml/macros.h"
#include "flutter/shell/platform/embedder/tests/embedder_test_context.h"
#include "flutter/testing/thread_test.h"

namespace flutter::testing {

class EmbedderTestContext;

class EmbedderTest : public ThreadTest {
 public:
  EmbedderTest();

  std::string GetFixturesDirectory() const;

  EmbedderTestContext& GetEmbedderContext();

 protected:
  std::unique_ptr<EmbedderTestContext> context_;

  FML_DISALLOW_COPY_AND_ASSIGN(EmbedderTest);
};

}  // namespace flutter::testing

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_TESTS_EMBEDDER_TEST_H_
