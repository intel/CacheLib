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
};
} // namespace tests
} // namespace cachelib
} // namespace facebook
