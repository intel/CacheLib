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

#include <folly/Conv.h>
#include <folly/ProducerConsumerQueue.h>
#include <folly/ThreadLocal.h>
#include <folly/lang/Aligned.h>
#include <folly/logging/xlog.h>
#include <folly/synchronization/Latch.h>
#include <folly/system/ThreadName.h>

#include "cachelib/cachebench/cache/Cache.h"
#include "cachelib/cachebench/util/Exceptions.h"
#include "cachelib/cachebench/util/Parallel.h"
#include "cachelib/cachebench/util/Request.h"
#include "cachelib/cachebench/workload/ReplayGeneratorBase.h"

namespace facebook {
namespace cachelib {
namespace cachebench {

// BinaryKVReplayGenerator generates the cachelib requests based on the
// requests read from the given binary trace file made with KVReplayGenerator
// In order to minimize the contentions for the request submission queues
// which might need to be dispatched by multiple stressor threads,
// the requests are sharded to each stressor by doing hashing over the key.
class BinaryKVReplayGenerator : public ReplayGeneratorBase {
 public:
  explicit BinaryKVReplayGenerator(const StressorConfig& config)
      : ReplayGeneratorBase(config), binaryStream_(config) {
    for (uint32_t i = 0; i < numShards_; ++i) {
      stressorCtxs_.emplace_back(std::make_unique<StressorCtx>(i));
      std::string_view s{"abc"};
      requestPtr_.emplace_back(
          new Request(s, reinterpret_cast<size_t*>(0), OpType::kGet, 0));
    }

    folly::Latch latch(1);
    genWorker_ = std::thread([this, &latch] {
      folly::setThreadName("cb_replay_gen");
      genRequests(latch);
    });

    latch.wait();

    XLOGF(INFO,
          "Started BinaryKVReplayGenerator (amp factor {}, # of stressor "
          "threads {}, fast foward {})",
          ampFactor_, numShards_, fastForwardCount_);
  }

  virtual ~BinaryKVReplayGenerator() {
    XCHECK(shouldShutdown());
    if (genWorker_.joinable()) {
      genWorker_.join();
    }
  }

  // getReq generates the next request from the trace file.
  const Request& getReq(
      uint8_t,
      std::mt19937_64&,
      std::optional<uint64_t> lastRequestId = std::nullopt) override;

  void renderStats(uint64_t, std::ostream& out) const override {
    out << std::endl << "== BinaryKVReplayGenerator Stats ==" << std::endl;

    out << folly::sformat("{}: {:.2f} million (parse error: {})",
                          "Total Processed Samples",
                          (double)parseSuccess.load() / 1e6, parseError.load())
        << std::endl;
  }

  void notifyResult(uint64_t requestId, OpResultType result) override;

  void markFinish() override { getStressorCtx().markFinish(); }

 private:
  // Interval at which the submission queue is polled when it is either
  // full (producer) or empty (consumer).
  // We use polling with the delay since the ProducerConsumerQueue does not
  // support blocking read or writes with a timeout
  static constexpr uint64_t checkIntervalUs_ = 100;
  static constexpr size_t kMaxRequests =
      500000000; // just stores pointers to mmap'd data

  using ReqQueue = folly::ProducerConsumerQueue<BinaryRequest*>;

  // StressorCtx keeps track of the state including the submission queues
  // per stressor thread. Since there is only one request generator thread,
  // lock-free ProducerConsumerQueue is used for performance reason.
  // Also, separate queue which is dispatched ahead of any requests in the
  // submission queue is used for keeping track of the requests which need to be
  // resubmitted (i.e., a request having remaining repeat count); there could
  // be more than one requests outstanding for async stressor while only one
  // can be outstanding for sync stressor
  struct StressorCtx {
    explicit StressorCtx(uint32_t id)
        : id_(id), reqQueue_(std::in_place_t{}, kMaxRequests) {}

    bool isFinished() { return finished_.load(std::memory_order_relaxed); }
    void markFinish() { finished_.store(true, std::memory_order_relaxed); }

    uint32_t id_{0};
    // std::queue<std::unique_ptr<BinaryReqWrapper>> resubmitQueue_;
    std::queue<BinaryRequest*> resubmitQueue_;
    folly::cacheline_aligned<ReqQueue> reqQueue_;
    // Thread that finish its operations mark it here, so we will skip
    // further request on its shard
    std::atomic<bool> finished_{false};
  };

  // Used to assign stressorIdx_
  std::atomic<uint32_t> incrementalIdx_{0};

  // A sticky index assigned to each stressor threads that calls into
  // the generator.
  folly::ThreadLocalPtr<uint32_t> stressorIdx_;

  // Vector size is equal to the # of stressor threads;
  // stressorIdx_ is used to index.
  std::vector<std::unique_ptr<StressorCtx>> stressorCtxs_;

  // Pointer to request object used to carry binary
  // request data
  std::vector<Request*> requestPtr_;

  // Class that holds a vector of pointers to the
  // binary data
  BinaryFileStream binaryStream_;

  std::thread genWorker_;

  // Used to signal end of file as EndOfTrace exception
  std::atomic<bool> eof{false};

  // Stats
  std::atomic<uint64_t> parseError = 0;
  std::atomic<uint64_t> parseSuccess = 0;

  void genRequests(folly::Latch& latch);

  void setEOF() { eof.store(true, std::memory_order_relaxed); }
  bool isEOF() { return eof.load(std::memory_order_relaxed); }

  inline StressorCtx& getStressorCtx(size_t shardId) {
    XCHECK_LT(shardId, numShards_);
    return *stressorCtxs_[shardId];
  }

  inline StressorCtx& getStressorCtx() {
    if (!stressorIdx_.get()) {
      stressorIdx_.reset(new uint32_t(incrementalIdx_++));
    }

    return getStressorCtx(*stressorIdx_);
  }

  inline Request* getRequestPtr(size_t shardId) {
    XCHECK_LT(shardId, numShards_);
    return requestPtr_[shardId];
  }

  inline Request* getRequestPtr() {
    if (!stressorIdx_.get()) {
      stressorIdx_.reset(new uint32_t(incrementalIdx_++));
    }

    return getRequestPtr(*stressorIdx_);
  }
};

inline void BinaryKVReplayGenerator::genRequests(folly::Latch& latch) {
  bool init = true;
  uint64_t nreqs = 0;
  binaryStream_.setOffset(fastForwardCount_);
  auto begin = util::getCurrentTimeSec();
  while (!shouldShutdown()) {
    try {
      BinaryRequest* req = binaryStream_.getNextPtr();
      auto key = req->getKey(binaryStream_.getKeyOffset());
      XDCHECK_LT(req->op_, 11);
      nreqs++;
      auto shardId = getShard(key);
      auto& stressorCtx = getStressorCtx(shardId);
      auto& reqQ = *stressorCtx.reqQueue_;
      while (!reqQ.write(req) && !stressorCtx.isFinished() &&
             !shouldShutdown()) {
        // ProducerConsumerQueue does not support blocking, so use sleep
        if (init) {
          latch.count_down();
          init = false;
        }
        std::this_thread::sleep_for(
            std::chrono::microseconds{checkIntervalUs_});
      }
      if (nreqs >= preLoadReqs_ && init) {
        auto end = util::getCurrentTimeSec();
        double reqsPerSec = nreqs / (double)(end - begin);
        XLOGF(INFO, "Parse rate: {:.2f} reqs/sec", reqsPerSec);
        latch.count_down();
        init = false;
      }
    } catch (const EndOfTrace& e) {
      if (init) {
        latch.count_down();
      }
      break;
    }
  }

  setEOF();
}

const Request& BinaryKVReplayGenerator::getReq(uint8_t,
                                               std::mt19937_64&,
                                               std::optional<uint64_t>) {
  BinaryRequest* req = nullptr;
  auto& stressorCtx = getStressorCtx();
  auto& reqQ = *stressorCtx.reqQueue_;
  auto& resubmitQueue = stressorCtx.resubmitQueue_;
  XDCHECK_EQ(req, nullptr);

  while (resubmitQueue.empty() && !reqQ.read(req)) {
    if (resubmitQueue.empty() && isEOF()) {
      throw cachelib::cachebench::EndOfTrace("Test stopped or EOF reached");
    }
    // ProducerConsumerQueue does not support blocking, so use sleep
    std::this_thread::sleep_for(std::chrono::microseconds{checkIntervalUs_});
  }

  if (req == nullptr) {
    XCHECK(!resubmitQueue.empty());
    req = resubmitQueue.front();
    resubmitQueue.pop();
  }
  XDCHECK_NE(req, nullptr);
  XDCHECK_NE(reinterpret_cast<uint64_t>(req), 0);
  XDCHECK_LT(req->op_, 12);
  auto key = req->getKey(binaryStream_.getKeyOffset());
  OpType op;
  switch (req->op_) {
  case 1:
    op = OpType::kGet;
    break;
  case 2:
    op = OpType::kSet;
    break;
  case 3:
    op = OpType::kDel;
    break;
  }
  auto r = getRequestPtr();
  r->update(key,
            const_cast<size_t*>(reinterpret_cast<size_t*>(&req->valueSize_)),
            op,
            req->ttl_,
            reinterpret_cast<uint64_t>(req));
  return *r;
}

void BinaryKVReplayGenerator::notifyResult(uint64_t requestId, OpResultType) {
  // requestId should point to the BinaryRequesat object.
  BinaryRequest* req = reinterpret_cast<BinaryRequest*>(requestId);
  if (req->repeats_ > 0) {
    req->repeats_ = req->repeats_ - 1;
    // need to insert into the queue again
    getStressorCtx().resubmitQueue_.emplace(req);
  }
}

} // namespace cachebench
} // namespace cachelib
} // namespace facebook
