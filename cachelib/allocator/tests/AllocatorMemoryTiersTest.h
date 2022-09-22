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
    config.setCacheSize(10 * Slab::kSize);
    config.enableCachePersistence("/tmp");
    config.usePosixForShm();
    config.configureMemoryTiers({
        MemoryTierCacheConfig::fromShm()
            .setRatio(1),
        MemoryTierCacheConfig::fromFile("/tmp/b" + std::to_string(::getpid()))
            .setRatio(1)
    });
    config.enableBackgroundEvictor(std::make_shared<FreeThresholdStrategy>(2, 10, 100, 40),
            std::chrono::milliseconds(10),1);
    config.enableBackgroundPromoter(std::make_shared<PromotionStrategy>(5, 4, 2),
            std::chrono::milliseconds(10),1);

    auto allocator = std::make_unique<AllocatorT>(AllocatorT::SharedMemNew, config);
    ASSERT(allocator != nullptr);
    const size_t numBytes = allocator->getCacheMemoryStats().cacheSize;

    auto poolId = allocator->addPool("default", numBytes);

    const unsigned int keyLen = 100;
    std::vector<uint32_t> sizes = {100};
    this->fillUpPoolUntilEvictions(*allocator, poolId, sizes, keyLen);
    
    const auto key = this->getRandomNewKey(*allocator, keyLen);
    auto handle = util::allocateAccessible(*allocator, poolId, key, sizes[0]);
    ASSERT_NE(nullptr, handle);
    const uint8_t cid = allocator->getAllocInfo(handle->getMemory()).classId;

    //wait for bg movers
    std::this_thread::sleep_for(std::chrono::seconds(1));

    auto stats = allocator->getGlobalCacheStats();
    auto perclassEstats = allocator->getBackgroundMoverClassStats(MoverDir::Evict);
    auto perclassPstats = allocator->getBackgroundMoverClassStats(MoverDir::Promote);

    EXPECT_GT(stats.evictionStats.numMovedItems,1);
    EXPECT_GT(stats.evictionStats.runCount,1);
    EXPECT_GT(stats.promotionStats.numMovedItems,1);
   
    EXPECT_GT(perclassEstats[0][0][cid], 1);
    EXPECT_GT(perclassPstats[1][0][cid], 1);
    
    auto slabStats = allocator->getAllocationClassStats(0,0,cid);

    ASSERT_GE(slabStats.approxFreePercent,10);
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
