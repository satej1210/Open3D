// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#pragma once

#include <tbb/concurrent_unordered_map.h>

#include <unordered_map>

#include "open3d/core/hashmap/CPU/HashmapBufferCPU.hpp"
#include "open3d/core/hashmap/DeviceHashmap.h"
#include "open3d/core/hashmap/Traits.h"

namespace open3d {
namespace core {
template <typename Hash, typename KeyEq>
class CPUHashmap : public DeviceHashmap<Hash, KeyEq> {
public:
    CPUHashmap(int64_t init_buckets,
               int64_t init_capacity,
               int64_t dsize_key,
               int64_t dsize_value,
               const Device& device);

    ~CPUHashmap();

    void Rehash(int64_t buckets) override;

    void Insert(const void* input_keys,
                const void* input_values,
                addr_t* output_addrs,
                bool* output_masks,
                int64_t count) override;

    void Activate(const void* input_keys,
                  addr_t* output_addrs,
                  bool* output_masks,
                  int64_t count) override;

    void Find(const void* input_keys,
              addr_t* output_addrs,
              bool* output_masks,
              int64_t count) override;

    void Erase(const void* input_keys,
               bool* output_masks,
               int64_t count) override;

    int64_t GetActiveIndices(addr_t* output_indices) override;

    std::vector<int64_t> BucketSizes() const override;
    float LoadFactor() const override;

    int64_t Size() const override;
    Tensor& GetKeyTensor() override { return buffer_->GetKeyTensor(); }
    Tensor& GetValueTensor() override { return buffer_->GetValueTensor(); }

protected:
    std::shared_ptr<tbb::concurrent_unordered_map<void*, addr_t, Hash, KeyEq>>
            impl_;
    std::shared_ptr<CPUHashmapBuffer> buffer_;

    void InsertImpl(const void* input_keys,
                    const void* input_values,
                    addr_t* output_addrs,
                    bool* output_masks,
                    int64_t count);
};

template <typename Hash, typename KeyEq>
CPUHashmap<Hash, KeyEq>::CPUHashmap(int64_t init_buckets,
                                    int64_t init_capacity,
                                    int64_t dsize_key,
                                    int64_t dsize_value,
                                    const Device& device)
    : DeviceHashmap<Hash, KeyEq>(
              init_buckets,
              init_capacity,  /// Dummy for std unordered_map, reserved for.
                              /// other hashmaps.
              dsize_key,
              dsize_value,
              device) {
    impl_ = std::make_shared<
            tbb::concurrent_unordered_map<void*, addr_t, Hash, KeyEq>>(
            init_buckets, Hash(this->dsize_key_), KeyEq(this->dsize_key_));
    buffer_ = std::make_shared<CPUHashmapBuffer>(
            this->capacity_, this->dsize_key_, this->dsize_value_,
            this->device_);
}

template <typename Hash, typename KeyEq>
CPUHashmap<Hash, KeyEq>::~CPUHashmap() {
    impl_->clear();
}

template <typename Hash, typename KeyEq>
int64_t CPUHashmap<Hash, KeyEq>::Size() const {
    return impl_->size();
}

template <typename Hash, typename KeyEq>
void CPUHashmap<Hash, KeyEq>::Insert(const void* input_keys,
                                     const void* input_values,
                                     addr_t* output_addrs,
                                     bool* output_masks,
                                     int64_t count) {
    int64_t new_size = Size() + count;
    if (new_size > this->capacity_) {
        float avg_capacity_per_bucket =
                float(this->capacity_) / float(this->bucket_count_);
        int64_t expected_buckets = std::max(
                this->bucket_count_ * 2,
                int64_t(std::ceil(new_size / avg_capacity_per_bucket)));
        Rehash(expected_buckets);
    }
    InsertImpl(input_keys, input_values, output_addrs, output_masks, count);
}

template <typename Hash, typename KeyEq>
void CPUHashmap<Hash, KeyEq>::Activate(const void* input_keys,
                                       addr_t* output_addrs,
                                       bool* output_masks,
                                       int64_t count) {
    int64_t new_size = Size() + count;
    if (new_size > this->capacity_) {
        float avg_capacity_per_bucket =
                float(this->capacity_) / float(this->bucket_count_);
        int64_t expected_buckets = std::max(
                this->bucket_count_ * 2,
                int64_t(std::ceil(new_size / avg_capacity_per_bucket)));
        Rehash(expected_buckets);
    }
    InsertImpl(input_keys, nullptr, output_addrs, output_masks, count);
}

template <typename Hash, typename KeyEq>
void CPUHashmap<Hash, KeyEq>::Find(const void* input_keys,
                                   addr_t* output_addrs,
                                   bool* output_masks,
                                   int64_t count) {
    auto buffer_ctx = buffer_->GetContext();
#pragma omp parallel for
    for (int64_t i = 0; i < count; ++i) {
        uint8_t* key = const_cast<uint8_t*>(
                static_cast<const uint8_t*>(input_keys) + this->dsize_key_ * i);

        auto iter = impl_->find(key);
        if (iter == impl_->end()) {
            output_masks[i] = false;
        } else {
            output_addrs[i] = iter->second;
            output_masks[i] = true;
        }
    }
}

template <typename Hash, typename KeyEq>
void CPUHashmap<Hash, KeyEq>::Erase(const void* input_keys,
                                    bool* output_masks,
                                    int64_t count) {
    auto buffer_ctx = buffer_->GetContext();
    for (int64_t i = 0; i < count; ++i) {
        uint8_t* key = const_cast<uint8_t*>(
                static_cast<const uint8_t*>(input_keys) + this->dsize_key_ * i);

        auto iter = impl_->find(key);
        if (iter == impl_->end()) {
            output_masks[i] = false;
        } else {
            buffer_ctx->Free(iter->second);
            impl_->unsafe_erase(iter);
            output_masks[i] = true;
        }
    }
    this->bucket_count_ = impl_->unsafe_bucket_count();
}

template <typename Hash, typename KeyEq>
int64_t CPUHashmap<Hash, KeyEq>::GetActiveIndices(addr_t* output_indices) {
    auto buffer_ctx = buffer_->GetContext();

    int64_t count = impl_->size();
    int64_t i = 0;
    for (auto iter = impl_->begin(); iter != impl_->end(); ++iter, ++i) {
        output_indices[i] = static_cast<int64_t>(iter->second);
    }

    return count;
}

template <typename Hash, typename KeyEq>
void CPUHashmap<Hash, KeyEq>::Rehash(int64_t buckets) {
    int64_t iterator_count = Size();

    Tensor active_keys;
    Tensor active_vals;

    if (iterator_count > 0) {
        Tensor active_addrs({iterator_count}, Dtype::Int32, this->device_);
        GetActiveIndices(static_cast<addr_t*>(active_addrs.GetDataPtr()));

        Tensor active_indices = active_addrs.To(Dtype::Int64);
        active_keys = buffer_->GetKeyTensor().IndexGet({active_indices});
        active_vals = buffer_->GetValueTensor().IndexGet({active_indices});
    }

    float avg_capacity_per_bucket =
            float(this->capacity_) / float(this->bucket_count_);

    this->capacity_ = int64_t(std::ceil(buckets * avg_capacity_per_bucket));
    impl_ = std::make_shared<
            tbb::concurrent_unordered_map<void*, addr_t, Hash, KeyEq>>(
            buckets, Hash(this->dsize_key_), KeyEq(this->dsize_key_));
    buffer_ = std::make_shared<CPUHashmapBuffer>(
            this->capacity_, this->dsize_key_, this->dsize_value_,
            this->device_);

    if (iterator_count > 0) {
        Tensor output_addrs({iterator_count}, Dtype::Int32, this->device_);
        Tensor output_masks({iterator_count}, Dtype::Bool, this->device_);

        InsertImpl(active_keys.GetDataPtr(), active_vals.GetDataPtr(),
                   static_cast<addr_t*>(output_addrs.GetDataPtr()),
                   static_cast<bool*>(output_masks.GetDataPtr()),
                   iterator_count);
    }

    impl_->rehash(buckets);
    this->bucket_count_ = impl_->unsafe_bucket_count();
}

template <typename Hash, typename KeyEq>
std::vector<int64_t> CPUHashmap<Hash, KeyEq>::BucketSizes() const {
    int64_t bucket_count = impl_->unsafe_bucket_count();
    std::vector<int64_t> ret;
    for (int64_t i = 0; i < bucket_count; ++i) {
        ret.push_back(impl_->unsafe_bucket_size(i));
    }
    return ret;
}

template <typename Hash, typename KeyEq>
float CPUHashmap<Hash, KeyEq>::LoadFactor() const {
    return impl_->load_factor();
}

template <typename Hash, typename KeyEq>
void CPUHashmap<Hash, KeyEq>::InsertImpl(const void* input_keys,
                                         const void* input_values,
                                         addr_t* output_addrs,
                                         bool* output_masks,
                                         int64_t count) {
    auto buffer_ctx = buffer_->GetContext();

#pragma omp parallel for
    for (int64_t i = 0; i < count; ++i) {
        const uint8_t* src_key =
                static_cast<const uint8_t*>(input_keys) + this->dsize_key_ * i;

        addr_t dst_kv_addr = buffer_ctx->Allocate();
        iterator_t dst_kv_iter = buffer_ctx->extract_iterator(dst_kv_addr);

        uint8_t* dst_key = static_cast<uint8_t*>(dst_kv_iter.first);
        uint8_t* dst_value = static_cast<uint8_t*>(dst_kv_iter.second);
        std::memcpy(dst_key, src_key, this->dsize_key_);

        if (input_values != nullptr) {
            const uint8_t* src_value =
                    static_cast<const uint8_t*>(input_values) +
                    this->dsize_value_ * i;
            std::memcpy(dst_value, src_value, this->dsize_value_);
        } else {
            std::memset(dst_value, 0, this->dsize_value_);
        }

        // Try insertion.
        auto res = impl_->insert({dst_key, dst_kv_addr});

        output_addrs[i] = dst_kv_addr;
        output_masks[i] = res.second;
    }

#pragma omp parallel for
    for (int64_t i = 0; i < count; ++i) {
        if (!output_masks[i]) {
            buffer_ctx->Free(output_addrs[i]);
        }
    }

    this->bucket_count_ = impl_->unsafe_bucket_count();
}

}  // namespace core
}  // namespace open3d
