/**
 * TimedKVCache.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2020 Apple Inc. and the FoundationDB project authors
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

#ifndef FDBSERVER_TIMED_KVCACHE_H
#define FDBSERVER_TIMED_KVCACHE_H

#pragma once

#include <chrono>
#include <list>
#include <memory>
#include <unordered_map>

#include <boost/noncopyable.hpp>

#include "flow/Error.h"

/**
 * @class TimedKVCache
 * @brief A key/value based cache that expires its key after a given time
 */
template <typename K, typename V>
class TimedKVCache : private boost::noncopyable {
public:
    using key_t = K;
    using value_t = V;
    using duration_t = std::chrono::duration<double, std::milli>;
    using clock_t = std::chrono::high_resolution_clock;
    using timepoint_t = clock_t::time_point;

    const duration_t CACHE_EXPIRING_TIME;

    explicit TimedKVCache(const std::chrono::milliseconds& expiringTime) :
        CACHE_EXPIRING_TIME(expiringTime) {}
    explicit TimedKVCache(const std::chrono::seconds& expiringTime) :
        CACHE_EXPIRING_TIME(std::chrono::duration_cast<duration_t>(expiringTime)) {}

    /**
     * Add a new key/value pair to the cache
     * @param key
     * @param value
     */
    void add(const key_t& key, const value_t& value) {
        timepoint_t now = clock_t().now();
        timestampedKey.emplace_back(std::make_pair(now, key));
        kvMapper[key] = value;
    }

    /**
     * Add a new key/value pair to the cache
     * @param key
     * @param value
     */
    void add(const key_t& key, value_t&& value) {
        timepoint_t now = clock_t().now();
        timestampedKey.emplace_back(std::make_pair(now, key));
        kvMapper.emplace(key, std::move(value));
    }

    /**
     * Check if a given key exists, remove the expired keys
     * @param key
     * @return bool
     */
    bool exists(const key_t& key) {
        sweep();
        return kvMapper.find(key) == kvMapper.end();
    }

    /**
     * Get the value of a key
     * @param key
     * @return value
     */
    value_t& get(const key_t& key) {
        return const_cast<TimedKVCache*>(this)->get(key);
    }

    const value_t& get(const key_t& key) const {
        try {
            return kvMapper.at(key);
        } catch (std::out_of_range&) {
            ASSERT(exists(key));
        }
    }

private:
    std::list<std::pair<timepoint_t, key_t>> timestampedKey;
    std::unordered_map<key_t, value_t> kvMapper;

    void sweep() {
        auto now = clock_t().now();
        while(!timestampedKey.empty() && 
                (now - timestampedKey.front().first) > CACHE_EXPIRING_TIME) {
            const auto& key = timestampedKey.front().second;
            kvMapper.erase(key);
            timestampedKey.pop_front();
        }
    }
};

#endif  // FDBSERVER_TIMED_KVCACHE_H