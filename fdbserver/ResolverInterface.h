/*
 * ResolverInterface.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
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

#ifndef FDBSERVER_RESOLVERINTERFACE_H
#define FDBSERVER_RESOLVERINTERFACE_H

#pragma once

#include "fdbclient/CommitTransaction.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/MasterProxyInterface.h"
#include "fdbrpc/fdbrpc.h"
#include "fdbrpc/Locality.h"

struct ResolverInterface {
	constexpr static FileIdentifier file_identifier = 1755944;
	enum { LocationAwareLoadBalance = 1 };
	enum { AlwaysFresh = 1 };

	LocalityData locality;
	UID uniqueID;
	RequestStream< struct ResolveTransactionBatchRequest > resolve;
	RequestStream< struct ResolutionMetricsRequest > metrics;
	RequestStream< struct ResolutionSplitRequest > split;

	RequestStream<ReplyPromise<Void>> waitFailure;

	ResolverInterface() : uniqueID( deterministicRandom()->randomUniqueID() ) {}
	UID id() const { return uniqueID; }
	std::string toString() const { return id().shortString(); }
	bool operator == ( ResolverInterface const& r ) const { return id() == r.id(); }
	bool operator != ( ResolverInterface const& r ) const { return id() != r.id(); }
	NetworkAddress address() const { return resolve.getEndpoint().getPrimaryAddress(); }
	void initEndpoints() {
		metrics.getEndpoint( TaskPriority::ResolutionMetrics );
		split.getEndpoint( TaskPriority::ResolutionMetrics );
	}

	template <class Ar> 
	void serialize( Ar& ar ) {
		serializer(ar, uniqueID, locality, resolve, metrics, split, waitFailure);
	}
};

struct StateTransactionRef {
	constexpr static FileIdentifier file_identifier = 6150271;
	StateTransactionRef() {}
	StateTransactionRef(const bool committed, VectorRef<MutationRef> const& mutations) : committed(committed), mutations(mutations) {}
	StateTransactionRef(Arena &p, const StateTransactionRef &toCopy) : committed(toCopy.committed), mutations(p, toCopy.mutations) {}
	bool committed;
	VectorRef<MutationRef> mutations;
	size_t expectedSize() const {
		return mutations.expectedSize();
	}

	template <class Archive>
	void serialize(Archive& ar) {
		serializer(ar, committed, mutations);
	}
};

struct ResolveTransactionBatchReply {
	constexpr static FileIdentifier file_identifier = 15472264;
	Arena arena;
	VectorRef<uint8_t> committed;
	Optional<UID> debugID;
	VectorRef<VectorRef<StateTransactionRef>> stateMutations;  // [version][transaction#] -> (committed, [mutation#])
	std::map<int, VectorRef<int>>
	    conflictingKeyRangeMap; // transaction index -> conflicting read_conflict_range ids given by the resolver

	template <class Archive>
	void serialize(Archive& ar) {
		serializer(ar, committed, stateMutations, debugID, conflictingKeyRangeMap, arena);
	}

};

struct ResolveTransactionBatchRequest {
	constexpr static FileIdentifier file_identifier = 16462858;
	Arena arena;

	SpanID spanContext;
	Version prevVersion;
	Version version;   // FIXME: ?
	Version lastReceivedVersion;
	VectorRef<struct CommitTransactionRef> transactions;
	VectorRef<int> txnStateTransactions;   // Offsets of elements of transactions that have (transaction subsystem state) mutations
	ReplyPromise<ResolveTransactionBatchReply> reply;
	Optional<UID> debugID;
	Optional<SplitTransaction> splitTransaction;

	template <class Archive>
	void serialize(Archive& ar) {
		serializer(ar, prevVersion, version, lastReceivedVersion, transactions, txnStateTransactions, reply, arena,
		           debugID, spanContext, splitTransaction);
	}
};

struct ResolutionMetricsReply {
	constexpr static FileIdentifier file_identifier = 3;

	int64_t value;
	ResolutionMetricsReply() = default;
	explicit ResolutionMetricsReply(int64_t value) : value(value) {}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, value);
	}
};

struct ResolutionMetricsRequest {
	constexpr static FileIdentifier file_identifier = 11663527;
	ReplyPromise<ResolutionMetricsReply> reply;

	template <class Archive>
	void serialize(Archive& ar) {
		serializer(ar, reply);
	}
};

struct ResolutionSplitReply {
	constexpr static FileIdentifier file_identifier = 12137765;
	Key key;
	int64_t used;
	template <class Archive>
	void serialize(Archive& ar) {
		serializer(ar, key, used);
	}

};

struct ResolutionSplitRequest {
	constexpr static FileIdentifier file_identifier = 167535;
	KeyRange range;
	int64_t offset;
	bool front;
	ReplyPromise<ResolutionSplitReply> reply;

	template <class Archive>
	void serialize(Archive& ar) {
		serializer(ar, range, offset, front, reply);
	}
};

#endif
