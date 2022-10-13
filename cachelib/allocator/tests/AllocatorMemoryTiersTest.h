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

#include <future>

#include "cachelib/allocator/CacheAllocatorConfig.h"
#include "cachelib/allocator/MemoryTierCacheConfig.h"
#include "cachelib/allocator/tests/TestBase.h"
#include "cachelib/allocator/FreeThresholdStrategy.h"
#include "cachelib/allocator/PromotionStrategy.h"

#include <folly/synchronization/Latch.h>

namespace facebook {
namespace cachelib {
namespace tests {

template <typename AllocatorT>
class AllocatorMemoryTiersTest : public AllocatorTest<AllocatorT> {
 private:
  template<typename MvCallback>
  void testMultiTiersAsyncOpDuringMove(std::unique_ptr<AllocatorT>& alloc,
                                       PoolId& pool, bool& quit, MvCallback&& moveCb) {
    typename AllocatorT::Config config;
    config.setCacheSize(4 * Slab::kSize);
    config.enableCachePersistence("/tmp");
    config.configureMemoryTiers({
        MemoryTierCacheConfig::fromShm()
            .setRatio(1).setMemBind({0}),
        MemoryTierCacheConfig::fromShm()
            .setRatio(1).setMemBind({0})
    });

    config.enableMovingOnSlabRelease(moveCb, {} /* ChainedItemsMoveSync */,
                                     -1 /* movingAttemptsLimit */);

    alloc = std::make_unique<AllocatorT>(AllocatorT::SharedMemNew, config);
    ASSERT(alloc != nullptr);
    pool = alloc->addPool("default", alloc->getCacheMemoryStats().cacheSize);

    int i = 0;
    while(!quit) {
      auto handle = alloc->allocate(pool, std::to_string(++i), std::string("value").size());
      ASSERT(handle != nullptr);
      ASSERT_NO_THROW(alloc->insertOrReplace(handle));
    }
  }
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
    auto stats = allocator->getGlobalCacheStats();
    auto slabStats = allocator->getAllocationClassStats(0,0,cid);
    const auto& mpStats = allocator->getPoolByTid(poolId, 0).getStats(); 
    //cache is 10MB should move about 1MB to reach 10% free
    uint32_t approxEvict = (1024*1024)/mpStats.acStats.at(cid).allocSize;
    while (stats.evictionStats.numMovedItems < approxEvict*0.95 && slabStats.approxFreePercent >= 9.5) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        stats = allocator->getGlobalCacheStats();
        slabStats = allocator->getAllocationClassStats(0,0,cid);
    }

    auto perclassEstats = allocator->getBackgroundMoverClassStats(MoverDir::Evict);
    auto perclassPstats = allocator->getBackgroundMoverClassStats(MoverDir::Promote);

    ASSERT_GE(stats.evictionStats.numMovedItems,1);
    ASSERT_GE(stats.evictionStats.runCount,1);
    ASSERT_GE(stats.promotionStats.numMovedItems,1);
   
    ASSERT_GE(perclassEstats[0][0][cid], 1);
    ASSERT_GE(perclassPstats[1][0][cid], 1);
    
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

  void testMultiTiersRemoveDuringEviction() {
    std::unique_ptr<AllocatorT> alloc;
    PoolId pool;
    std::unique_ptr<std::thread> t;
    folly::Latch latch(1);
    bool quit = false;

    auto moveCb = [&] (typename AllocatorT::Item& oldItem,
                       typename AllocatorT::Item& newItem,
                       typename AllocatorT::Item* /* parentPtr */) {
      
      auto key = oldItem.getKey();
      t = std::make_unique<std::thread>([&](){
            // remove() function is blocked by wait context
            // till item is moved to next tier. So that, we should
            // notify latch before calling remove()
            latch.count_down();
            alloc->remove(key);
          });
      // wait till async thread is running
      latch.wait();
      memcpy(newItem.getMemory(), oldItem.getMemory(), oldItem.getSize());
      quit = true;
    };

    testMultiTiersAsyncOpDuringMove(alloc, pool, quit, moveCb);

    t->join();
  }

  void testMultiTiersReplaceDuringEviction() {
    std::unique_ptr<AllocatorT> alloc;
    PoolId pool;
    std::unique_ptr<std::thread> t;
    folly::Latch latch(1);
    bool quit = false;

    auto moveCb = [&] (typename AllocatorT::Item& oldItem,
                       typename AllocatorT::Item& newItem,
                       typename AllocatorT::Item* /* parentPtr */) {
      auto key = oldItem.getKey();
      if(!quit) {
        // we need to replace only once because subsequent allocate calls
        // will cause evictions recursevly
        quit = true;
        t = std::make_unique<std::thread>([&](){
              auto handle = alloc->allocate(pool, key, std::string("new value").size());
              // insertOrReplace() function is blocked by wait context
              // till item is moved to next tier. So that, we should
              // notify latch before calling insertOrReplace()
              latch.count_down();
              ASSERT_NO_THROW(alloc->insertOrReplace(handle));
            });
        // wait till async thread is running
        latch.wait();
      }
      memcpy(newItem.getMemory(), oldItem.getMemory(), oldItem.getSize());
    };

    testMultiTiersAsyncOpDuringMove(alloc, pool, quit, moveCb);

    t->join();
  }
  
  // This test will first allocate a normal item. And then it will
  // keep allocating new chained items and appending them to the
  // first item until the system runs out of memory.
  // After that it will drop the normal item's handle and try
  // allocating again to verify that it can be evicted.
  void testAddChainedItemUntilEviction() {
    // create an allocator worth 10 slabs.
    typename AllocatorT::Config config;
    config.configureChainedItems();
    config.setCacheSize(10 * Slab::kSize);
    config.enableCachePersistence("/tmp");
    config.usePosixForShm();
    config.configureMemoryTiers({
        MemoryTierCacheConfig::fromShm()
            .setRatio(1),
        MemoryTierCacheConfig::fromFile("/tmp/b" + std::to_string(::getpid()))
            .setRatio(1)
    });
    auto alloc = std::make_unique<AllocatorT>(AllocatorT::SharedMemNew, config);
    const size_t numBytes = alloc->getCacheMemoryStats().cacheSize;
    const auto poolSize = numBytes;

    const auto pid = alloc->addPool("one", poolSize);

    const std::vector<uint32_t> sizes = {100, 500, 1000, 2000, 100000};

    // Allocate chained allocs until allocations fail.
    // It will fail because "hello1" has an outsanding item handle alive,
    // and thus it cannot be evicted, so none of its chained allocations
    // can be evicted either
    auto itemHandle = alloc->allocate(pid, "hello1", sizes[0]);
    uint32_t exhaustedSize = 0;
    for (unsigned int i = 0;; ++i) {
      auto chainedItemHandle =
          alloc->allocateChainedItem(itemHandle, sizes[i % sizes.size()]);
      if (chainedItemHandle == nullptr) {
        exhaustedSize = sizes[i % sizes.size()];
        break;
      }
      alloc->addChainedItem(itemHandle, std::move(chainedItemHandle));
    }

    // Verify some of the items live in tier 1
    uint64_t allocs[2] = { 0, 0 };
    auto headHandle = alloc->findChainedItem(*itemHandle);
    auto* head = &headHandle.get()->asChainedItem();
    while (head) {
        const auto tid = alloc->getTierId(*head);
        allocs[tid]++;
        head = head->getNext(alloc->compressor_);
    }
    ASSERT_GT(allocs[1],10);
    headHandle.reset();
    // Verify we cannot allocate a new item either.
    ASSERT_EQ(nullptr, alloc->allocate(pid, "hello2", exhaustedSize));

    // We should be able to allocate a new item after releasing the old one
    itemHandle.reset();
    ASSERT_NE(nullptr, alloc->allocate(pid, "hello2", exhaustedSize));
    
  }
};
} // namespace tests
} // namespace cachelib
} // namespace facebook
