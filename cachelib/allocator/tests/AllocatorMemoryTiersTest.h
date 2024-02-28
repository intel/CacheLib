/*
 * Copyright (c) Meta Platforms, Inc. and its affiliates.
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

#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <semaphore.h>
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
            .setRatio(1).setMemBind(std::string("0")),
        MemoryTierCacheConfig::fromShm()
            .setRatio(1).setMemBind(std::string("0"))
    });

    config.enableMovingOnSlabRelease(moveCb, {} /* ChainedItemsMoveSync */,
                                     -1 /* movingAttemptsLimit */);

    alloc = std::make_unique<AllocatorT>(AllocatorT::SharedMemNew, config);
    ASSERT(alloc != nullptr);
    pool = alloc->addPool("default", alloc->getCacheMemoryStats().ramCacheSize);

    int i = 0;
    while(!quit) {
      auto handle = alloc->allocate(pool, std::to_string(++i), std::string("value").size());
      ASSERT(handle != nullptr);
      ASSERT_NO_THROW(alloc->insertOrReplace(handle));
    }
  }
  
 public:
  void testMultiTiersInvalid() {
    typename AllocatorT::Config config;
    config.setCacheSize(100 * Slab::kSize);
    ASSERT_NO_THROW(config.configureMemoryTiers(
        {MemoryTierCacheConfig::fromShm().setRatio(1).setMemBind(
             std::string("0")),
         MemoryTierCacheConfig::fromShm().setRatio(1).setMemBind(
             std::string("0"))}));
  }

  void testMultiTiersValid() {
    typename AllocatorT::Config config;
    config.setCacheSize(100 * Slab::kSize);
    config.enableCachePersistence("/tmp");
    ASSERT_NO_THROW(config.configureMemoryTiers(
        {MemoryTierCacheConfig::fromShm().setRatio(1).setMemBind(
             std::string("0")),
         MemoryTierCacheConfig::fromShm().setRatio(1).setMemBind(
             std::string("0"))}));

    auto alloc = std::make_unique<AllocatorT>(AllocatorT::SharedMemNew, config);
    ASSERT(alloc != nullptr);

    auto pool = alloc->addPool("default", alloc->getCacheMemoryStats().ramCacheSize);
    auto handle = alloc->allocate(pool, "key", std::string("value").size());
    ASSERT(handle != nullptr);
    ASSERT_NO_THROW(alloc->insertOrReplace(handle));
  }
  
  void testMultiTiersValidStats() {
    typename AllocatorT::Config config;
    size_t nSlabs = 20;
    config.setCacheSize(nSlabs * Slab::kSize);
    config.enableCachePersistence("/tmp");
    ASSERT_NO_THROW(config.configureMemoryTiers(
        {MemoryTierCacheConfig::fromShm().setRatio(1).setMemBind(
             std::string("0")),
         MemoryTierCacheConfig::fromShm().setRatio(2).setMemBind(
             std::string("0"))}));

    auto alloc = std::make_unique<AllocatorT>(AllocatorT::SharedMemNew, config);
    ASSERT(alloc != nullptr);
    size_t keyLen = 8;
    auto pool = alloc->addPool("default", alloc->getCacheMemoryStats().ramCacheSize);
    std::vector<uint32_t> valsize = {1000};
    std::vector<uint32_t> itemCount;
    std::vector<uint32_t> evictCount;
    for (uint32_t tid = 0; tid < 2; tid++) {
        this->fillUpPoolUntilEvictions(*alloc, tid, pool, valsize, keyLen);
        auto stats = alloc->getPoolStats(tid, pool);
        const auto& classIds = stats.mpStats.classIds;
        uint32_t prev = 0;
        ClassId cid = 0;
        for (const ClassId c : classIds) {
            uint32_t currSize = stats.cacheStats[c].allocSize;
            if (prev <= valsize[0] && valsize[0] <= currSize) {
                cid = c;
                break;
            }
            prev = currSize;
        }

        std::cout << "Tid: " << tid << " cid: " << static_cast<uint32_t>(cid)
                  << " items: " << stats.cacheStats[cid].numItems()
                  << " evicts: " << stats.cacheStats[cid].numEvictions()
                  << std::endl;
        ASSERT_GE(stats.cacheStats[cid].numItems(), 1);
        ASSERT_EQ(stats.cacheStats[cid].numEvictions(), 1);
        itemCount.push_back(stats.cacheStats[cid].numItems());
        evictCount.push_back(stats.cacheStats[cid].numEvictions());
        //first tier should have some writebacks to second tier
        //second tier should not have any writebacks since it
        //is last memory tier
        if (tid == 0) {
            ASSERT_EQ(stats.cacheStats[cid].numWritebacks, 1);
        } else {
            ASSERT_EQ(0, stats.cacheStats[cid].numWritebacks);
        }
    }
    for (uint32_t tid = 1; tid < 2; tid++) {
        ASSERT_NE(itemCount[tid],itemCount[tid-1]);
        ASSERT_EQ(evictCount[tid],evictCount[tid-1]);
    }
  }

  void testMultiTiersBackgroundMovers() {
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    config.enableCachePersistence("/tmp");
    config.usePosixForShm();
    config.configureMemoryTiers({
        MemoryTierCacheConfig::fromShm()
            .setRatio(1).setMemBind(std::string("0")),
        MemoryTierCacheConfig::fromShm()
            .setRatio(1).setMemBind(std::string("0"))
    });
    config.enableBackgroundEvictor(std::make_shared<FreeThresholdStrategy>(2, 10, 100, 40),
            std::chrono::milliseconds(10),1);
    config.enableBackgroundPromoter(std::make_shared<PromotionStrategy>(5, 4, 2),
            std::chrono::milliseconds(10),1);

    auto allocator = std::make_unique<AllocatorT>(AllocatorT::SharedMemNew, config);
    ASSERT(allocator != nullptr);
    const size_t numBytes = allocator->getCacheMemoryStats().ramCacheSize;

    auto poolId = allocator->addPool("default", numBytes);

    const unsigned int keyLen = 100;
    const unsigned int size = 100;
    unsigned int allocs = 0;

    //we should work on pool stats because filluppooluntil evictions
    //will finish once we evict an item from tier 0 to tier 1 and
    //there will be unallocated memory left.
    while (allocs < 174760) {
      const auto key = this->getRandomNewKey(*allocator, keyLen);
      ASSERT_EQ(allocator->find(key), nullptr);
      auto handle = util::allocateAccessible(*allocator, poolId, key, size);
      allocs++;
    }
   
    const auto key = this->getRandomNewKey(*allocator, keyLen);
    auto handle = util::allocateAccessible(*allocator, poolId, key, size);
    ASSERT_NE(nullptr, handle);
    const uint8_t cid = allocator->getAllocInfo(handle->getMemory()).classId;
    ASSERT_EQ(cid,5);
    auto stats = allocator->getGlobalCacheStats();
    auto slabStats = allocator->getACStats(0,0,cid);
    const auto& mpStats = allocator->getPoolByTid(poolId, 0).getStats(); 
    //cache is 10MB should move about 1MB to reach 10% free
    uint32_t approxEvict = (1024*1024)/mpStats.acStats.at(cid).allocSize;
    while (stats.evictionStats[0].numMovedItems < approxEvict*0.95 && (1-slabStats.usageFraction()) >= 0.095) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        stats = allocator->getGlobalCacheStats();
        slabStats = allocator->getACStats(0,0,cid);
    }
    ASSERT_GE(1-slabStats.usageFraction(),0.095);

    auto perclassEstats = allocator->getBackgroundMoverClassStats(MoverDir::Evict);
    auto perclassPstats = allocator->getBackgroundMoverClassStats(MoverDir::Promote);

    ASSERT_GE(stats.evictionStats[0].numMovedItems,1);
    ASSERT_GE(stats.evictionStats[0].runCount,1);
    ASSERT_GE(stats.promotionStats[0].numMovedItems,1);
    
    MemoryDescriptorType tier0(0,0,cid);
    MemoryDescriptorType tier1(1,0,cid);
    ASSERT_GE(perclassEstats[tier0], 1);
    ASSERT_GE(perclassPstats[tier1], 1);
    
  }

  void testMultiTiersValidMixed() {
    typename AllocatorT::Config config;
    config.setCacheSize(100 * Slab::kSize);
    config.enableCachePersistence("/tmp");
    ASSERT_NO_THROW(config.configureMemoryTiers(
        {MemoryTierCacheConfig::fromShm().setRatio(1).setMemBind(
             std::string("0")),
         MemoryTierCacheConfig::fromShm().setRatio(1).setMemBind(
             std::string("0"))}));

    auto alloc = std::make_unique<AllocatorT>(AllocatorT::SharedMemNew, config);
    ASSERT(alloc != nullptr);

    auto pool = alloc->addPool("default", alloc->getCacheMemoryStats().ramCacheSize);
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


  void gdb_sync1() {}
  void gdb_sync2() {}
  void gdb_sync3() {}
  using ReadHandle = typename AllocatorT::ReadHandle;
  void testMultiTiersReplaceDuringEvictionWithReader() {
    sem_unlink ("/gdb1_sem");
    sem_t *sem = sem_open ("/gdb1_sem", O_CREAT | O_EXCL, S_IRUSR | S_IWUSR, 0);
    int gdbfd = open("/tmp/gdb1.gdb",O_CREAT | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR);
    char gdbcmds[] = 
                     "set attached=1\n"
                     "break gdb_sync1\n"
                     "break gdb_sync2\n"
                     "break moveRegularItem\n"
                     "c\n"
                     "set scheduler-locking on\n"
                     "thread 1\n"
                     "c\n"
                     "thread 4\n"
                     "c\n"
                     "thread 5\n"
                     "break nativeFutexWaitImpl thread 5\n"
                     "c\n"
                     "thread 4\n"
                     "break nativeFutexWaitImpl thread 4\n"
                     "c\n"
                     "thread 1\n"
                     "break releaseBackToAllocator\n"
                     "c\n"
                     "c\n"
                     "thread 5\n"
                     "c\n"
                     "thread 4\n"
                     "c\n"
                     "thread 1\n"
                     "break gdb_sync3\n"
                     "c\n"
                     "quit\n";
    int ret = write(gdbfd,gdbcmds,strlen(gdbcmds));
    int ppid = getpid(); //parent pid
    //int pid = 0;
    int pid = fork();
    if (pid == 0) {
        sem_wait(sem);
        sem_close(sem);
        sem_unlink("/gdb1_sem");
        char cmdpid[256];
        sprintf(cmdpid,"%d",ppid);
        int f = execlp("gdb","gdb","--pid",cmdpid,"--batch-silent","--command=/tmp/gdb1.gdb",(char*) 0);
        ASSERT(f != -1);
    }
    sem_post(sem);
    //wait for gdb to run
    int attached = 0;
    while (attached == 0);
    
    std::unique_ptr<AllocatorT> alloc;
    PoolId pool;
    bool quit = false;
    
    typename AllocatorT::Config config;
    config.setCacheSize(4 * Slab::kSize);
    config.enableCachePersistence("/tmp");
    config.configureMemoryTiers({
        MemoryTierCacheConfig::fromShm()
            .setRatio(1).setMemBind(std::string("0")),
        MemoryTierCacheConfig::fromShm()
            .setRatio(1).setMemBind(std::string("0"))
    });

    alloc = std::make_unique<AllocatorT>(AllocatorT::SharedMemNew, config);
    ASSERT(alloc != nullptr);
    pool = alloc->addPool("default", alloc->getCacheMemoryStats().ramCacheSize);

    int i = 0;
    typename AllocatorT::Item* evicted;
    std::unique_ptr<std::thread> t;
    std::unique_ptr<std::thread> r;
    while(!quit) {
      auto handle = alloc->allocate(pool, std::to_string(++i), std::string("value").size());
      ASSERT(handle != nullptr);
      if (i == 1) {
          evicted = static_cast<typename AllocatorT::Item*>(handle.get());
          folly::Latch latch_t(1);
          t = std::make_unique<std::thread>([&](){
                auto handleNew = alloc->allocate(pool, std::to_string(1), std::string("new value").size());
                ASSERT(handleNew != nullptr);
                latch_t.count_down();
                //first breakpoint will be this one because 
                //thread 1 still has more items to fill up the
                //cache before an evict is evicted
                gdb_sync1();
                ASSERT(evicted->isMoving());
                //need to suspend thread 1 - who is doing the eviction
                //gdb will do this for us
                folly::Latch latch(1);
                r = std::make_unique<std::thread>([&](){
                    ASSERT(evicted->isMoving());
                    latch.count_down();
                    auto handleEvict = alloc->find(std::to_string(1));
                    //does find block until done moving?? yes
                    while (evicted->isMarkedForEviction()); //move will fail
                    XDCHECK(handleEvict == nullptr) << handleEvict->toString();
                    ASSERT(handleEvict == nullptr);
                });
                latch.wait();
                gdb_sync2();
                alloc->insertOrReplace(handleNew);
                ASSERT(!evicted->isAccessible()); //move failed
                quit = true;
              });
          latch_t.wait();
      }
      ASSERT_NO_THROW(alloc->insertOrReplace(handle));
    }
    t->join();
    r->join();
    gdb_sync3();
  }
};
} // namespace tests
} // namespace cachelib
} // namespace facebook
