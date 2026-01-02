// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/tests/embedder_test.h"

#include "testing/testing.h"

namespace flutter::testing {

EmbedderTest::EmbedderTest() = default;

std::string EmbedderTest::GetFixturesDirectory() const {
  return GetFixturesPath();
}

EmbedderTestContext& EmbedderTest::GetEmbedderContext() {
  if (!context_) {
    context_ = std::make_unique<EmbedderTestContext>(GetFixturesDirectory());
  }
  return *context_;
}

}  // namespace flutter::testing
