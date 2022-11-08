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

// class-specific dynamic free (high) threshold strategy

#include "cachelib/allocator/DynamicFreeThresholdStrategy.h"

#include <folly/logging/xlog.h>
#include <stdlib.h>

#include <cmath>
#include <fstream>
#include <random>

#include "cachelib/allocator/Cache.h"
#include "cachelib/allocator/CacheStats.h"
#include "cachelib/allocator/memory/MemoryAllocator.h"
#include "cachelib/allocator/memory/MemoryPoolManager.h"
namespace facebook {
namespace cachelib {

DynamicFreeThresholdStrategy::DynamicFreeThresholdStrategy(
    double lowEvictionAcWatermark,
    double highEvictionAcWatermark,
    uint64_t maxEvictionBatch,
    uint64_t minEvictionBatch,
    double highEvictionDelt)
    : lowEvictionAcWatermark(lowEvictionAcWatermark),
      highEvictionAcWatermark(highEvictionAcWatermark),
      maxEvictionBatch(maxEvictionBatch),
      minEvictionBatch(minEvictionBatch),
      highEvictionDelta(highEvictionDelta),
      highEvictionAcWatermarks(
          CacheBase::kMaxTiers,
          std::vector<std::vector<std::vector<double>>>(
              MemoryPoolManager::kMaxPools,
              std::vector<std::vector<double>>(
                  MemoryAllocator::kMaxClasses,
                  std::vector<double>(
                      3,
                      highEvictionAcWatermark)))), // initialize
                                                   // highEvictionAcWatermarks

      acLatencies(
          CacheBase::kMaxTiers,
          std::vector<std::vector<std::vector<double>>>(
              MemoryPoolManager::kMaxPools,
              std::vector<std::vector<double>>(MemoryAllocator::kMaxClasses,
                                               std::vector<double>(2, 0.0)))),
      acToFreeMemPercents(
          CacheBase::kMaxTiers,
          std::vector<std::vector<std::vector<double>>>(
              MemoryPoolManager::kMaxPools,
              std::vector<std::vector<double>>(MemoryAllocator::kMaxClasses,
                                               std::vector<double>(2, 0.0)))) {
} // initialize acToFreeMemPercents

std::vector<size_t> DynamicFreeThresholdStrategy::calculateBatchSizes(
    const CacheBase& cache, std::vector<MemoryDescriptorType> acVec) {
  std::vector<size_t> batches{}; // contain number of items to evict for ac
                                 // classes in the batch

  for (auto [tid, pid, cid] : acVec) {
    auto stats = cache.getAllocationClassStats(tid, pid, cid); // ac class stats
    auto acFree =
        stats.approxFreePercent; // amount of free memory in the ac class

    if (acFree >= lowEvictionAcWatermark) { // if the amount of free memory in
                                            // the ac class is above
                                            // lowEvictionAcWatermark,
      batches.push_back(0);                 // we do not evict
    } else {
      auto acHighThresholdAtI =
          highEvictionAcWatermarks[tid][pid][cid][0]; // current high threshold
      auto acHighThresholdAtINew =
          highEvictionAcWatermarks[tid][pid][cid][0]; // new high threshold,
                                                      // will be adjusted
      auto acHighThresholdAtIMinus1 =
          highEvictionAcWatermarks[tid][pid][cid][1]; // previous high threshold
      auto acHighThresholdAtIMinus2 =
          highEvictionAcWatermarks[tid][pid][cid][2]; // previous of previous
                                                      // high threshold
      auto toFreeMemPercentAtI = acToFreeMemPercents[tid][pid][cid][0];
      auto toFreeMemPercentAtIMinus1 =
          acToFreeMemPercents[tid][pid][cid][1]; // previous amount of memory to
                                                 // free up in the ac class
      auto acAllocLatencyNs =
          cache.getAllocationClassStats(tid, pid, cid)
              .allocLatencyNs.estimate(); // moving avg latency estimation for
                                          // ac class

      calculateLatency(acAllocLatencyNs, tid, pid, cid);

      std::default_random_engine generator;
      std::normal_distribution<double> distribution(1);
      double number = distribution(generator);
      acHighThresholdAtINew += number * highEvictionDelta;
      std::normal_distribution<double> randDistribution(0.0, 1.0);
      double randoNum = randDistribution(generator);

      if (acLatencies[tid][pid][cid][0] <
          acLatencies[tid][pid][cid][1]) { // if current latency reduced
        // then increase threshold to increase eviction
        // acHighThresholdAtINew+= number*highEvictionDelta;
        highEvictionAcWatermarks[tid][pid][cid][0] = acHighThresholdAtINew;
        highEvictionAcWatermarks[tid][pid][cid][1] = acHighThresholdAtI;
        highEvictionAcWatermarks[tid][pid][cid][2] = acHighThresholdAtIMinus1;
      }
      // rand boundary,
      float diff =
          acLatencies[tid][pid][cid][0] - acLatencies[tid][pid][cid][1];
      // calculate metropolis acceptance criterion
      auto t = initial_latency / ((float)tid + 1);
      float metropolis = exp(-abs(diff) / t);
      if (diff < 0 || randoNum < metropolis) {
        // acHighThresholdAtINew-= number*highEvictionDelta;
        highEvictionAcWatermarks[tid][pid][cid][0] = acHighThresholdAtINew;
        highEvictionAcWatermarks[tid][pid][cid][1] = acHighThresholdAtI;
        highEvictionAcWatermarks[tid][pid][cid][2] = acHighThresholdAtIMinus1;
      }

      acHighThresholdAtINew =
          std::max(acHighThresholdAtINew,
                   lowEvictionAcWatermark); // high threshold cannot be less
                                            // than the low threshold
      auto CurrentItemsPercent = 100 - acFree;
      auto toFreeMemPercent = CurrentItemsPercent - acHighThresholdAtINew;

      acToFreeMemPercents[tid][pid][cid][1] =
          toFreeMemPercentAtI; // update acToFreeMemPercents
      acToFreeMemPercents[tid][pid][cid][0] =
          toFreeMemPercent; // update acToFreeMemPercents

      auto toFreeItems =
          static_cast<size_t>(toFreeMemPercent * stats.memorySize /
                              stats.allocSize); // calculate number of items to
                                                // evict for current ac class
      batches.push_back(toFreeItems);           // append batches
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

void DynamicFreeThresholdStrategy::calculateLatency(uint64_t acLatency,
                                                    unsigned int tid,
                                                    PoolId pid,
                                                    ClassId cid) {
  auto best_latency = acLatencies[tid][pid][cid][0];
  acLatencies[tid][pid][cid][1] = best_latency;
  acLatencies[tid][pid][cid][0] = acLatency;
}
} // namespace cachelib
} // namespace facebook
