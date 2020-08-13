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

bool shouldSplitCommitTransactionRequest(const CommitTransactionRequest& commitTxnRequest, const int numProxies) {

	if (numProxies < 2 || commitTxnRequest.transaction.mutations.size() < 2) {
		return false;
	}

	for (auto& mutation : commitTxnRequest.transaction.mutations) {
		if (mutation.param1.toString() == "do_split") {
			return true;
		}
	}

	const int size =
	    std::accumulate(commitTxnRequest.transaction.mutations.begin(), commitTxnRequest.transaction.mutations.end(), 0,
	                    [](int total, const MutationRef& ref) { return total + ref.param2.size(); });

	return size >= MAX_SINGLE_TRANSACTION_VALUES_SIZE;
}

/**
 * In order to distribute the commit transactions to multiple proxies, all
 * commits must share the same read conflict and write conflicts, together with
 * the same splitID
 */
static std::vector<CommitTransactionRequest> prepareSplitTransactions(const CommitTransactionRequest& commitTxnRequest,
                                                                      const int numProxies) {

	std::vector<CommitTransactionRequest> result;
	UID splitID = deterministicRandom()->randomUniqueID();

	result.reserve(numProxies);

	for (auto i = 0; i < numProxies; ++i) {
		result.emplace_back(CommitTransactionRequest(commitTxnRequest));

		auto& newRef = result.back();
		newRef.splitTransaction = SplitTransaction(splitID, numProxies, i);

		// Add FLAG_FIRST_IN_BATCH, to ensure the split transaction is single
		newRef.flags |= CommitTransactionRequest::FLAG_FIRST_IN_BATCH;

		newRef.transaction.mutations = VectorRef<MutationRef>();
	}

	return result;
}

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

static void distributeMutations(const CommitTransactionRequest& request,
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
		                        // original transaction might still be useful by other part of the
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
