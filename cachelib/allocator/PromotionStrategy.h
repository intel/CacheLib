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
#include "cachelib/allocator/BackgroundEvictorStrategy.h"

namespace facebook {
namespace cachelib {


// Base class for background eviction strategy.
class PromotionStrategy : public BackgroundEvictorStrategy {

public:
  PromotionStrategy(uint64_t promotionAcWatermark, uint64_t maxPromotionBatch, uint64_t minPromotionBatch):
  promotionAcWatermark(promotionAcWatermark), maxPromotionBatch(maxPromotionBatch), minPromotionBatch(minPromotionBatch)
  {

  }
  ~PromotionStrategy() {}

  std::vector<size_t> calculateBatchSizes(const CacheBase& cache,
                                       std::vector<std::tuple<TierId, PoolId, ClassId>> acVec) {
    std::vector<size_t> batches{};
    for (auto [tid, pid, cid] : acVec) {
      XDCHECK(tid > 0);
      auto stats = cache.getAllocationClassStats(tid - 1, pid, cid);
      if (stats.approxFreePercent < promotionAcWatermark)
        batches.push_back(0);
      else {
        auto maxPossibleItemsToPromote = static_cast<size_t>((promotionAcWatermark - stats.approxFreePercent) *
          stats.memorySize / stats.allocSize);
        batches.push_back(maxPossibleItemsToPromote);
      }
    }

   if (batches.size() == 0) {
     return batches;
   }

    auto maxBatch = *std::max_element(batches.begin(), batches.end());
    if (maxBatch == 0)
      return batches;

    std::transform(batches.begin(), batches.end(), batches.begin(), [&](auto numItems){
      if (numItems == 0) {
        return 0UL;
      }

      auto cappedBatchSize = maxPromotionBatch * numItems / maxBatch;
      if (cappedBatchSize < minPromotionBatch)
        return minPromotionBatch;
      else
        return cappedBatchSize;
    });

    return batches;
  }
private:
  double promotionAcWatermark{4.0};
  uint64_t maxPromotionBatch{40};
  uint64_t minPromotionBatch{5};
};

} // namespace cachelib
} // namespace facebook
