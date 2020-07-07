/*
 * SplitIDVersionCache.cpp
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

#include "fdbserver/SplitIDVersionCache.h"

std::unique_ptr<SplitIDVersionCache> SplitIDVersionCache::pInstance = nullptr;


SplitIDVersionCache& SplitIDVersionCache::instance() {
    // NOTE This is not thread safe.
    if (!SplitIDVersionCache::pInstance) {
        SplitIDVersionCache::pInstance.reset(new SplitIDVersionCache());
    }
    return *SplitIDVersionCache::pInstance;
}


void SplitIDVersionCache::add(const UID& uid, const Version& version) {
    timepoint_t now = std::chrono::system_clock().now();
    timestampedUID.emplace_back(std::make_pair(now, uid));
    uidVersionMapper[uid] = version;

}

Optional<Version> SplitIDVersionCache::query(const UID& uid) {
    if (uidVersionMapper.find(uid) == uidVersionMapper.end()) {
        return Optional<Version>();
    }
    return Optional<Version>(uidVersionMapper[uid]);
}


void SplitIDVersionCache::sweep() {
    auto now = std::chrono::system_clock().now();

    while (!timestampedUID.empty() &&
            (now - timestampedUID.front().first) > CACHE_EXPIRING_TIME) {

        auto uid = timestampedUID.front().second;
        uidVersionMapper.erase(uid);
    }
}