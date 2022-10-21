/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
#include <folly/Benchmark.h>
#include <gflags/gflags.h>

#include "cachelib/common/PercentileStats.h"

DECLARE_bool(report_api_latency);
DECLARE_string(report_ac_memory_usage_stats);

namespace facebook {
namespace cachelib {
namespace cachebench {


struct Stats {
  std::vector<BackgroundMoverStats> backgroundEvictorStats;
  std::vector<BackgroundMoverStats> backgroundPromoStats;
  ReaperStats reaperStats;

  std::vector<uint64_t> numEvictions;
  std::vector<uint64_t> numWritebacks;
  std::vector<uint64_t> numCacheHits;
  std::vector<uint64_t> numItems;

  std::vector<uint64_t> evictAttempts{0};
  std::vector<uint64_t> allocAttempts{0};
  std::vector<uint64_t> allocFailures{0};

  std::map<TierId,std::vector<double>> poolUsageFraction;

  uint64_t numCacheGets{0};
  uint64_t numCacheGetMiss{0};
  uint64_t numCacheEvictions{0};
  uint64_t numRamDestructorCalls{0};
  uint64_t numNvmGets{0};
  uint64_t numNvmGetMiss{0};
  uint64_t numNvmGetCoalesced{0};

  uint64_t numNvmItems{0};
  uint64_t numNvmPuts{0};
  uint64_t numNvmPutErrs{0};
  uint64_t numNvmAbortedPutOnTombstone{0};
  uint64_t numNvmAbortedPutOnInflightGet{0};
  uint64_t numNvmPutFromClean{0};
  uint64_t numNvmUncleanEvict{0};
  uint64_t numNvmCleanEvict{0};
  uint64_t numNvmCleanDoubleEvict{0};
  uint64_t numNvmDestructorCalls{0};
  uint64_t numNvmEvictions{0};
  uint64_t numNvmBytesWritten{0};
  uint64_t numNvmNandBytesWritten{0};
  uint64_t numNvmLogicalBytesWritten{0};

  uint64_t numNvmItemRemovedSetSize{0};

  util::PercentileStats::Estimates cacheAllocateLatencyNs;
  util::PercentileStats::Estimates cacheFindLatencyNs;

  double nvmReadLatencyMicrosP50{0};
  double nvmReadLatencyMicrosP90{0};
  double nvmReadLatencyMicrosP99{0};
  double nvmReadLatencyMicrosP999{0};
  double nvmReadLatencyMicrosP9999{0};
  double nvmReadLatencyMicrosP99999{0};
  double nvmReadLatencyMicrosP999999{0};
  double nvmReadLatencyMicrosP100{0};
  double nvmWriteLatencyMicrosP50{0};
  double nvmWriteLatencyMicrosP90{0};
  double nvmWriteLatencyMicrosP99{0};
  double nvmWriteLatencyMicrosP999{0};
  double nvmWriteLatencyMicrosP9999{0};
  double nvmWriteLatencyMicrosP99999{0};
  double nvmWriteLatencyMicrosP999999{0};
  double nvmWriteLatencyMicrosP100{0};

  uint64_t numNvmExceededMaxRetry{0};

  uint64_t numNvmDeletes{0};
  uint64_t numNvmSkippedDeletes{0};

  uint64_t slabsReleased{0};
  uint64_t numAbortedSlabReleases{0};
  uint64_t numReaperSkippedSlabs{0};
  uint64_t moveAttemptsForSlabRelease{0};
  uint64_t moveSuccessesForSlabRelease{0};
  uint64_t evictionAttemptsForSlabRelease{0};
  uint64_t evictionSuccessesForSlabRelease{0};
  uint64_t numNvmRejectsByExpiry{0};
  uint64_t numNvmRejectsByClean{0};

  uint64_t inconsistencyCount{0};
  bool isNvmCacheDisabled{false};
  uint64_t invalidDestructorCount{0};
  int64_t unDestructedItemCount{0};

  std::map<MemoryDescriptorType, ACStats> allocationClassStats;

  // populate the counters related to nvm usage. Cache implementation can decide
  // what to populate since not all of those are interesting when running
  // cachebench.
  std::unordered_map<std::string, double> nvmCounters;
  
  using ClassBgStatsType = std::map<MemoryDescriptorType,uint64_t>;

  ClassBgStatsType backgroundEvictionClasses;
  ClassBgStatsType backgroundPromotionClasses;

  // errors from the nvm engine.
  std::unordered_map<std::string, double> nvmErrors;

  void render(std::ostream& out) const {
    auto totalMisses = getTotalMisses();
    const double overallHitRatio = invertPctFn(totalMisses, numCacheGets);
    const auto nTiers = numItems.size();
    for (TierId tid = 0; tid < nTiers; tid++) {
        out << folly::sformat("Items in Tier {}  : {:,}", tid, numItems[tid]) << std::endl;
    }
    out << folly::sformat("Items in NVM    : {:,}", numNvmItems) << std::endl;
    for (TierId tid = 0; tid < nTiers; tid++) {
        out << folly::sformat("Tier {} Alloc Attempts: {:,}\n"
                              "Tier {} Alloc Success: {:.2f}%",
                              tid, allocAttempts[tid],
                              tid, invertPctFn(allocFailures[tid], allocAttempts[tid]))
            << std::endl;
    }
    for (TierId tid = 0; tid < nTiers; tid++) {
        out << folly::sformat(
                   "Tier {} Evict Attempts: {:,}\n"
                   "Tier {} Success: {:.2f}%",
                   tid, evictAttempts[tid],
                   tid, invertPctFn(evictAttempts[tid] - numEvictions[tid], evictAttempts[tid]))
            << std::endl;
    }
    for (TierId tid = 0; tid < nTiers; tid++) {
        out << folly::sformat("Tier {} Evictions: {:,}\n"
                              "Tier {} Writebacks: {:,}\n"
                              "Tier {} Success: {:.2f}%",
                              tid, numEvictions[tid],
                              tid, numWritebacks[tid],
                              tid, invertPctFn(numEvictions[tid] - numWritebacks[tid], numEvictions[tid]))
            << std::endl;
    }
    
    auto foreachAC = [&](auto &map, auto cb) {
      for (const auto& [key, value] : map) {
        auto [tid,pid,cid] = key;
        cb(tid, pid, cid, value);
      }
    };

    for (auto entry : poolUsageFraction) {
        auto tid = entry.first;
        auto usageFraction = entry.second;
        for (auto pid = 0U; pid < usageFraction.size(); pid++) {
          out << folly::sformat("Tier {} fraction of pool {:,} used : {:.2f}",
                                tid,
                                pid,
                                usageFraction[pid])
              << std::endl;
        }
    }

    if (FLAGS_report_ac_memory_usage_stats != "") {
      auto formatMemory = [&](size_t bytes) -> std::tuple<std::string, double> {
        if (FLAGS_report_ac_memory_usage_stats == "raw") {
          return {"B", bytes};
        }

        constexpr double KB = 1024.0;
        constexpr double MB = 1024.0 * 1024;
        constexpr double GB = 1024.0 * 1024 * 1024;

        if (bytes >= GB) {
          return {"GB", static_cast<double>(bytes) / GB};
        } else if (bytes >= MB) {
          return {"MB", static_cast<double>(bytes) / MB};
        } else if (bytes >= KB) {
          return {"KB", static_cast<double>(bytes) / KB};
        } else {
          return {"B", bytes};
        }
      };

      auto foreachAC = [&](auto cb) {
        for (const auto& [key, value] : allocationClassStats) {
          auto [tid,pid,cid] = key;
          cb(tid, pid, cid, value);
        }
      };
 
      foreachAC([&](auto tid, auto pid, auto cid, auto stats) {
        auto [allocSizeSuffix, allocSize] = formatMemory(stats.allocSize);
        auto [memorySizeSuffix, memorySize] =
            formatMemory(stats.totalAllocatedSize());

        // If the pool is not full, extrapolate usageFraction for AC assuming it
        // will grow at the same rate. This value will be the same for all ACs.
        if (memorySize > 0) {
          const auto acUsageFraction = stats.approxUsage();
          out << folly::sformat(
                   "tid{:2} pid{:2} cid{:4} {:8.2f}{} usage fraction: {:4.2f}\n"
                   "tid{:2} pid{:2} cid{:4} {:8.2f}{} memory size in {}: {:8.2f}\n"
                   "tid{:2} pid{:2} cid{:4} {:8.2f}{} eviction success: {:4.2f}\n"
                   "tid{:2} pid{:2} cid{:4} {:8.2f}{} rolling avg alloc latency in ns: {:8.2f}",
                   tid, pid, cid, allocSize, allocSizeSuffix, acUsageFraction,
                   tid, pid, cid, allocSize, allocSizeSuffix, memorySizeSuffix, memorySize,
                   tid, pid, cid, allocSize, allocSizeSuffix, (double)(stats.evictions/(double)stats.evictionAttempts),
                   tid, pid, cid, allocSize, allocSizeSuffix, stats.allocLatencyNs.estimate())
            << std::endl;
        }
      });
    }

    int bgId = 1;
    for (auto &bgWorkerStats : backgroundEvictorStats) {
        if (bgWorkerStats.numMovedItems > 0) {
          out << folly::sformat(" == Background Evictor Threads ==") << std::endl;
          out << folly::sformat("Background Evictor Thread {} Evicted Items: {:,}\n"
                                "Background Evictor Thread {} Traversals: {:,}\n"
                                "Background Evictor Thread {} Run Count: {:,}\n"
                                "Background Evictor Thread {} Avg Time Per Traversal in ns: {:,}\n"
                                "Background Evictor Thread {} Avg Items Evicted: {:.2f}",
                                bgId, bgWorkerStats.numMovedItems,
                                bgId, bgWorkerStats.numTraversals,
                                bgId, bgWorkerStats.runCount,
                                bgId, bgWorkerStats.avgTraversalTimeNs,
                                bgId, (double)bgWorkerStats.numMovedItems/(double)bgWorkerStats.numTraversals)
              << std::endl;
        }
        bgId++;

    }
    bgId = 1;
    for (auto &bgWorkerStats : backgroundPromoStats) {
        if (bgWorkerStats.numMovedItems > 0) {
          out << folly::sformat(" == Background Promoter Threads ==") << std::endl;
          out << folly::sformat("Background Promoter Thread {} Promoted Items: {:,}\n"
                                "Background Promoter Thread {} Traversals: {:,}\n"
                                "Background Promoter Thread {} Run Count: {:,}\n"
                                "Background Promoter Thread {} Avg Time Per Traversal in ns: {:,}\n"
                                "Background Promoter Thread {} Avg Items Promoted: {:.2f}",
                                bgId, bgWorkerStats.numMovedItems,
                                bgId, bgWorkerStats.numTraversals,
                                bgId, bgWorkerStats.runCount,
                                bgId, bgWorkerStats.avgTraversalTimeNs,
                                bgId, (double)bgWorkerStats.numMovedItems/(double)bgWorkerStats.numTraversals)
              << std::endl;
        }
        bgId++;

    }
    if (numCacheGets > 0) {
      out << folly::sformat("Cache Gets    : {:,}", numCacheGets) << std::endl;
      out << folly::sformat("Hit Ratio     : {:6.2f}%", overallHitRatio)
          << std::endl;
      for (TierId tid = 0; tid < numCacheHits.size(); tid++) {
        double tierHitRatio = pctFn(numCacheHits[tid],numCacheGets);
        out << folly::sformat("Tier {} Hit Ratio     : {:6.2f}%", tid, tierHitRatio)
            << std::endl;
      }
      if (FLAGS_report_api_latency) {
        auto printLatencies =
            [&out](folly::StringPiece cat,
                   const util::PercentileStats::Estimates& latency) {
              auto fmtLatency = [&out, &cat](folly::StringPiece pct,
                                             double val) {
                out << folly::sformat("{:20} {:8} in ns: {:>10.2f}\n", cat, pct, val);
              };

              fmtLatency("p50", latency.p50);
              fmtLatency("p90", latency.p90);
              fmtLatency("p99", latency.p99);
              fmtLatency("p999", latency.p999);
              fmtLatency("p9999", latency.p9999);
              fmtLatency("p99999", latency.p99999);
              fmtLatency("p999999", latency.p999999);
              fmtLatency("p100", latency.p100);
            };

        printLatencies("Cache Find API latency", cacheFindLatencyNs);
        printLatencies("Cache Allocate API latency", cacheAllocateLatencyNs);
      }
    }

    uint64_t totalbgevicted = 0;
    uint64_t totalpromoted = 0;
    for (int i = 0; i < backgroundEvictorStats.size(); i++) {
        totalbgevicted += backgroundEvictorStats[i].numMovedItems;
    }
    for (int i = 0; i < backgroundPromoStats.size(); i++) {
        totalpromoted += backgroundPromoStats[i].numMovedItems;
    }
    if (!backgroundEvictionClasses.empty() && totalbgevicted > 0 ) {
      out << "== Class Background Eviction Counters Map ==" << std::endl;
      foreachAC(backgroundEvictionClasses, [&](auto tid, auto pid, auto cid, auto evicted){
        if (evicted > 0) {
          out << folly::sformat("tid{:2} pid{:2} cid{:4} evicted: {:4}",
            tid, pid, cid, evicted) << std::endl;
        }
      });
    }
    
    if (!backgroundPromotionClasses.empty() && totalpromoted) {
      out << "== Class Background Promotion Counters Map ==" << std::endl;
      foreachAC(backgroundPromotionClasses, [&](auto tid, auto pid, auto cid, auto promoted){
        if (promoted > 0) {
          out << folly::sformat("tid{:2} pid{:2} cid{:4} promoted: {:4}",
            tid, pid, cid, promoted) << std::endl;
        }
      });
    }

    if (reaperStats.numReapedItems > 0) {

      out << folly::sformat("Reaper reaped: {:,} visited: {:,} traversals: {:,} avg traversal time: {:,}",
              reaperStats.numReapedItems,reaperStats.numVisitedItems,
              reaperStats.numTraversals,reaperStats.avgTraversalTimeMs)
              << std::endl;
    }

    if (numNvmGets > 0 || numNvmDeletes > 0 || numNvmPuts > 0) {
      const double ramHitRatio = invertPctFn(numCacheGetMiss, numCacheGets);
      const double nvmHitRatio = invertPctFn(numNvmGetMiss, numNvmGets);

      out << folly::sformat(
          "RAM Hit Ratio : {:6.2f}%\n"
          "NVM Hit Ratio : {:6.2f}%\n",
          ramHitRatio, nvmHitRatio);

      out << folly::sformat(
          "RAM eviction rejects expiry : {:,}\nRAM eviction rejects clean : "
          "{:,}\n",
          numNvmRejectsByExpiry, numNvmRejectsByClean);

      folly::StringPiece readCat = "NVM Read  Latency";
      folly::StringPiece writeCat = "NVM Write Latency";
      auto fmtLatency = [&](folly::StringPiece cat, folly::StringPiece pct,
                            double val) {
        out << folly::sformat("{:20} {:8} : {:>10.2f} us\n", cat, pct, val);
      };

      fmtLatency(readCat, "p50", nvmReadLatencyMicrosP50);
      fmtLatency(readCat, "p90", nvmReadLatencyMicrosP90);
      fmtLatency(readCat, "p99", nvmReadLatencyMicrosP99);
      fmtLatency(readCat, "p999", nvmReadLatencyMicrosP999);
      fmtLatency(readCat, "p9999", nvmReadLatencyMicrosP9999);
      fmtLatency(readCat, "p99999", nvmReadLatencyMicrosP99999);
      fmtLatency(readCat, "p999999", nvmReadLatencyMicrosP999999);
      fmtLatency(readCat, "p100", nvmReadLatencyMicrosP100);

      fmtLatency(writeCat, "p50", nvmWriteLatencyMicrosP50);
      fmtLatency(writeCat, "p90", nvmWriteLatencyMicrosP90);
      fmtLatency(writeCat, "p99", nvmWriteLatencyMicrosP99);
      fmtLatency(writeCat, "p999", nvmWriteLatencyMicrosP999);
      fmtLatency(writeCat, "p9999", nvmWriteLatencyMicrosP9999);
      fmtLatency(writeCat, "p99999", nvmWriteLatencyMicrosP99999);
      fmtLatency(writeCat, "p999999", nvmWriteLatencyMicrosP999999);
      fmtLatency(writeCat, "p100", nvmWriteLatencyMicrosP100);

      constexpr double GB = 1024.0 * 1024 * 1024;
      double appWriteAmp =
          pctFn(numNvmBytesWritten, numNvmLogicalBytesWritten) / 100.0;

      double devWriteAmp =
          pctFn(numNvmNandBytesWritten, numNvmBytesWritten) / 100.0;
      out << folly::sformat("NVM bytes written (physical) in GB : {:6.2f}\n",
                            numNvmBytesWritten / GB);
      out << folly::sformat("NVM bytes written (logical) in GB  : {:6.2f}\n",
                            numNvmLogicalBytesWritten / GB);
      out << folly::sformat("NVM bytes written (nand) in GB     : {:6.2f}\n",
                            numNvmNandBytesWritten / GB);
      out << folly::sformat("NVM app write amplification        : {:6.2f}\n",
                            appWriteAmp);
      out << folly::sformat("NVM dev write amplification        : {:6.2f}\n",
                            devWriteAmp);
    }
    const double putSuccessPct =
        invertPctFn(numNvmPutErrs + numNvmAbortedPutOnInflightGet +
                        numNvmAbortedPutOnTombstone,
                    numNvmPuts);
    const double cleanEvictPct = pctFn(numNvmCleanEvict, numNvmEvictions);
    const double getCoalescedPct = pctFn(numNvmGetCoalesced, numNvmGets);
    out << folly::sformat("{:30}: {:10,}\n"
                          "{:30}: {:10.2f}",
                          "NVM Gets", numNvmGets,
                          "NVM Coalesced in pct", getCoalescedPct)
        << std::endl;
    out << folly::sformat(
               "{:30}: {:10,}\n"
               "{:30}: {:10.2f}\n"
               "{:30}: {:10.2f}\n"
               "{:30}: {:10,}\n"
               "{:30}: {:10,}",
               "NVM Puts", numNvmPuts,
               "NVM Puts Success in pct", putSuccessPct,
               "NVM Puts from Clean in pct", pctFn(numNvmPutFromClean, numNvmPuts),
               "NVM AbortsFromDel", numNvmAbortedPutOnTombstone,
               "NVM AbortsFromGet", numNvmAbortedPutOnInflightGet)
        << std::endl;
    out << folly::sformat(
               "{:30}: {:10,}\n"
               "{:30}: {:10.2f}\n"
               "{:30}: {:10,}\n"
               "{:30}: {:10,}",
               "NVM Evicts", numNvmEvictions,
               "NVM Clean Evicts in pct", cleanEvictPct,
               "NVM Unclean Evicts", numNvmUncleanEvict,
               "NVM Clean Double Evicts", numNvmCleanDoubleEvict)
        << std::endl;
    const double skippedDeletesPct = pctFn(numNvmSkippedDeletes, numNvmDeletes);
    out << folly::sformat("{:30}: {:10,}\n"
                          "{:30}: {:10.2f}",
                          "NVM Deletes", numNvmDeletes,
                          "NVM Skipped Deletes in pct", skippedDeletesPct)
        << std::endl;
    if (numNvmExceededMaxRetry > 0) {
      out << folly::sformat("{:30}: {:10,}", "NVM max read retry reached",
                            numNvmExceededMaxRetry)
          << std::endl;
    }

    if (slabsReleased > 0) {
      out << folly::sformat(
                 "Released slabs: {:,}\n"
                 "Slab Move attempts: {:10,}\n"
                 "Slab Move success in pct: {:6.2f}\n"
                 "Slab Eviction attempts: {:10,}\n"
                 "Slab Eviction success in pct: {:6.2f}",
                 slabsReleased,
                 moveAttemptsForSlabRelease,
                 pctFn(moveSuccessesForSlabRelease, moveAttemptsForSlabRelease),
                 evictionAttemptsForSlabRelease,
                 pctFn(evictionSuccessesForSlabRelease, evictionAttemptsForSlabRelease))
          << std::endl;
    }

    if (!nvmCounters.empty()) {
      out << "== NVM Counters Map ==" << std::endl;
      for (const auto& it : nvmCounters) {
        out << it.first << "  :  " << it.second << std::endl;
      }
    }

    if (numRamDestructorCalls > 0 || numNvmDestructorCalls > 0) {
      out << folly::sformat("Destructor executed from RAM {}, from NVM {}",
                            numRamDestructorCalls, numNvmDestructorCalls)
          << std::endl;
    }

    if (numCacheEvictions > 0) {
      out << folly::sformat("Total evictions executed  : {:10,}", numCacheEvictions)
              << std::endl;
      out << folly::sformat("Total background evictions: {:10,}", totalbgevicted)
              << std::endl;
    }
    if (totalpromoted > 0) {
      out << folly::sformat("Total promotions          : {:10,}", totalpromoted) << std::endl;
    }
  }

  uint64_t getTotalMisses() const {
    return numNvmGets > 0 ? numNvmGetMiss : numCacheGetMiss;
  }

  std::tuple<double, double, double> getHitRatios(
      const Stats& prevStats) const {
    double overallHitRatio = 0.0;
    double ramHitRatio = 0.0;
    double nvmHitRatio = 0.0;

    if (numCacheGets > prevStats.numCacheGets) {
      auto totalMisses = getTotalMisses();
      auto prevTotalMisses = prevStats.getTotalMisses();

      overallHitRatio = invertPctFn(totalMisses - prevTotalMisses,
                                    numCacheGets - prevStats.numCacheGets);

      ramHitRatio = invertPctFn(numCacheGetMiss - prevStats.numCacheGetMiss,
                                numCacheGets - prevStats.numCacheGets);
    }

    if (numNvmGets > prevStats.numNvmGets) {
      nvmHitRatio = invertPctFn(numNvmGetMiss - prevStats.numNvmGetMiss,
                                numNvmGets - prevStats.numNvmGets);
    }

    return std::make_tuple(overallHitRatio, ramHitRatio, nvmHitRatio);
  }

  // Render the stats based on the delta between overall stats and previous
  // stats. It can be used to render the stats in the last time period.
  void render(const Stats& prevStats, std::ostream& out) const {
    if (numCacheGets > prevStats.numCacheGets) {
      auto [overallHitRatio, ramHitRatio, nvmHitRatio] =
          getHitRatios(prevStats);
      out << folly::sformat("Cache Gets    : {:,}",
                            numCacheGets - prevStats.numCacheGets)
          << std::endl;
      out << folly::sformat("Hit Ratio     : {:6.2f}%", overallHitRatio)
          << std::endl;

      out << folly::sformat(
          "RAM Hit Ratio : {:6.2f}%\n"
          "NVM Hit Ratio : {:6.2f}%\n",
          ramHitRatio, nvmHitRatio);
    }
  }

  void render(folly::UserCounters& counters) {
    auto calcInvertPctFn = [](uint64_t ops, uint64_t total) {
      return static_cast<int64_t>(invertPctFn(ops, total) * 100);
    };

    auto totalMisses = getTotalMisses();
    //TODO: per tier
    counters["num_items"] = std::accumulate(numItems.begin(),numItems.end(),0);
    counters["num_nvm_items"] = numNvmItems;
    counters["hit_rate"] = calcInvertPctFn(totalMisses, numCacheGets);

    counters["find_latency_p99"] = cacheFindLatencyNs.p99;
    counters["alloc_latency_p99"] = cacheAllocateLatencyNs.p99;

    counters["ram_hit_rate"] = calcInvertPctFn(numCacheGetMiss, numCacheGets);
    counters["nvm_hit_rate"] = calcInvertPctFn(numCacheGetMiss, numCacheGets);

    counters["nvm_read_latency_p99"] =
        static_cast<int64_t>(nvmReadLatencyMicrosP99);
    counters["nvm_write_latency_p99"] =
        static_cast<int64_t>(nvmWriteLatencyMicrosP99);

    constexpr double MB = 1024.0 * 1024;
    double appWriteAmp =
        pctFn(numNvmBytesWritten, numNvmLogicalBytesWritten) / 100.0;

    double devWriteAmp =
        pctFn(numNvmNandBytesWritten, numNvmBytesWritten) / 100.0;

    counters["nvm_bytes_written_physical_mb"] =
        static_cast<int64_t>(numNvmBytesWritten / MB);
    counters["nvm_bytes_written_logical_mb"] =
        static_cast<int64_t>(numNvmLogicalBytesWritten / MB);
    counters["nvm_bytes_written_nand_mb"] =
        static_cast<int64_t>(numNvmNandBytesWritten / MB);
    counters["nvm_app_write_amp"] = static_cast<int64_t>(appWriteAmp);
    counters["nvm_dev_write_amp"] = static_cast<int64_t>(devWriteAmp);
  }

  bool renderIsTestPassed(std::ostream& out) {
    bool pass = true;

    if (isNvmCacheDisabled) {
      out << "NVM Cache was disabled during test!" << std::endl;
      pass = false;
    }

    if (inconsistencyCount) {
      out << "Found " << inconsistencyCount << " inconsistent cases"
          << std::endl;
      pass = false;
    }

    if (invalidDestructorCount) {
      out << "Found " << invalidDestructorCount
          << " invalid item destructor cases" << std::endl;
      pass = false;
    }

    if (unDestructedItemCount) {
      out << "Found " << unDestructedItemCount
          << " items missing destructor cases" << std::endl;
      pass = false;
    }

    for (const auto& kv : nvmErrors) {
      std::cout << "NVM error. " << kv.first << " : " << kv.second << std::endl;
      pass = false;
    }

    if (numNvmItemRemovedSetSize != 0) {
      std::cout << "NVM error. ItemRemoved not empty" << std::endl;
      pass = false;
    }

    return pass;
  }

 private:
  static double pctFn(uint64_t ops, uint64_t total) {
    return total == 0
               ? 0
               : 100.0 * static_cast<double>(ops) / static_cast<double>(total);
  }

  static double invertPctFn(uint64_t ops, uint64_t total) {
    return 100 - pctFn(ops, total);
  }

}; // namespace cachebench

} // namespace cachebench
} // namespace cachelib
} // namespace facebook
