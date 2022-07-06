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
BackgroundEvictor<CacheT>::BackgroundEvictor(Cache& cache,
                               std::shared_ptr<BackgroundEvictorStrategy> strategy)
    : cache_(cache),
      strategy_(strategy)
{
}

template <typename CacheT>
BackgroundEvictor<CacheT>::~BackgroundEvictor() { stop(std::chrono::seconds(0)); }

template <typename CacheT>
void BackgroundEvictor<CacheT>::work() {
  try {
    checkAndRun();
  } catch (const std::exception& ex) {
    XLOGF(ERR, "BackgroundEvictor interrupted due to exception: {}", ex.what());
  }
}

template <typename CacheT>
void BackgroundEvictor<CacheT>::setAssignedMemory(std::vector<std::tuple<TierId, PoolId, ClassId>> &&assignedMemory)
{
  XLOG(INFO, "Class assigned to background worker:");
  for (auto [tid, pid, cid] : assignedMemory) {
    XLOGF(INFO, "Tid: {}, Pid: {}, Cid: {}", tid, pid, cid);
  }

  mutex.lock_combine([this, &assignedMemory]{
    this->assignedMemory_ = std::move(assignedMemory);
  });
}

// Look for classes that exceed the target memory capacity
// and return those for eviction
template <typename CacheT>
void BackgroundEvictor<CacheT>::checkAndRun() {
  auto assignedMemory = mutex.lock_combine([this]{
    return assignedMemory_;
  });

  unsigned int evictions = 0;
  std::set<ClassId> classes{};
  auto batches = strategy_->calculateBatchSizes(cache_,assignedMemory);

  for (size_t i = 0; i < batches.size(); i++) {
    const auto [tid, pid, cid] = assignedMemory[i];
    const auto batch = batches[i];
  
    classes.insert(cid);
    const auto& mpStats = cache_.getPoolByTid(pid,tid).getStats();

    if (!batch) {
      continue;
    }

    stats.evictionSize.add(batch * mpStats.acStats.at(cid).allocSize);
  
    //try evicting BATCH items from the class in order to reach free target
    auto evicted =
        BackgroundEvictorAPIWrapper<CacheT>::traverseAndEvictItems(cache_,
            tid,pid,cid,batch);
    evictions += evicted;
    evictions_per_class_[tid][pid][cid] += evicted;
  }

  stats.numTraversals.inc();
  stats.numEvictedItems.add(evictions);
  stats.totalClasses.add(classes.size());
}

template <typename CacheT>
BackgroundEvictionStats BackgroundEvictor<CacheT>::getStats() const noexcept {
  BackgroundEvictionStats evicStats;
  evicStats.numEvictedItems = stats.numEvictedItems.get();
  evicStats.runCount = stats.numTraversals.get();
  evicStats.evictionSize = stats.evictionSize.get();
  evicStats.totalClasses = stats.totalClasses.get();

  return evicStats;
}

template <typename CacheT>
std::map<TierId, std::map<PoolId, std::map<ClassId, uint64_t>>>
BackgroundEvictor<CacheT>::getClassStats() const noexcept {
  return evictions_per_class_;
}

} // namespace cachelib
} // namespace facebook
