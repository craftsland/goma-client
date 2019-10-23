// Copyright 2019 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include "luci_context.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string input(reinterpret_cast<const char*>(data), size);

  devtools_goma::LuciOAuthTokenResponse resp;
  devtools_goma::ParseLuciOAuthTokenResponse(input, &resp);

  return 0;
}
