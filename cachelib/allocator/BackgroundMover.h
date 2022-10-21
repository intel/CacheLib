/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

#include "cachelib/allocator/BackgroundMoverStrategy.h"
#include "cachelib/allocator/CacheStats.h"
#include "cachelib/common/PeriodicWorker.h"

namespace facebook::cachelib {
// wrapper that exposes the private APIs of CacheType that are specifically
// needed for the cache api
template <typename C>
struct BackgroundMoverAPIWrapper {
  static size_t traverseAndEvictItems(C& cache,
                                      unsigned int tid,
                                      unsigned int pid,
                                      unsigned int cid,
                                      size_t batch) {
    return cache.traverseAndEvictItems(tid, pid, cid, batch);
  }

  static size_t traverseAndPromoteItems(C& cache,
                                        unsigned int tid,
                                        unsigned int pid,
                                        unsigned int cid,
                                        size_t batch) {
    return cache.traverseAndPromoteItems(tid, pid, cid, batch);
  }
};

enum class MoverDir { Evict = 0, Promote };

// Periodic worker that evicts items from tiers in batches
// The primary aim is to reduce insertion times for new items in the
// cache
template <typename CacheT>
class BackgroundMover : public PeriodicWorker {
 public:
  using ClassBgStatsType = std::map<MemoryDescriptorType,uint64_t>;
  using Cache = CacheT;
  // @param cache               the cache interface
  // @param strategy            the stragey class that defines how objects are
  // moved (promoted vs. evicted and how much)
  BackgroundMover(Cache& cache,
                  std::shared_ptr<BackgroundMoverStrategy> strategy,
                  MoverDir direction_);

  ~BackgroundMover() override;

  BackgroundMoverStats getStats() const noexcept;
  ClassBgStatsType getClassStats() const noexcept {
    return movesPerClass_;
  }

  void setAssignedMemory(std::vector<MemoryDescriptorType>&& assignedMemory);

  // return id of the worker responsible for promoting/evicting from particlar
  // pool and allocation calss (id is in range [0, numWorkers))
  static size_t workerId(TierId tid, PoolId pid, ClassId cid, size_t numWorkers);

 private:
  ClassBgStatsType movesPerClass_;

  struct TraversalStats {
    // record a traversal and its time taken
    void recordTraversalTime(uint64_t nsTaken);

    uint64_t getAvgTraversalTimeNs(uint64_t numTraversals) const;
    uint64_t getMinTraversalTimeNs() const { return minTraversalTimeNs_; }
    uint64_t getMaxTraversalTimeNs() const { return maxTraversalTimeNs_; }
    uint64_t getLastTraversalTimeNs() const { return lastTraversalTimeNs_; }

   private:
    // time it took us the last time to traverse the cache.
    uint64_t lastTraversalTimeNs_{0};
    uint64_t minTraversalTimeNs_{
        std::numeric_limits<uint64_t>::max()};
    uint64_t maxTraversalTimeNs_{0};
    uint64_t totalTraversalTimeNs_{0};
  };

  TraversalStats traversalStats_;
  // cache allocator's interface for evicting
  using Item = typename Cache::Item;

  Cache& cache_;
  std::shared_ptr<BackgroundMoverStrategy> strategy_;
  MoverDir direction_;

  std::function<size_t(
      Cache&, unsigned int, unsigned int, unsigned int, size_t)>
      moverFunc;

  // implements the actual logic of running the background evictor
  void work() override final;
  void checkAndRun();

  uint64_t numMovedItems{0};
  uint64_t numTraversals{0};
  uint64_t totalClasses{0};
  uint64_t totalBytesMoved{0};

  std::vector<MemoryDescriptorType> assignedMemory_;
  folly::DistributedMutex mutex_;
};

template <typename CacheT>
BackgroundMover<CacheT>::BackgroundMover(
    Cache& cache,
    std::shared_ptr<BackgroundMoverStrategy> strategy,
    MoverDir direction)
    : cache_(cache), strategy_(strategy), direction_(direction) {
  if (direction_ == MoverDir::Evict) {
    moverFunc = BackgroundMoverAPIWrapper<CacheT>::traverseAndEvictItems;

  } else if (direction_ == MoverDir::Promote) {
    moverFunc = BackgroundMoverAPIWrapper<CacheT>::traverseAndPromoteItems;
  }
}

template <typename CacheT>
void BackgroundMover<CacheT>::TraversalStats::recordTraversalTime(uint64_t nsTaken) {
  lastTraversalTimeNs_ = nsTaken;
  minTraversalTimeNs_ = std::min(minTraversalTimeNs_, nsTaken);
  maxTraversalTimeNs_ = std::max(maxTraversalTimeNs_, nsTaken);
  totalTraversalTimeNs_ += nsTaken;
}

template <typename CacheT>
uint64_t BackgroundMover<CacheT>::TraversalStats::getAvgTraversalTimeNs(
    uint64_t numTraversals) const {
  return numTraversals ? totalTraversalTimeNs_ / numTraversals : 0;
}

template <typename CacheT>
BackgroundMover<CacheT>::~BackgroundMover() {
  stop(std::chrono::seconds(0));
}

template <typename CacheT>
void BackgroundMover<CacheT>::work() {
  try {
    checkAndRun();
  } catch (const std::exception& ex) {
    XLOGF(ERR, "BackgroundMover interrupted due to exception: {}", ex.what());
  }
}

template <typename CacheT>
void BackgroundMover<CacheT>::setAssignedMemory(
    std::vector<MemoryDescriptorType>&& assignedMemory) {
  XLOG(INFO, "Class assigned to background worker:");
  for (auto [tid, pid, cid] : assignedMemory) {
    XLOGF(INFO, "Tid: {}, Pid: {}, Cid: {}", tid, pid, cid);
  }

  mutex_.lock_combine([this, &assignedMemory] {
    this->assignedMemory_ = std::move(assignedMemory);
  });
}

// Look for classes that exceed the target memory capacity
// and return those for eviction
template <typename CacheT>
void BackgroundMover<CacheT>::checkAndRun() {
  auto assignedMemory = mutex_.lock_combine([this] { return assignedMemory_; });

  while (true) {
    unsigned int moves = 0;
    std::set<ClassId> classes{};
    auto batches = strategy_->calculateBatchSizes(cache_, assignedMemory);

    const auto begin = util::getCurrentTimeNs();
    for (size_t i = 0; i < batches.size(); i++) {
      const auto [tid, pid, cid] = assignedMemory[i];
      const auto batch = batches[i];
      if (!batch) {
        continue;
      }

      // try moving BATCH items from the class in order to reach free target
      auto moved = moverFunc(cache_, tid, pid, cid, batch);
      moves += moved;
      movesPerClass_[assignedMemory[i]] += moved;
    }
    auto end = util::getCurrentTimeNs();
    if (moves > 0) {
      traversalStats_.recordTraversalTime(end > begin ? end - begin : 0);
      numMovedItems += moves;
      numTraversals++;
    }

    //we didn't move any objects done with this run
    if (moves == 0 || shouldStopWork()) {
        break;
    }
  }
}

template <typename CacheT>
BackgroundMoverStats BackgroundMover<CacheT>::getStats() const noexcept {
  BackgroundMoverStats stats;
  stats.numMovedItems = numMovedItems;
  stats.totalBytesMoved = totalBytesMoved;
  stats.totalClasses = totalClasses;
  auto runCount = getRunCount();
  stats.runCount = runCount;
  stats.numTraversals = numTraversals;
  stats.avgItemsMoved = (double) stats.numMovedItems / (double)runCount;
  stats.lastTraversalTimeNs = traversalStats_.getLastTraversalTimeNs();
  stats.avgTraversalTimeNs = traversalStats_.getAvgTraversalTimeNs(numTraversals);
  stats.minTraversalTimeNs = traversalStats_.getMinTraversalTimeNs();
  stats.maxTraversalTimeNs = traversalStats_.getMaxTraversalTimeNs();

  return stats;
}

template <typename CacheT>
size_t BackgroundMover<CacheT>::workerId(TierId tid,
                                         PoolId pid,
                                         ClassId cid,
                                         size_t numWorkers) {
  XDCHECK(numWorkers);

  // TODO: came up with some better sharding (use hashing?)
  return (tid + pid + cid) % numWorkers;
}
} // namespace facebook::cachelib
