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
#include "cachelib/allocator/FreeThresholdStrategy.h"
#include "cachelib/allocator/PromotionStrategy.h"

namespace facebook {
namespace cachelib {
namespace tests {

template <typename AllocatorT>
class AllocatorMemoryTiersTest : public AllocatorTest<AllocatorT> {
 public:
  void testMultiTiersFormFileInvalid() {
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

  void testMultiTiersFromFileValid() {
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
  
  void testMultiTiersBackgroundMovers() {
    typename AllocatorT::Config config;
    config.setCacheSize(4 * Slab::kSize);
    config.enableCachePersistence("/tmp");
    config.usePosixForShm();
    config.configureMemoryTiers({
        MemoryTierCacheConfig::fromShm()
            .setRatio(1),
        MemoryTierCacheConfig::fromFile("/tmp/b" + std::to_string(::getpid()))
            .setRatio(1)
    });
    config.enableBackgroundEvictor(std::make_shared<FreeThresholdStrategy>(10, 20, 4, 2),
            std::chrono::milliseconds(10),1);
    config.enableBackgroundPromoter(std::make_shared<PromotionStrategy>(5, 4, 2),
            std::chrono::milliseconds(10),1);

    auto allocator = std::make_unique<AllocatorT>(AllocatorT::SharedMemNew, config);
    ASSERT(allocator != nullptr);

    const size_t numBytes = allocator->getCacheMemoryStats().cacheSize;
    const size_t kItemSize = 100;
    auto poolId = allocator->addPool("default", numBytes);

    const int numItems = 10000;
    
    int numAllocatedItems = 0;
    for (unsigned int i = 0; i < numItems; i++) {
      auto handle = util::allocateAccessible(
          *allocator, poolId, folly::to<std::string>(i), kItemSize, 0);
      ++numAllocatedItems;
    }

    ASSERT_GT(numAllocatedItems, 0);

    const unsigned int keyLen = 100;
    const unsigned int nSizes = 10;
    const auto sizes =
        this->getValidAllocSizes(*allocator, poolId, nSizes, keyLen);
    this->fillUpPoolUntilEvictions(*allocator, poolId, sizes, keyLen);

    auto stats = allocator->getGlobalCacheStats();
    auto perclassEstats = allocator->getBackgroundMoverClassStats(MoverDir::Evict);
    auto perclassPstats = allocator->getBackgroundMoverClassStats(MoverDir::Promote);

    EXPECT_GT(1, stats.evictionStats.numMovedItems);
    EXPECT_GT(1, stats.promotionStats.numMovedItems);
   
    auto cid = 2;
    EXPECT_GT(1, perclassEstats[0][0][cid]);
    EXPECT_GT(1, perclassPstats[1][0][cid]);
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

  void testMultiTiersNumaBindingsSysVValid() {
    typename AllocatorT::Config config;
    config.setCacheSize(100 * Slab::kSize);
    config.enableCachePersistence("/tmp");
    config.configureMemoryTiers({
        MemoryTierCacheConfig::fromShm()
            .setRatio(1).setMemBind({0}),
        MemoryTierCacheConfig::fromShm()
            .setRatio(1).setMemBind({0})
    });

    auto alloc = std::make_unique<AllocatorT>(AllocatorT::SharedMemNew, config);
    ASSERT(alloc != nullptr);

    auto pool = alloc->addPool("default", alloc->getCacheMemoryStats().cacheSize);
    auto handle = alloc->allocate(pool, "key", std::string("value").size());
    ASSERT(handle != nullptr);
    ASSERT_NO_THROW(alloc->insertOrReplace(handle));
  }

  void testMultiTiersNumaBindingsPosixValid() {
    typename AllocatorT::Config config;
    config.setCacheSize(100 * Slab::kSize);
    config.enableCachePersistence("/tmp");
    config.usePosixForShm();
    config.configureMemoryTiers({
        MemoryTierCacheConfig::fromShm()
            .setRatio(1).setMemBind({0}),
        MemoryTierCacheConfig::fromShm()
            .setRatio(1).setMemBind({0})
    });

    auto alloc = std::make_unique<AllocatorT>(AllocatorT::SharedMemNew, config);
    ASSERT(alloc != nullptr);

    auto pool = alloc->addPool("default", alloc->getCacheMemoryStats().cacheSize);
    auto handle = alloc->allocate(pool, "key", std::string("value").size());
    ASSERT(handle != nullptr);
    ASSERT_NO_THROW(alloc->insertOrReplace(handle));
  }
};
} // namespace tests
} // namespace cachelib
} // namespace facebook
