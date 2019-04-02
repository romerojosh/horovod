// Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include<climits>
#include<cstring>

#include "response_cache.h"

namespace horovod {
namespace common {

void ResponseCache::clear() {
  capacity_ = 0;
  bits_outdated_ = false;
  cache_.clear();
  cache_iters_.clear();
  tensor_name_to_bit_.clear();
}

void ResponseCache::set_capacity(uint32_t capacity) {
  // Clear cache in case set_capacity is called multiple times if autotuning.
  // Only clear if capacity is modified.
  if (capacity != capacity_) {
    this->clear();
  }

  capacity_ = capacity;
  cache_iters_.reserve(capacity);
}

uint32_t ResponseCache::capacity() const { return capacity_; }

size_t ResponseCache::num_active_bits() const { return cache_iters_.size(); }

ResponseCache::CacheState ResponseCache::cached(const Request& message) const {
  auto it = tensor_name_to_bit_.find(message.tensor_name());
  if (it != tensor_name_to_bit_.end()) {
    // If entry associated with this request already exists in cache, check
    // if tensor parameters match. If not, return that entry is invalid.
    uint32_t cache_bit = it->second;
    auto& cache_params = std::get<1>(*cache_iters_[cache_bit]);
    return (cache_params.device == message.device() &&
            cache_params.dtype == message.tensor_type() &&
            cache_params.shape == message.tensor_shape())
               ? CacheState::HIT
               : CacheState::INVALID;
  } else {
    return CacheState::MISS;
  }
}

ResponseCache::CacheState
ResponseCache::cached(const Response& response,
                      const TensorParams& params) const {
  assert(response.tensor_names().size() == 1);
  auto it = tensor_name_to_bit_.find(response.tensor_names()[0]);
  if (it != tensor_name_to_bit_.end()) {
    uint32_t cache_bit = it->second;
    auto& cache_params = std::get<1>(*cache_iters_[cache_bit]);
    return (cache_params.device == params.device &&
            cache_params.dtype == params.dtype &&
            cache_params.shape == params.shape)
               ? CacheState::HIT
               : CacheState::INVALID;
  } else {
    return CacheState::MISS;
  }
}

void ResponseCache::put_(const Response& response, TensorParams& params) {
  uint32_t cache_bit;
  auto cache_state = this->cached(response, params);

  // Disallow caching name-conflicted responses here. Invalid cache entries
  // must be removed prior to caching new entries.
  if (cache_state == CacheState::INVALID) {
    throw std::logic_error(
        "Trying to overwrite cached response with existing name. "
        "This is not allowed.");
  }

  if (this->cached(response, params) == CacheState::HIT) {
    cache_bit = tensor_name_to_bit_[response.tensor_names()[0]];
    auto it = cache_iters_[cache_bit];
    cache_.push_front(std::move(*it));
    cache_.erase(it);
  } else if (cache_.size() == capacity_) {
    auto& entry = cache_.back().first;
    cache_bit = tensor_name_to_bit_[entry.tensor_names()[0]];
    tensor_name_to_bit_.erase(entry.tensor_names()[0]);
    cache_.pop_back();
    cache_.push_front(std::make_pair(response, std::move(params)));
  } else {
    cache_bit = cache_iters_.size();
    cache_iters_.resize(cache_bit + 1);
    cache_.push_front(std::make_pair(response, std::move(params)));
  }

  cache_iters_[cache_bit] = cache_.begin();
  tensor_name_to_bit_[response.tensor_names()[0]] = cache_bit;

  bits_outdated_ = true;
}

void ResponseCache::put(const Response& response, const TensorTable& tensor_table) {
  if (capacity_ == 0) {
    return;
  }

  // If response is fused, split back into individual responses
  if (response.tensor_names().size() > 1) {
    for (auto& name : response.tensor_names()) {
      Response new_response;
      new_response.add_tensor_name(name);
      new_response.set_response_type(response.response_type());
      new_response.set_devices(response.devices());
      new_response.set_tensor_sizes(response.tensor_sizes());

      // Populate tensor parameters from tensor_table entry
      auto& tensor_entry = tensor_table.at(name);
      TensorParams params;
      params.device = tensor_entry.device;
      params.dtype = tensor_entry.tensor->dtype();
      params.shape = tensor_entry.tensor->shape().to_vector();

      this->put_(new_response, params);
    }
  } else {
    auto& tensor_entry = tensor_table.at(response.tensor_names()[0]);
    TensorParams params;
    params.device = tensor_entry.device;
    params.dtype = tensor_entry.tensor->dtype();
    params.shape = tensor_entry.tensor->shape().to_vector();

    this->put_(response, params);
  }
}

const Response& ResponseCache::get_response(uint32_t cache_bit) {
  assert(cache_bit < cache_iters_.size());
  auto it = cache_iters_[cache_bit];
  cache_.push_front(std::move(*it));
  cache_.erase(it);
  cache_iters_[cache_bit] = cache_.begin();
  bits_outdated_ = true;
  return cache_.front().first;
}

const Response& ResponseCache::peek_response(uint32_t cache_bit) const {
  assert(cache_bit < cache_iters_.size());
  return std::get<0>(*cache_iters_[cache_bit]);
}

uint32_t ResponseCache::peek_cache_bit(const Request& message) const {
  assert(this->cached(message));
  return tensor_name_to_bit_.at(message.tensor_name());
}

void ResponseCache::erase_response(uint32_t cache_bit) {
  assert(cache_bit < cache_iters_.size());
  auto it = cache_iters_[cache_bit];
  tensor_name_to_bit_.erase(it->first.tensor_names()[0]);
  cache_.erase(it);
  // When erasing entry, do not resize cache_iters_ vector to ensure previously
  // returned cache bit positions remain valid. cache_iters_ is resized and bit
  // posiions are reset *only* when update_cache_bits function is called.

  cache_iters_[cache_bit] = cache_.end();
  bits_outdated_ = true;
}

void ResponseCache::update_cache_bits() {
  // Note: This method invalidates all previously returned cache bit positions.

  if (!bits_outdated_) {
    return;
  }

  // Iterate over current cache state and reassign cache bits. Least recently
  // used get lower cache positions.
  auto it = --cache_.end();
  for (int i = 0; i < (int)cache_.size(); ++i) {
    cache_iters_[i] = it;
    tensor_name_to_bit_[it->first.tensor_names()[0]] = i;
    --it;
  }

  // Resize cache_iters_ vector to contain only valid bit positions.
  cache_iters_.resize(cache_.size());

  bits_outdated_ = false;
}

CacheCoordinator::CacheCoordinator(size_t num_active_bits) {
  num_active_bits_ = num_active_bits;
}

void CacheCoordinator::record_hit(uint32_t bit) {
  assert(!synced_);
  cache_hits_.insert(bit);
}

void CacheCoordinator::record_invalid_bit(uint32_t bit) {
  assert(!synced_);
  invalid_bits_.insert(bit);
  invalid_in_queue_ = true;
}

void CacheCoordinator::set_should_shut_down(bool should_shut_down) {
  assert(!synced_);
  should_shut_down_ = should_shut_down;
}

void CacheCoordinator::set_uncached_in_queue(bool uncached_in_queue) {
  assert(!synced_);
  uncached_in_queue_ = uncached_in_queue;
}

const std::set<uint32_t>& CacheCoordinator::cache_hits() const {
  assert(synced_);
  return cache_hits_;
}

const std::set<uint32_t>& CacheCoordinator::invalid_bits() const {
  assert(synced_);
  return invalid_bits_;
}

const std::set<uint32_t>& CacheCoordinator::timeline_bits() const {
  assert(synced_);
  return timeline_bits_;
}

bool CacheCoordinator::should_shut_down() const {
  assert(synced_);
  return should_shut_down_;
}

bool CacheCoordinator::uncached_in_queue() const {
  assert(synced_);
  return uncached_in_queue_;
}

void CacheCoordinator::sync(MPIContext& ctx, bool timeline_enabled) {
  assert(!synced_);

  // Resize and initialize bit vector.
  int nbits = num_active_bits_ + NUM_STATUS_BITS;
  int count = (nbits + sizeof(long long) * CHAR_BIT - 1) /
              (sizeof(long long) * CHAR_BIT);

  // Allocate extended bit vector for timeline handling if required.
  int fullcount = count;
  if (timeline_enabled) {
    fullcount *= 2;
  }

  bitvector_.resize(fullcount);
  std::memset(&bitvector_[0], 0, count * sizeof(long long));
  if (timeline_enabled) {
    std::memset(&bitvector_[count], -1, count * sizeof(long long));
  }

  // Set reserved status bits for additional states.
  if (!should_shut_down_) {
    bitvector_[0] |= (1ull << StatusBit::SHOULD_SHUT_DOWN);
  }
  if (!uncached_in_queue_) {
    bitvector_[0] |= (1ull << StatusBit::UNCACHED_IN_QUEUE);
  }
  if (!invalid_in_queue_) {
    bitvector_[0] |= (1ull << StatusBit::INVALID_IN_QUEUE);
  }

  // For each cache hit on this worker, flip associated bit in bit vector.
  for (auto bit : cache_hits_) {
    int shifted_bit = bit + NUM_STATUS_BITS;
    int shift = shifted_bit / (sizeof(long long) * CHAR_BIT);
    bitvector_[shift] |=
        (1ull << (shifted_bit % (sizeof(long long) * CHAR_BIT)));
    if (timeline_enabled) {
      // Set corresponding bit in extended section for timeline if needed.
      bitvector_[count + shift] ^=
          (1ull << (shifted_bit % (sizeof(long long) * CHAR_BIT)));
    }
  }

  // Global MPI AND operation to get intersected bit array.
  MPI_Allreduce(MPI_IN_PLACE, bitvector_.data(), fullcount,
                MPI_LONG_LONG_INT, MPI_BAND, ctx.mpi_comm);

  // Search for flipped bits to populate common cache hit set. There will never
  // be invalid bits in this set.
  cache_hits_.clear();
  for (int i = 0; i < count; ++i) {
    int shift = i * sizeof(long long) * CHAR_BIT;
    long long ll = bitvector_[i];
    while (ll) {
      int idx = __builtin_ffsll(ll);
      int shifted_bit = shift + idx - 1;
      cache_hits_.insert(shifted_bit - NUM_STATUS_BITS);
      ll &= ~(1ull << (idx - 1));
    }
  }

  // Set states from reserved status bits
  if (!cache_hits_.erase(StatusBit::SHOULD_SHUT_DOWN - NUM_STATUS_BITS)) {
    should_shut_down_ = true;
  }
  if (!cache_hits_.erase(StatusBit::UNCACHED_IN_QUEUE - NUM_STATUS_BITS)) {
    uncached_in_queue_ = true;
  }
  if (!cache_hits_.erase(StatusBit::INVALID_IN_QUEUE - NUM_STATUS_BITS)) {
    invalid_in_queue_ = true;
  }

  // If any worker has invalid cache entries, communicate invalid bits across
  // workers using a second MPI allreduce operation.
  if (invalid_in_queue_) {
    std::memset(&bitvector_[0], 0, count * sizeof(long long));
    for (auto bit : invalid_bits_) {
      int shift = bit / (sizeof(long long) * CHAR_BIT);
      bitvector_[shift] |=
          (1ull << (bit % (sizeof(long long) * CHAR_BIT)));
    }

    // Global MPI OR operation to get common invalid bits.
    MPI_Allreduce(MPI_IN_PLACE, bitvector_.data(), count,
                  MPI_LONG_LONG_INT, MPI_BOR, ctx.mpi_comm);

    // Search for flipped bits to populate common invalid bit set.
    invalid_bits_.clear();
    for (int i = 0; i < count; ++i) {
      int shift = i * sizeof(long long) * CHAR_BIT;
      long long ll = bitvector_[i];
      while (ll) {
        int idx = __builtin_ffsll(ll);
        int bit = shift + idx - 1;
        invalid_bits_.insert(bit);
        ll &= ~(1ull << (idx - 1));
      }
    }
  }

  if (timeline_enabled) {
    // For timeline, add bits with cache hits on *any* worker to
    // timeline bit set to mark start of negotiation phase. This
    // information is encoded in an extended section of the bit vector
    // from [count, 2*count]
    for (int i = 0; i < count; ++i) {
      int shift = i * sizeof(long long) * CHAR_BIT;
      long long ll = ~bitvector_[count + i];
      while (ll) {
        int idx = __builtin_ffsll(ll);
        int shifted_bit = shift + idx - 1;
        // Only add valid bits to set here. Timeline handling for
        // invalid bits will proceed to the non-bypass coordination path.
        if (invalid_bits_.find(shifted_bit - NUM_STATUS_BITS) ==
            invalid_bits_.end()) {
          timeline_bits_.insert(shifted_bit - NUM_STATUS_BITS);
        }
        ll &= ~(1ull << (idx - 1));
      }
    }
  }

  synced_ = true;
}

} // namespace common
} // namespace horovod
