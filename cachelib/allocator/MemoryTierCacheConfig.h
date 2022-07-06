/*
 * Copyright (c) Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <string>

#include "cachelib/shm/ShmCommon.h"

namespace facebook {
namespace cachelib {
class MemoryTierCacheConfig {
public:
  // Creates instance of MemoryTierCacheConfig for file-backed memory.
  // @param path to file which CacheLib will use to map memory from.
  // TODO: add fromDirectory, fromAnonymousMemory
  static MemoryTierCacheConfig fromFile(const std::string& _file) {
    MemoryTierCacheConfig config;
    config.shmOpts = FileShmSegmentOpts(_file);
    return config;
  }

  // Creates instance of MemoryTierCacheConfig for Posix/SysV Shared memory.
  static MemoryTierCacheConfig fromShm() {
    MemoryTierCacheConfig config;
    config.shmOpts = PosixSysVSegmentOpts();
    return config;
  }

  // Specifies ratio of this memory tier to other tiers. Absolute size
  // of each tier can be calculated as:
  // cacheSize * tierRatio / Sum of ratios for all tiers.
  MemoryTierCacheConfig& setRatio(size_t _ratio) {
    if (!_ratio) {
      throw std::invalid_argument("Tier ratio must be an integer number >=1.");
    }
    ratio = _ratio;
    return *this;
  }

  size_t getRatio() const noexcept { return ratio; }

  const ShmTypeOpts& getShmTypeOpts() const noexcept { return shmOpts; }

  size_t calculateTierSize(size_t totalCacheSize, size_t partitionNum) const {
    if (!partitionNum) {
      throw std::invalid_argument(
          "The total number of tier ratios must be an integer number >=1.");
    }

    if (partitionNum > totalCacheSize) {
      throw std::invalid_argument(
          "Ratio must be less or equal to total cache size.");
    }

    return static_cast<size_t>(getRatio() * (static_cast<double>(totalCacheSize) / partitionNum));
  }

    // TODO: move it to MMContainer config
  double markUsefulChance{100.0}; // call mark useful only with this
  uint8_t lruInsertionPointSpec{0}; // look at LRU/LRU2Q description (possible values vary)

private:
  // Ratio is a number of parts of the total cache size to be allocated for this
  // tier. E.g. if X is a total cache size, Yi are ratios specified for memory
  // tiers, and Y is the sum of all Yi, then size of the i-th tier
  // Xi = (X / Y) * Yi. For examle, to configure 2-tier cache where each
  // tier is a half of the total cache size, set both tiers' ratios to 1.
  size_t ratio{1};

  // Options specific to shm type
  ShmTypeOpts shmOpts;

  MemoryTierCacheConfig() = default;
};
} // namespace cachelib
} // namespace facebook
