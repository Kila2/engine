// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "testing.h"

namespace testing {

std::string GetCurrentTestName() {
  return UnitTest::GetInstance()->current_test_info()->name();
}

}  // namespace testing