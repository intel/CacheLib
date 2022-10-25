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

#include "cachelib/allocator/BackgroundMoverStrategy.h"
#include "cachelib/allocator/Cache.h"
#include "cachelib/allocator/CacheStats.h"
#include <vector>

namespace facebook {
namespace cachelib {


// Base class for background eviction strategy.
class DynamicFreeThresholdStrategy : public BackgroundMoverStrategy {

public:
  DynamicFreeThresholdStrategy(double lowEvictionAcWatermark, double highEvictionAcWatermark, uint64_t maxEvictionBatch, uint64_t minEvictionBatch, double highEvictionDelta);
  ~DynamicFreeThresholdStrategy() {}

  std::vector<size_t> calculateBatchSizes(const CacheBase& cache, std::vector<std::tuple<TierId, PoolId, ClassId>> acVecs); //function to calculate number of items to evict for alloc. classes in the batch

  //BackgroundStrategyStats getStats();

private:
  double lowEvictionAcWatermark{2.0}; //threshold to activate eviction in a certain alloc. class
                                      //for now: static threshold, same for all alloc. classes, TODO: implement dynamic low threshold
  double highEvictionAcWatermark{5.0}; //threshold to calculate number of items to evict from a certain alloc. class
                                       //this threshold is adjusted internally, individually for each ac class
  uint64_t maxEvictionBatch{0};
  uint64_t minEvictionBatch{0};
  double highEvictionDelta{0.3}; //step size of hill-climbing algorithm
                                 //TODO: tune this param, use access freq or other access stat as basis, perhaps use the benefit function to adjust this param (binned)?
  double acLatencyDelta{0.1};
  double initial_latency {100.0};
  double alpha {0.99};
  std::vector<std::vector<std::vector<std::vector<double>>>> highEvictionAcWatermarks; //individual dynamic thresholds for each ac class
  //index 0 for i-th window
  //index 1 for i-1 window
  //index 2 for i-2 window

  //std::vector<std::vector<std::vector<std::vector<double>>>> acBenefits; //benefit value for each ac class, for now: derived from moving avg alloc. latency
  std::vector<std::vector<std::vector<uint8_t>>> acLatencyInvariantRun;  //tracking the runlength of latency invariance for each ac class
  std::vector<std::vector<std::vector<std::vector<double>>>> acLatencies;  //benefit value for each ac class, for now: derived from moving avg alloc. latency
  
  //index 0 for current benefit (i-th window)
  //index 1 for previous benefit (i-1 window)
  
  std::vector<std::vector<std::vector<std::vector<double>>>> acToFreeMemPercents; //amount of memory to free up for each ac class (logging it for comparison purposes)

  //index 0 for current toFreeMemPercent (i-th window)
  //index 1 for previous toFreeMemPercent (i-1 window)

private:
  //void calculateBenefitMig(uint64_t p99, unsigned int tid, PoolId pid, ClassId cid); //function to calculate the benefit of eviction for a certain ac class
  void calculateLatency(uint64_t p99, unsigned int tid, PoolId pid, ClassId cid);
};

} // namespace cachelib
} // namespace facebook
