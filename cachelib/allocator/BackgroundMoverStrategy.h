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

#include "cachelib/allocator/Cache.h"


namespace facebook {
namespace cachelib {


// Base class for background eviction strategy.
class BackgroundMoverStrategy {
 public:
  virtual std::vector<size_t> calculateBatchSizes(
      const CacheBase& cache,
      std::vector<MemoryDescriptorType> acVec) = 0;
};

class DefaultBackgroundMoverStrategy : public BackgroundMoverStrategy {
  public:
    DefaultBackgroundMoverStrategy(uint64_t batchSize, double targetFree)
      : batchSize_(batchSize), targetFree_((double)targetFree/100.0) {}
    ~DefaultBackgroundMoverStrategy() {}

  std::vector<size_t> calculateBatchSizes(
      const CacheBase& cache,
      std::vector<MemoryDescriptorType> acVec) {
    std::vector<size_t> batches{};
    for (auto [tid, pid, cid] : acVec) {
        double usage = cache.getPoolByTid(pid, tid).getApproxUsage(cid);
        uint32_t perSlab = cache.getPoolByTid(pid, tid).getPerSlab(cid);
        if (usage >= (1.0-targetFree_)) {
          uint32_t batch = batchSize_ > perSlab ? perSlab : batchSize_;
          batches.push_back(batch);
        } else {
          batches.push_back(0);
        }
    }
    return batches;
  }
  private:
    uint64_t batchSize_{100};
    double targetFree_{0.05};
};

/*
class KeepFreeBackgroundMoverStrategy : public BackgroundMoverStrategy {
  std::vector<size_t> calculateBatchSizes(
      const CacheBase& cache,
      std::vector<MemoryDescriptorType> acVec) {
    std::vector<size_t> batches{};
    for (auto [tid, pid, cid] : acVec) {
        auto stats = cache.getACStats(tid, pid, cid);
        uint32_t toFree = stats.activeAllocsstats.freeAllocs
        batches.push_back(100);
    }
    return batches;
  }
};
*/

} // namespace cachelib
} // namespace facebook
