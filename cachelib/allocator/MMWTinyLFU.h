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

#include <atomic>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <folly/Format.h>
#include <folly/Math.h>
#pragma GCC diagnostic pop

#include "cachelib/allocator/Cache.h"
#include "cachelib/allocator/CacheStats.h"
#include "cachelib/allocator/Util.h"
#include "cachelib/allocator/datastruct/MultiDList.h"
#include "cachelib/allocator/memory/serialize/gen-cpp2/objects_types.h"
#include "cachelib/common/CompilerUtils.h"
#include "cachelib/common/CountMinSketch.h"
#include "cachelib/common/Mutex.h"

namespace facebook::cachelib {
// Implements the W-TinyLFU cache eviction policy as described in -
// https://dgraph.io/blog/refs/TinyLFU%20-%20A%20Highly%20Efficient%20Cache%20Admission%20Policy.pdf
// https://highscalability.com/design-of-a-modern-cache/
//
// The cache is split into 3 parts,  the tiny cache, probation segment and
// protection segment. The tiny cache is typically sized to be 1% of the total
// cache with the main cache being the rest 99% consisting of probation and
// protection segments. W-TinyLFU uses the Segmented LRU (SLRU) policy for long
// term retention. An entry starts in the probationary segment and if accessed
// more than a configured threshold it is promoted to the protected segment
// (capped at 80% capacity). When the protected segment is full it evicts into
// the probationary segment, which may trigger a probationary entry to be
// discarded. This ensures that entries with a small reuse interval (the
// hottest) are retained and those that are less often reused (the coldest)
// become eligible for eviction.

// CacheAllocator with W Tiny LFU eviction policy with the protection segment
// It has a window initially to gauage the frequency of accesses of newly
// inserted items. The Main Cache is broken down into probation segment taking
// ~20% queue size and protection segment taking ~ 80%. For popular items
// that exceed a defined protected frequence. It will be preserved in the
// protection segment. If protectionSegment is full, it will no immediate
// evict out main queue, but moved into the tail of probation segment similar to
// modifed 2Q. This will prevent the popular items from being evicted out
// immediately.

// Counter Overhead:
// The windowToCacheSizeRatio determines the size of counters. The default
// value is 32 which means the counting window size is 32 times the
// cache size. After every 32 X cache_capacity number of items, the
// counts are halved to weigh frequency by recency. The function
// counterSize() returns the size of the counters
// in bytes. See MMWTinyLFU::maybeGrowAccessCountersLocked()
// implementation for how the size is computed. The decay described here only
// happens for tiny and not for other frequency counts in probation and
// protection.
//
// Tiny cache size:
// This default to 1%. There's no need to tune this parameter.
class MMWTinyLFU {
 public:
  // unique identifier per MMType
  static const int kId;

  // forward declaration;
  template <typename T>
  using Hook = DListHook<T>;
  using SerializationType = serialization::MMTinyLFUObject;
  using SerializationConfigType = serialization::MMTinyLFUConfig;
  using SerializationTypeContainer = serialization::MMTinyLFUCollection;

  enum LruType { Main, Probation, Tiny, NumTypes };

  // Config class for MMTinfyLFU
  struct Config {
    // create from serialized config
    explicit Config(SerializationConfigType configState)
        : Config(*configState.lruRefreshTime(),
                 *configState.lruRefreshRatio(),
                 *configState.updateOnWrite(),
                 *configState.updateOnRead(),
                 *configState.tryLockUpdate(),
                 *configState.windowToCacheSizeRatio(),
                 *configState.tinySizePercent(),
                 *configState.mmReconfigureIntervalSecs(),
                 *configState.newcomerWinsOnTie(),
                 *configState.protectionFreq_(),
                 *configState.protectionSegmentSizePct()) {}

    // @param time        the LRU refresh time in seconds.
    //                    An item will be promoted only once in each lru refresh
    //                    time depite the number of accesses it gets.
    // @param udpateOnW   whether to promote the item on write
    // @param updateOnR   whether to promote the item on read
    Config(uint32_t time, bool updateOnW, bool updateOnR)
        : Config(time,
                 updateOnW,
                 updateOnR,
                 /* try lock update */ false,
                 16,
                 1) {}

    // @param time              the LRU refresh time in seconds.
    //                          An item will be promoted only once in each lru
    //                          refresh time depite the number of accesses it
    //                          gets.
    // @param udpateOnW         whether to promote the item on write
    // @param updateOnR         whether to promote the item on read
    // @param windowToCacheSize multiplier of window size to cache size
    // @param tinySizePct       percentage number of tiny size to overall size
    Config(uint32_t time,
           bool updateOnW,
           bool updateOnR,
           size_t windowToCacheSize,
           size_t tinySizePct)
        : Config(time,
                 updateOnW,
                 updateOnR,
                 /* try lock update */ false,
                 windowToCacheSize,
                 tinySizePct) {}

    // @param time              the LRU refresh time in seconds.
    //                          An item will be promoted only once in each lru
    //                          refresh time depite the number of accesses it
    //                          gets.
    // @param udpateOnW         whether to promote the item on write
    // @param updateOnR         whether to promote the item on read
    // @param tryLockU          whether to use a try lock when doing update.
    // @param windowToCacheSize multiplier of window size to cache size
    // @param tinySizePct       percentage number of tiny size to overall size
    Config(uint32_t time,
           bool updateOnW,
           bool updateOnR,
           bool tryLockU,
           size_t windowToCacheSize,
           size_t tinySizePct)
        : Config(time,
                 0.,
                 updateOnW,
                 updateOnR,
                 tryLockU,
                 windowToCacheSize,
                 tinySizePct) {}

    // @param time                    the LRU refresh time in seconds.
    //                                An item will be promoted only once in each
    //                                lru refresh time depite the number of
    //                                accesses it gets.
    // @param ratio                   the lru refresh ratio. The ratio times the
    //                                oldest element's lifetime in warm queue
    //                                would be the minimum value of LRU refresh
    //                                time.
    // @param udpateOnW               whether to promote the item on write
    // @param updateOnR               whether to promote the item on read
    // @param tryLockU                whether to use a try lock when doing
    //                                update.
    // @param windowToCacheSize       multiplier of window size to cache size
    // @param tinySizePct             percentage number of tiny size to overall
    //                                size
    Config(uint32_t time,
           double ratio,
           bool updateOnW,
           bool updateOnR,
           bool tryLockU,
           size_t windowToCacheSize,
           size_t tinySizePct)
        : Config(time,
                 ratio,
                 updateOnW,
                 updateOnR,
                 tryLockU,
                 windowToCacheSize,
                 tinySizePct,
                 0) {}

    // @param time                    the LRU refresh time in seconds.
    //                                An item will be promoted only once in each
    //                                lru refresh time depite the number of
    //                                accesses it gets.
    // @param ratio                   the lru refresh ratio. The ratio times the
    //                                oldest element's lifetime in warm queue
    //                                would be the minimum value of LRU refresh
    //                                time.
    // @param udpateOnW               whether to promote the item on write
    // @param updateOnR               whether to promote the item on read
    // @param tryLockU                whether to use a try lock when doing
    //                                update.
    // @param windowToCacheSize       multiplier of window size to cache size
    // @param tinySizePct             percentage number of tiny size to overall
    //                                size
    // @param mmReconfigureInterval   Time interval for recalculating lru
    //                                refresh time according to the ratio.
    Config(uint32_t time,
           double ratio,
           bool updateOnW,
           bool updateOnR,
           bool tryLockU,
           size_t windowToCacheSize,
           size_t tinySizePct,
           uint32_t mmReconfigureInterval)
        : defaultLruRefreshTime(time),
          lruRefreshRatio(ratio),
          updateOnWrite(updateOnW),
          updateOnRead(updateOnR),
          tryLockUpdate(tryLockU),
          windowToCacheSizeRatio(windowToCacheSize),
          tinySizePercent(tinySizePct),
          mmReconfigureIntervalSecs(
              std::chrono::seconds(mmReconfigureInterval)) {
      checkConfig();
    }

    // @param time                    the LRU refresh time in seconds.
    //                                An item will be promoted only once in each
    //                                lru refresh time depite the number of
    //                                accesses it gets.
    // @param ratio                   the lru refresh ratio. The ratio times the
    //                                oldest element's lifetime in warm queue
    //                                would be the minimum value of LRU refresh
    //                                time.
    // @param udpateOnW               whether to promote the item on write
    // @param updateOnR               whether to promote the item on read
    // @param tryLockU                whether to use a try lock when doing
    //                                update.
    // @param windowToCacheSize       multiplier of window size to cache size
    // @param tinySizePct             percentage number of tiny size to overall
    //                                size
    // @param mmReconfigureInterval   Time interval for recalculating lru
    //                                refresh time according to the ratio.
    // @param newcomerWinsOnTie       If true, new comer will replace existing
    //                                item if their access frequencies tie.
    Config(uint32_t time,
           double ratio,
           bool updateOnW,
           bool updateOnR,
           bool tryLockU,
           size_t windowToCacheSize,
           size_t tinySizePct,
           uint32_t mmReconfigureInterval,
           bool _newcomerWinsOnTie)
        : defaultLruRefreshTime(time),
          lruRefreshRatio(ratio),
          updateOnWrite(updateOnW),
          updateOnRead(updateOnR),
          tryLockUpdate(tryLockU),
          windowToCacheSizeRatio(windowToCacheSize),
          tinySizePercent(tinySizePct),
          mmReconfigureIntervalSecs(
              std::chrono::seconds(mmReconfigureInterval)),
          newcomerWinsOnTie(_newcomerWinsOnTie) {
      checkConfig();
    }

    // @param time                     the LRU refresh time in seconds.
    //                                 An item will be promoted only once in
    //                                 each lru refresh time depite the number
    //                                 of accesses it gets.
    // @param ratio                    the lru refresh ratio. The ratio times
    // the
    //                                 oldest element's lifetime in warm queue
    //                                 would be the minimum value of LRU refresh
    //                                 time.
    // @param udpateOnW                whether to promote the item on write
    // @param updateOnR                whether to promote the item on read
    // @param tryLockU                 whether to use a try lock when doing
    //                                 update.
    // @param windowToCacheSize        multiplier of window size to cache size
    // @param tinySizePct              percentage number of tiny size to overall
    //                                 size
    // @param mmReconfigureInterval    Time interval for recalculating lru
    //                                 refresh time according to the ratio.
    // @param newcomerWinsOnTie        If true, new comer will replace existing
    //                                 item if their access frequencies tie.
    // @param enabledProtectionSegment If true, items that has been accessed
    //                                 more than protection frequency will be
    //                                 moved probation segment before evicted
    //                                 out from main cache.
    // @param protectionSegmentSizePct percentage number of protection segment
    //                                 to the main cache size.
    Config(uint32_t time,
           double ratio,
           bool updateOnW,
           bool updateOnR,
           bool tryLockU,
           size_t windowToCacheSize,
           size_t tinySizePct,
           uint32_t mmReconfigureInterval,
           bool _newcomerWinsOnTie,
           int protectionFreq_,
           int protectionSegmentSizePct)
        : defaultLruRefreshTime(time),
          lruRefreshRatio(ratio),
          updateOnWrite(updateOnW),
          updateOnRead(updateOnR),
          tryLockUpdate(tryLockU),
          windowToCacheSizeRatio(windowToCacheSize),
          tinySizePercent(tinySizePct),
          mmReconfigureIntervalSecs(
              std::chrono::seconds(mmReconfigureInterval)),
          newcomerWinsOnTie(_newcomerWinsOnTie),
          protectionFreq_(protectionFreq_),
          protectionSegmentSizePct(protectionSegmentSizePct) {
      checkConfig();
    }

    Config() = default;
    Config(const Config& rhs) = default;
    Config(Config&& rhs) = default;

    Config& operator=(const Config& rhs) = default;
    Config& operator=(Config&& rhs) = default;

    void checkConfig() {
      if (tinySizePercent < 1 || tinySizePercent > 50) {
        throw std::invalid_argument(
            folly::sformat("Invalid tiny cache size {}. Tiny cache size "
                           "must be between 1% and 50% of total cache size ",
                           tinySizePercent));
      }
      if (windowToCacheSizeRatio < 2 || windowToCacheSizeRatio > 128) {
        throw std::invalid_argument(
            folly::sformat("Invalid window to cache size ratio {}. The ratio "
                           "must be between 2 and 128",
                           windowToCacheSizeRatio));
      }
    }

    template <typename... Args>
    void addExtraConfig(Args...) {}

    // threshold value in seconds to compare with a node's update time to
    // determine if we need to update the position of the node in the linked
    // list. By default this is 60s to reduce the contention on the lru lock.
    uint32_t defaultLruRefreshTime{60};
    uint32_t lruRefreshTime{defaultLruRefreshTime};

    // ratio of LRU refresh time to the tail age. If a refresh time computed
    // according to this ratio is larger than lruRefreshtime, we will adopt
    // this one instead of the lruRefreshTime set.
    double lruRefreshRatio{0.};

    // whether the lru needs to be updated on writes for recordAccess. If
    // false, accessing the cache for writes does not promote the cached item
    // to the head of the lru.
    bool updateOnWrite{false};

    // whether the lru needs to be updated on reads for recordAccess. If
    // false, accessing the cache for reads does not promote the cached item
    // to the head of the lru.
    bool updateOnRead{true};

    // whether to tryLock or lock the lru lock when attempting promotion on
    // access. If set, and tryLock fails, access will not result in promotion.
    bool tryLockUpdate{false};

    // The multiplier for window size given the cache size.
    size_t windowToCacheSizeRatio{32};

    // The size of tiny cache, as a percentage of the total size.
    size_t tinySizePercent{1};

    // Minimum interval between reconfigurations. If 0, reconfigure is never
    // called.
    std::chrono::seconds mmReconfigureIntervalSecs{};

    // If true, then if an item in the tail of the Tiny queue ties with the
    // item in the tail of the main queue, the item from Tiny (newcomer) will
    // replace the item from Main. This is fine for a default, but for
    // strictly scan patterns (access a key exactly once and move on), this
    // is not a desirable behavior (we'll always cache miss).
    bool newcomerWinsOnTie{true};

    // The min access frequency in order to be pushed into the protection
    // segment.
    size_t protectionFreq_{3};

    //  The size of protection segment, as a percentage of the main cache size.
    size_t protectionSegmentSizePct{80};
  };

  // The container object which can be used to keep track of objects of type
  // T. T must have a public member of type Hook. This object is wrapper
  // around DList, is thread safe and can be accessed from multiple threads.
  // The current implementation models an LRU using the above DList
  // implementation.
  template <typename T, Hook<T> T::*HookPtr>
  struct Container {
   private:
    using LruList = MultiDList<T, HookPtr>;
    using Mutex = folly::SpinLock;
    using LockHolder = std::unique_lock<Mutex>;
    using PtrCompressor = typename T::PtrCompressor;
    using Time = typename Hook<T>::Time;
    using CompressedPtrType = typename T::CompressedPtrType;
    using RefFlags = typename T::Flags;

   public:
    Container() = default;
    Container(Config c, PtrCompressor compressor)
        : lru_(LruType::NumTypes, std::move(compressor)),
          config_(std::move(c)) {
      maybeGrowAccessCountersLocked();
      lruRefreshTime_ = config_.lruRefreshTime;
      nextReconfigureTime_ =
          config_.mmReconfigureIntervalSecs.count() == 0
              ? std::numeric_limits<Time>::max()
              : static_cast<Time>(util::getCurrentTimeSec()) +
                    config_.mmReconfigureIntervalSecs.count();
    }
    Container(serialization::MMTinyLFUObject object, PtrCompressor compressor);

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;

    // records the information that the node was accessed. This could bump up
    // the node to the head of the lru depending on the time when the node was
    // last updated in lru and the kLruRefreshTime. If the node was moved to
    // the head in the lru, the node's updateTime will be updated
    // accordingly.
    //
    // @param node  node that we want to mark as relevant/accessed
    // @param mode  the mode for the access operation.
    //
    // @return      True if the information is recorded and bumped the node
    //              to the head of the lru, returns false otherwise
    bool recordAccess(T& node, AccessMode mode) noexcept;

    // adds the given node into the container and marks it as being present in
    // the container. The node is added to the head of the lru.
    //
    // @param node  The node to be added to the container.
    // @return  True if the node was successfully added to the container. False
    //          if the node was already in the contianer. On error state of node
    //          is unchanged.
    bool add(T& node) noexcept;

    // removes the node from the lru and sets it previous and next to nullptr.
    //
    // @param node  The node to be removed from the container.
    // @return  True if the node was successfully removed from the container.
    //          False if the node was not part of the container. On error, the
    //          state of node is unchanged.
    bool remove(T& node) noexcept;

    class LockedIterator;
    // same as the above but uses an iterator context. The iterator is updated
    // on removal of the corresponding node to point to the next node. The
    // iterator context holds the lock on the lru.
    //
    // iterator will be advanced to the next node after removing the node
    //
    // @param it    Iterator that will be removed
    void remove(LockedIterator& it) noexcept;

    // replaces one node with another, at the same position
    //
    // @param oldNode   node being replaced
    // @param newNode   node to replace oldNode with
    //
    // @return true  If the replace was successful. Returns false if the
    //               destination node did not exist in the container, or if the
    //               source node already existed.
    bool replace(T& oldNode, T& newNode) noexcept;

    // context for iterating the MM container. At any given point of time,
    // there can be only one iterator active since we need to lock the LRU for
    // iteration. we can support multiple iterators at same time, by using a
    // shared ptr in the context for the lock holder in the future.
    class LockedIterator {
     public:
      using ListIterator = typename LruList::DListIterator;
      // noncopyable but movable.
      LockedIterator(const LockedIterator&) = delete;
      LockedIterator& operator=(const LockedIterator&) = delete;
      LockedIterator(LockedIterator&&) noexcept = default;

      LockedIterator& operator++() noexcept {
        ++getIter();
        return *this;
      }

      LockedIterator& operator--() {
        throw std::invalid_argument(
            "Decrementing eviction iterator is not supported");
      }

      T* operator->() const noexcept { return getIter().operator->(); }
      T& operator*() const noexcept { return getIter().operator*(); }

      bool operator==(const LockedIterator& other) const noexcept {
        return &c_ == &other.c_ && tIter_ == other.tIter_ &&
               pIter_ == other.pIter_ && mIter_ == other.mIter_;
      }

      bool operator!=(const LockedIterator& other) const noexcept {
        return !(*this == other);
      }

      explicit operator bool() const noexcept {
        return tIter_ || pIter_ || mIter_;
      }

      T* get() const noexcept { return getIter().get(); }

      // Invalidates this iterator
      void reset() noexcept {
        // Point iterator to first list's rend
        tIter_.reset();
        pIter_.reset();
        mIter_.reset();
      }

      // 1. Invalidate this iterator
      // 2. Unlock
      void destroy() {
        reset();
        if (l_.owns_lock()) {
          l_.unlock();
        }
      }

      // Reset this iterator to the beginning
      void resetToBegin() {
        if (!l_.owns_lock()) {
          l_.lock();
        }
        tIter_.resetToBegin();
        pIter_.resetToBegin();
        mIter_.resetToBegin();
      }

     private:
      // private because it's easy to misuse and cause deadlock for MMWTinyLFU
      LockedIterator& operator=(LockedIterator&&) noexcept = default;

      // create an lru iterator with the lock being held.
      explicit LockedIterator(LockHolder l,
                              const Container<T, HookPtr>& c) noexcept;
      // The iterator first go through tiny till the tail frequency is high than
      // the probation segment. Second iterate through probation segment. Lastly
      // iterate through the protection segment.
      const ListIterator& getIter() const noexcept {
        if (!pIter_ && !mIter_) {
          return tIter_;
        }
        if (!pIter_ && !tIter_) {
          return mIter_;
        }
        if (!tIter_ && !mIter_) {
          return pIter_;
        }

        if (!pIter_) {
          return tIter_;
        }
        if (!tIter_) {
          return pIter_;
        }

        return !c_.admitToProbation(*tIter_, *pIter_) ? tIter_ : pIter_;
      }

      ListIterator& getIter() noexcept {
        return const_cast<ListIterator&>(
            static_cast<const LockedIterator*>(this)->getIter());
      }

      // only the container can create iterators
      friend Container<T, HookPtr>;

      const Container<T, HookPtr>& c_;
      // Tiny and main cache iterators
      ListIterator tIter_;
      ListIterator pIter_;
      ListIterator mIter_;
      // lock protecting the validity of the iterator
      LockHolder l_;
    };

    Config getConfig() const;

    void setConfig(const Config& newConfig);

    bool isEmpty() const noexcept {
      LockHolder l(lruMutex_);
      return lru_.size() == 0;
    }

    size_t size() const noexcept {
      LockHolder l(lruMutex_);
      return lru_.size();
    }

    // reconfigure the MMContainer: update refresh time according to current
    // tail age
    void reconfigureLocked(const Time& currTime);

    size_t counterSize() const noexcept {
      LockHolder l(lruMutex_);
      return accessFreq_.getByteSize();
    }

    // Returns the eviction age stats. See CacheStats.h for details
    EvictionAgeStat getEvictionAgeStat(uint64_t projectedLength) const noexcept;

    // Obtain an iterator that start from the tail and can be used
    // to search for evictions. This iterator holds a lock to this
    // container and only one such iterator can exist at a time
    LockedIterator getEvictionIterator() const noexcept;

    // Execute provided function under container lock. Function gets
    // iterator passed as parameter.
    template <typename F>
    void withEvictionIterator(F&& f);

    // Execute provided function under container lock.
    template <typename F>
    void withContainerLock(F&& f);

    // for saving the state of the lru
    //
    // precondition:  serialization must happen without any reader or writer
    // present. Any modification of this object afterwards will result in an
    // invalid, inconsistent state for the serialized data.
    //
    serialization::MMTinyLFUObject saveState() const noexcept;

    // return the stats for this container.
    MMContainerStat getStats() const noexcept;

    static LruType getLruType(const T& node) noexcept {
      return isTiny(node)
                 ? LruType::Tiny
                 : (isProbation(node) ? LruType::Probation : LruType::Main);
    }

   private:
    EvictionAgeStat getEvictionAgeStatLocked(
        uint64_t projectedLength) const noexcept;

    static Time getUpdateTime(const T& node) noexcept {
      return (node.*HookPtr).getUpdateTime();
    }

    static void setUpdateTime(T& node, Time time) noexcept {
      (node.*HookPtr).setUpdateTime(time);
    }

    // As the cache grows, the frequency counters may need to grow.
    void maybeGrowAccessCountersLocked() noexcept;

    // Update frequency count for the node. Halve all counts if
    // we've reached the end of the window.
    void updateFrequenciesLocked(const T& node) noexcept;

    // Promote the tail of tinyCache to mainCache probation segment  (tiny >>
    // probation) if it has higher frequency count than the tail of the
    // main cache.
    void maybePromoteTailLocked() noexcept;

    // Returns the hash of node's key
    static size_t hashNode(const T& node) noexcept {
      return folly::hasher<folly::StringPiece>()(node.getKey());
    }

    // Returns true if tiny node must be admitted to main cache since its
    // frequency is higher than that of the main node.
    bool admitToProbation(const T& tinyNode, const T& mainNode) const noexcept {
      auto tinyFreq = accessFreq_.getCount(hashNode(tinyNode));
      auto mainFreq = accessFreq_.getCount(hashNode(mainNode));
      if (config_.newcomerWinsOnTie) {
        return tinyFreq >= mainFreq;
      } else {
        return tinyFreq > mainFreq;
      }
    }

    // remove node from lru and adjust insertion points
    //
    // @param node          node to remove
    void removeLocked(T& node) noexcept;

    static bool isTiny(const T& node) noexcept {
      return node.template isFlagSet<RefFlags::kMMFlag0>();
    }

    static bool isAccessed(const T& node) noexcept {
      return node.template isFlagSet<RefFlags::kMMFlag1>();
    }

    static bool isProbation(const T& node) noexcept {
      return node.template isFlagSet<RefFlags::kMMFlag2>();
    }

    // Bit MM_BIT_0 is used to record if the item is in tiny cache.
    static void markTiny(T& node) noexcept {
      node.template setFlag<RefFlags::kMMFlag0>();
    }
    static void unmarkTiny(T& node) noexcept {
      node.template unSetFlag<RefFlags::kMMFlag0>();
    }

    // Bit MM_BIT_1 is used to record if the item has been accessed since being
    // written in cache. Unaccessed items are ignored when determining projected
    // update time.
    static void markAccessed(T& node) noexcept {
      node.template setFlag<RefFlags::kMMFlag1>();
    }
    static void unmarkAccessed(T& node) noexcept {
      node.template unSetFlag<RefFlags::kMMFlag1>();
    }

    static void markProbation(T& node) noexcept {
      node.template setFlag<RefFlags::kMMFlag2>();
    }
    static void unmarkProbation(T& node) noexcept {
      node.template unSetFlag<RefFlags::kMMFlag2>();
    }

    // Initial cache capacity estimate for count-min-sketch
    static constexpr size_t kDefaultCapacity = 100;

    // Number of hashes
    static constexpr size_t kHashCount = 4;

    // The error threshold for frequency calculation
    static constexpr size_t kErrorThreshold = 5;

    // decay rate for frequency
    static constexpr double kDecayFactor = 0.5;

    // protects all operations on the lru. We never really just read the state
    // of the LRU. Hence we dont really require a RW mutex at this point of
    // time.
    mutable Mutex lruMutex_;

    // the lru
    LruList lru_;

    // the window size counter
    size_t windowSize_{0};

    // maximum value of window size which when hit the counters are halved
    size_t maxWindowSize_{0};

    // The capacity for which the counters are sized
    size_t capacity_{0};

    // The next time to reconfigure the container.
    std::atomic<Time> nextReconfigureTime_{};

    // How often to promote an item in the eviction queue.
    std::atomic<uint32_t> lruRefreshTime_{};

    // Max lruFreshTime.
    static constexpr uint32_t kLruRefreshTimeCap{900};

    // Config for this lru.
    // Write access to the MMWTinyLFU Config is serialized.
    // Reads may be racy.
    Config config_{};

    // Approximate streaming frequency counters. The counts are halved every
    // time the maxWindowSize is hit.
    facebook::cachelib::util::CountMinSketch accessFreq_{};

    FRIEND_TEST(MMWTinyLFUTest, SegmentStress);
    FRIEND_TEST(MMWTinyLFUTest, TinyLFUBasic);
    FRIEND_TEST(MMWTinyLFUTest, Reconfigure);
  };
};

/* Container Interface Implementation */
template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
MMWTinyLFU::Container<T, HookPtr>::Container(
    serialization::MMTinyLFUObject object, PtrCompressor compressor)
    : lru_(*object.lrus(), std::move(compressor)), config_(*object.config()) {
  lruRefreshTime_ = config_.lruRefreshTime;
  nextReconfigureTime_ = config_.mmReconfigureIntervalSecs.count() == 0
                             ? std::numeric_limits<Time>::max()
                             : static_cast<Time>(util::getCurrentTimeSec()) +
                                   config_.mmReconfigureIntervalSecs.count();
  maybeGrowAccessCountersLocked();
}

template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
void MMWTinyLFU::Container<T,
                           HookPtr>::maybeGrowAccessCountersLocked() noexcept {
  size_t capacity = lru_.size();
  // If the new capacity ask is more than double the current size, recreate
  // the approx frequency counters.
  if (2 * capacity_ > capacity) {
    return;
  }

  capacity_ = std::max(capacity, kDefaultCapacity);

  // The window counter that's incremented on every fetch.
  windowSize_ = 0;

  // The frequency counters are halved every maxWindowSize_ fetches to decay the
  // frequency counts.
  maxWindowSize_ = capacity_ * config_.windowToCacheSizeRatio;

  // Number of frequency counters - roughly equal to the window size divided by
  // error tolerance.
  size_t numCounters =
      static_cast<size_t>(std::exp(1.0) * maxWindowSize_ / kErrorThreshold);

  numCounters = folly::nextPowTwo(numCounters);

  // The CountMinSketch frequency counter
  accessFreq_ =
      facebook::cachelib::util::CountMinSketch(numCounters, kHashCount);
}

template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
bool MMWTinyLFU::Container<T, HookPtr>::recordAccess(T& node,
                                                     AccessMode mode) noexcept {
  if ((mode == AccessMode::kWrite && !config_.updateOnWrite) ||
      (mode == AccessMode::kRead && !config_.updateOnRead)) {
    return false;
  }

  const auto curr = static_cast<Time>(util::getCurrentTimeSec());
  // check if the node is still being memory managed
  if (node.isInMMContainer() &&
      ((curr >= getUpdateTime(node) +
                    lruRefreshTime_.load(std::memory_order_relaxed)) ||
       !isAccessed(node))) {
    if (!isAccessed(node)) {
      markAccessed(node);
    }
    LockHolder l(lruMutex_, std::defer_lock);
    if (config_.tryLockUpdate) {
      l.try_lock();
    } else {
      l.lock();
    }
    if (!l.owns_lock()) {
      return false;
    }
    reconfigureLocked(curr);
    if (!node.isInMMContainer()) {
      return false;
    }

    auto lruType = getLruType(node);
    lru_.getList(lruType).moveToHead(node);

    if (lruType == LruType::Probation) {
      auto freq = accessFreq_.getCount(hashNode(node));
      if (freq > config_.protectionFreq_) {
        lru_.getList(LruType::Probation).remove(node);
        lru_.getList(LruType::Main).linkAtHead(node);
        unmarkProbation(node);

        const auto totalMainSize = lru_.getList(LruType::Probation).size() +
                                   lru_.getList(LruType::Main).size();
        const auto expectedMainSize =
            config_.protectionSegmentSizePct * totalMainSize / 100;
        // Since all segments are implemented as LRU (Least Recently Used)
        // queues, adding demoted items to the head of the probation queue could
        // potentially evict fresh items. This could lead to a reduced average
        // life cycle of items in the queue. Therefore evicting the tail of the
        // probation queue which contains either the least frequently used or
        // least recently used items.
        if (lru_.getList(LruType::Main).size() > expectedMainSize) {
          auto mainTail = lru_.getList(LruType::Main).getTail();
          if (mainTail) {
            lru_.getList(LruType::Main).remove(*mainTail);
            lru_.getList(LruType::Probation).linkAtTail(*mainTail);
            markProbation(*mainTail);
          }
        }
      }
    }
    setUpdateTime(node, curr);
    updateFrequenciesLocked(node);
    return true;
  }
  return false;
}

template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
cachelib::EvictionAgeStat MMWTinyLFU::Container<T, HookPtr>::getEvictionAgeStat(
    uint64_t projectedLength) const noexcept {
  LockHolder l(lruMutex_);
  return getEvictionAgeStatLocked(projectedLength);
}

template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
cachelib::EvictionAgeStat
MMWTinyLFU::Container<T, HookPtr>::getEvictionAgeStatLocked(
    uint64_t projectedLength) const noexcept {
  EvictionAgeStat stat;
  const auto curr = static_cast<Time>(util::getCurrentTimeSec());

  auto& list = lru_.getList(LruType::Main);
  auto it = list.rbegin();
  stat.warmQueueStat.oldestElementAge =
      it != list.rend() ? curr - getUpdateTime(*it) : 0;
  stat.warmQueueStat.size = list.size();
  for (size_t numSeen = 0; numSeen < projectedLength && it != list.rend();
       ++numSeen, ++it) {
  }
  stat.warmQueueStat.projectedAge = it != list.rend()
                                        ? curr - getUpdateTime(*it)
                                        : stat.warmQueueStat.oldestElementAge;
  return stat;
}

template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
void MMWTinyLFU::Container<T, HookPtr>::updateFrequenciesLocked(
    const T& node) noexcept {
  accessFreq_.increment(hashNode(node));
  ++windowSize_;
  // decay counts every maxWindowSize_ .  This avoids having items that were
  // accessed frequently (were hot) but aren't being accessed anymore (are
  // cold) from staying in cache forever.
  if (windowSize_ == maxWindowSize_) {
    windowSize_ >>= 1;
    accessFreq_.decayCountsBy(kDecayFactor);
  }
}

// evict items from the higher tier cache to lower tier cache if it is full
template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
void MMWTinyLFU::Container<T, HookPtr>::maybePromoteTailLocked() noexcept {
  // Choose eviction candidate and place it at the tail of tiny cache
  // from where evictions occur.
  auto probationNode = lru_.getList(LruType::Probation).getTail();
  if (!probationNode) {
    return;
  }
  auto tinyNode = lru_.getList(LruType::Tiny).getTail();
  if (!tinyNode) {
    return;
  }

  if (admitToProbation(*tinyNode, *probationNode)) {
    lru_.getList(LruType::Tiny).remove(*tinyNode);
    lru_.getList(LruType::Probation).linkAtHead(*tinyNode);
    unmarkTiny(*tinyNode);
    markTiny(*probationNode);
    lru_.getList(LruType::Probation).remove(*probationNode);
    lru_.getList(LruType::Tiny).linkAtTail(*probationNode);
    unmarkProbation(*probationNode);
    markProbation(*tinyNode);
    return;
  }

  // A node with high frequency at the tail of main cache might prevent
  // promotions from tiny cache from happening for a long time. Relocate
  // the tail of main cache to prevent this.
  lru_.getList(LruType::Probation).moveToHead(*probationNode);
}

template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
bool MMWTinyLFU::Container<T, HookPtr>::add(T& node) noexcept {
  const auto currTime = static_cast<Time>(util::getCurrentTimeSec());
  LockHolder l(lruMutex_);
  if (node.isInMMContainer()) {
    return false;
  }

  auto& tinyLru = lru_.getList(LruType::Tiny);
  tinyLru.linkAtHead(node);
  markTiny(node);
  // Initialize the frequency count for this node.
  updateFrequenciesLocked(node);
  // If tiny cache is full, unconditionally promote tail to main cache.
  const auto expectedSize = config_.tinySizePercent * lru_.size() / 100;
  if (lru_.getList(LruType::Tiny).size() > expectedSize) {
    auto tailNode = tinyLru.getTail();
    tinyLru.remove(*tailNode);

    lru_.getList(LruType::Probation).linkAtHead(*tailNode);
    unmarkTiny(*tailNode);
    markProbation(*tailNode);
  } else {
    // In case the tiny and probation cache are full. Swap the tails
    // if the tiny tail has a higher frequency than the probation tail.
    maybePromoteTailLocked();
  }
  // If the number of counters are too small for the cache size, double them.
  // TODO: If this shows in latency, we may need to grow the counters
  // asynchronously.
  maybeGrowAccessCountersLocked();

  node.markInMMContainer();
  setUpdateTime(node, currTime);
  unmarkAccessed(node);
  return true;
}

template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
typename MMWTinyLFU::Container<T, HookPtr>::LockedIterator
MMWTinyLFU::Container<T, HookPtr>::getEvictionIterator() const noexcept {
  LockHolder l(lruMutex_);
  return LockedIterator{std::move(l), *this};
}

template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
template <typename F>
void MMWTinyLFU::Container<T, HookPtr>::withEvictionIterator(F&& fun) {
  // TinyLFU uses spin lock which does not support combined locking
  fun(getEvictionIterator());
}

template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
template <typename F>
void MMWTinyLFU::Container<T, HookPtr>::withContainerLock(F&& fun) {
  LockHolder l(lruMutex_);
  fun();
}

template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
void MMWTinyLFU::Container<T, HookPtr>::removeLocked(T& node) noexcept {
  if (isTiny(node)) {
    lru_.getList(LruType::Tiny).remove(node);
    unmarkTiny(node);
  } else if (isProbation(node)) {
    lru_.getList(LruType::Probation).remove(node);
    unmarkProbation(node);
  } else {
    lru_.getList(LruType::Main).remove(node);
  }

  unmarkAccessed(node);
  node.unmarkInMMContainer();
  return;
}

template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
bool MMWTinyLFU::Container<T, HookPtr>::remove(T& node) noexcept {
  LockHolder l(lruMutex_);
  if (!node.isInMMContainer()) {
    return false;
  }
  removeLocked(node);
  return true;
}

template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
void MMWTinyLFU::Container<T, HookPtr>::remove(LockedIterator& it) noexcept {
  T& node = *it;
  XDCHECK(node.isInMMContainer());
  ++it;
  removeLocked(node);
}

template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
bool MMWTinyLFU::Container<T, HookPtr>::replace(T& oldNode,
                                                T& newNode) noexcept {
  LockHolder l(lruMutex_);
  if (isTiny(newNode) || isProbation(newNode)) {
    return false;
  }

  if (!oldNode.isInMMContainer() || newNode.isInMMContainer()) {
    return false;
  }
  const auto updateTime = getUpdateTime(oldNode);

  if (isTiny(oldNode)) {
    lru_.getList(LruType::Tiny).replace(oldNode, newNode);
    unmarkTiny(oldNode);
    markTiny(newNode);
  } else if (isProbation(oldNode)) {
    lru_.getList(LruType::Probation).replace(oldNode, newNode);
    unmarkProbation(oldNode);
    markProbation(newNode);
  } else {
    lru_.getList(LruType::Main).replace(oldNode, newNode);
  }

  oldNode.unmarkInMMContainer();
  newNode.markInMMContainer();
  setUpdateTime(newNode, updateTime);
  if (isAccessed(oldNode)) {
    markAccessed(newNode);
  } else {
    unmarkAccessed(newNode);
  }
  return true;
}

template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
typename MMWTinyLFU::Config MMWTinyLFU::Container<T, HookPtr>::getConfig()
    const {
  LockHolder l(lruMutex_);
  return config_;
}

template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
void MMWTinyLFU::Container<T, HookPtr>::setConfig(const Config& c) {
  LockHolder l(lruMutex_);
  config_ = c;
  lruRefreshTime_.store(config_.lruRefreshTime, std::memory_order_relaxed);
  nextReconfigureTime_ = config_.mmReconfigureIntervalSecs.count() == 0
                             ? std::numeric_limits<Time>::max()
                             : static_cast<Time>(util::getCurrentTimeSec()) +
                                   config_.mmReconfigureIntervalSecs.count();
}

template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
serialization::MMTinyLFUObject MMWTinyLFU::Container<T, HookPtr>::saveState()
    const noexcept {
  serialization::MMTinyLFUConfig configObject;
  *configObject.lruRefreshTime() =
      lruRefreshTime_.load(std::memory_order_relaxed);
  *configObject.lruRefreshRatio() = config_.lruRefreshRatio;
  *configObject.updateOnWrite() = config_.updateOnWrite;
  *configObject.updateOnRead() = config_.updateOnRead;
  *configObject.windowToCacheSizeRatio() = config_.windowToCacheSizeRatio;
  *configObject.tinySizePercent() = config_.tinySizePercent;
  *configObject.mmReconfigureIntervalSecs() =
      config_.mmReconfigureIntervalSecs.count();
  *configObject.newcomerWinsOnTie() = config_.newcomerWinsOnTie;
  *configObject.protectionSegmentSizePct() = config_.protectionSegmentSizePct;
  *configObject.protectionFreq_() = config_.protectionFreq_;

  // TODO: May be save/restore the counters.

  serialization::MMTinyLFUObject object;
  *object.config() = configObject;
  *object.lrus() = lru_.saveState();
  return object;
}

template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
MMContainerStat MMWTinyLFU::Container<T, HookPtr>::getStats() const noexcept {
  LockHolder l(lruMutex_);
  auto* tail = lru_.size() == 0 ? nullptr : lru_.rbegin().get();
  return {lru_.size(),
          tail == nullptr ? 0 : getUpdateTime(*tail),
          lruRefreshTime_.load(std::memory_order_relaxed),
          0,
          0,
          0,
          0};
}

template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
void MMWTinyLFU::Container<T, HookPtr>::reconfigureLocked(
    const Time& currTime) {
  if (currTime < nextReconfigureTime_) {
    return;
  }
  nextReconfigureTime_ = currTime + config_.mmReconfigureIntervalSecs.count();

  // update LRU refresh time
  auto stat = getEvictionAgeStatLocked(0);
  auto lruRefreshTime = std::min(
      std::max(config_.defaultLruRefreshTime,
               static_cast<uint32_t>(stat.warmQueueStat.oldestElementAge *
                                     config_.lruRefreshRatio)),
      kLruRefreshTimeCap);

  lruRefreshTime_.store(lruRefreshTime, std::memory_order_relaxed);
}

// Locked Iterator Context Implementation
template <typename T, MMWTinyLFU::Hook<T> T::*HookPtr>
MMWTinyLFU::Container<T, HookPtr>::LockedIterator::LockedIterator(
    LockHolder l, const Container<T, HookPtr>& c) noexcept
    : c_(c),
      tIter_(c.lru_.getList(LruType::Tiny).rbegin()),
      pIter_(c.lru_.getList(LruType::Probation).rbegin()),
      mIter_(c.lru_.getList(LruType::Main).rbegin()),
      l_(std::move(l)) {}
} // namespace facebook::cachelib
