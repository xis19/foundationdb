/*
 * MasterProxyInterface.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2020 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *	 http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbclient/MasterProxyInterface.h"

#include <algorithm>
#include <numeric>
#include <queue>
#include <utility>
#include <vector>

#include "fdbclient/Knobs.h"
#include "flow/IRandom.h"

enum {
	SPLIT_TRANSACTION_MASK = 0b1,

	DISABLE_SPLIT_TRANSACTION = 0b0,
	ENABLE_SPLIT_TRANSACTION = 0b1,

	CONFLICTS_MASK = 0b110,

	CONFLICTS_EVENLY_DISTRIBUTE = 0b000,
	CONFLICTS_TO_ONE_PROXY = 0b010
};

bool shouldSplitCommitTransactionRequest(const CommitTransactionRequest& commitTxnRequest, const int numProxies) {

	if (numProxies < 2 || commitTxnRequest.transaction.mutations.size() < 2 ||
	    ((CLIENT_KNOBS->TRANSACTION_SPLIT_MODE & SPLIT_TRANSACTION_MASK) == DISABLE_SPLIT_TRANSACTION)) {
		return false;
	}

	// FIXME
	/*for (auto& mutation : commitTxnRequest.transaction.mutations) {
	    if (mutation.param1.toString() == "do_split") {
	        return true;
	    }
	}*/

	const int size =
	    std::accumulate(commitTxnRequest.transaction.mutations.begin(), commitTxnRequest.transaction.mutations.end(), 0,
	                    [](int total, const MutationRef& ref) { return total + ref.param2.size(); });

	TraceEvent("ShouldSplitCommitTransaction")
	    .detail("Size", size)
	    .detail("Criteria", CLIENT_KNOBS->LARGE_TRANSACTION_CRITERIA);

	return size >= CLIENT_KNOBS->LARGE_TRANSACTION_CRITERIA;
}

namespace {
/**
 * In order to distribute the commit transactions to multiple proxies, all
 * commits must share the same read conflict and write conflicts, together with
 * the same splitID
 */
std::vector<CommitTransactionRequest> prepareSplitTransactions(const CommitTransactionRequest& commitTxnRequest,
                                                                      const int numProxies) {

	std::vector<CommitTransactionRequest> result;
	UID splitID = deterministicRandom()->randomUniqueID();

	result.reserve(numProxies);

	for (auto i = 0; i < numProxies; ++i) {
		result.emplace_back(CommitTransactionRequest(commitTxnRequest));

		auto& newRequest= result.back();
		newRequest.splitTransaction = SplitTransaction(splitID, numProxies, i);

		// Add FLAG_FIRST_IN_BATCH, to ensure the split transaction is single
		newRequest.flags |= CommitTransactionRequest::FLAG_FIRST_IN_BATCH;

		newRequest.transaction.mutations = VectorRef<MutationRef>();
		newRequest.transaction.read_conflict_ranges = VectorRef<KeyRangeRef>();
		newRequest.transaction.write_conflict_ranges = VectorRef<KeyRangeRef>();
	}

	// Distribute the conflicts to proxies
	auto conflict_split_mode = CLIENT_KNOBS->TRANSACTION_SPLIT_MODE & CONFLICTS_MASK;

	if (conflict_split_mode == CONFLICTS_TO_ONE_PROXY) {

		const int proxyWithAllConflictsIndex = deterministicRandom()->randomInt(0, numProxies);
		auto& requestWithAllConflicts = result[proxyWithAllConflictsIndex];
		requestWithAllConflicts.transaction.read_conflict_ranges = commitTxnRequest.transaction.read_conflict_ranges;
		requestWithAllConflicts.transaction.write_conflict_ranges = commitTxnRequest.transaction.write_conflict_ranges;

	} else if (conflict_split_mode == CONFLICTS_EVENLY_DISTRIBUTE) {

		const auto transaction = commitTxnRequest.transaction;
		int proxyIndex = 0;

		for (int i = 0; i < transaction.read_conflict_ranges.size(); ++i) {
			result[proxyIndex].transaction.read_conflict_ranges.emplace_back(
				result[proxyIndex].arena, transaction.read_conflict_ranges[i]
			);
			proxyIndex = (proxyIndex + 1) % numProxies;
		}

		for (int i = 0; i < transaction.write_conflict_ranges.size(); ++i) {
			result[proxyIndex].transaction.write_conflict_ranges.emplace_back(
				result[proxyIndex].arena, transaction.write_conflict_ranges[i]
			);
			proxyIndex = (proxyIndex + 1) % numProxies;
		}

	} else {
		UNREACHABLE();
	}

	return result;
}
}	// anonymous amespace

struct MutationValueSizeIndex {
	int valueSize = 0;
	int index;

	bool operator<(const MutationValueSizeIndex& another) const { return this->valueSize < another.valueSize; }
};

struct MutationTotalValueSizeIndex {
	int totalValueSize = 0;
	int index;

	bool operator>(const MutationTotalValueSizeIndex& another) const {
		return this->totalValueSize > another.totalValueSize;
	}
};

void distributeMutations(const CommitTransactionRequest& request,
                                std::vector<CommitTransactionRequest>& splitCommitTxnRequests) {

	const int NUM_PROXIES = splitCommitTxnRequests.size();
	const int NUM_MUTATIONS = request.transaction.mutations.size();

	const auto& mutations = request.transaction.mutations;

	// NOTE since the partition problem is NP-complete, a greed approach is used
	// instead. REF: https://en.wikipedia.org/wiki/Partition_problem

	using MutationValueSizeIndexHeap = std::priority_queue<MutationValueSizeIndex>;
	using MutationTotalValueSizeIndexHeap =
	    std::priority_queue<MutationTotalValueSizeIndex, std::vector<MutationTotalValueSizeIndex>,
	                        std::greater<MutationTotalValueSizeIndex>>;

	// First int is the value size, second int is the index
	MutationValueSizeIndexHeap valueSizeIndex;
	for (auto i = 0; i < NUM_MUTATIONS; ++i) {
		valueSizeIndex.push(MutationValueSizeIndex{ mutations[i].param2.size(), i });
	}

	// Now distribute the mutations per proxies. Since the mutations are sorted
	// by value size, descendingly, always put the mutations to the split
	// transactions with minimal value size.
	MutationTotalValueSizeIndexHeap txnTotalValueSizeHeap;
	for (auto i = 0; i < NUM_PROXIES; ++i) txnTotalValueSizeHeap.push(MutationTotalValueSizeIndex{ 0, i });

	while (!valueSizeIndex.empty()) {
		auto item = valueSizeIndex.top();
		valueSizeIndex.pop();

		// Select the transaction with minimal value size
		auto selectedTxn = txnTotalValueSizeHeap.top();
		txnTotalValueSizeHeap.pop();

		auto& currTxnReq = splitCommitTxnRequests[selectedTxn.index];
		auto& arena = currTxnReq.arena;
		auto& currMutations = currTxnReq.transaction.mutations;
		currMutations.push_back(arena,
		                        // The mutation item is *copied* rather than *moved*, as the
		                        // original transaction might still be useful by other parts of the
		                        // code.
		                        MutationRef(arena, mutations[item.index]));

		selectedTxn.totalValueSize += item.valueSize;
		txnTotalValueSizeHeap.push(selectedTxn);
	}
}

/**
 * Evenly split mutations in a given transaction into multiple transactions.
 */
std::vector<CommitTransactionRequest> splitCommitTransactionRequest(const CommitTransactionRequest& commitTxnRequest,
                                                                    const int numProxies) {

	std::vector<CommitTransactionRequest> result(prepareSplitTransactions(commitTxnRequest, numProxies));

	distributeMutations(commitTxnRequest, result);

	return result;
}
