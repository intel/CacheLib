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

#include "cachelib/allocator/DynamicFreeThresholdStrategy.h"

#include <folly/logging/xlog.h>
#include <std::pair>
#include <std::vector>

namespace facebook {
namespace cachelib {



DynamicFreeThresholdStrategy::DynamicFreeThresholdStrategy(double lowEvictionAcWatermark, double highEvictionAcWatermark,  double highEvictionAcWatermarkPreviousStart, double highEvictionAcWatermarkPreviousEnd, uint64_t evictionHotnessThreshold, double previousBenefitMig, double currentBenefitMig, double toFreeMemPercent)
    : lowEvictionAcWatermark(lowEvictionAcWatermark), highEvictionAcWatermark(highEvictionAcWatermark), highEvictionAcWatermarkPreviousStart(highEvictionAcWatermarkPreviousStart), highEvictionAcWatermarkPreviousEnd(highEvictionAcWatermarkPreviousEnd), evictionHotnessThreshold(evictionHotnessThreshold), previousBenefitMig(previousBenefitMig), currentBenefitMig(currentBenefitMig), toFreeMemPercent(toFreeMemPercent) {}

size_t DynamicFreeThresholdStrategy::calculateBatchSize(const CacheBase& cache,
                                       unsigned int tid,
                                       PoolId pid,
                                       ClassId cid,
                                       size_t allocSize,
                                       size_t acMemorySize) {
  auto acFree = cache.acFreePercentage(tid, pid, cid);
  
  if (toFreeMemPercent < acFree / 2) {
    highEvictionAcWatermark--;
  } else {
    if (currentBenefitMig > previousBenefitMig) {
      if (highEvictionAcWatermarkPreviousEnd > highEvictionAcWatermarkPreviousStart) {
        highEvictionAcWatermark++; //have a dynamic/config param to increase/decrease with (maybe base it on access freq or access stat)
      } else {
        highEvictionAcWatermark--;
      }
    } else {
      if (highEvictionAcWatermarkPreviousEnd < highEvictionAcWatermarkPreviousStart) {
        highEvictionAcWatermark++;
      } else {
        highEvictionAcWatermark--;
      }
    }
  }
  toFreeMemPercent = highEvictionAcWatermark - acFree;
  auto toFreeItems = static_cast<size_t>(toFreeMemPercent * acMemorySize / allocSize);
  
  return toFreeItems;
}

double DynamicFreeThresholdStrategy::calculateBenefitMig() {
    previousBenefitMig = currentBenefitMig;
    //TODO: currentBenefitMig = ???
}

} // namespace cachelib
} // namespace facebook
