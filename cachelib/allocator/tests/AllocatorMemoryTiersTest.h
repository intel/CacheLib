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

#pragma once

#include "cachelib/allocator/CacheAllocatorConfig.h"
#include "cachelib/allocator/MemoryTierCacheConfig.h"
#include "cachelib/allocator/tests/TestBase.h"

namespace facebook {
namespace cachelib {
namespace tests {

template <typename AllocatorT>
class AllocatorMemoryTiersTest : public AllocatorTest<AllocatorT> {
 public:
  auto makeDefaultConfig() {
    typename AllocatorT::Config config;
    config.setCacheSize(300 * Slab::kSize);
    config.enableCachePersistence("/tmp");
    config.usePosixForShm();
    config.configureMemoryTiers({
        MemoryTierCacheConfig::fromFile("/tmp/a" + std::to_string(::getpid()))
            .setRatio(1),
        MemoryTierCacheConfig::fromFile("/tmp/b" + std::to_string(::getpid()))
            .setRatio(1)
    });
    return config;
  }

  void testMultiTiersInvalid() {
    typename AllocatorT::Config config;
    config.setCacheSize(100 * Slab::kSize);
    config.configureMemoryTiers({
        MemoryTierCacheConfig::fromFile("/tmp/a" + std::to_string(::getpid()))
            .setRatio(1),
        MemoryTierCacheConfig::fromFile("/tmp/b" + std::to_string(::getpid()))
            .setRatio(1)
    });

    // More than one tier is not supported
    ASSERT_THROW(std::make_unique<AllocatorT>(AllocatorT::SharedMemNew, config),
                 std::invalid_argument);
  }

  void testMultiTiersValid() {
    typename AllocatorT::Config config;
    config.setCacheSize(100 * Slab::kSize);
    config.enableCachePersistence("/tmp");
    config.usePosixForShm();
    config.configureMemoryTiers({
        MemoryTierCacheConfig::fromFile("/tmp/a" + std::to_string(::getpid()))
            .setRatio(1),
        MemoryTierCacheConfig::fromFile("/tmp/b" + std::to_string(::getpid()))
            .setRatio(1)
    });

    auto alloc = std::make_unique<AllocatorT>(AllocatorT::SharedMemNew, config);
    ASSERT(alloc != nullptr);

    auto pool = alloc->addPool("default", alloc->getCacheMemoryStats().cacheSize);
    auto handle = alloc->allocate(pool, "key", std::string("value").size());
    ASSERT(handle != nullptr);
    ASSERT_NO_THROW(alloc->insertOrReplace(handle));
  }

  void testMultiTiersValidMixed() {
    typename AllocatorT::Config config;
    config.setCacheSize(100 * Slab::kSize);
    config.enableCachePersistence("/tmp");
    config.usePosixForShm();
    config.configureMemoryTiers({
        MemoryTierCacheConfig::fromShm()
            .setRatio(1),
        MemoryTierCacheConfig::fromFile("/tmp/b" + std::to_string(::getpid()))
            .setRatio(1)
    });

    auto alloc = std::make_unique<AllocatorT>(AllocatorT::SharedMemNew, config);
    ASSERT(alloc != nullptr);

    auto pool = alloc->addPool("default", alloc->getCacheMemoryStats().cacheSize);
    auto handle = alloc->allocate(pool, "key", std::string("value").size());
    ASSERT(handle != nullptr);
    ASSERT_NO_THROW(alloc->insertOrReplace(handle));
  }

  void testMultiTiersForceTierAllocation() {
    auto config = makeDefaultConfig();
    config.forceAllocationTier = 0;

    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      auto pool = alloc.addPool("default", alloc.getCacheMemoryStats().cacheSize);
      auto handle = alloc.allocate(pool, "key", std::string("value").size());
      ASSERT(handle != nullptr);
      ASSERT_NE(alloc.getCacheMemoryStats().slabsApproxFreePercentages[0], 100.0);
      ASSERT_EQ(alloc.getCacheMemoryStats().slabsApproxFreePercentages[1], 100.0);
    }

    config = makeDefaultConfig();
    config.forceAllocationTier = 1;
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      auto pool = alloc.addPool("default", alloc.getCacheMemoryStats().cacheSize);
      auto handle = alloc.allocate(pool, "key", std::string("value").size());
      ASSERT(handle != nullptr);
      ASSERT_EQ(alloc.getCacheMemoryStats().slabsApproxFreePercentages[0], 100.0);
      ASSERT_NE(alloc.getCacheMemoryStats().slabsApproxFreePercentages[1], 100.0);
    }
  }

  void testMultiTiersWatermarkAllocation() {
    auto config = makeDefaultConfig();

    // always allocate in upper tier
    config.maxAcAllocationWatermark = 0.0;
    config.minAcAllocationWatermark = 0.0;

    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      auto pool = alloc.addPool("default", alloc.getCacheMemoryStats().cacheSize);
      auto handle = alloc.allocate(pool, "key", std::string("value").size());
      ASSERT(handle != nullptr);
      ASSERT_NE(alloc.getCacheMemoryStats().slabsApproxFreePercentages[0], 100.0);
      ASSERT_EQ(alloc.getCacheMemoryStats().slabsApproxFreePercentages[1], 100.0);
    }

    // always allocate in lower tier
    config.maxAcAllocationWatermark = 101.0;
    config.minAcAllocationWatermark = 100.0;
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      auto pool = alloc.addPool("default", alloc.getCacheMemoryStats().cacheSize);
      auto handle = alloc.allocate(pool, "key", std::string("value").size());
      ASSERT(handle != nullptr);
      ASSERT_EQ(alloc.getCacheMemoryStats().slabsApproxFreePercentages[0], 100.0);
      ASSERT_NE(alloc.getCacheMemoryStats().slabsApproxFreePercentages[1], 100.0);
    }

    // allocate in tier based on size
    config.maxAcAllocationWatermark = 101.0;
    config.minAcAllocationWatermark = -1.0;
    config.sizeThresholdPolicy = 1000;
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      auto pool = alloc.addPool("default", alloc.getCacheMemoryStats().cacheSize);
      auto handle = alloc.allocate(pool, "key", 100);
      ASSERT(handle != nullptr);

      // item should be allocated in upper tier
      ASSERT_NE(alloc.getCacheMemoryStats().slabsApproxFreePercentages[0], 100.0);
      ASSERT_EQ(alloc.getCacheMemoryStats().slabsApproxFreePercentages[1], 100.0);

      handle = alloc.allocate(pool, "key", 1001);
      ASSERT(handle != nullptr);

      // item should be allocated in lower tier
      ASSERT_NE(alloc.getCacheMemoryStats().slabsApproxFreePercentages[0], 100.0);
      ASSERT_NE(alloc.getCacheMemoryStats().slabsApproxFreePercentages[1], 100.0);
      ASSERT_EQ(alloc.getCacheMemoryStats().slabsApproxFreePercentages[0],
        alloc.getCacheMemoryStats().slabsApproxFreePercentages[1]);
    }
  }
};
} // namespace tests
} // namespace cachelib
} // namespace facebook
