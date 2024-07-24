/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/OrderedHashMap.h"

#include "hermes/Support/ErrorHandling.h"
#include "hermes/VM/BuildMetadata.h"
#include "hermes/VM/GCPointer-inline.h"
#include "hermes/VM/Operations.h"

namespace hermes {
namespace vm {
//===----------------------------------------------------------------------===//
// class HashMapEntryBase

template <typename Data>
const VTable HashMapEntryBase<Data>::vt{
    CellKind::HashMapEntryKind,
    cellSize<HashMapEntryBase<Data>>()};

void HashMapEntryBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  const auto *self = static_cast<const HashMapEntry *>(cell);
  mb.setVTable(&HashMapEntry::vt);
  mb.addField("key", &self->key);
  mb.addField("value", &self->value);
  mb.addField("prevIterationEntry", &self->prevIterationEntry);
  mb.addField("nextIterationEntry", &self->nextIterationEntry);
}

void HashSetEntryBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  const auto *self = static_cast<const HashSetEntry *>(cell);
  mb.setVTable(&HashSetEntry::vt);
  mb.addField("key", &self->key);
  mb.addField("prevIterationEntry", &self->prevIterationEntry);
  mb.addField("nextIterationEntry", &self->nextIterationEntry);
}

template <typename Data>
CallResult<PseudoHandle<HashMapEntryBase<Data>>> HashMapEntryBase<Data>::create(
    Runtime &runtime) {
  return createPseudoHandle(runtime.makeAFixed<HashMapEntryBase<Data>>());
}

template <typename Data>
CallResult<PseudoHandle<HashMapEntryBase<Data>>>
HashMapEntryBase<Data>::createLongLived(Runtime &runtime) {
  return createPseudoHandle(runtime.makeAFixed<
                            HashMapEntryBase<Data>,
                            HasFinalizer::No,
                            LongLived::Yes>());
}

template class HashMapEntryBase<HashMapEntryKeyValue>;
template class HashMapEntryBase<HashMapEntryKey>;

//===----------------------------------------------------------------------===//
// class OrderedHashMapBase

template <typename BucketType>
const VTable OrderedHashMapBase<BucketType>::vt{
    CellKind::OrderedHashMapKind,
    cellSize<OrderedHashMapBase<BucketType>>()};

void OrderedHashMapBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  const auto *self = static_cast<const OrderedHashMap *>(cell);
  mb.setVTable(&OrderedHashMap::vt);
  mb.addField("hashTable", &self->hashTable_);
  mb.addField("firstIterationEntry", &self->firstIterationEntry_);
  mb.addField("lastIterationEntry", &self->lastIterationEntry_);
}

void OrderedHashSetBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  const auto *self = static_cast<const OrderedHashSet *>(cell);
  mb.setVTable(&OrderedHashSet::vt);
  mb.addField("hashTable", &self->hashTable_);
  mb.addField("firstIterationEntry", &self->firstIterationEntry_);
  mb.addField("lastIterationEntry", &self->lastIterationEntry_);
}

template <typename BucketType>
OrderedHashMapBase<BucketType>::OrderedHashMapBase(
    Runtime &runtime,
    Handle<SegmentedArraySmall> hashTableStorage)
    : hashTable_(runtime, hashTableStorage.get(), runtime.getHeap()) {}

template <typename BucketType>
CallResult<PseudoHandle<OrderedHashMapBase<BucketType>>>
OrderedHashMapBase<BucketType>::create(Runtime &runtime) {
  auto arrRes =
      SegmentedArraySmall::create(runtime, INITIAL_CAPACITY, INITIAL_CAPACITY);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto hashTableStorage =
      runtime.makeHandle<SegmentedArraySmall>(std::move(*arrRes));

  return createPseudoHandle(
      runtime.makeAFixed<OrderedHashMapBase>(runtime, hashTableStorage));
}

template <typename BucketType>
void OrderedHashMapBase<BucketType>::removeLinkedListNode(
    Runtime &runtime,
    BucketType *entry,
    GC &gc) {
  assert(
      entry != lastIterationEntry_.get(runtime) &&
      "Cannot remove the last entry");
  if (entry->prevIterationEntry) {
    entry->prevIterationEntry.getNonNull(runtime)->nextIterationEntry.set(
        runtime, entry->nextIterationEntry, gc);
  }
  if (entry->nextIterationEntry) {
    entry->nextIterationEntry.getNonNull(runtime)->prevIterationEntry.set(
        runtime, entry->prevIterationEntry, gc);
  }
  if (entry == firstIterationEntry_.get(runtime)) {
    firstIterationEntry_.set(runtime, entry->nextIterationEntry, gc);
  }
  entry->prevIterationEntry.setNull(runtime.getHeap());
}

template <typename BucketType>
std::pair<BucketType *, uint32_t>
OrderedHashMapBase<BucketType>::lookupInBucket(
    Runtime &runtime,
    uint32_t bucket,
    HermesValue key) {
  assert(
      hashTable_.getNonNull(runtime)->size(runtime) == capacity_ &&
      "Inconsistent capacity");
  [[maybe_unused]] const uint32_t firstBucket = bucket;

  NoAllocScope noAlloc{runtime};
  NoHandleScope noHandle{runtime};

  // We can deref the handles because we are in NoAllocScope.
  SegmentedArraySmall *hashTable = hashTable_.getNonNull(runtime);
  const uint32_t mask = capacity_ - 1;

  // Each bucket must be either empty, deleted (Null) or BucketType.
  while (true) {
    auto shv = hashTable->at(runtime, bucket);
    if (shv.isEmpty()) {
      break;
    }

    if (!isDeleted(shv)) {
      assert(shv.isObject());
      auto *entry = vmcast<BucketType>(shv.getObject(runtime));
      if (isSameValueZero(entry->key.unboxToHV(runtime), key)) {
        return {entry, bucket};
      }
    }

    bucket = (bucket + 1) & mask;
    assert(bucket != firstBucket && "Hash table has wrapped around");
  }

  return {nullptr, bucket};
}

template <typename BucketType>
ExecutionStatus OrderedHashMapBase<BucketType>::rehash(
    Handle<OrderedHashMapBase> self,
    Runtime &runtime,
    bool beforeAdd) {
  // NOTE: we have ensured that self->capacity_ * 4 never overflows uint32_t by
  // setting MAX_CAPACITY to the appropriate value. self->size_ is always <=
  // self->capacity_, so this applies to self->size_ as well.
  static_assert(
      MAX_CAPACITY <= UINT32_MAX / 4,
      "Avoid overflow checks on multiplying capacity by 4");

  const uint32_t newCapacity =
      nextCapacity(self->capacity_, self->size_ + (beforeAdd ? 1 : 0));

  // Set new capacity first to update the hash function.
  self->capacity_ = newCapacity;

  // Create a new hash table.
  auto arrRes = SegmentedArraySmall::create(runtime, newCapacity, newCapacity);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // Now re-add all entries to the hash table.
  NoAllocScope noAlloc{runtime};
  auto newHashTableHandle =
      runtime.makeHandle<SegmentedArraySmall>(std::move(*arrRes));
  MutableHandle<> keyHandle{runtime};

  NoHandleScope noHandle{runtime};

  // We can deref the handles because we are in NoAllocScope.
  SegmentedArraySmall *newHashTable = newHashTableHandle.get();
  OrderedHashMapBase<BucketType> *rawSelf = *self;
  const uint32_t mask = rawSelf->capacity_ - 1;
  auto entry = rawSelf->firstIterationEntry_.get(runtime);
  while (entry) {
    if (!entry->isDeleted()) {
      keyHandle = entry->key.unboxToHV(runtime);
      uint32_t bucket = hashToBucket(rawSelf->capacity_, runtime, keyHandle);
      [[maybe_unused]] const uint32_t firstBucket = bucket;
      while (!newHashTable->at(runtime, bucket).isEmpty()) {
        // Find another bucket if it is not empty.
        bucket = (bucket + 1) & mask;
        assert(firstBucket != bucket && "Hash table is full!");
      }
      // Set the new entry to the bucket.
      newHashTable->set(
          runtime, bucket, SmallHermesValue::encodeObjectValue(entry, runtime));
    }
    entry = entry->nextIterationEntry.get(runtime);
  }

  rawSelf->deletedCount_ = 0;
  rawSelf->hashTable_.setNonNull(runtime, newHashTable, runtime.getHeap());
  assert(
      rawSelf->hashTable_.getNonNull(runtime)->size(runtime) ==
          rawSelf->capacity_ &&
      "Inconsistent capacity");
  return ExecutionStatus::RETURNED;
}

template <typename BucketType>
bool OrderedHashMapBase<BucketType>::has(
    Handle<OrderedHashMapBase> self,
    Runtime &runtime,
    Handle<> key) {
  auto bucket = hashToBucket(self->capacity_, runtime, key);
  return self->lookupInBucket(runtime, bucket, key.getHermesValue()).first;
}

template <typename BucketType>
BucketType *OrderedHashMapBase<BucketType>::find(
    Handle<OrderedHashMapBase> self,
    Runtime &runtime,
    Handle<> key) {
  auto bucket = hashToBucket(self->capacity_, runtime, key);
  return self->lookupInBucket(runtime, bucket, key.getHermesValue()).first;
}

template <typename BucketType>
HermesValue OrderedHashMapBase<BucketType>::get(
    Handle<OrderedHashMapBase> self,
    Runtime &runtime,
    Handle<> key) {
  auto *entry = find(self, runtime, key);
  if (!entry) {
    return HermesValue::encodeUndefinedValue();
  }
  return entry->getValue().unboxToHV(runtime);
}

template <typename BucketType>
template <typename>
ExecutionStatus OrderedHashMapBase<BucketType>::insert(
    Handle<OrderedHashMapBase> self,
    Runtime &runtime,
    Handle<> key,
    Handle<> value) {
  uint32_t bucket = hashToBucket(self->capacity_, runtime, key);

  // Find the bucket for this key. It the entry already exists, update the value
  // and return.
  {
    // Note that SmallHermesValue::encodeHermesValue() may allocate, so we need
    // to call it before getting raw pointer for entry.
    auto shv =
        SmallHermesValue::encodeHermesValue(value.getHermesValue(), runtime);
    BucketType *entry = nullptr;
    std::tie(entry, bucket) =
        self->lookupInBucket(runtime, bucket, key.getHermesValue());
    if (entry) {
      // Element for the key already exists, update value and return.
      entry->value.set(shv, runtime.getHeap());
      return ExecutionStatus::RETURNED;
    }
  }

  return doInsert(self, runtime, bucket, key, value);
}

template <typename BucketType>
template <typename>
ExecutionStatus OrderedHashMapBase<BucketType>::insert(
    Handle<OrderedHashMapBase> self,
    Runtime &runtime,
    Handle<> key) {
  uint32_t bucket = hashToBucket(self->capacity_, runtime, key);

  // Find the bucket for this key. It the entry already exists, then return.
  {
    BucketType *entry = nullptr;
    std::tie(entry, bucket) =
        self->lookupInBucket(runtime, bucket, key.getHermesValue());
    if (entry) {
      return ExecutionStatus::RETURNED;
    }
  }

  return doInsert(
      self, runtime, bucket, key, HandleRootOwner::getUndefinedValue());
}

template <typename BucketType>
ExecutionStatus OrderedHashMapBase<BucketType>::doInsert(
    Handle<OrderedHashMapBase> self,
    Runtime &runtime,
    uint32_t bucket,
    Handle<> key,
    Handle<> value) {
  // Run rehash if necessary before inserting.
  if (shouldRehash(self->capacity_, self->size_, self->deletedCount_)) {
    if (LLVM_UNLIKELY(
            self->rehash(self, runtime, true) == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // Find a new empty bucket after rehash.
    bucket = hashToBucket(self->capacity_, runtime, key);
    BucketType *entry = nullptr;
    std::tie(entry, bucket) =
        self->lookupInBucket(runtime, bucket, key.getHermesValue());
    assert(!entry && "After rehash, we must be able to find an empty bucket");
  }

  // Create a new entry, set the key and value.
  // Allocate the new entry in the same generation as the hash table to avoid
  // pointers from old gen to young gen.
  auto crtRes = runtime.getHeap().inYoungGen(self->hashTable_.get(runtime))
      ? BucketType::create(runtime)
      : BucketType::createLongLived(runtime);
  if (LLVM_UNLIKELY(crtRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // Note that SmallHermesValue::encodeHermesValue() may allocate, so we need to
  // call it and set to newMapEntry one at a time.
  auto newMapEntry = runtime.makeHandle(std::move(*crtRes));
  auto k = SmallHermesValue::encodeHermesValue(key.getHermesValue(), runtime);
  newMapEntry->key.set(k, runtime.getHeap());
  if constexpr (std::is_same_v<BucketType, HashMapEntry>) {
    auto v =
        SmallHermesValue::encodeHermesValue(value.getHermesValue(), runtime);
    newMapEntry->value.set(v, runtime.getHeap());
  }

  // After here, no allocation
  NoAllocScope noAlloc{runtime};
  NoHandleScope noHandle{runtime};

  // We can deref in NoAllocScope.
  OrderedHashMapBase<BucketType> *rawSelf = *self;
  GC &heap = runtime.getHeap();

  // Set the newly inserted entry as the front of this bucket chain.
  rawSelf->hashTable_.getNonNull(runtime)->set(
      runtime,
      bucket,
      SmallHermesValue::encodeObjectValue(*newMapEntry, runtime));

  if (!rawSelf->firstIterationEntry_) {
    // If we are inserting the first ever element, update
    // first iteration entry pointer.
    rawSelf->firstIterationEntry_.set(runtime, newMapEntry.get(), heap);
    rawSelf->lastIterationEntry_.set(runtime, newMapEntry.get(), heap);
  } else {
    // Connect the new entry with the last entry.
    rawSelf->lastIterationEntry_.getNonNull(runtime)->nextIterationEntry.set(
        runtime, newMapEntry.get(), heap);
    newMapEntry->prevIterationEntry.set(
        runtime, rawSelf->lastIterationEntry_, heap);

    BucketType *previousLastEntry = rawSelf->lastIterationEntry_.get(runtime);
    rawSelf->lastIterationEntry_.set(runtime, newMapEntry.get(), heap);

    if (previousLastEntry && previousLastEntry->isDeleted()) {
      // If the last entry was a deleted entry, we no longer need to keep it.
      rawSelf->removeLinkedListNode(runtime, previousLastEntry, heap);
    }
  }

  rawSelf->size_++;
  return ExecutionStatus::RETURNED;
}

template <typename BucketType>
bool OrderedHashMapBase<BucketType>::erase(
    Handle<OrderedHashMapBase> self,
    Runtime &runtime,
    Handle<> key) {
  uint32_t bucket = hashToBucket(self->capacity_, runtime, key);
  BucketType *entry = nullptr;
  std::tie(entry, bucket) =
      self->lookupInBucket(runtime, bucket, key.getHermesValue());
  if (!entry) {
    // Element does not exist.
    return false;
  }

  // Mark the bucket as deleted. We remove this in rehash.
  deleteBucket(self, runtime, bucket);
  entry->markDeleted(runtime);
  self->deletedCount_++;
  self->size_--;

  // Now delete the entry from the iteration linked list.
  // If we are deleting the last entry, don't remove it from the list yet.
  // This will ensure that if any iterator out there is currently pointing to
  // this entry, it will be able to follow on if we add new entries in the
  // future.
  if (entry != self->lastIterationEntry_.get(runtime)) {
    self->removeLinkedListNode(runtime, entry, runtime.getHeap());
  }

  if (shouldShrink(self->capacity_, self->size_))
    self->rehash(self, runtime);

  return true;
}

template <typename BucketType>
BucketType *OrderedHashMapBase<BucketType>::iteratorNext(
    Runtime &runtime,
    BucketType *entry) const {
  if (entry == nullptr) {
    // Starting a new iteration from the first entry.
    entry = firstIterationEntry_.get(runtime);
  } else {
    // Advance to the next entry.
    entry = entry->nextIterationEntry.get(runtime);
  }

  // Make sure the entry we returned (if not nullptr) must not be deleted.
  while (entry && entry->isDeleted()) {
    entry = entry->nextIterationEntry.get(runtime);
  }
  return entry;
}

template <typename BucketType>
void OrderedHashMapBase<BucketType>::clear(Runtime &runtime) {
  if (!firstIterationEntry_) {
    // Empty set.
    return;
  }

  // Clear the hash table.
  for (unsigned i = 0; i < capacity_; ++i) {
    auto shv = hashTable_.getNonNull(runtime)->at(runtime, i);
    // Delete every element reachable from the hash table.
    if (shv.isObject()) {
      vmcast<BucketType>(shv.getObject(runtime))->markDeleted(runtime);
    }
    // Clear every element in the hash table.
    hashTable_.getNonNull(runtime)->setNonPtr(
        runtime, i, SmallHermesValue::encodeEmptyValue());
  }
  // Resize the hash table to the initial size.
  SegmentedArraySmall::resizeWithinCapacity(
      hashTable_.getNonNull(runtime), runtime, INITIAL_CAPACITY);
  capacity_ = INITIAL_CAPACITY;

  // After clearing, we will still keep the last deleted entry
  // in case there is an iterator out there
  // pointing to the middle of the iteration chain. We need it to be
  // able to merge back eventually.
  firstIterationEntry_.set(runtime, lastIterationEntry_, runtime.getHeap());
  firstIterationEntry_.getNonNull(runtime)->prevIterationEntry.setNull(
      runtime.getHeap());
  size_ = 0;
}

template class OrderedHashMapBase<HashMapEntry>;
template class OrderedHashMapBase<HashSetEntry>;

template ExecutionStatus OrderedHashMap::insert(
    Handle<OrderedHashMapBase> self,
    Runtime &runtime,
    Handle<> key,
    Handle<> value);
template ExecutionStatus OrderedHashSet::insert(
    Handle<OrderedHashMapBase> self,
    Runtime &runtime,
    Handle<> key);

} // namespace vm
} // namespace hermes
