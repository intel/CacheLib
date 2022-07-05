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
BackgroundPromoter<CacheT>::BackgroundPromoter(Cache& cache,
                               std::shared_ptr<BackgroundEvictorStrategy> strategy)
    : cache_(cache),
      strategy_(strategy)
{
}

template <typename CacheT>
BackgroundPromoter<CacheT>::~BackgroundPromoter() { stop(std::chrono::seconds(0)); }

template <typename CacheT>
void BackgroundPromoter<CacheT>::work() {
  try {
    checkAndRun();
  } catch (const std::exception& ex) {
    XLOGF(ERR, "BackgroundPromoter interrupted due to exception: {}", ex.what());
  }
}

template <typename CacheT>
void BackgroundPromoter<CacheT>::setAssignedMemory(std::vector<std::tuple<TierId, PoolId, ClassId>> &&assignedMemory)
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
void BackgroundPromoter<CacheT>::checkAndRun() {
  auto assignedMemory = mutex.lock_combine([this]{
    return assignedMemory_;
  });

  unsigned int promotions = 0;
  std::set<ClassId> classes{};

  for (const auto [tid, pid, cid] : assignedMemory) {
    classes.insert(cid);
    const auto& mpStats = cache_.getPoolByTid(pid,tid).getStats();
    auto batch = strategy_->calculateBatchSize(cache_,tid,pid,cid, cache_.acAllocSize(tid, pid, cid), cache_.acMemorySize(tid, pid, cid));
    if (!batch) {
      continue;
    }

    // stats.promotionsize.add(batch * mpStats.acStats.at(cid).allocSize);
  
    //try evicting BATCH items from the class in order to reach free target
    auto promoted =
        BackgroundPromoterAPIWrapper<CacheT>::traverseAndPromoteItems(cache_,
            tid,pid,cid,batch);
    promotions += promoted;

    //const size_t cid_id = (size_t)mpStats.acStats.at(cid).allocSize;
    auto it = promotions_per_class_.find(cid);
    if (it != promotions_per_class_.end()) {
        it->second += promoted;
    } else if (promoted > 0) {
        promotions_per_class_[cid] = promoted;
    }
  }

  stats.numTraversals.inc();
  stats.numPromotedItems.add(promotions);
  // stats.totalClasses.add(classes.size());
}

template <typename CacheT>
 BackgroundPromotionStats BackgroundPromoter<CacheT>::getStats() const noexcept {
   BackgroundPromotionStats promoStats;
   promoStats.numPromotedItems = stats.numPromotedItems.get();
   promoStats.runCount = stats.numTraversals.get();

   return promoStats;
 }

 template <typename CacheT>
 std::map<uint32_t,uint64_t> BackgroundPromoter<CacheT>::getClassStats() const noexcept {
   return promotions_per_class_;
 }

} // namespace cachelib
} // namespace facebook
