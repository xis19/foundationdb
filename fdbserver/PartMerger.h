/**
 * PartMerger.h
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

#ifndef FDBSERVER_PARTMERGER_H
#define FDBSERVER_PARTMERGER_H

#pragma once

#include <algorithm>
#include <utility>
#include <vector>

#include "fdbserver/TimedKVCache.h"

template <typename K, typename V>
class PartMerger {
public:
    using key_t = K;
    using value_t = V;
    using TimedKVCache_t = TimedKVCache<key_t, std::pair<std::vector<bool>, value_t>>;
    using duration_t = typename TimedKVCache_t::duration_t;

    explicit PartMerger(const std::chrono::milliseconds& expiringTime) : parts(expiringTime) {}
    explicit PartMerger(const std::chrono::seconds& expiringTime) : parts(expiringTime) {}

    bool insert(const key_t& k, const int partIndex, const int totalParts, const value_t& v) {
        if (!parts.exists(k)) {
            parts.add(k, std::make_pair(std::vector<bool>(totalParts, false), v));
            parts.get(k).first[partIndex] = true;
        } else {
            auto& existingItem = parts.get(k);
            auto& partList = existingItem.first;
            auto& existingValue = existingItem.second;
            if (!partList[partIndex]) {
                merge(existingValue, v);
                partList[partIndex] = true;
            }
        }

        return isComplete(k);
    }

    bool isComplete(const key_t& k) {
        const auto& partList = parts.get(k).first;
        return std::all_of(partList.begin(), partList.end(), [](bool v) { return v; });
    }

    bool exists(const key_t& key) const {
        return parts.exists(key);
    }

    value_t& get(const key_t& key) {
        return parts.get(key).second;
    }

    void erase(const key_t& key) {
        return parts.erase(key);
    }

protected:
    virtual void merge(V&, const V&) = 0;

private:
    TimedKVCache_t parts;
};

#endif  // FDBSERVER_PARTMERGER_H