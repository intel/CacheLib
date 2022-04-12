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
 * See the License for the specific language governing fmm permissions and
 * limitations under the License.
 */

#pragma once

namespace facebook {
namespace cachelib {

template <typename CacheTrait>
CacheAllocator<CacheTrait>::CacheAllocator(Config config)
    : CacheAllocator(InitMemType::kNone, config) {
  initCommon(false);
}

template <typename CacheTrait>
CacheAllocator<CacheTrait>::CacheAllocator(SharedMemNewT, Config config)
    : CacheAllocator(InitMemType::kMemNew, config) {
  initCommon(false);
  shmManager_->removeShm(detail::kShmInfoName);
}

template <typename CacheTrait>
CacheAllocator<CacheTrait>::CacheAllocator(SharedMemAttachT, Config config)
    : CacheAllocator(InitMemType::kMemAttach, config) {
  for (auto pid : *metadata_.compactCachePools()) {
    isCompactCachePool_[pid] = true;
  }

  initCommon(true);

  // We will create a new info shm segment on shutDown(). If we don't remove
  // this info shm segment here and the new info shm segment's size is larger
  // than this one, creating new one will fail.
  shmManager_->removeShm(detail::kShmInfoName);
}

template <typename CacheTrait>
CacheAllocator<CacheTrait>::CacheAllocator(
    typename CacheAllocator<CacheTrait>::InitMemType type, Config config)
    : isOnShm_{type != InitMemType::kNone ? true
                                          : config.memMonitoringEnabled()},
      config_(config.validate()),
      tempShm_(type == InitMemType::kNone && isOnShm_
                   ? std::make_unique<TempShmMapping>(config_.size)
                   : nullptr),
      shmManager_(type != InitMemType::kNone
                      ? std::make_unique<ShmManager>(config_.cacheDir,
                                                     config_.usePosixShm)
                      : nullptr),
      deserializer_(type == InitMemType::kMemAttach ? createDeserializer()
                                                    : nullptr),
      metadata_{type == InitMemType::kMemAttach
                    ? deserializeCacheAllocatorMetadata(*deserializer_)
                    : serialization::CacheAllocatorMetadata{}},
      allocator_(initAllocator(type)),
      compactCacheManager_(type != InitMemType::kMemAttach
                               ? std::make_unique<CCacheManager>(*allocator_)
                               : restoreCCacheManager()),
      compressor_(createPtrCompressor()),
      mmContainers_(type == InitMemType::kMemAttach
                        ? deserializeMMContainers(*deserializer_, compressor_)
                        : MMContainers{}),
      accessContainer_(initAccessContainer(
          type, detail::kShmHashTableName, config.accessConfig)),
      chainedItemAccessContainer_(
          initAccessContainer(type,
                              detail::kShmChainedItemHashTableName,
                              config.chainedItemAccessConfig)),
      chainedItemLocks_(config_.chainedItemsLockPower,
                        std::make_shared<MurmurHash2>()),
      cacheCreationTime_{
          type != InitMemType::kMemAttach
              ? util::getCurrentTimeSec()
              : static_cast<uint32_t>(*metadata_.cacheCreationTime())},
      cacheInstanceCreationTime_{type != InitMemType::kMemAttach
                                     ? cacheCreationTime_
                                     : util::getCurrentTimeSec()},
      // Pass in cacheInstnaceCreationTime_ as the current time to keep
      // nvmCacheState's current time in sync
      nvmCacheState_{cacheInstanceCreationTime_, config_.cacheDir,
                     config_.isNvmCacheEncryptionEnabled(),
                     config_.isNvmCacheTruncateAllocSizeEnabled()} {}

template <typename CacheTrait>
CacheAllocator<CacheTrait>::~CacheAllocator() {
  XLOG(DBG, "destructing CacheAllocator");
  // Stop all workers. In case user didn't call shutDown, we want to
  // terminate all background workers and nvmCache before member variables
  // go out of scope.
  stopWorkers();
  nvmCache_.reset();
}

template <typename CacheTrait>
std::unique_ptr<MemoryAllocator>
CacheAllocator<CacheTrait>::createNewMemoryAllocator() {
  ShmSegmentOpts opts;
  opts.alignment = sizeof(Slab);
  return std::make_unique<MemoryAllocator>(
      getAllocatorConfig(config_),
      shmManager_
          ->createShm(detail::kShmCacheName, config_.size,
                      config_.slabMemoryBaseAddr, opts)
          .addr,
      config_.size);
}

template <typename CacheTrait>
std::unique_ptr<MemoryAllocator>
CacheAllocator<CacheTrait>::restoreMemoryAllocator() {
  ShmSegmentOpts opts;
  opts.alignment = sizeof(Slab);
  return std::make_unique<MemoryAllocator>(
      deserializer_->deserialize<MemoryAllocator::SerializationType>(),
      shmManager_
          ->attachShm(detail::kShmCacheName, config_.slabMemoryBaseAddr, opts)
          .addr,
      config_.size,
      config_.disableFullCoredump);
}

template <typename CacheTrait>
std::unique_ptr<CCacheManager>
CacheAllocator<CacheTrait>::restoreCCacheManager() {
  return std::make_unique<CCacheManager>(
      deserializer_->deserialize<CCacheManager::SerializationType>(),
      *allocator_);
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::initCommon(bool dramCacheAttached) {
  if (config_.nvmConfig.has_value()) {
    if (config_.nvmCacheAP) {
      nvmAdmissionPolicy_ = config_.nvmCacheAP;
    } else if (config_.rejectFirstAPNumEntries) {
      nvmAdmissionPolicy_ = std::make_shared<RejectFirstAP<CacheT>>(
          config_.rejectFirstAPNumEntries, config_.rejectFirstAPNumSplits,
          config_.rejectFirstSuffixIgnoreLength,
          config_.rejectFirstUseDramHitSignal);
    }
    if (config_.nvmAdmissionMinTTL > 0) {
      if (!nvmAdmissionPolicy_) {
        nvmAdmissionPolicy_ = std::make_shared<NvmAdmissionPolicy<CacheT>>();
      }
      nvmAdmissionPolicy_->initMinTTL(config_.nvmAdmissionMinTTL);
    }
  }
  initStats();
  initNvmCache(dramCacheAttached);

  if (!config_.delayCacheWorkersStart) {
    initWorkers();
  }
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::initNvmCache(bool dramCacheAttached) {
  if (!config_.nvmConfig.has_value()) {
    return;
  }

  // for some usecases that create pools, restoring nvmcache when dram cache
  // is not persisted is not supported.
  const bool shouldDrop = config_.dropNvmCacheOnShmNew && !dramCacheAttached;

  // if we are dealing with persistency, cache directory should be enabled
  const bool truncate = config_.cacheDir.empty() ||
                        nvmCacheState_.shouldStartFresh() || shouldDrop;
  if (truncate) {
    nvmCacheState_.markTruncated();
  }

  nvmCache_ = std::make_unique<NvmCacheT>(*this, *config_.nvmConfig, truncate,
                                          config_.itemDestructor);
  if (!config_.cacheDir.empty()) {
    nvmCacheState_.clearPrevState();
  }
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::initWorkers() {
  if (config_.poolResizingEnabled() && !poolResizer_) {
    startNewPoolResizer(config_.poolResizeInterval,
                        config_.poolResizeSlabsPerIter,
                        config_.poolResizeStrategy);
  }

  if (config_.poolRebalancingEnabled() && !poolRebalancer_) {
    startNewPoolRebalancer(config_.poolRebalanceInterval,
                           config_.defaultPoolRebalanceStrategy,
                           config_.poolRebalancerFreeAllocThreshold);
  }

  if (config_.memMonitoringEnabled() && !memMonitor_) {
    if (!isOnShm_) {
      throw std::invalid_argument(
          "Memory monitoring is not supported for cache on heap. It is "
          "supported "
          "for cache on a shared memory segment only.");
    }
    startNewMemMonitor(config_.memMonitorInterval,
                       config_.memMonitorConfig,
                       config_.poolAdviseStrategy);
  }

  if (config_.itemsReaperEnabled() && !reaper_) {
    startNewReaper(config_.reaperInterval, config_.reaperConfig);
  }

  if (config_.poolOptimizerEnabled() && !poolOptimizer_) {
    startNewPoolOptimizer(config_.regularPoolOptimizeInterval,
                          config_.compactCacheOptimizeInterval,
                          config_.poolOptimizeStrategy,
                          config_.ccacheOptimizeStepSizePercent);
  }
}

template <typename CacheTrait>
std::unique_ptr<MemoryAllocator> CacheAllocator<CacheTrait>::initAllocator(
    InitMemType type) {
  if (type == InitMemType::kNone) {
    if (isOnShm_ == true) {
      return std::make_unique<MemoryAllocator>(
          getAllocatorConfig(config_), tempShm_->getAddr(), config_.size);
    } else {
      return std::make_unique<MemoryAllocator>(getAllocatorConfig(config_),
                                               config_.size);
    }
  } else if (type == InitMemType::kMemNew) {
    return createNewMemoryAllocator();
  } else if (type == InitMemType::kMemAttach) {
    return restoreMemoryAllocator();
  }

  // Invalid type
  throw std::runtime_error(folly::sformat(
      "Cannot initialize memory allocator, unknown InitMemType: {}.",
      static_cast<int>(type)));
}

template <typename CacheTrait>
std::unique_ptr<typename CacheAllocator<CacheTrait>::AccessContainer>
CacheAllocator<CacheTrait>::initAccessContainer(InitMemType type,
                                                const std::string name,
                                                AccessConfig config) {
  if (type == InitMemType::kNone) {
    return std::make_unique<AccessContainer>(
        config, compressor_,
        [this](Item* it) -> WriteHandle { return acquire(it); });
  } else if (type == InitMemType::kMemNew) {
    return std::make_unique<AccessContainer>(
        config,
        shmManager_
            ->createShm(
                name,
                AccessContainer::getRequiredSize(config.getNumBuckets()),
                nullptr,
                ShmSegmentOpts(config.getPageSize()))
            .addr,
        compressor_,
        [this](Item* it) -> WriteHandle { return acquire(it); });
  } else if (type == InitMemType::kMemAttach) {
    return std::make_unique<AccessContainer>(
        deserializer_->deserialize<AccessSerializationType>(),
        config,
        shmManager_->attachShm(name),
        compressor_,
        [this](Item* it) -> WriteHandle { return acquire(it); });
  }

  // Invalid type
  throw std::runtime_error(folly::sformat(
      "Cannot initialize access container, unknown InitMemType: {}.",
      static_cast<int>(type)));
}

template <typename CacheTrait>
std::unique_ptr<Deserializer> CacheAllocator<CacheTrait>::createDeserializer() {
  auto infoAddr = shmManager_->attachShm(detail::kShmInfoName);
  return std::make_unique<Deserializer>(
      reinterpret_cast<uint8_t*>(infoAddr.addr),
      reinterpret_cast<uint8_t*>(infoAddr.addr) + infoAddr.size);
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::allocate(PoolId poolId,
                                     typename Item::Key key,
                                     uint32_t size,
                                     uint32_t ttlSecs,
                                     uint32_t creationTime) {
  if (creationTime == 0) {
    creationTime = util::getCurrentTimeSec();
  }
  return allocateInternal(poolId, key, size, creationTime,
                          ttlSecs == 0 ? 0 : creationTime + ttlSecs);
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::allocateInternal(PoolId pid,
                                             typename Item::Key key,
                                             uint32_t size,
                                             uint32_t creationTime,
                                             uint32_t expiryTime) {
  util::LatencyTracker tracker{stats().allocateLatency_};

  SCOPE_FAIL { stats_.invalidAllocs.inc(); };

  // number of bytes required for this item
  const auto requiredSize = Item::getRequiredSize(key, size);

  // the allocation class in our memory allocator.
  const auto cid = allocator_->getAllocationClassId(pid, requiredSize);

  (*stats_.allocAttempts)[pid][cid].inc();

  void* memory = allocator_->allocate(pid, requiredSize);
  if (memory == nullptr && !config_.isEvictionDisabled()) {
    memory = findEviction(pid, cid);
  }

  WriteHandle handle;
  if (memory != nullptr) {
    // At this point, we have a valid memory allocation that is ready for use.
    // Ensure that when we abort from here under any circumstances, we free up
    // the memory. Item's handle could throw because the key size was invalid
    // for example.
    SCOPE_FAIL {
      // free back the memory to the allocator since we failed.
      allocator_->free(memory);
    };

    handle = acquire(new (memory) Item(key, size, creationTime, expiryTime));
    if (handle) {
      handle.markNascent();
      (*stats_.fragmentationSize)[pid][cid].add(
          util::getFragmentation(*this, *handle));
    }

  } else { // failed to allocate memory.
    (*stats_.allocFailures)[pid][cid].inc();
    // wake up rebalancer
    if (poolRebalancer_) {
      poolRebalancer_->wakeUp();
    }
  }

  if (auto eventTracker = getEventTracker()) {
    const auto result =
        handle ? AllocatorApiResult::ALLOCATED : AllocatorApiResult::FAILED;
    eventTracker->record(AllocatorApiEvent::ALLOCATE, key, result, size,
                         expiryTime ? expiryTime - creationTime : 0);
  }

  return handle;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::allocateChainedItem(const ReadHandle& parent,
                                                uint32_t size) {
  if (!parent) {
    throw std::invalid_argument(
        "Cannot call allocate chained item with a empty parent handle!");
  }

  auto it = allocateChainedItemInternal(parent, size);
  if (auto eventTracker = getEventTracker()) {
    const auto result =
        it ? AllocatorApiResult::ALLOCATED : AllocatorApiResult::FAILED;
    eventTracker->record(AllocatorApiEvent::ALLOCATE_CHAINED, parent->getKey(),
                         result, size, parent->getConfiguredTTL().count());
  }
  return it;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::allocateChainedItemInternal(
    const ReadHandle& parent, uint32_t size) {
  util::LatencyTracker tracker{stats().allocateLatency_};

  SCOPE_FAIL { stats_.invalidAllocs.inc(); };

  // number of bytes required for this item
  const auto requiredSize = ChainedItem::getRequiredSize(size);

  const auto pid = allocator_->getAllocInfo(parent->getMemory()).poolId;
  const auto cid = allocator_->getAllocationClassId(pid, requiredSize);

  (*stats_.allocAttempts)[pid][cid].inc();

  void* memory = allocator_->allocate(pid, requiredSize);
  if (memory == nullptr) {
    memory = findEviction(pid, cid);
  }
  if (memory == nullptr) {
    (*stats_.allocFailures)[pid][cid].inc();
    return WriteHandle{};
  }

  SCOPE_FAIL { allocator_->free(memory); };

  auto child = acquire(
      new (memory) ChainedItem(compressor_.compress(parent.getInternal()), size,
                               util::getCurrentTimeSec()));

  if (child) {
    child.markNascent();
    (*stats_.fragmentationSize)[pid][cid].add(
        util::getFragmentation(*this, *child));
  }

  return child;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::addChainedItem(WriteHandle& parent,
                                                WriteHandle child) {
  if (!parent || !child || !child->isChainedItem()) {
    throw std::invalid_argument(
        folly::sformat("Invalid parent or child. parent: {}, child: {}",
                       parent ? parent->toString() : "nullptr",
                       child ? child->toString() : "nullptr"));
  }

  auto l = chainedItemLocks_.lockExclusive(parent->getKey());

  // Insert into secondary lookup table for chained allocation
  auto oldHead = chainedItemAccessContainer_->insertOrReplace(*child);
  if (oldHead) {
    child->asChainedItem().appendChain(oldHead->asChainedItem(), compressor_);
  }

  // Count an item that just became a new parent
  if (!parent->hasChainedItem()) {
    stats_.numChainedParentItems.inc();
  }
  // Parent needs to be marked before inserting child into MM container
  // so the parent-child relationship is established before an eviction
  // can be triggered from the child
  parent->markHasChainedItem();
  // Count a new child
  stats_.numChainedChildItems.inc();

  insertInMMContainer(*child);

  // Increment refcount since this chained item is now owned by the parent
  // Parent will decrement the refcount upon release. Since this is an
  // internal refcount, we dont include it in active handle tracking.
  child->incRef();
  XDCHECK_EQ(2u, child->getRefCount());

  invalidateNvm(*parent);
  if (auto eventTracker = getEventTracker()) {
    eventTracker->record(AllocatorApiEvent::ADD_CHAINED, parent->getKey(),
                         AllocatorApiResult::INSERTED, child->getSize(),
                         child->getConfiguredTTL().count());
  }
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::popChainedItem(WriteHandle& parent) {
  if (!parent || !parent->hasChainedItem()) {
    throw std::invalid_argument(folly::sformat(
        "Invalid parent {}", parent ? parent->toString() : nullptr));
  }

  WriteHandle head;
  { // scope of chained item lock.
    auto l = chainedItemLocks_.lockExclusive(parent->getKey());

    head = findChainedItem(*parent);
    if (head->asChainedItem().getNext(compressor_) != nullptr) {
      chainedItemAccessContainer_->insertOrReplace(
          *head->asChainedItem().getNext(compressor_));
    } else {
      chainedItemAccessContainer_->remove(*head);
      parent->unmarkHasChainedItem();
      stats_.numChainedParentItems.dec();
    }
    head->asChainedItem().setNext(nullptr, compressor_);

    invalidateNvm(*parent);
  }
  const auto res = removeFromMMContainer(*head);
  XDCHECK(res == true);

  // decrement the refcount to indicate this item is unlinked from its parent
  head->decRef();
  stats_.numChainedChildItems.dec();

  if (auto eventTracker = getEventTracker()) {
    eventTracker->record(AllocatorApiEvent::POP_CHAINED, parent->getKey(),
                         AllocatorApiResult::REMOVED, head->getSize(),
                         head->getConfiguredTTL().count());
  }

  return head;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::Key
CacheAllocator<CacheTrait>::getParentKey(const Item& chainedItem) {
  XDCHECK(chainedItem.isChainedItem());
  if (!chainedItem.isChainedItem()) {
    throw std::invalid_argument(folly::sformat(
        "Item must be chained item! Item: {}", chainedItem.toString()));
  }
  return reinterpret_cast<const ChainedItem&>(chainedItem)
      .getParentItem(compressor_)
      .getKey();
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::transferChainLocked(WriteHandle& parent,
                                                     WriteHandle& newParent) {
  // parent must be in a state to not have concurrent readers. Eviction code
  // paths rely on holding the last item handle. Since we hold on to an item
  // handle here, the chain will not be touched by any eviction code path.
  XDCHECK(parent);
  XDCHECK(newParent);
  XDCHECK_EQ(parent->getKey(), newParent->getKey());
  XDCHECK(parent->hasChainedItem());

  if (newParent->hasChainedItem()) {
    throw std::invalid_argument(folly::sformat(
        "New Parent {} has invalid state", newParent->toString()));
  }

  auto headHandle = findChainedItem(*parent);
  XDCHECK(headHandle);

  // remove from the access container since we are changing the key
  chainedItemAccessContainer_->remove(*headHandle);

  // change the key of the chain to have them belong to the new parent.
  ChainedItem* curr = &headHandle->asChainedItem();
  const auto newParentPtr = compressor_.compress(newParent.get());
  while (curr) {
    XDCHECK_EQ(curr == headHandle.get() ? 2u : 1u, curr->getRefCount());
    XDCHECK(curr->isInMMContainer());
    curr->changeKey(newParentPtr);
    curr = curr->getNext(compressor_);
  }

  newParent->markHasChainedItem();
  auto oldHead = chainedItemAccessContainer_->insertOrReplace(*headHandle);
  if (oldHead) {
    throw std::logic_error(
        folly::sformat("Did not expect to find an existing chain for {}",
                       newParent->toString(), oldHead->toString()));
  }
  parent->unmarkHasChainedItem();
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::transferChainAndReplace(
    WriteHandle& parent, WriteHandle& newParent) {
  if (!parent || !newParent) {
    throw std::invalid_argument("invalid parent or new parent");
  }
  { // scope for chained item lock
    auto l = chainedItemLocks_.lockExclusive(parent->getKey());
    transferChainLocked(parent, newParent);
  }

  if (replaceIfAccessible(*parent, *newParent)) {
    newParent.unmarkNascent();
  }
  invalidateNvm(*parent);
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::replaceIfAccessible(Item& oldItem,
                                                     Item& newItem) {
  XDCHECK(!newItem.isAccessible());

  // Inside the access container's lock, this checks if the old item is
  // accessible, and only in that case replaces it. If the old item is not
  // accessible anymore, it may have been replaced or removed earlier and there
  // is no point in proceeding with a move.
  if (!accessContainer_->replaceIfAccessible(oldItem, newItem)) {
    return false;
  }

  // Inside the MM container's lock, this checks if the old item exists to
  // make sure that no other thread removed it, and only then replaces it.
  if (!replaceInMMContainer(oldItem, newItem)) {
    accessContainer_->remove(newItem);
    return false;
  }

  // Replacing into the MM container was successful, but someone could have
  // called insertOrReplace() or remove() before or after the
  // replaceInMMContainer() operation, which would invalidate newItem.
  if (!newItem.isAccessible()) {
    removeFromMMContainer(newItem);
    return false;
  }
  return true;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::replaceChainedItem(Item& oldItem,
                                               WriteHandle newItemHandle,
                                               Item& parent) {
  if (!newItemHandle) {
    throw std::invalid_argument("Empty handle for newItem");
  }
  auto l = chainedItemLocks_.lockExclusive(parent.getKey());

  if (!oldItem.isChainedItem() || !newItemHandle->isChainedItem() ||
      &oldItem.asChainedItem().getParentItem(compressor_) !=
          &newItemHandle->asChainedItem().getParentItem(compressor_) ||
      &oldItem.asChainedItem().getParentItem(compressor_) != &parent ||
      newItemHandle->isInMMContainer() || !oldItem.isInMMContainer()) {
    throw std::invalid_argument(folly::sformat(
        "Invalid args for replaceChainedItem. oldItem={}, newItem={}, "
        "parent={}",
        oldItem.toString(), newItemHandle->toString(), parent.toString()));
  }

  auto oldItemHdl =
      replaceChainedItemLocked(oldItem, std::move(newItemHandle), parent);
  invalidateNvm(parent);
  return oldItemHdl;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::replaceChainedItemLocked(Item& oldItem,
                                                     WriteHandle newItemHdl,
                                                     const Item& parent) {
  XDCHECK(newItemHdl != nullptr);
  XDCHECK_GE(1u, oldItem.getRefCount());

  // grab the handle to the old item so that we can return this. Also, we need
  // to drop the refcount the parent holds on oldItem by manually calling
  // decRef.  To do that safely we need to have a proper outstanding handle.
  auto oldItemHdl = acquire(&oldItem);

  // Replace the old chained item with new item in the MMContainer before we
  // actually replace the old item in the chain

  if (!replaceChainedItemInMMContainer(oldItem, *newItemHdl)) {
    // This should never happen since we currently hold an valid
    // parent handle. None of its chained items can be removed
    throw std::runtime_error(folly::sformat(
        "chained item cannot be replaced in MM container, oldItem={}, "
        "newItem={}, parent={}",
        oldItem.toString(), newItemHdl->toString(), parent.toString()));
  }

  XDCHECK(!oldItem.isInMMContainer());
  XDCHECK(newItemHdl->isInMMContainer());

  auto head = findChainedItem(parent);
  XDCHECK(head != nullptr);
  XDCHECK_EQ(reinterpret_cast<uintptr_t>(
                 &head->asChainedItem().getParentItem(compressor_)),
             reinterpret_cast<uintptr_t>(&parent));

  // if old item is the head, replace the head in the chain and insert into
  // the access container and append its chain.
  if (head.get() == &oldItem) {
    chainedItemAccessContainer_->insertOrReplace(*newItemHdl);
  } else {
    // oldItem is in the middle of the chain, find its previous and fix the
    // links
    auto* prev = &head->asChainedItem();
    auto* curr = prev->getNext(compressor_);
    while (curr != nullptr && curr != &oldItem) {
      prev = curr;
      curr = curr->getNext(compressor_);
    }

    XDCHECK(curr != nullptr);
    prev->setNext(&newItemHdl->asChainedItem(), compressor_);
  }

  newItemHdl->asChainedItem().setNext(
      oldItem.asChainedItem().getNext(compressor_), compressor_);
  oldItem.asChainedItem().setNext(nullptr, compressor_);

  // this should not result in 0 refcount. We are bumping down the internal
  // refcount. If it did, we would leak an item.
  oldItem.decRef();
  XDCHECK_LT(0u, oldItem.getRefCount()) << oldItem.toString();

  // increment refcount to indicate parent owns this similar to addChainedItem
  // Since this is an internal refcount, we dont include it in active handle
  // tracking.

  newItemHdl->incRef();
  return oldItemHdl;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::releaseBackToAllocator(Item& it,
                                                        RemoveContext ctx,
                                                        bool callDestructor,
                                                        const Item* toRelease,
                                                        bool recycle) {
  if (!it.isDrained()) {
    throw std::runtime_error(
        folly::sformat("cannot release this item: {}", it.toString()));
  }

  const auto allocInfo = allocator_->getAllocInfo(it.getMemory());

  if (ctx == RemoveContext::kEviction) {
    const auto timeNow = util::getCurrentTimeSec();
    const auto refreshTime = timeNow - it.getLastAccessTime();
    const auto lifeTime = timeNow - it.getCreationTime();
    stats_.ramEvictionAgeSecs_.trackValue(refreshTime);
    stats_.ramItemLifeTimeSecs_.trackValue(lifeTime);
    stats_.perPoolEvictionAgeSecs_[allocInfo.poolId].trackValue(refreshTime);
  }

  (*stats_.fragmentationSize)[allocInfo.poolId][allocInfo.classId].sub(
      util::getFragmentation(*this, it));

  // Chained items can only end up in this place if the user has allocated
  // memory for a chained item but has decided not to insert the chained item
  // to a parent item and instead drop the chained item handle. In this case,
  // we free the chained item directly without calling remove callback.
  if (it.isChainedItem()) {
    if (recycle) {
      throw std::runtime_error(folly::sformat(
          "Can not recycle a chained item {}, toRelease", it.toString(),
          toRelease ? "" : toRelease->toString()));
    }

    allocator_->free(&it);
    return &it == toRelease || !toRelease;
  }

  if (callDestructor && config_.removeCb) {
    config_.removeCb(RemoveCbData{ctx, it, viewAsChainedAllocsRange(it)});
  }

  // only skip destructor for evicted items that are either in the queue to put
  // into nvm or already in nvm
  if (callDestructor && config_.itemDestructor &&
      (ctx != RemoveContext::kEviction || !it.isNvmClean() ||
       it.isNvmEvicted())) {
    try {
      config_.itemDestructor(DestructorData{
          ctx, it, viewAsChainedAllocsRange(it), allocInfo.poolId});
      stats().numRamDestructorCalls.inc();
    } catch (const std::exception& e) {
      stats().numDestructorExceptions.inc();
      XLOG_EVERY_N(INFO, 100)
          << "Catch exception from user's item destructor: " << e.what();
    }
  }

  bool res = !toRelease; // for nullptr, we always succeed in releasing

  // Free chained allocs if there are any
  if (it.hasChainedItem()) {
    // At this point, the parent is only accessible within this thread
    // and thus no one else can add or remove any chained items associated
    // with this parent. So we're free to go through the list and free
    // chained items one by one.
    XDCHECK(!it.isExclusive());
    auto headHandle = findChainedItem(it);
    ChainedItem* head = &headHandle.get()->asChainedItem();
    XDCHECK(!head->isExclusive());
    headHandle.reset();

    if (head == nullptr || &head->getParentItem(compressor_) != &it) {
      throw std::runtime_error(folly::sformat(
          "Mismatch parent pointer. This should not happen. Key: {}",
          it.getKey()));
    }

    if (!chainedItemAccessContainer_->remove(*head)) {
      throw std::runtime_error(folly::sformat(
          "Chained item associated with {} cannot be removed from hash table "
          "This should not happen here.",
          it.getKey()));
    }

    while (head) {
      auto next = head->getNext(compressor_);

      const auto childInfo =
          allocator_->getAllocInfo(static_cast<const void*>(head));
      (*stats_.fragmentationSize)[childInfo.poolId][childInfo.classId].sub(
          util::getFragmentation(*this, *head));

      removeFromMMContainer(*head);

      // Decref and check if we were the last reference.
      const auto childRef = head->decRef();

      if (childRef != 0) {
        throw std::runtime_error(folly::sformat(
            "chained item refcount is not zero. We cannot proceed! "
            "Ref: {}, Chained Item: {}",
            childRef, head->toString()));
      }

      if (head == toRelease) {
        res = true;
      }

      if (!recycle || (head != toRelease && toRelease)) {
        allocator_->free(head);
      }

      stats_.numChainedChildItems.dec();
      head = next;
    }
    stats_.numChainedParentItems.dec();
  }

  if (&it == toRelease) {
    res = true;
  }

  if (!recycle || (&it != toRelease && toRelease)) {
    XDCHECK(it.isDrained());
    allocator_->free(&it);
  }

  return res;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::incRef(Item& it) {
  if (it.incRef()) {
    ++handleCount_.tlStats();
    return true;
  }
  return false;
}

template <typename CacheTrait>
RefcountWithFlags::Value CacheAllocator<CacheTrait>::decRef(Item& it) {
  const auto ret = it.decRef();
  // do this after we ensured that we incremented a reference.
  --handleCount_.tlStats();
  return ret;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::acquire(Item* it) {
  if (UNLIKELY(!it)) {
    return WriteHandle{};
  }

  SCOPE_FAIL { stats_.numRefcountOverflow.inc(); };

  if (LIKELY(incRef(*it))) {
    return WriteHandle{it, *this};
  } else {
    // item is being evicted
    return WriteHandle{};
  }
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::release(Item* it, bool isNascent) {
  // decrement the reference and if it drops to 0, release it back to the
  // memory allocator, and invoke the removal callback if there is one.
  if (UNLIKELY(!it)) {
    return;
  }

  const auto ref = decRef(*it);

  if (UNLIKELY(ref == 0)) {
    const auto res =
        releaseBackToAllocator(*it, RemoveContext::kNormal, !isNascent);
    XDCHECK(res);
  }
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::removeFromMMContainer(Item& item) {
  // remove it from the mm container.
  if (item.isInMMContainer()) {
    auto& mmContainer = getMMContainer(item);
    return mmContainer.remove(item);
  }
  return false;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::replaceInMMContainer(Item& oldItem,
                                                      Item& newItem) {
  auto& oldContainer = getMMContainer(oldItem);
  auto& newContainer = getMMContainer(newItem);
  if (&oldContainer == &newContainer) {
    return oldContainer.replace(oldItem, newItem);
  } else {
    return oldContainer.remove(oldItem) && newContainer.add(newItem);
  }
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::replaceChainedItemInMMContainer(
    Item& oldItem, Item& newItem) {
  auto& oldMMContainer = getMMContainer(oldItem);
  auto& newMMContainer = getMMContainer(newItem);
  if (&oldMMContainer == &newMMContainer) {
    return oldMMContainer.replace(oldItem, newItem);
  } else {
    if (!oldMMContainer.remove(oldItem)) {
      return false;
    }

    // This cannot fail because a new item should not have been inserted
    const auto newRes = newMMContainer.add(newItem);
    XDCHECK(newRes);
    return true;
  }
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::insertInMMContainer(Item& item) {
  XDCHECK(!item.isInMMContainer());
  auto& mmContainer = getMMContainer(item);
  if (!mmContainer.add(item)) {
    throw std::runtime_error(folly::sformat(
        "Invalid state. Node {} was already in the container.", &item));
  }
}

/**
 * There is a potential race with inserts and removes that. While T1 inserts
 * the key, there is T2 that removes the key. There can be an interleaving of
 * inserts and removes into the MM and Access Conatainers.It does not matter
 * what the outcome of this race is (ie  key can be present or not present).
 * However, if the key is not accessible, it should also not be in
 * MMContainer. To ensure that, we always add to MMContainer on inserts before
 * adding to the AccessContainer. Both the code paths on success/failure,
 * preserve the appropriate state in the MMContainer. Note that this insert
 * will also race with the removes we do in SlabRebalancing code paths.
 */

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::insert(const WriteHandle& handle) {
  return insertImpl(handle, AllocatorApiEvent::INSERT);
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::insertImpl(const WriteHandle& handle,
                                            AllocatorApiEvent event) {
  XDCHECK(handle);
  XDCHECK(event == AllocatorApiEvent::INSERT ||
          event == AllocatorApiEvent::INSERT_FROM_NVM);
  if (handle->isAccessible()) {
    throw std::invalid_argument("Handle is already accessible");
  }

  if (nvmCache_ != nullptr && !handle->isNvmClean()) {
    throw std::invalid_argument("Can't use insert API with nvmCache enabled");
  }

  // insert into the MM container before we make it accessible. Find will
  // return this item as soon as it is accessible.
  insertInMMContainer(*(handle.getInternal()));

  AllocatorApiResult result;
  if (!accessContainer_->insert(*(handle.getInternal()))) {
    // this should destroy the handle and release it back to the allocator.
    removeFromMMContainer(*(handle.getInternal()));
    result = AllocatorApiResult::FAILED;
  } else {
    handle.unmarkNascent();
    result = AllocatorApiResult::INSERTED;
  }

  if (auto eventTracker = getEventTracker()) {
    eventTracker->record(event, handle->getKey(), result, handle->getSize(),
                         handle->getConfiguredTTL().count());
  }

  return result == AllocatorApiResult::INSERTED;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::insertOrReplace(const WriteHandle& handle) {
  XDCHECK(handle);
  if (handle->isAccessible()) {
    throw std::invalid_argument("Handle is already accessible");
  }

  HashedKey hk{handle->getKey()};

  insertInMMContainer(*(handle.getInternal()));
  WriteHandle replaced;
  try {
    auto lock = nvmCache_ ? nvmCache_->getItemDestructorLock(hk)
                          : std::unique_lock<std::mutex>();

    replaced = accessContainer_->insertOrReplace(*(handle.getInternal()));

    if (replaced && replaced->isNvmClean() && !replaced->isNvmEvicted()) {
      // item is to be replaced and the destructor will be executed
      // upon memory released, mark it in nvm to avoid destructor
      // executed from nvm
      nvmCache_->markNvmItemRemovedLocked(hk);
    }
  } catch (const std::exception&) {
    removeFromMMContainer(*(handle.getInternal()));
    if (auto eventTracker = getEventTracker()) {
      eventTracker->record(AllocatorApiEvent::INSERT_OR_REPLACE,
                           handle->getKey(),
                           AllocatorApiResult::FAILED,
                           handle->getSize(),
                           handle->getConfiguredTTL().count());
    }
    throw;
  }

  // Remove from LRU as well if we do have a handle of old item
  if (replaced) {
    removeFromMMContainer(*replaced);
  }

  if (UNLIKELY(nvmCache_ != nullptr)) {
    // We can avoid nvm delete only if we have non nvm clean item in cache.
    // In all other cases we must enqueue delete.
    if (!replaced || replaced->isNvmClean()) {
      nvmCache_->remove(hk, nvmCache_->createDeleteTombStone(hk));
    }
  }

  handle.unmarkNascent();

  if (auto eventTracker = getEventTracker()) {
    XDCHECK(handle);
    const auto result =
        replaced ? AllocatorApiResult::REPLACED : AllocatorApiResult::INSERTED;
    eventTracker->record(AllocatorApiEvent::INSERT_OR_REPLACE, handle->getKey(),
                         result, handle->getSize(),
                         handle->getConfiguredTTL().count());
  }

  return replaced;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::moveRegularItem(WriteHandle&& oldItemHdl,
                                                 WriteHandle& newItemHdl) {
  XDCHECK(config_.moveCb);
  util::LatencyTracker tracker{stats_.moveRegularLatency_};

  auto& oldItem = *oldItemHdl;
  if (!oldItem.isAccessible() || oldItem.isExpired()) {
    return false;
  }

  XDCHECK_EQ(newItemHdl->getSize(), oldItem.getSize());
  XDCHECK_EQ(reinterpret_cast<uintptr_t>(&getMMContainer(oldItem)),
             reinterpret_cast<uintptr_t>(&getMMContainer(*newItemHdl)));

  // take care of the flags before we expose the item to be accessed. this
  // will ensure that when another thread removes the item from RAM, we issue
  // a delete accordingly. See D7859775 for an example
  if (oldItem.isNvmClean()) {
    newItemHdl->markNvmClean();
  }

  // Execute the move callback. We cannot make any guarantees about the
  // consistency of the old item beyond this point, because the callback can
  // do more than a simple memcpy() e.g. update external references. If there
  // are any remaining handles to the old item, it is the caller's
  // responsibility to invalidate them. The move can only fail after this
  // statement if the old item has been removed or replaced, in which case it
  // should be fine for it to be left in an inconsistent state.
  config_.moveCb(oldItem, *newItemHdl, nullptr);

  // Inside the access container's lock, this checks if the old item is
  // accessible and its refcount is one. If the item is not accessible,
  // there is no point to replace it since it had already been removed
  // or in the process of being removed. If the item is in cache but the
  // refcount is non-one, it means user could be attempting to remove
  // this item through an API such as remove(itemHandle). In this case,
  // it is unsafe to replace the old item with a new one, so we should
  // also abort.
  if (!accessContainer_->replaceIf(oldItem, *newItemHdl,
                                   itemSlabReleasePredicate)) {
    return false;
  }

  // Inside the MM container's lock, this checks if the old item exists to
  // make sure that no other thread removed it, and only then replaces it.
  if (!replaceInMMContainer(oldItem, *newItemHdl)) {
    accessContainer_->remove(*newItemHdl);
    return false;
  }

  // Replacing into the MM container was successful, but someone could have
  // called insertOrReplace() or remove() before or after the
  // replaceInMMContainer() operation, which would invalidate newItemHdl.
  if (!newItemHdl->isAccessible()) {
    removeFromMMContainer(*newItemHdl);
    return false;
  }

  // no one can add or remove chained items at this point
  if (oldItem.hasChainedItem()) {
    XDCHECK_EQ(1u, oldItemHdl->getRefCount()) << oldItemHdl->toString();
    XDCHECK(!newItemHdl->hasChainedItem()) << newItemHdl->toString();
    try {
      auto l = chainedItemLocks_.lockExclusive(oldItem.getKey());
      transferChainLocked(oldItemHdl, newItemHdl);
    } catch (const std::exception& e) {
      // this should never happen because we drained all the handles.
      XLOGF(DFATAL, "{}", e.what());
      throw;
    }

    XDCHECK(!oldItem.hasChainedItem());
    XDCHECK(newItemHdl->hasChainedItem());
  }
  newItemHdl.unmarkNascent();

  auto* item = oldItemHdl.release();
  XDCHECK(item == &oldItem);

  auto ref = decRef(*item);
  XDCHECK(ref == 0);
  const auto res =
      releaseBackToAllocator(*item, RemoveContext::kEviction, false);
  XDCHECK(res);

  return true;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::ReadHandle
CacheAllocator<CacheTrait>::validateAndGetParentHandleForChainedMoveLocked(
    const ChainedItem& item, const Key& parentKey) {
  ReadHandle parentHandle{};
  try {
    parentHandle = findInternal(parentKey);
    // If the parent is not the same as the parent of the chained item,
    // it means someone has replaced our old parent already. So we abort.
    if (!parentHandle ||
        parentHandle.get() != &item.getParentItem(compressor_)) {
      return {};
    }
  } catch (const exception::RefcountOverflow&) {
    return {};
  }

  return parentHandle;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::moveChainedItem(ChainedItem& oldItem,
                                                 WriteHandle&& parentHandle,
                                                 WriteHandle& newItemHdl) {
  XDCHECK(config_.moveCb);
  util::LatencyTracker tracker{stats_.moveChainedLatency_};

  XDCHECK(!parentHandle->isExclusive());

  const std::string parentKey = parentHandle->getKey().str();
  auto l = chainedItemLocks_.lockExclusive(parentKey);

  // verify old item under the lock
  if (validateAndGetParentHandleForChainedMoveLocked(oldItem, parentKey) !=
      parentHandle) {
    return false;
  }

  // now that we now, that the item is still in the chain we know it cannot be
  // under eviction - we would not be able to get parent item in that case
  XDCHECK(!oldItem.isExclusive());

  // once we have the moving sync and valid parent for the old item, check if
  // the original allocation was made correctly. If not, we destroy the
  // allocation to indicate a retry to moving logic above.
  if (reinterpret_cast<uintptr_t>(
          &newItemHdl->asChainedItem().getParentItem(compressor_)) !=
      reinterpret_cast<uintptr_t>(&parentHandle->asChainedItem())) {
    newItemHdl.reset();
    return false;
  }

  XDCHECK_EQ(reinterpret_cast<uintptr_t>(
                 &newItemHdl->asChainedItem().getParentItem(compressor_)),
             reinterpret_cast<uintptr_t>(&parentHandle->asChainedItem()));

  // In case someone else had removed this chained item from its parent by now
  // So we check again to see if the it has been unlinked from its parent
  if (!oldItem.isInMMContainer()) {
    return false;
  }

  auto parentPtr = parentHandle.getInternal();

  XDCHECK_EQ(reinterpret_cast<uintptr_t>(parentPtr),
             reinterpret_cast<uintptr_t>(&oldItem.getParentItem(compressor_)));

  // Invoke the move callback to fix up any user data related to the chain
  config_.moveCb(oldItem, *newItemHdl, parentPtr);

  // Replace the new item in the position of the old one before both in the
  // parent's chain and the MMContainer.
  auto oldItemHandle =
      replaceChainedItemLocked(oldItem, std::move(newItemHdl), *parentHandle);
  XDCHECK(!oldItemHandle->isExclusive());
  XDCHECK(!oldItemHandle->isInMMContainer());

  // oldItemHandle is the last handle
  auto* item = oldItemHandle.release();
  auto ref = decRef(*item);
  XDCHECK(ref == 0);
  const auto res =
      releaseBackToAllocator(*item, RemoveContext::kEviction, false);
  XDCHECK(res);

  return true;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::NvmCacheT::PutToken
CacheAllocator<CacheTrait>::createPutToken(Item& item) {
  const bool evictToNvmCache = shouldWriteToNvmCache(item);
  return evictToNvmCache ? nvmCache_->createPutToken(item.getKey())
                         : typename NvmCacheT::PutToken{};
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::unlinkItemExclusive(Item& it) {
  XDCHECK(it.isExclusive());
  XDCHECK(it.getRefCount() == 0);

  auto removed = accessContainer_->remove(it);
  if (removed) {
    removeFromMMContainer(it);
  }

  const auto ref = it.unmarkExclusive();

  // Since we managed to mark the item as exclusive we must be the only
  // owner of the item. However, some other thread might attempt to remove
  // or replace the item concurrently. In that case, we might fail to
  // remove the item from AC. It's still OK to recycle the item, we
  // just need to make sure the item is not present in the MMContainer.
  XDCHECK(ref == 0u || it.isOnlyInMMContainer());

  if (it.isOnlyInMMContainer()) {
    removeFromMMContainer(it);
  }
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::Item*
CacheAllocator<CacheTrait>::findEviction(PoolId pid, ClassId cid) {
  auto& mmContainer = getMMContainer(pid, cid);

  // Keep searching for a candidate until we were able to evict it
  // or until the search limit has been exhausted
  unsigned int searchTries = 0;
  while ((config_.evictionSearchTries == 0 ||
          config_.evictionSearchTries > searchTries)) {
    Item* toRecycle = nullptr;
    Item* candidate = nullptr;
    typename NvmCacheT::PutToken token;

    mmContainer.withEvictionIterator([this, pid, cid, &candidate, &toRecycle,
                                      &searchTries, &mmContainer,
                                      &token](auto&& itr) {
      if (!itr) {
        ++searchTries;
        (*stats_.evictionAttempts)[pid][cid].inc();
        return;
      }

      while ((config_.evictionSearchTries == 0 ||
              config_.evictionSearchTries > searchTries) &&
             itr) {
        ++searchTries;
        (*stats_.evictionAttempts)[pid][cid].inc();

        auto* toRecycle_ = itr.get();
        auto* candidate_ =
            toRecycle_->isChainedItem()
                ? &toRecycle_->asChainedItem().getParentItem(compressor_)
                : toRecycle_;

        token = createPutToken(*candidate_);

        if (shouldWriteToNvmCache(*candidate_) && !token.isValid()) {
          stats_.evictFailConcurrentFill.inc();
        } else if (candidate_->markExclusive()) {
          // markExclusive to make sure no other thead is evicting the item
          // nor holding a handle to that item

          toRecycle = toRecycle_;
          candidate = candidate_;

          // Check if parent changed for chained items - if yes, we cannot
          // remove the child from the mmContainer as we will not be evicting
          // it. We could abort right here, but we need to cleanup in case
          // unmarkExclusive() returns 0 - so just go through normal path.
          if (!toRecycle_->isChainedItem() ||
              &toRecycle->asChainedItem().getParentItem(compressor_) ==
                  candidate)
            mmContainer.remove(itr);
          return;
        }

        if (candidate_->hasChainedItem()) {
          stats_.evictFailParentAC.inc();
        } else {
          stats_.evictFailAC.inc();
        }

        ++itr;
      }
    });

    if (!toRecycle)
      continue;

    XDCHECK(toRecycle);
    XDCHECK(candidate);

    // for chained items, the ownership of the parent can change. We try to
    // evict what we think as parent and see if the eviction of parent
    // recycles the child we intend to.
    unlinkItemExclusive(*candidate);

    if (token.isValid() && shouldWriteToNvmCacheExclusive(*candidate)) {
      auto handle = acquire(candidate);
      nvmCache_->put(handle, std::move(token));
      auto r = decRef(*handle.release());
      XDCHECK(r == 0u);
    }

    // recycle the item. it's safe to do so, even if toReleaseHandle was
    // NULL. If `ref` == 0 then it means that we are the last holder of
    // that item.
    if (candidate->hasChainedItem()) {
      (*stats_.chainedItemEvictions)[pid][cid].inc();
    } else {
      (*stats_.regularItemEvictions)[pid][cid].inc();
    }

    if (auto eventTracker = getEventTracker()) {
      eventTracker->record(AllocatorApiEvent::DRAM_EVICT, candidate->getKey(),
                           AllocatorApiResult::EVICTED, candidate->getSize(),
                           candidate->getConfiguredTTL().count());
    }

    // check if by releasing the item we intend to, we actually
    // recycle the candidate.
    auto recycled = releaseBackToAllocator(*candidate, RemoveContext::kEviction,
                                           /* callDestructor */ true, toRecycle,
                                           /* recycle */ true);
    if (recycled) {
      return toRecycle;
    }
  }
  return nullptr;
}

template <typename CacheTrait>
folly::Range<typename CacheAllocator<CacheTrait>::ChainedItemIter>
CacheAllocator<CacheTrait>::viewAsChainedAllocsRange(const Item& parent) const {
  return parent.hasChainedItem()
             ? folly::Range<ChainedItemIter>{ChainedItemIter{
                                                 findChainedItem(parent).get(),
                                                 compressor_},
                                             ChainedItemIter{}}
             : folly::Range<ChainedItemIter>{};
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::shouldWriteToNvmCache(const Item& item) {
  // write to nvmcache when it is enabled and the item says that it is not
  // nvmclean or evicted by nvm while present in DRAM.
  bool doWrite = nvmCache_ && nvmCache_->isEnabled();
  if (!doWrite) {
    return false;
  }

  doWrite = !item.isExpired();
  if (!doWrite) {
    stats_.numNvmRejectsByExpiry.inc();
    return false;
  }

  doWrite = (!item.isNvmClean() || item.isNvmEvicted());
  if (!doWrite) {
    stats_.numNvmRejectsByClean.inc();
    return false;
  }
  return true;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::shouldWriteToNvmCacheExclusive(
    const Item& item) {
  auto chainedItemRange = viewAsChainedAllocsRange(item);

  if (nvmAdmissionPolicy_ &&
      !nvmAdmissionPolicy_->accept(item, chainedItemRange)) {
    stats_.numNvmRejectsByAP.inc();
    return false;
  }

  return true;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::RemoveRes
CacheAllocator<CacheTrait>::remove(typename Item::Key key) {
  // While we issue this delete, there can be potential races that change the
  // state of the cache between ram and nvm. If we find the item in RAM and
  // obtain a handle, the situation is simpler. The complicated ones are the
  // following scenarios where when the delete checks RAM, we don't find
  // anything in RAM. The end scenario is that in the absence of any
  // concurrent inserts, after delete, there should be nothing in nvm and ram.
  //
  // == Racing async fill from nvm with delete ==
  // 1. T1 finds nothing in ram and issues a nvmcache look that is async. We
  // enqueue the get holding the fill lock and drop it.
  // 2. T2 finds nothing in ram, enqueues delete to nvmcache.
  // 3. T1's async fetch finishes and fills the item in cache, but right
  // before the delete is enqueued above
  //
  // To deal with this race, we first enqueue the nvmcache delete tombstone
  // and  when we finish the async fetch, we check if a tombstone was enqueued
  // meanwhile and cancel the fill.
  //
  // == Racing async fill from nvm with delete ==
  // there is a key in nvmcache and nothing in RAM.
  // 1.  T1 issues delete while nothing is in RAM and enqueues nvm cache
  // remove
  // 2. before the nvmcache remove gets enqueued, T2 does a find() that
  // fetches from nvm.
  // 3. T2 inserts in cache from nvmcache and T1 observes that item and tries
  // to remove it only from RAM.
  //
  //  to fix this, we do the nvmcache remove always the last thing and enqueue
  //  a tombstone to avoid concurrent fills while we are in the process of
  //  doing the nvmcache remove.
  //
  // == Racing eviction with delete ==
  // 1. T1 is evicting an item, trying to remove from the hashtable and is in
  // the process  of enqueing the put to nvmcache.
  // 2. T2 is removing and finds nothing in ram, enqueue the nvmcache delete.
  // The delete to nvmcache gets enqueued after T1 fills in ram.
  //
  // If T2 finds the item in ram, eviction can not proceed and the race does
  // not exist. If T2 does not find anything in RAM, it is likely that T1 is
  // in the process of issuing an nvmcache put. In this case, T1's nvmcache
  // put will check if there was a delete enqueued while the eviction was in
  // flight after removing from the hashtable.
  //
  stats_.numCacheRemoves.inc();
  HashedKey hk{key};

  using Guard = typename NvmCacheT::DeleteTombStoneGuard;
  auto tombStone = nvmCache_ ? nvmCache_->createDeleteTombStone(hk) : Guard{};

  auto handle = findInternal(key);
  if (!handle) {
    if (nvmCache_) {
      nvmCache_->remove(hk, std::move(tombStone));
    }
    if (auto eventTracker = getEventTracker()) {
      eventTracker->record(AllocatorApiEvent::REMOVE, key,
                           AllocatorApiResult::NOT_FOUND);
    }
    return RemoveRes::kNotFoundInRam;
  }

  return removeImpl(hk, *handle, std::move(tombStone));
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::removeFromRamForTesting(
    typename Item::Key key) {
  return removeImpl(HashedKey{key}, *findInternal(key), DeleteTombStoneGuard{},
                    false /* removeFromNvm */) == RemoveRes::kSuccess;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::removeFromNvmForTesting(
    typename Item::Key key) {
  if (nvmCache_) {
    HashedKey hk{key};
    nvmCache_->remove(hk, nvmCache_->createDeleteTombStone(hk));
  }
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::pushToNvmCacheFromRamForTesting(
    typename Item::Key key) {
  auto handle = findInternal(key);

  if (handle && nvmCache_ && shouldWriteToNvmCache(*handle) &&
      shouldWriteToNvmCacheExclusive(*handle)) {
    nvmCache_->put(handle, nvmCache_->createPutToken(handle->getKey()));
    return true;
  }
  return false;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::flushNvmCache() {
  if (nvmCache_) {
    nvmCache_->flushPendingOps();
  }
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::RemoveRes
CacheAllocator<CacheTrait>::remove(AccessIterator& it) {
  stats_.numCacheRemoves.inc();
  if (auto eventTracker = getEventTracker()) {
    eventTracker->record(AllocatorApiEvent::REMOVE, it->getKey(),
                         AllocatorApiResult::REMOVED, it->getSize(),
                         it->getConfiguredTTL().count());
  }
  HashedKey hk{it->getKey()};
  auto tombstone =
      nvmCache_ ? nvmCache_->createDeleteTombStone(hk) : DeleteTombStoneGuard{};
  return removeImpl(hk, *it, std::move(tombstone));
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::RemoveRes
CacheAllocator<CacheTrait>::remove(const ReadHandle& it) {
  stats_.numCacheRemoves.inc();
  if (!it) {
    throw std::invalid_argument("Trying to remove a null item handle");
  }
  HashedKey hk{it->getKey()};
  auto tombstone =
      nvmCache_ ? nvmCache_->createDeleteTombStone(hk) : DeleteTombStoneGuard{};
  return removeImpl(hk, *(it.getInternal()), std::move(tombstone));
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::RemoveRes
CacheAllocator<CacheTrait>::removeImpl(HashedKey hk,
                                       Item& item,
                                       DeleteTombStoneGuard tombstone,
                                       bool removeFromNvm,
                                       bool recordApiEvent) {
  bool success = false;
  {
    auto lock = nvmCache_ ? nvmCache_->getItemDestructorLock(hk)
                          : std::unique_lock<std::mutex>();

    success = accessContainer_->remove(item);

    if (removeFromNvm && success && item.isNvmClean() && !item.isNvmEvicted()) {
      // item is to be removed and the destructor will be executed
      // upon memory released, mark it in nvm to avoid destructor
      // executed from nvm
      nvmCache_->markNvmItemRemovedLocked(hk);
    }
  }
  XDCHECK(!item.isAccessible());

  // remove it from the mm container. this will be no-op if it is already
  // removed.
  removeFromMMContainer(item);

  // Enqueue delete to nvmCache if we know from the item that it was pulled in
  // from NVM. If the item was not pulled in from NVM, it is not possible to
  // have it be written to NVM.
  if (removeFromNvm && item.isNvmClean()) {
    XDCHECK(tombstone);
    nvmCache_->remove(hk, std::move(tombstone));
  }

  auto eventTracker = getEventTracker();
  if (recordApiEvent && eventTracker) {
    const auto result =
        success ? AllocatorApiResult::REMOVED : AllocatorApiResult::NOT_FOUND;
    eventTracker->record(AllocatorApiEvent::REMOVE, item.getKey(), result,
                         item.getSize(), item.getConfiguredTTL().count());
  }

  // the last guy with reference to the item will release it back to the
  // allocator.
  if (success) {
    stats_.numCacheRemoveRamHits.inc();
    return RemoveRes::kSuccess;
  }
  return RemoveRes::kNotFoundInRam;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::invalidateNvm(Item& item) {
  if (nvmCache_ != nullptr && item.isAccessible() && item.isNvmClean()) {
    HashedKey hk{item.getKey()};
    {
      auto lock = nvmCache_->getItemDestructorLock(hk);
      if (!item.isNvmEvicted() && item.isNvmClean() && item.isAccessible()) {
        // item is being updated and invalidated in nvm. Mark the item to avoid
        // destructor to be executed from nvm
        nvmCache_->markNvmItemRemovedLocked(hk);
      }
      item.unmarkNvmClean();
    }
    nvmCache_->remove(hk, nvmCache_->createDeleteTombStone(hk));
  }
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::MMContainer&
CacheAllocator<CacheTrait>::getMMContainer(const Item& item) const noexcept {
  const auto allocInfo =
      allocator_->getAllocInfo(static_cast<const void*>(&item));
  return getMMContainer(allocInfo.poolId, allocInfo.classId);
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::MMContainer&
CacheAllocator<CacheTrait>::getMMContainer(PoolId pid,
                                           ClassId cid) const noexcept {
  XDCHECK_LT(static_cast<size_t>(pid), mmContainers_.size());
  XDCHECK_LT(static_cast<size_t>(cid), mmContainers_[pid].size());
  return *mmContainers_[pid][cid];
}

template <typename CacheTrait>
MMContainerStat CacheAllocator<CacheTrait>::getMMContainerStat(
    PoolId pid, ClassId cid) const noexcept {
  if (static_cast<size_t>(pid) >= mmContainers_.size()) {
    return MMContainerStat{};
  }
  if (static_cast<size_t>(cid) >= mmContainers_[pid].size()) {
    return MMContainerStat{};
  }
  return mmContainers_[pid][cid] ? mmContainers_[pid][cid]->getStats()
                                 : MMContainerStat{};
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::ReadHandle
CacheAllocator<CacheTrait>::peek(typename Item::Key key) {
  return findInternal(key);
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::couldExistFast(typename Item::Key key) {
  // At this point, a key either definitely exists or does NOT exist in cache
  auto handle = findFastInternal(key, AccessMode::kRead);
  if (handle) {
    if (handle->isExpired()) {
      return false;
    }
    return true;
  }

  if (!nvmCache_) {
    return false;
  }

  // When we have to go to NvmCache, we can only probalistically determine
  // if a key could possibly exist in cache, or definitely NOT exist.
  return nvmCache_->couldExistFast(HashedKey{key});
}

template <typename CacheTrait>
std::pair<typename CacheAllocator<CacheTrait>::ReadHandle,
          typename CacheAllocator<CacheTrait>::ReadHandle>
CacheAllocator<CacheTrait>::inspectCache(typename Item::Key key) {
  std::pair<ReadHandle, ReadHandle> res;
  res.first = findInternal(key);
  res.second = nvmCache_ ? nvmCache_->peek(key) : nullptr;
  return res;
}

// findFast and find() are the most performance critical parts of
// CacheAllocator. Hence the sprinkling of UNLIKELY/LIKELY to tell the
// compiler which executions we don't want to optimize on.
template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::findFastInternal(typename Item::Key key,
                                             AccessMode mode) {
  auto handle = findInternal(key);

  stats_.numCacheGets.inc();
  if (UNLIKELY(!handle)) {
    stats_.numCacheGetMiss.inc();
    return handle;
  }

  markUseful(handle, mode);
  return handle;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::findFastImpl(typename Item::Key key,
                                         AccessMode mode) {
  auto handle = findFastInternal(key, mode);
  auto eventTracker = getEventTracker();
  if (UNLIKELY(eventTracker != nullptr)) {
    if (handle) {
      eventTracker->record(AllocatorApiEvent::FIND_FAST, key,
                           AllocatorApiResult::FOUND,
                           folly::Optional<uint32_t>(handle->getSize()),
                           handle->getConfiguredTTL().count());
    } else {
      eventTracker->record(AllocatorApiEvent::FIND_FAST, key,
                           AllocatorApiResult::NOT_FOUND);
    }
  }
  return handle;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::ReadHandle
CacheAllocator<CacheTrait>::findFast(typename Item::Key key) {
  return findFastImpl(key, AccessMode::kRead);
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::findFastToWrite(typename Item::Key key,
                                            bool doNvmInvalidation) {
  auto handle = findFastImpl(key, AccessMode::kWrite);
  if (handle == nullptr) {
    return nullptr;
  }
  if (doNvmInvalidation) {
    invalidateNvm(*handle);
  }
  return handle;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::findImpl(typename Item::Key key, AccessMode mode) {
  auto handle = findFastInternal(key, mode);

  if (handle) {
    if (UNLIKELY(handle->isExpired())) {
      // update cache miss stats if the item has already been expired.
      stats_.numCacheGetMiss.inc();
      stats_.numCacheGetExpiries.inc();
      auto eventTracker = getEventTracker();
      if (UNLIKELY(eventTracker != nullptr)) {
        eventTracker->record(AllocatorApiEvent::FIND, key,
                             AllocatorApiResult::NOT_FOUND);
      }
      WriteHandle ret;
      ret.markExpired();
      return ret;
    }

    auto eventTracker = getEventTracker();
    if (UNLIKELY(eventTracker != nullptr)) {
      eventTracker->record(AllocatorApiEvent::FIND, key,
                           AllocatorApiResult::FOUND, handle->getSize(),
                           handle->getConfiguredTTL().count());
    }
    return handle;
  }

  auto eventResult = AllocatorApiResult::NOT_FOUND;

  if (nvmCache_) {
    handle = nvmCache_->find(HashedKey{key});
    eventResult = AllocatorApiResult::NOT_FOUND_IN_MEMORY;
  }

  auto eventTracker = getEventTracker();
  if (UNLIKELY(eventTracker != nullptr)) {
    eventTracker->record(AllocatorApiEvent::FIND, key, eventResult);
  }

  return handle;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::findToWrite(typename Item::Key key,
                                        bool doNvmInvalidation) {
  auto handle = findImpl(key, AccessMode::kWrite);
  if (handle == nullptr) {
    return nullptr;
  }
  if (doNvmInvalidation) {
    invalidateNvm(*handle);
  }
  return handle;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::ReadHandle
CacheAllocator<CacheTrait>::find(typename Item::Key key) {
  return findImpl(key, AccessMode::kRead);
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::markUseful(const ReadHandle& handle,
                                            AccessMode mode) {
  if (!handle) {
    return;
  }

  auto& item = *(handle.getInternal());
  bool recorded = recordAccessInMMContainer(item, mode);

  // if parent is not recorded, skip children as well when the config is set
  if (LIKELY(!item.hasChainedItem() ||
             (!recorded && config_.isSkipPromoteChildrenWhenParentFailed()))) {
    return;
  }

  forEachChainedItem(item, [this, mode](ChainedItem& chainedItem) {
    recordAccessInMMContainer(chainedItem, mode);
  });
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::recordAccessInMMContainer(Item& item,
                                                           AccessMode mode) {
  const auto allocInfo =
      allocator_->getAllocInfo(static_cast<const void*>(&item));
  (*stats_.cacheHits)[allocInfo.poolId][allocInfo.classId].inc();

  // track recently accessed items if needed
  if (UNLIKELY(config_.trackRecentItemsForDump)) {
    ring_->trackItem(reinterpret_cast<uintptr_t>(&item), item.getSize());
  }

  auto& mmContainer = getMMContainer(allocInfo.poolId, allocInfo.classId);
  return mmContainer.recordAccess(item, mode);
}

template <typename CacheTrait>
uint32_t CacheAllocator<CacheTrait>::getUsableSize(const Item& item) const {
  const auto allocSize =
      allocator_->getAllocInfo(static_cast<const void*>(&item)).allocSize;
  return item.isChainedItem()
             ? allocSize - ChainedItem::getRequiredSize(0)
             : allocSize - Item::getRequiredSize(item.getKey(), 0);
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::SampleItem
CacheAllocator<CacheTrait>::getSampleItem() {
  // Sampling from DRAM cache
  auto item = reinterpret_cast<const Item*>(allocator_->getRandomAlloc());
  if (!item) {
    return SampleItem{};
  }

  // Check that item returned is the same that was sampled

  auto sharedHdl = std::make_shared<ReadHandle>(findInternal(item->getKey()));
  if (sharedHdl->get() != item) {
    return SampleItem{};
  }

  const auto allocInfo = allocator_->getAllocInfo(item->getMemory());

  // Convert the Item to IOBuf to make SampleItem
  auto iobuf = folly::IOBuf{
      folly::IOBuf::TAKE_OWNERSHIP, sharedHdl->getInternal(),
      item->getOffsetForMemory() + item->getSize(),
      [](void* /*unused*/, void* userData) {
        auto* hdl = reinterpret_cast<std::shared_ptr<ReadHandle>*>(userData);
        delete hdl;
      } /* freeFunc */,
      new std::shared_ptr<ReadHandle>{sharedHdl} /* userData for freeFunc */};

  iobuf.markExternallySharedOne();

  return SampleItem(std::move(iobuf), allocInfo);
}

template <typename CacheTrait>
std::vector<std::string> CacheAllocator<CacheTrait>::dumpEvictionIterator(
    PoolId pid, ClassId cid, size_t numItems) {
  if (numItems == 0) {
    return {};
  }

  if (static_cast<size_t>(pid) >= mmContainers_.size() ||
      static_cast<size_t>(cid) >= mmContainers_[pid].size()) {
    throw std::invalid_argument(
        folly::sformat("Invalid PoolId: {} and ClassId: {}.", pid, cid));
  }

  std::vector<std::string> content;

  auto& mm = *mmContainers_[pid][cid];

  mm.withEvictionIterator([&content, numItems](auto&& itr) {
    while (itr && content.size() < numItems) {
      content.push_back(itr->toString());
      ++itr;
    }
  });

  return content;
}

template <typename CacheTrait>
template <typename Handle>
folly::IOBuf CacheAllocator<CacheTrait>::convertToIOBufT(Handle& handle) {
  if (!handle) {
    throw std::invalid_argument("null item handle for converting to IOBUf");
  }

  Item* item = handle.getInternal();
  const uint32_t dataOffset = item->getOffsetForMemory();

  using ConvertChainedItem = std::function<std::unique_ptr<folly::IOBuf>(
      Item * item, ChainedItem & chainedItem)>;
  folly::IOBuf iobuf;
  ConvertChainedItem converter;

  // based on current refcount and threshold from config
  // determine to use a new Item Handle for each chain items
  // or use shared Item Handle for all chain items
  if (item->getRefCount() > config_.thresholdForConvertingToIOBuf) {
    auto sharedHdl = std::make_shared<Handle>(std::move(handle));

    iobuf = folly::IOBuf{
        folly::IOBuf::TAKE_OWNERSHIP, item,

        // Since we'll be moving the IOBuf data pointer forward
        // by dataOffset, we need to adjust the IOBuf length
        // accordingly
        dataOffset + item->getSize(),

        [](void* /*unused*/, void* userData) {
          auto* hdl = reinterpret_cast<std::shared_ptr<Handle>*>(userData);
          delete hdl;
        } /* freeFunc */,
        new std::shared_ptr<Handle>{sharedHdl} /* userData for freeFunc */};

    if (item->hasChainedItem()) {
      converter = [sharedHdl](Item*, ChainedItem& chainedItem) {
        const uint32_t chainedItemDataOffset = chainedItem.getOffsetForMemory();

        return folly::IOBuf::takeOwnership(
            &chainedItem,

            // Since we'll be moving the IOBuf data pointer forward by
            // dataOffset,
            // we need to adjust the IOBuf length accordingly
            chainedItemDataOffset + chainedItem.getSize(),

            [](void*, void* userData) {
              auto* hdl = reinterpret_cast<std::shared_ptr<Handle>*>(userData);
              delete hdl;
            } /* freeFunc */,
            new std::shared_ptr<Handle>{sharedHdl} /* userData for freeFunc */);
      };
    }

  } else {
    // following IOBuf will take the item's ownership and trigger freeFunc to
    // release the reference count.
    handle.release();
    iobuf = folly::IOBuf{folly::IOBuf::TAKE_OWNERSHIP, item,

                         // Since we'll be moving the IOBuf data pointer forward
                         // by dataOffset, we need to adjust the IOBuf length
                         // accordingly
                         dataOffset + item->getSize(),

                         [](void* buf, void* userData) {
                           Handle{reinterpret_cast<Item*>(buf),
                                  *reinterpret_cast<decltype(this)>(userData)}
                               .reset();
                         } /* freeFunc */,
                         this /* userData for freeFunc */};

    if (item->hasChainedItem()) {
      converter = [this](Item* parentItem, ChainedItem& chainedItem) {
        const uint32_t chainedItemDataOffset = chainedItem.getOffsetForMemory();

        // Each IOBuf converted from a child item will hold one additional
        // refcount on the parent item. This ensures that as long as the user
        // holds any IOBuf pointing anywhere in the chain, the whole chain
        // will not be evicted from cache.
        //
        // We can safely bump the refcount on the parent here only because
        // we already have an item handle on the parent (which has just been
        // moved into the IOBuf above). Normally, the only place we can
        // bump an item handle safely is through the AccessContainer.
        acquire(parentItem).release();

        return folly::IOBuf::takeOwnership(
            &chainedItem,

            // Since we'll be moving the IOBuf data pointer forward by
            // dataOffset,
            // we need to adjust the IOBuf length accordingly
            chainedItemDataOffset + chainedItem.getSize(),

            [](void* buf, void* userData) {
              auto* cache = reinterpret_cast<decltype(this)>(userData);
              auto* child = reinterpret_cast<ChainedItem*>(buf);
              auto* parent = &child->getParentItem(cache->compressor_);
              Handle{parent, *cache}.reset();
            } /* freeFunc */,
            this /* userData for freeFunc */);
      };
    }
  }

  iobuf.trimStart(dataOffset);
  iobuf.markExternallySharedOne();

  if (item->hasChainedItem()) {
    auto appendHelper = [&](ChainedItem& chainedItem) {
      const uint32_t chainedItemDataOffset = chainedItem.getOffsetForMemory();

      auto nextChain = converter(item, chainedItem);

      nextChain->trimStart(chainedItemDataOffset);
      nextChain->markExternallySharedOne();

      // Append immediately after the parent, IOBuf will present the data
      // in the original insertion order.
      //
      // i.e. 1. Allocate parent
      //      2. add A, add B, add C
      //
      //      In memory: parent -> C -> B -> A
      //      In IOBuf:  parent -> A -> B -> C
      iobuf.appendChain(std::move(nextChain));
    };

    forEachChainedItem(*item, std::move(appendHelper));
  }

  return iobuf;
}

template <typename CacheTrait>
folly::IOBuf CacheAllocator<CacheTrait>::wrapAsIOBuf(const Item& item) {
  folly::IOBuf ioBuf{folly::IOBuf::WRAP_BUFFER, item.getMemory(),
                     item.getSize()};

  if (item.hasChainedItem()) {
    auto appendHelper = [&](ChainedItem& chainedItem) {
      auto nextChain = folly::IOBuf::wrapBuffer(chainedItem.getMemory(),
                                                chainedItem.getSize());

      // Append immediately after the parent, IOBuf will present the data
      // in the original insertion order.
      //
      // i.e. 1. Allocate parent
      //      2. add A, add B, add C
      //
      //      In memory: parent -> C -> B -> A
      //      In IOBuf:  parent -> A -> B -> C
      ioBuf.appendChain(std::move(nextChain));
    };

    forEachChainedItem(item, std::move(appendHelper));
  }
  return ioBuf;
}

template <typename CacheTrait>
PoolId CacheAllocator<CacheTrait>::addPool(
    folly::StringPiece name,
    size_t size,
    const std::set<uint32_t>& allocSizes,
    MMConfig config,
    std::shared_ptr<RebalanceStrategy> rebalanceStrategy,
    std::shared_ptr<RebalanceStrategy> resizeStrategy,
    bool ensureProvisionable) {
  folly::SharedMutex::WriteHolder w(poolsResizeAndRebalanceLock_);
  auto pid = allocator_->addPool(name, size, allocSizes, ensureProvisionable);
  createMMContainers(pid, std::move(config));
  setRebalanceStrategy(pid, std::move(rebalanceStrategy));
  setResizeStrategy(pid, std::move(resizeStrategy));
  return pid;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::overridePoolRebalanceStrategy(
    PoolId pid, std::shared_ptr<RebalanceStrategy> rebalanceStrategy) {
  if (static_cast<size_t>(pid) >= mmContainers_.size()) {
    throw std::invalid_argument(folly::sformat(
        "Invalid PoolId: {}, size of pools: {}", pid, mmContainers_.size()));
  }
  setRebalanceStrategy(pid, std::move(rebalanceStrategy));
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::overridePoolResizeStrategy(
    PoolId pid, std::shared_ptr<RebalanceStrategy> resizeStrategy) {
  if (static_cast<size_t>(pid) >= mmContainers_.size()) {
    throw std::invalid_argument(folly::sformat(
        "Invalid PoolId: {}, size of pools: {}", pid, mmContainers_.size()));
  }
  setResizeStrategy(pid, std::move(resizeStrategy));
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::overridePoolOptimizeStrategy(
    std::shared_ptr<PoolOptimizeStrategy> optimizeStrategy) {
  setPoolOptimizeStrategy(std::move(optimizeStrategy));
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::overridePoolConfig(PoolId pid,
                                                    const MMConfig& config) {
  if (static_cast<size_t>(pid) >= mmContainers_.size()) {
    throw std::invalid_argument(folly::sformat(
        "Invalid PoolId: {}, size of pools: {}", pid, mmContainers_.size()));
  }

  auto& pool = allocator_->getPool(pid);
  for (unsigned int cid = 0; cid < pool.getNumClassId(); ++cid) {
    MMConfig mmConfig = config;
    mmConfig.addExtraConfig(
        config_.trackTailHits
            ? pool.getAllocationClass(static_cast<ClassId>(cid))
                  .getAllocsPerSlab()
            : 0);
    DCHECK_NOTNULL(mmContainers_[pid][cid].get());
    mmContainers_[pid][cid]->setConfig(mmConfig);
  }
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::createMMContainers(const PoolId pid,
                                                    MMConfig config) {
  auto& pool = allocator_->getPool(pid);
  for (unsigned int cid = 0; cid < pool.getNumClassId(); ++cid) {
    config.addExtraConfig(
        config_.trackTailHits
            ? pool.getAllocationClass(static_cast<ClassId>(cid))
                  .getAllocsPerSlab()
            : 0);
    mmContainers_[pid][cid].reset(new MMContainer(config, compressor_));
  }
}

template <typename CacheTrait>
PoolId CacheAllocator<CacheTrait>::getPoolId(
    folly::StringPiece name) const noexcept {
  return allocator_->getPoolId(name.str());
}

// The Function returns a consolidated vector of Release Slab
// events from Pool Workers { Pool rebalancer, Pool Resizer and
// Memory Monitor}.
template <typename CacheTrait>
AllSlabReleaseEvents CacheAllocator<CacheTrait>::getAllSlabReleaseEvents(
    PoolId poolId) const {
  AllSlabReleaseEvents res;
  // lock protects against workers being restarted
  {
    std::lock_guard<std::mutex> l(workersMutex_);
    if (poolRebalancer_) {
      res.rebalancerEvents = poolRebalancer_->getSlabReleaseEvents(poolId);
    }
    if (poolResizer_) {
      res.resizerEvents = poolResizer_->getSlabReleaseEvents(poolId);
    }
    if (memMonitor_) {
      res.monitorEvents = memMonitor_->getSlabReleaseEvents(poolId);
    }
  }
  return res;
}

template <typename CacheTrait>
std::set<PoolId> CacheAllocator<CacheTrait>::filterCompactCachePools(
    const PoolIds& poolIds) const {
  PoolIds ret;
  folly::SharedMutex::ReadHolder lock(compactCachePoolsLock_);
  for (auto poolId : poolIds) {
    if (!isCompactCachePool_[poolId]) {
      // filter out slab pools backing the compact caches.
      ret.insert(poolId);
    }
  }
  return ret;
}

template <typename CacheTrait>
std::set<PoolId> CacheAllocator<CacheTrait>::getRegularPoolIds() const {
  folly::SharedMutex::ReadHolder r(poolsResizeAndRebalanceLock_);
  return filterCompactCachePools(allocator_->getPoolIds());
}

template <typename CacheTrait>
std::set<PoolId> CacheAllocator<CacheTrait>::getCCachePoolIds() const {
  PoolIds ret;
  folly::SharedMutex::ReadHolder lock(compactCachePoolsLock_);
  for (PoolId id = 0; id < static_cast<PoolId>(MemoryPoolManager::kMaxPools);
       id++) {
    if (isCompactCachePool_[id]) {
      // filter out slab pools backing the compact caches.
      ret.insert(id);
    }
  }
  return ret;
}

template <typename CacheTrait>
std::set<PoolId> CacheAllocator<CacheTrait>::getRegularPoolIdsForResize()
    const {
  folly::SharedMutex::ReadHolder r(poolsResizeAndRebalanceLock_);
  // If Slabs are getting advised away - as indicated by non-zero
  // getAdvisedMemorySize - then pools may be overLimit even when
  // all slabs are not allocated. Otherwise, pools may be overLimit
  // only after all slabs are allocated.
  //
  return (allocator_->allSlabsAllocated()) ||
                 (allocator_->getAdvisedMemorySize() != 0)
             ? filterCompactCachePools(allocator_->getPoolsOverLimit())
             : std::set<PoolId>{};
}

template <typename CacheTrait>
const std::string CacheAllocator<CacheTrait>::getCacheName() const {
  return config_.cacheName;
}

template <typename CacheTrait>
PoolStats CacheAllocator<CacheTrait>::getPoolStats(PoolId poolId) const {
  const auto& pool = allocator_->getPool(poolId);
  const auto& allocSizes = pool.getAllocSizes();
  auto mpStats = pool.getStats();
  const auto& classIds = mpStats.classIds;

  // check if this is a compact cache.
  bool isCompactCache = false;
  {
    folly::SharedMutex::ReadHolder lock(compactCachePoolsLock_);
    isCompactCache = isCompactCachePool_[poolId];
  }

  std::unordered_map<ClassId, CacheStat> cacheStats;
  uint64_t totalHits = 0;
  // cacheStats is only menaningful for pools that are not compact caches.
  // TODO export evictions, numItems etc from compact cache directly.
  if (!isCompactCache) {
    for (const ClassId cid : classIds) {
      uint64_t classHits = (*stats_.cacheHits)[poolId][cid].get();
      cacheStats.insert(
          {cid,
           {allocSizes[cid], (*stats_.allocAttempts)[poolId][cid].get(),
            (*stats_.evictionAttempts)[poolId][cid].get(),
            (*stats_.allocFailures)[poolId][cid].get(),
            (*stats_.fragmentationSize)[poolId][cid].get(), classHits,
            (*stats_.chainedItemEvictions)[poolId][cid].get(),
            (*stats_.regularItemEvictions)[poolId][cid].get(),
            getMMContainerStat(poolId, cid)}});
      totalHits += classHits;
    }
  }

  PoolStats ret;
  ret.isCompactCache = isCompactCache;
  ret.poolName = allocator_->getPoolName(poolId);
  ret.poolSize = pool.getPoolSize();
  ret.poolUsableSize = pool.getPoolUsableSize();
  ret.poolAdvisedSize = pool.getPoolAdvisedSize();
  ret.cacheStats = std::move(cacheStats);
  ret.mpStats = std::move(mpStats);
  ret.numPoolGetHits = totalHits;
  ret.evictionAgeSecs = stats_.perPoolEvictionAgeSecs_[poolId].estimate();

  return ret;
}

template <typename CacheTrait>
PoolEvictionAgeStats CacheAllocator<CacheTrait>::getPoolEvictionAgeStats(
    PoolId pid, unsigned int slabProjectionLength) const {
  PoolEvictionAgeStats stats;

  const auto& pool = allocator_->getPool(pid);
  const auto& allocSizes = pool.getAllocSizes();
  for (ClassId cid = 0; cid < static_cast<ClassId>(allocSizes.size()); ++cid) {
    auto& mmContainer = getMMContainer(pid, cid);
    const auto numItemsPerSlab =
        allocator_->getPool(pid).getAllocationClass(cid).getAllocsPerSlab();
    const auto projectionLength = numItemsPerSlab * slabProjectionLength;
    stats.classEvictionAgeStats[cid] =
        mmContainer.getEvictionAgeStat(projectionLength);
  }

  return stats;
}

template <typename CacheTrait>
CacheMetadata CacheAllocator<CacheTrait>::getCacheMetadata() const noexcept {
  return CacheMetadata{kCachelibVersion, kCacheRamFormatVersion,
                       kCacheNvmFormatVersion, config_.size};
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::releaseSlab(PoolId pid,
                                             ClassId cid,
                                             SlabReleaseMode mode,
                                             const void* hint) {
  releaseSlab(pid, cid, Slab::kInvalidClassId, mode, hint);
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::releaseSlab(PoolId pid,
                                             ClassId victim,
                                             ClassId receiver,
                                             SlabReleaseMode mode,
                                             const void* hint) {
  stats_.numActiveSlabReleases.inc();
  SCOPE_EXIT { stats_.numActiveSlabReleases.dec(); };
  switch (mode) {
  case SlabReleaseMode::kRebalance:
    stats_.numReleasedForRebalance.inc();
    break;
  case SlabReleaseMode::kResize:
    stats_.numReleasedForResize.inc();
    break;
  case SlabReleaseMode::kAdvise:
    stats_.numReleasedForAdvise.inc();
    break;
  }

  try {
    auto releaseContext = allocator_->startSlabRelease(
        pid, victim, receiver, mode, hint,
        [this]() -> bool { return shutDownInProgress_; });

    // No work needed if the slab is already released
    if (releaseContext.isReleased()) {
      return;
    }

    releaseSlabImpl(releaseContext);
    if (!allocator_->allAllocsFreed(releaseContext)) {
      throw std::runtime_error(
          folly::sformat("Was not able to free all allocs. PoolId: {}, AC: {}",
                         releaseContext.getPoolId(),
                         releaseContext.getClassId()));
    }

    allocator_->completeSlabRelease(releaseContext);
  } catch (const exception::SlabReleaseAborted& e) {
    stats_.numAbortedSlabReleases.inc();
    throw exception::SlabReleaseAborted(folly::sformat(
        "Slab release aborted while releasing "
        "a slab in pool {} victim {} receiver {}. Original ex msg: ",
        pid, static_cast<int>(victim), static_cast<int>(receiver), e.what()));
  }
}

template <typename CacheTrait>
SlabReleaseStats CacheAllocator<CacheTrait>::getSlabReleaseStats()
    const noexcept {
  std::lock_guard<std::mutex> l(workersMutex_);
  return SlabReleaseStats{stats_.numActiveSlabReleases.get(),
                          stats_.numReleasedForRebalance.get(),
                          stats_.numReleasedForResize.get(),
                          stats_.numReleasedForAdvise.get(),
                          poolRebalancer_ ? poolRebalancer_->getRunCount()
                                          : 0ULL,
                          poolResizer_ ? poolResizer_->getRunCount() : 0ULL,
                          memMonitor_ ? memMonitor_->getRunCount() : 0ULL,
                          stats_.numMoveAttempts.get(),
                          stats_.numMoveSuccesses.get(),
                          stats_.numEvictionAttempts.get(),
                          stats_.numEvictionSuccesses.get(),
                          stats_.numSlabReleaseStuck.get()};
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::releaseSlabImpl(
    const SlabReleaseContext& releaseContext) {
  auto startTime = std::chrono::milliseconds(util::getCurrentTimeMs());
  bool releaseStuck = false;

  SCOPE_EXIT {
    if (releaseStuck) {
      stats_.numSlabReleaseStuck.dec();
    }
  };

  util::Throttler throttler(
      config_.throttleConfig,
      [this, &startTime, &releaseStuck](std::chrono::milliseconds curTime) {
        if (!releaseStuck &&
            curTime >= startTime + config_.slabReleaseStuckThreshold) {
          stats().numSlabReleaseStuck.inc();
          releaseStuck = true;
        }
      });

  // Active allocations need to be freed before we can release this slab
  // The idea is:
  //  1. Iterate through each active allocation
  //  2. Under AC lock, acquire handle to the item (or it's parent)
  //  3. If 2 is successful, Move or Evict
  //  4. Move on to the next item if current item is freed
  for (auto alloc : releaseContext.getActiveAllocations()) {
    auto startTimeSec = util::getCurrentTimeSec();
    Item& item = *static_cast<Item*>(alloc);

    // Try to move this item and make sure we can free the memory
    const bool isMoved = moveForSlabRelease(releaseContext, item, throttler);

    // if moving fails, evict it
    if (!isMoved) {
      while (true) {
        if (evictForSlabRelease(releaseContext, item, throttler)) {
          break;
        }

        if (shutDownInProgress_) {
          allocator_->abortSlabRelease(releaseContext);
          throw exception::SlabReleaseAborted(
              folly::sformat("Slab Release aborted while trying to evict"
                             " Item: {} Pool: {}, Class: {}.",
                             item.toString(), releaseContext.getPoolId(),
                             releaseContext.getClassId()));
        }

        throttleWith(throttler, [&] {
          XLOGF(
              WARN,
              "Spent {} seconds, slab release still trying to evict Item: {}. "
              "Pool: {}, Class: {}.",
              util::getCurrentTimeSec() - startTimeSec, item.toString(),
              releaseContext.getPoolId(), releaseContext.getClassId());
        });
      }
    }
    XDCHECK(allocator_->isAllocFreed(releaseContext, alloc));
  }
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::throttleWith(util::Throttler& t,
                                              std::function<void()> fn) {
  const unsigned int rateLimit = 1024;
  // execute every 1024 times we have actually throttled
  if (t.throttle() && (t.numThrottles() % rateLimit) == 0) {
    fn();
  }
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::moveForSlabRelease(
    const SlabReleaseContext& ctx, Item& oldItem, util::Throttler& throttler) {
  if (!config_.moveCb) {
    return false;
  }

  bool isMoved = false;
  auto startTime = util::getCurrentTimeSec();
  WriteHandle newItemHdl{};

  for (unsigned int itemMovingAttempts = 0;
       itemMovingAttempts < config_.movingTries;
       ++itemMovingAttempts) {
    stats_.numMoveAttempts.inc();

    throttleWith(throttler, [&] {
      XLOGF(WARN,
            "Spent {} seconds, slab release still trying to move Item: {}. "
            "Pool: {}, Class: {}.",
            util::getCurrentTimeSec() - startTime, oldItem.toString(),
            ctx.getPoolId(), ctx.getClassId());
    });

    if (allocator_->isAllocFreed(ctx, &oldItem)) {
      // nothing to do
      return true;
    }

    auto* owner = oldItem.isChainedItem()
                      ? &oldItem.asChainedItem().getParentItem(compressor_)
                      : &oldItem;
    auto handle = accessContainer_->find(owner->getKey());
    if (!handle) {
      continue;
    }

    if (!oldItem.isChainedItem() && handle.get() != &oldItem) {
      // make sure we have a proper handle and non one removed the item
      // this check is only for regular items, chained items must be
      // handled under a lock so it's done later
      continue;
    }

    if (!newItemHdl) {
      // try to allocate again if it previously wasn't successful
      newItemHdl = allocateNewItemForOldItem(oldItem, handle);
    }

    // if we have a valid handle, try to move, if not, we retry.
    if (newItemHdl) {
      isMoved = tryMovingForSlabRelease(oldItem, std::move(handle), newItemHdl);
      if (isMoved) {
        break;
      }
    }
  }

  // Return false if we've exhausted moving tries.
  if (!isMoved) {
    return false;
  }

  stats_.numMoveSuccesses.inc();
  return true;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::allocateNewItemForOldItem(const Item& oldItem,
                                                      WriteHandle& handle) {
  XDCHECK(handle);
  if (oldItem.isChainedItem()) {
    const auto& oldChainedItem = oldItem.asChainedItem();
    const auto parentKey = oldChainedItem.getParentItem(compressor_).getKey();

    auto l = chainedItemLocks_.lockExclusive(parentKey);

    if (validateAndGetParentHandleForChainedMoveLocked(oldChainedItem,
                                                       parentKey) != handle) {
      return {};
    }

    // Set up the destination for the move. Since we hold handle to parent,
    // it won't be picked for eviction.
    auto newItemHdl =
        allocateChainedItemInternal(handle, oldChainedItem.getSize());
    if (!newItemHdl) {
      return {};
    }

    XDCHECK_EQ(newItemHdl->getSize(), oldChainedItem.getSize());
    auto parentPtr = handle.getInternal();
    XDCHECK_EQ(reinterpret_cast<uintptr_t>(parentPtr),
               reinterpret_cast<uintptr_t>(
                   &oldChainedItem.getParentItem(compressor_)));

    return newItemHdl;
  }

  const auto allocInfo =
      allocator_->getAllocInfo(static_cast<const void*>(&oldItem));

  // Set up the destination for the move. Since we hold handle to oldItem,
  // it won't be picked for eviction.
  XDCHECK(&oldItem == handle.get());
  auto newItemHdl = allocateInternal(allocInfo.poolId,
                                     oldItem.getKey(),
                                     oldItem.getSize(),
                                     oldItem.getCreationTime(),
                                     oldItem.getExpiryTime());
  if (!newItemHdl) {
    return {};
  }

  XDCHECK_EQ(newItemHdl->getSize(), oldItem.getSize());
  XDCHECK_EQ(reinterpret_cast<uintptr_t>(&getMMContainer(oldItem)),
             reinterpret_cast<uintptr_t>(&getMMContainer(*newItemHdl)));

  return newItemHdl;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::tryMovingForSlabRelease(
    Item& oldItem, WriteHandle&& handle, WriteHandle& newItemHdl) {
  // By holding onto a user-level synchronization object, we ensure moving
  // a regular item or chained item is synchronized with any potential
  // user-side mutation.
  std::unique_ptr<SyncObj> syncObj;
  if (config_.movingSync) {
    if (!oldItem.isChainedItem()) {
      syncObj = config_.movingSync(oldItem.getKey());
    } else {
      // Copy the key so we have a valid key to work with if the chained
      // item is still valid.
      const std::string parentKey =
          oldItem.asChainedItem().getParentItem(compressor_).getKey().str();
      syncObj = config_.movingSync(parentKey);
    }

    // We need to differentiate between the following three scenarios:
    // 1. nullptr indicates no move sync required for this particular item
    // 2. moveSync.isValid() == true meaning we've obtained the sync
    // 3. moveSync.isValid() == false meaning we need to abort and retry
    if (syncObj && !syncObj->isValid()) {
      return false;
    }
  }

  return oldItem.isChainedItem()
             ? moveChainedItem(oldItem.asChainedItem(), std::move(handle),
                               newItemHdl)
             : moveRegularItem(std::move(handle), newItemHdl);
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::evictForSlabRelease(
    const SlabReleaseContext& ctx,
    Item& toRelease,
    util::Throttler& throttler) {
  XDCHECK(!config_.isEvictionDisabled());

  auto startTime = util::getCurrentTimeSec();
  stats_.numEvictionAttempts.inc();

  if (allocator_->isAllocFreed(ctx, &toRelease)) {
    // nothing to do
    return true;
  }

  auto* candidate = toRelease.isChainedItem()
                        ? &toRelease.asChainedItem().getParentItem(compressor_)
                        : &toRelease;

  auto token = createPutToken(*candidate);

  if (!candidate->markExclusive()) {
    return false;
  }

  unlinkItemExclusive(*candidate);

  if (token.isValid() && shouldWriteToNvmCacheExclusive(*candidate)) {
    auto handle = acquire(candidate);
    nvmCache_->put(handle, std::move(token));
    auto r = decRef(*handle.release());
    XDCHECK(r == 0u);
  }

  const auto allocInfo =
      allocator_->getAllocInfo(static_cast<const void*>(candidate));
  if (candidate->hasChainedItem()) {
    (*stats_.chainedItemEvictions)[allocInfo.poolId][allocInfo.classId].inc();
  } else {
    (*stats_.regularItemEvictions)[allocInfo.poolId][allocInfo.classId].inc();
  }

  stats_.numEvictionSuccesses.inc();
  return releaseBackToAllocator(*candidate, RemoveContext::kEviction, true,
                                &toRelease);
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::removeIfExpired(const ReadHandle& handle) {
  if (!handle) {
    return false;
  }

  // We remove the item from both access and mm containers.
  // We want to make sure the caller is the only one holding the handle.
  auto removedHandle =
      accessContainer_->removeIf(*(handle.getInternal()), itemExpiryPredicate);
  if (removedHandle) {
    removeFromMMContainer(*(handle.getInternal()));
    return true;
  }

  return false;
}

template <typename CacheTrait>
template <typename CCacheT, typename... Args>
CCacheT* CacheAllocator<CacheTrait>::addCompactCache(folly::StringPiece name,
                                                     size_t size,
                                                     Args&&... args) {
  if (!config_.isCompactCacheEnabled()) {
    throw std::logic_error("Compact cache is not enabled");
  }

  folly::SharedMutex::WriteHolder lock(compactCachePoolsLock_);
  auto poolId = allocator_->addPool(name, size, {Slab::kSize});
  isCompactCachePool_[poolId] = true;

  auto ptr = std::make_unique<CCacheT>(
      compactCacheManager_->addAllocator(name.str(), poolId),
      std::forward<Args>(args)...);
  auto it = compactCaches_.emplace(poolId, std::move(ptr));
  XDCHECK(it.second);
  return static_cast<CCacheT*>(it.first->second.get());
}

template <typename CacheTrait>
template <typename CCacheT, typename... Args>
CCacheT* CacheAllocator<CacheTrait>::attachCompactCache(folly::StringPiece name,
                                                        Args&&... args) {
  auto& allocator = compactCacheManager_->getAllocator(name.str());
  auto poolId = allocator.getPoolId();
  // if a compact cache with this name already exists, return without creating
  // new instance
  folly::SharedMutex::WriteHolder lock(compactCachePoolsLock_);
  if (compactCaches_.find(poolId) != compactCaches_.end()) {
    return static_cast<CCacheT*>(compactCaches_[poolId].get());
  }

  auto ptr = std::make_unique<CCacheT>(allocator, std::forward<Args>(args)...);
  auto it = compactCaches_.emplace(poolId, std::move(ptr));
  XDCHECK(it.second);
  return static_cast<CCacheT*>(it.first->second.get());
}

template <typename CacheTrait>
const ICompactCache& CacheAllocator<CacheTrait>::getCompactCache(
    PoolId pid) const {
  folly::SharedMutex::ReadHolder lock(compactCachePoolsLock_);
  if (!isCompactCachePool_[pid]) {
    throw std::invalid_argument(
        folly::sformat("PoolId {} is not a compact cache", pid));
  }

  auto it = compactCaches_.find(pid);
  if (it == compactCaches_.end()) {
    throw std::invalid_argument(folly::sformat(
        "PoolId {} belongs to an un-attached compact cache", pid));
  }
  return *it->second;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::setPoolOptimizerFor(PoolId poolId,
                                                     bool enableAutoResizing) {
  optimizerEnabled_[poolId] = enableAutoResizing;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::resizeCompactCaches() {
  compactCacheManager_->resizeAll();
}

template <typename CacheTrait>
typename CacheTrait::MMType::LruType CacheAllocator<CacheTrait>::getItemLruType(
    const Item& item) const {
  return getMMContainer(item).getLruType(item);
}

// The order of the serialization is as follows:
//
// This is also the order of deserialization in the constructor, when
// we restore the cache allocator.
//
// ---------------------------------
// | accessContainer_              |
// | mmContainers_                 |
// | compactCacheManager_          |
// | allocator_                    |
// | metadata_                     |
// ---------------------------------
template <typename CacheTrait>
folly::IOBufQueue CacheAllocator<CacheTrait>::saveStateToIOBuf() {
  if (stats_.numActiveSlabReleases.get() != 0) {
    throw std::logic_error(
        "There are still slabs being released at the moment");
  }

  *metadata_.allocatorVersion() = kCachelibVersion;
  *metadata_.ramFormatVersion() = kCacheRamFormatVersion;
  *metadata_.cacheCreationTime() = static_cast<int64_t>(cacheCreationTime_);
  *metadata_.mmType() = MMType::kId;
  *metadata_.accessType() = AccessType::kId;

  metadata_.compactCachePools()->clear();
  const auto pools = getPoolIds();
  {
    folly::SharedMutex::ReadHolder lock(compactCachePoolsLock_);
    for (PoolId pid : pools) {
      for (unsigned int cid = 0; cid < (*stats_.fragmentationSize)[pid].size();
           ++cid) {
        metadata_.fragmentationSize()[pid][static_cast<ClassId>(cid)] =
            (*stats_.fragmentationSize)[pid][cid].get();
      }
      if (isCompactCachePool_[pid]) {
        metadata_.compactCachePools()->push_back(pid);
      }
    }
  }

  *metadata_.numChainedParentItems() = stats_.numChainedParentItems.get();
  *metadata_.numChainedChildItems() = stats_.numChainedChildItems.get();
  *metadata_.numAbortedSlabReleases() = stats_.numAbortedSlabReleases.get();

  auto serializeMMContainers = [](MMContainers& mmContainers) {
    MMSerializationTypeContainer state;
    for (unsigned int i = 0; i < mmContainers.size(); ++i) {
      for (unsigned int j = 0; j < mmContainers[i].size(); ++j) {
        if (mmContainers[i][j]) {
          state.pools_ref()[i][j] = mmContainers[i][j]->saveState();
        }
      }
    }
    return state;
  };
  MMSerializationTypeContainer mmContainersState =
      serializeMMContainers(mmContainers_);

  AccessSerializationType accessContainerState = accessContainer_->saveState();
  MemoryAllocator::SerializationType allocatorState = allocator_->saveState();
  CCacheManager::SerializationType ccState = compactCacheManager_->saveState();

  AccessSerializationType chainedItemAccessContainerState =
      chainedItemAccessContainer_->saveState();

  // serialize to an iobuf queue. The caller can then copy over the serialized
  // results into a single buffer.
  folly::IOBufQueue queue;
  Serializer::serializeToIOBufQueue(queue, metadata_);
  Serializer::serializeToIOBufQueue(queue, allocatorState);
  Serializer::serializeToIOBufQueue(queue, ccState);
  Serializer::serializeToIOBufQueue(queue, mmContainersState);
  Serializer::serializeToIOBufQueue(queue, accessContainerState);
  Serializer::serializeToIOBufQueue(queue, chainedItemAccessContainerState);
  return queue;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::stopWorkers(std::chrono::seconds timeout) {
  bool success = true;
  success &= stopPoolRebalancer(timeout);
  success &= stopPoolResizer(timeout);
  success &= stopMemMonitor(timeout);
  success &= stopReaper(timeout);
  return success;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::ShutDownStatus
CacheAllocator<CacheTrait>::shutDown() {
  using ShmShutDownRes = typename ShmManager::ShutDownRes;
  XLOG(DBG, "shutting down CacheAllocator");
  if (shmManager_ == nullptr) {
    throw std::invalid_argument(
        "shutDown can only be called once from a cached manager created on "
        "shared memory. You may also be incorrectly constructing your "
        "allocator. Are you passing in "
        "AllocatorType::SharedMem* ?");
  }
  XDCHECK(!config_.cacheDir.empty());

  if (config_.enableFastShutdown) {
    shutDownInProgress_ = true;
  }

  stopWorkers();

  const auto handleCount = getNumActiveHandles();
  if (handleCount != 0) {
    XLOGF(ERR, "Found {} active handles while shutting down cache. aborting",
          handleCount);
    return ShutDownStatus::kFailed;
  }

  const auto nvmShutDownStatusOpt = saveNvmCache();
  saveRamCache();
  const auto shmShutDownStatus = shmManager_->shutDown();
  const auto shmShutDownSucceeded =
      (shmShutDownStatus == ShmShutDownRes::kSuccess);
  shmManager_.reset();

  if (shmShutDownSucceeded) {
    if (!nvmShutDownStatusOpt || *nvmShutDownStatusOpt)
      return ShutDownStatus::kSuccess;

    if (nvmShutDownStatusOpt && !*nvmShutDownStatusOpt)
      return ShutDownStatus::kSavedOnlyDRAM;
  }

  XLOGF(ERR, "Could not shutdown DRAM cache cleanly. ShutDownRes={}",
        (shmShutDownStatus == ShmShutDownRes::kFailedWrite ? "kFailedWrite"
                                                           : "kFileDeleted"));

  if (nvmShutDownStatusOpt && *nvmShutDownStatusOpt) {
    return ShutDownStatus::kSavedOnlyNvmCache;
  }

  return ShutDownStatus::kFailed;
}

template <typename CacheTrait>
std::optional<bool> CacheAllocator<CacheTrait>::saveNvmCache() {
  if (!nvmCache_) {
    return std::nullopt;
  }

  // throw any exceptions from shutting down nvmcache since we dont know the
  // state of RAM as well.
  if (!nvmCache_->isEnabled()) {
    nvmCache_->shutDown();
    return std::nullopt;
  }

  if (!nvmCache_->shutDown()) {
    XLOG(ERR, "Could not shutdown nvmcache cleanly");
    return false;
  }

  nvmCacheState_.markSafeShutDown();
  return true;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::saveRamCache() {
  // serialize the cache state
  auto serializedBuf = saveStateToIOBuf();
  std::unique_ptr<folly::IOBuf> ioBuf = serializedBuf.move();
  ioBuf->coalesce();

  void* infoAddr =
      shmManager_->createShm(detail::kShmInfoName, ioBuf->length()).addr;
  Serializer serializer(reinterpret_cast<uint8_t*>(infoAddr),
                        reinterpret_cast<uint8_t*>(infoAddr) + ioBuf->length());
  serializer.writeToBuffer(std::move(ioBuf));
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::MMContainers
CacheAllocator<CacheTrait>::deserializeMMContainers(
    Deserializer& deserializer,
    const typename Item::PtrCompressor& compressor) {
  const auto container =
      deserializer.deserialize<MMSerializationTypeContainer>();

  MMContainers mmContainers;

  for (auto& kvPool : *container.pools_ref()) {
    auto i = static_cast<PoolId>(kvPool.first);
    auto& pool = getPool(i);
    for (auto& kv : kvPool.second) {
      auto j = static_cast<ClassId>(kv.first);
      MMContainerPtr ptr =
          std::make_unique<typename MMContainerPtr::element_type>(kv.second,
                                                                  compressor);
      auto config = ptr->getConfig();
      config.addExtraConfig(config_.trackTailHits
                                ? pool.getAllocationClass(j).getAllocsPerSlab()
                                : 0);
      ptr->setConfig(config);
      mmContainers[i][j] = std::move(ptr);
    }
  }
  // We need to drop the unevictableMMContainer in the desierializer.
  // TODO: remove this at version 17.
  if (metadata_.allocatorVersion() <= 15) {
    deserializer.deserialize<MMSerializationTypeContainer>();
  }
  return mmContainers;
}

template <typename CacheTrait>
serialization::CacheAllocatorMetadata
CacheAllocator<CacheTrait>::deserializeCacheAllocatorMetadata(
    Deserializer& deserializer) {
  auto meta = deserializer.deserialize<serialization::CacheAllocatorMetadata>();
  // TODO:
  // Once everyone is on v8 or later, remove the outter if.
  if (kCachelibVersion > 8) {
    if (*meta.ramFormatVersion() != kCacheRamFormatVersion) {
      throw std::runtime_error(
          folly::sformat("Expected cache ram format version {}. But found {}.",
                         kCacheRamFormatVersion, *meta.ramFormatVersion()));
    }
  }

  if (*meta.accessType() != AccessType::kId) {
    throw std::invalid_argument(
        folly::sformat("Expected {}, got {} for AccessType", *meta.accessType(),
                       AccessType::kId));
  }

  if (*meta.mmType() != MMType::kId) {
    throw std::invalid_argument(folly::sformat("Expected {}, got {} for MMType",
                                               *meta.mmType(), MMType::kId));
  }
  return meta;
}

template <typename CacheTrait>
int64_t CacheAllocator<CacheTrait>::getNumActiveHandles() const {
  return handleCount_.getSnapshot();
}

template <typename CacheTrait>
int64_t CacheAllocator<CacheTrait>::getHandleCountForThread() const {
  return handleCount_.tlStats();
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::resetHandleCountForThread_private() {
  handleCount_.tlStats() = 0;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::adjustHandleCountForThread_private(
    int64_t delta) {
  handleCount_.tlStats() += delta;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::initStats() {
  stats_.init();

  // deserialize the fragmentation size of each thread.
  for (const auto& pid : *metadata_.fragmentationSize()) {
    for (const auto& cid : pid.second) {
      (*stats_.fragmentationSize)[pid.first][cid.first].set(
          static_cast<uint64_t>(cid.second));
    }
  }

  // deserialize item counter stats
  stats_.numChainedParentItems.set(*metadata_.numChainedParentItems());
  stats_.numChainedChildItems.set(*metadata_.numChainedChildItems());
  stats_.numAbortedSlabReleases.set(
      static_cast<uint64_t>(*metadata_.numAbortedSlabReleases()));
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::forEachChainedItem(
    const Item& parent, std::function<void(ChainedItem&)> func) {
  auto l = chainedItemLocks_.lockShared(parent.getKey());

  auto headHandle = findChainedItem(parent);
  if (!headHandle) {
    return;
  }

  ChainedItem* head = &headHandle.get()->asChainedItem();
  while (head) {
    func(*head);
    head = head->getNext(compressor_);
  }
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::findChainedItem(const Item& parent) const {
  const auto cPtr = compressor_.compress(&parent);
  return chainedItemAccessContainer_->find(
      Key{reinterpret_cast<const char*>(&cPtr), ChainedItem::kKeySize});
}

template <typename CacheTrait>
template <typename Handle, typename Iter>
CacheChainedAllocs<CacheAllocator<CacheTrait>, Handle, Iter>
CacheAllocator<CacheTrait>::viewAsChainedAllocsT(const Handle& parent) {
  XDCHECK(parent);
  auto handle = parent.clone();
  if (!handle) {
    throw std::invalid_argument("Failed to clone item handle");
  }

  if (!handle->hasChainedItem()) {
    throw std::invalid_argument(
        folly::sformat("Failed to materialize chain. Parent does not have "
                       "chained items. Parent: {}",
                       parent->toString()));
  }

  auto l = chainedItemLocks_.lockShared(handle->getKey());
  auto head = findChainedItem(*handle);
  return CacheChainedAllocs<CacheAllocator<CacheTrait>, Handle, Iter>{
      std::move(l), std::move(handle), *head, compressor_};
}

template <typename CacheTrait>
GlobalCacheStats CacheAllocator<CacheTrait>::getGlobalCacheStats() const {
  GlobalCacheStats ret{};
  stats_.populateGlobalCacheStats(ret);

  ret.numItems = accessContainer_->getStats().numKeys;

  const uint64_t currTime = util::getCurrentTimeSec();
  ret.cacheInstanceUpTime = currTime - cacheInstanceCreationTime_;
  ret.ramUpTime = currTime - cacheCreationTime_;
  ret.nvmUpTime = currTime - nvmCacheState_.getCreationTime();
  ret.nvmCacheEnabled = nvmCache_ ? nvmCache_->isEnabled() : false;
  ret.reaperStats = getReaperStats();
  ret.numActiveHandles = getNumActiveHandles();

  ret.isNewRamCache = cacheCreationTime_ == cacheInstanceCreationTime_;
  ret.isNewNvmCache =
      nvmCacheState_.getCreationTime() == cacheInstanceCreationTime_;

  return ret;
}

template <typename CacheTrait>
CacheMemoryStats CacheAllocator<CacheTrait>::getCacheMemoryStats() const {
  const auto totalCacheSize = allocator_->getMemorySize();

  auto addSize = [this](size_t a, PoolId pid) {
    return a + allocator_->getPool(pid).getPoolSize();
  };
  const auto regularPoolIds = getRegularPoolIds();
  const auto ccCachePoolIds = getCCachePoolIds();
  size_t regularCacheSize = std::accumulate(
      regularPoolIds.begin(), regularPoolIds.end(), 0ULL, addSize);
  size_t compactCacheSize = std::accumulate(
      ccCachePoolIds.begin(), ccCachePoolIds.end(), 0ULL, addSize);

  return CacheMemoryStats{totalCacheSize,
                          regularCacheSize,
                          compactCacheSize,
                          allocator_->getAdvisedMemorySize(),
                          memMonitor_ ? memMonitor_->getMaxAdvisePct() : 0,
                          allocator_->getUnreservedMemorySize(),
                          nvmCache_ ? nvmCache_->getSize() : 0,
                          util::getMemAvailable(),
                          util::getRSSBytes()};
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::autoResizeEnabledForPool(PoolId pid) const {
  folly::SharedMutex::ReadHolder lock(compactCachePoolsLock_);
  if (isCompactCachePool_[pid]) {
    // compact caches need to be registered to enable auto resizing
    return optimizerEnabled_[pid];
  } else {
    // by default all regular pools participate in auto resizing
    return true;
  }
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::startCacheWorkers() {
  initWorkers();
}

template <typename CacheTrait>
template <typename T>
bool CacheAllocator<CacheTrait>::stopWorker(folly::StringPiece name,
                                            std::unique_ptr<T>& worker,
                                            std::chrono::seconds timeout) {
  std::lock_guard<std::mutex> l(workersMutex_);
  if (!worker) {
    return true;
  }

  bool ret = worker->stop(timeout);
  if (ret) {
    XLOGF(DBG1, "Stopped worker '{}'", name);
  } else {
    XLOGF(ERR, "Couldn't stop worker '{}', timeout: {} seconds", name,
          timeout.count());
  }
  worker.reset();
  return ret;
}

template <typename CacheTrait>
template <typename T, typename... Args>
bool CacheAllocator<CacheTrait>::startNewWorker(
    folly::StringPiece name,
    std::unique_ptr<T>& worker,
    std::chrono::milliseconds interval,
    Args&&... args) {
  if (!stopWorker(name, worker)) {
    return false;
  }

  std::lock_guard<std::mutex> l(workersMutex_);
  worker = std::make_unique<T>(*this, std::forward<Args>(args)...);
  bool ret = worker->start(interval, name);
  if (ret) {
    XLOGF(DBG1, "Started worker '{}'", name);
  } else {
    XLOGF(ERR, "Couldn't start worker '{}', interval: {} milliseconds", name,
          interval.count());
  }
  return ret;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::startNewPoolRebalancer(
    std::chrono::milliseconds interval,
    std::shared_ptr<RebalanceStrategy> strategy,
    unsigned int freeAllocThreshold) {
  return startNewWorker("PoolRebalancer", poolRebalancer_, interval, strategy,
                        freeAllocThreshold);
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::startNewPoolResizer(
    std::chrono::milliseconds interval,
    unsigned int poolResizeSlabsPerIter,
    std::shared_ptr<RebalanceStrategy> strategy) {
  return startNewWorker("PoolResizer", poolResizer_, interval,
                        poolResizeSlabsPerIter, strategy);
}
template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::startNewPoolOptimizer(
    std::chrono::seconds regularInterval,
    std::chrono::seconds ccacheInterval,
    std::shared_ptr<PoolOptimizeStrategy> strategy,
    unsigned int ccacheStepSizePercent) {
  // For now we are asking the worker to wake up every second to see whether
  // it should do actual size optimization. Probably need to move to using
  // the same interval for both, with confirmation of further experiments.
  const auto workerInterval = std::chrono::seconds(1);
  return startNewWorker("PoolOptimizer", poolOptimizer_, workerInterval,
                        strategy, regularInterval.count(),
                        ccacheInterval.count(), ccacheStepSizePercent);
}
template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::startNewMemMonitor(
    std::chrono::milliseconds interval,
    MemoryMonitor::Config config,
    std::shared_ptr<RebalanceStrategy> strategy) {
  return startNewWorker("MemoryMonitor", memMonitor_, interval,
                        std::move(config), strategy);
}
template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::startNewReaper(
    std::chrono::milliseconds interval,
    util::Throttler::Config reaperThrottleConfig) {
  return startNewWorker("Reaper", reaper_, interval, reaperThrottleConfig);
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::stopPoolRebalancer(
    std::chrono::seconds timeout) {
  return stopWorker("PoolRebalancer", poolRebalancer_, timeout);
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::stopPoolResizer(std::chrono::seconds timeout) {
  return stopWorker("PoolResizer", poolResizer_, timeout);
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::stopPoolOptimizer(
    std::chrono::seconds timeout) {
  return stopWorker("PoolOptimizer", poolOptimizer_, timeout);
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::stopMemMonitor(std::chrono::seconds timeout) {
  return stopWorker("MemoryMonitor", memMonitor_, timeout);
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::stopReaper(std::chrono::seconds timeout) {
  return stopWorker("Reaper", reaper_, timeout);
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::cleanupStrayShmSegments(
    const std::string& cacheDir, bool posix) {
  if (util::getStatIfExists(cacheDir, nullptr) && util::isDir(cacheDir)) {
    try {
      // cache dir exists. clean up only if there are no other processes
      // attached. if another process was attached, the following would fail.
      ShmManager::cleanup(cacheDir, posix);
    } catch (const std::exception& e) {
      XLOGF(ERR, "Error cleaning up {}. Exception: ", cacheDir, e.what());
      return false;
    }
  } else {
    // cache dir did not exist. Try to nuke the segments we know by name.
    // Any other concurrent process can not be attached to the segments or
    // even if it does, we want to mark it for destruction.
    ShmManager::removeByName(cacheDir, detail::kShmInfoName, posix);
    ShmManager::removeByName(cacheDir, detail::kShmCacheName, posix);
    ShmManager::removeByName(cacheDir, detail::kShmHashTableName, posix);
    ShmManager::removeByName(cacheDir, detail::kShmChainedItemHashTableName,
                             posix);
  }
  return true;
}

template <typename CacheTrait>
uint64_t CacheAllocator<CacheTrait>::getItemPtrAsOffset(const void* ptr) {
  // Return unt64_t instead of uintptr_t to accommodate platforms where
  // the two differ (e.g. Mac OS 12) - causing templating instantiation
  // errors downstream.

  // if this succeeeds, the address is valid within the cache.
  allocator_->getAllocInfo(ptr);

  if (!isOnShm_ || !shmManager_) {
    throw std::invalid_argument("Shared memory not used");
  }

  const auto& shm = shmManager_->getShmByName(detail::kShmCacheName);

  return reinterpret_cast<uint64_t>(ptr) -
         reinterpret_cast<uint64_t>(shm.getCurrentMapping().addr);
}

template <typename CacheTrait>
std::unordered_map<std::string, double>
CacheAllocator<CacheTrait>::getNvmCacheStatsMap() const {
  auto ret = nvmCache_ ? nvmCache_->getStatsMap()
                       : std::unordered_map<std::string, double>{};
  if (nvmAdmissionPolicy_) {
    auto policyStats = nvmAdmissionPolicy_->getCounters();
    for (const auto& kv : policyStats) {
      ret[kv.first] = kv.second;
    }
  }
  return ret;
}

} // namespace cachelib
} // namespace facebook
