/*
 * Copyright (c) Intel and its affiliates.
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

namespace facebook {
namespace cachelib {

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
  lastTraversalTimeNs_.store(nsTaken, std::memory_order_relaxed);
  minTraversalTimeNs_.store(std::min(minTraversalTimeNs_.load(), nsTaken),
                            std::memory_order_relaxed);
  maxTraversalTimeNs_.store(std::max(maxTraversalTimeNs_.load(), nsTaken),
                            std::memory_order_relaxed);
  totalTraversalTimeNs_.fetch_add(nsTaken, std::memory_order_relaxed);
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

  mutex.lock_combine([this, &assignedMemory] {
    this->assignedMemory_ = std::move(assignedMemory);
  });
}

// Look for classes that exceed the target memory capacity
// and return those for eviction
template <typename CacheT>
void BackgroundMover<CacheT>::checkAndRun() {
  auto assignedMemory = mutex.lock_combine([this] { return assignedMemory_; });

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
    moves_per_class_[assignedMemory[i]] += moved;
  }
  auto end = util::getCurrentTimeNs();
  if (moves > 0) {
    traversalStats_.recordTraversalTime(end > begin ? end - begin : 0);
    numMovedItems.add(moves);
    numTraversals.inc();
  }

}

template <typename CacheT>
BackgroundMoverStats BackgroundMover<CacheT>::getStats() const noexcept {
  BackgroundMoverStats stats;
  stats.numMovedItems = numMovedItems.get();
  stats.totalBytesMoved = totalBytesMoved.get();
  stats.totalClasses = totalClasses.get();
  auto runCount = getRunCount();
  stats.runCount = runCount;
  stats.numTraversals = numTraversals.get();
  stats.avgItemsMoved = (double) stats.numMovedItems / (double)runCount;
  stats.lastTraversalTimeNs = traversalStats_.getLastTraversalTimeNs();
  stats.avgTraversalTimeNs = traversalStats_.getAvgTraversalTimeNs(runCount);
  stats.minTraversalTimeNs = traversalStats_.getMinTraversalTimeNs();
  stats.maxTraversalTimeNs = traversalStats_.getMaxTraversalTimeNs();

  return stats;
}

template <typename CacheT>
std::map<MemoryDescriptorType,uint64_t>
BackgroundMover<CacheT>::getClassStats() const noexcept {
  return moves_per_class_;
}

} // namespace cachelib
} // namespace facebook
