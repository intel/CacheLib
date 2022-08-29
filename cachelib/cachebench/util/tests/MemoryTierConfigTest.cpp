/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Copyright 2022-present Facebook. All Rights Reserved.

#include <algorithm>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "cachelib/cachebench/util/CacheConfig.h"

namespace facebook {
namespace cachelib {
namespace cachebench {

TEST(MemoryTierConfigTest, MemBind_SingleNumaNode) {
  const std::string configString =
    "{"
    "  \"ratio\": 1,"
    "  \"memBindNodes\": 1"
    "}";

  const std::vector<size_t> expectedNumaNodes = {1};

  auto configJson = folly::parseJson(folly::json::stripComments(configString));
  
  MemoryTierConfig memoryTierConfig(configJson);
  MemoryTierCacheConfig tierCacheConfig = memoryTierConfig.getMemoryTierCacheConfig();

  auto parsedNumaNodes = tierCacheConfig.getMemBind();
  ASSERT_TRUE(std::equal(expectedNumaNodes.begin(), expectedNumaNodes.end(), parsedNumaNodes.begin()));
}

TEST(MemoryTierConfigTest, MemBind_RangeNumaNodes) {
  const std::string configString =
    "{"
    "  \"ratio\": 1,"
    "  \"memBindNodes\": \"0-2\""
    "}";

  const std::vector<size_t> expectedNumaNodes = {0, 1, 2};

  auto configJson = folly::parseJson(folly::json::stripComments(configString));
  
  MemoryTierConfig memoryTierConfig(configJson);
  MemoryTierCacheConfig tierCacheConfig = memoryTierConfig.getMemoryTierCacheConfig();

  auto parsedNumaNodes = tierCacheConfig.getMemBind();
  ASSERT_TRUE(std::equal(expectedNumaNodes.begin(), expectedNumaNodes.end(), parsedNumaNodes.begin()));
}

TEST(MemoryTierConfigTest, MemBind_SingleAndRangeNumaNodes) {
  const std::string configString =
    "{"
    "  \"ratio\": 1,"
    "  \"memBindNodes\": \"0,2-5\""
    "}";

  const std::vector<size_t> expectedNumaNodes = {0, 2, 3, 4, 5};

  auto configJson = folly::parseJson(folly::json::stripComments(configString));
  
  MemoryTierConfig memoryTierConfig(configJson);
  MemoryTierCacheConfig tierCacheConfig = memoryTierConfig.getMemoryTierCacheConfig();

  auto parsedNumaNodes = tierCacheConfig.getMemBind();
  ASSERT_TRUE(std::equal(expectedNumaNodes.begin(), expectedNumaNodes.end(), parsedNumaNodes.begin()));
}

} // namespace facebook
} // namespace cachelib
} // namespace cachebench