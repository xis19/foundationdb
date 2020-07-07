/*
 * SplitIDVersionCache.h
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


#ifndef _FDBSERVER_SPLIT_ID_VERSION_CACHE_H_
#define _FDBSERVER_SPLIT_ID_VERSION_CACHE_H_

#pragma once

#include <chrono>
#include <list>
#include <memory>
#include <unordered_map>

#include "fdbclient/FDBTypes.h"
#include "flow/IRandom.h"


class SplitIDVersionCache {
    typedef std::chrono::system_clock::time_point timepoint_t;

    std::list<std::pair<timepoint_t, UID>> timestampedUID;
    std::unordered_map<UID, Version> uidVersionMapper;

    static std::unique_ptr<SplitIDVersionCache> pInstance;

    SplitIDVersionCache() {}
public:
    /// By default FDB only holds latest 5 seconds of versions, so the cache
    /// should handle at most 5 seconds of split UID/read version.
    static constexpr auto CACHE_EXPIRING_TIME = std::chrono::seconds(5);

    /** Get the SplitIDVersionCache instance. It is *NOT* thread safe. */
    static SplitIDVersionCache& instance();

    /**
     * Add a new UID/Version pair to the cache
     * @param UID UID
     * @param Version Version
     */
    void add(const UID&, const Version&);

    /**
     * Queries the cache for a given UID.
     * @param UID UID
     * @return Optional<Version>
     */
    Optional<Version> query(const UID&);

private:
    /**
     * Sweep out the outdated items in the cache
     */
    void sweep();
};

#endif  // _FDBSERVER_SPLIT_ID_VERSION_CACHE_H_