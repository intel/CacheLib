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

#include "cachelib/allocator/FreeThresholdStrategy.h"

#include <folly/logging/xlog.h>

namespace facebook {
namespace cachelib {

FreeThresholdStrategy::FreeThresholdStrategy(double lowEvictionAcWatermark,
                                             double highEvictionAcWatermark,
                                             uint64_t maxEvictionBatch,
                                             uint64_t minEvictionBatch)
    : lowEvictionAcWatermark(lowEvictionAcWatermark),
      highEvictionAcWatermark(highEvictionAcWatermark),
      maxEvictionBatch(maxEvictionBatch),
      minEvictionBatch(minEvictionBatch) {}

std::vector<size_t> FreeThresholdStrategy::calculateBatchSizes(
    const CacheBase& cache,
    std::vector<MemoryDescriptorType> acVec) {
  std::vector<size_t> batches{};
  for (auto [tid, pid, cid] : acVec) {
    auto stats = cache.getACStats(tid, pid, cid);
    if ((1-stats.usageFraction())*100 >= highEvictionAcWatermark) {
      batches.push_back(0);
    } else {
      auto toFreeMemPercent = highEvictionAcWatermark - (1-stats.usageFraction())*100;
      auto toFreeItems = static_cast<size_t>(
          toFreeMemPercent * (stats.totalSlabs() * Slab::kSize) / stats.allocSize);
      batches.push_back(toFreeItems);
    }
  }

  if (batches.size() == 0) {
    return batches;
  }

  auto maxBatch = *std::max_element(batches.begin(), batches.end());
  if (maxBatch == 0)
    return batches;

  std::transform(
      batches.begin(), batches.end(), batches.begin(), [&](auto numItems) {
        if (numItems == 0) {
          return 0UL;
        }

        auto cappedBatchSize = maxEvictionBatch * numItems / maxBatch;
        if (cappedBatchSize < minEvictionBatch)
          return minEvictionBatch;
        else
          return cappedBatchSize;
      });

  return batches;
}

} // namespace cachelib
} // namespace facebook
