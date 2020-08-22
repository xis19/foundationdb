/*
 * TLogInterface.h
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

#ifndef FDBSERVER_TLOGINTERFACE_H
#define FDBSERVER_TLOGINTERFACE_H
#pragma once

#include <iterator>

#include "fdbclient/CommitTransaction.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/MasterProxyInterface.h"
#include "fdbclient/MutationList.h"
#include "fdbclient/StorageServerInterface.h"

struct TLogInterface {
	constexpr static FileIdentifier file_identifier = 16308510;
	enum { LocationAwareLoadBalance = 1 };
	enum { AlwaysFresh = 1 };

	LocalityData filteredLocality;
	UID uniqueID;
	UID sharedTLogID;

	RequestStream< struct TLogPeekRequest > peekMessages;
	RequestStream< struct TLogPopRequest > popMessages;

	RequestStream< struct TLogCommitRequest > commit;
	RequestStream< ReplyPromise< struct TLogLockResult > > lock; // first stage of database recovery
	RequestStream< struct TLogQueuingMetricsRequest > getQueuingMetrics;
	RequestStream< struct TLogConfirmRunningRequest > confirmRunning; // used for getReadVersion requests from client
	RequestStream<ReplyPromise<Void>> waitFailure;
	RequestStream< struct TLogRecoveryFinishedRequest > recoveryFinished;
	RequestStream< struct TLogDisablePopRequest> disablePopRequest;
	RequestStream< struct TLogEnablePopRequest> enablePopRequest;
	RequestStream< struct TLogSnapRequest> snapRequest;

	TLogInterface() {}
	explicit TLogInterface(const LocalityData& locality) : uniqueID( deterministicRandom()->randomUniqueID() ), filteredLocality(locality) { sharedTLogID = uniqueID; }
	TLogInterface(UID sharedTLogID, const LocalityData& locality) : uniqueID( deterministicRandom()->randomUniqueID() ), sharedTLogID(sharedTLogID), filteredLocality(locality) {}
	TLogInterface(UID uniqueID, UID sharedTLogID, const LocalityData& locality) : uniqueID(uniqueID), sharedTLogID(sharedTLogID), filteredLocality(locality) {}
	UID id() const { return uniqueID; }
	UID getSharedTLogID() const { return sharedTLogID; }
	std::string toString() const { return id().shortString(); }
	bool operator == ( TLogInterface const& r ) const { return id() == r.id(); }
	NetworkAddress address() const { return peekMessages.getEndpoint().getPrimaryAddress(); }
	Optional<NetworkAddress> secondaryAddress() const { return peekMessages.getEndpoint().addresses.secondaryAddress; }

	void initEndpoints() {
		std::vector<std::pair<FlowReceiver*, TaskPriority>> streams;
		streams.push_back(peekMessages.getReceiver(TaskPriority::TLogPeek));
		streams.push_back(popMessages.getReceiver(TaskPriority::TLogPop));
		streams.push_back(commit.getReceiver(TaskPriority::TLogCommit));
		streams.push_back(lock.getReceiver());
		streams.push_back(getQueuingMetrics.getReceiver(TaskPriority::TLogQueuingMetrics));
		streams.push_back(confirmRunning.getReceiver(TaskPriority::TLogConfirmRunning));
		streams.push_back(waitFailure.getReceiver());
		streams.push_back(recoveryFinished.getReceiver());
		streams.push_back(disablePopRequest.getReceiver());
		streams.push_back(enablePopRequest.getReceiver());
		streams.push_back(snapRequest.getReceiver());
		FlowTransport::transport().addEndpoints(streams);
	}

	template <class Ar> 
	void serialize( Ar& ar ) {
		if constexpr (!is_fb_function<Ar>) {
			ASSERT(ar.isDeserializing || uniqueID != UID());
		}
		serializer(ar, uniqueID, sharedTLogID, filteredLocality, peekMessages);
		if( Ar::isDeserializing ) {
			popMessages = RequestStream< struct TLogPopRequest >( peekMessages.getEndpoint().getAdjustedEndpoint(1) );
			commit = RequestStream< struct TLogCommitRequest >( peekMessages.getEndpoint().getAdjustedEndpoint(2) );
			lock = RequestStream< ReplyPromise< struct TLogLockResult > >( peekMessages.getEndpoint().getAdjustedEndpoint(3) );
			getQueuingMetrics = RequestStream< struct TLogQueuingMetricsRequest >( peekMessages.getEndpoint().getAdjustedEndpoint(4) );
			confirmRunning = RequestStream< struct TLogConfirmRunningRequest >( peekMessages.getEndpoint().getAdjustedEndpoint(5) );
			waitFailure = RequestStream< ReplyPromise<Void> >( peekMessages.getEndpoint().getAdjustedEndpoint(6) );
			recoveryFinished = RequestStream< struct TLogRecoveryFinishedRequest >( peekMessages.getEndpoint().getAdjustedEndpoint(7) );
			disablePopRequest = RequestStream< struct TLogDisablePopRequest >( peekMessages.getEndpoint().getAdjustedEndpoint(8) );
			enablePopRequest = RequestStream< struct TLogEnablePopRequest >( peekMessages.getEndpoint().getAdjustedEndpoint(9) );
			snapRequest = RequestStream< struct TLogSnapRequest >( peekMessages.getEndpoint().getAdjustedEndpoint(10) );
		}
	}
};

struct TLogRecoveryFinishedRequest {
	constexpr static FileIdentifier file_identifier = 8818668;
	ReplyPromise<Void> reply;

	TLogRecoveryFinishedRequest() {}

	template <class Ar> 
	void serialize( Ar& ar ) {
		serializer(ar, reply);
	}
};

struct TLogLockResult {
	constexpr static FileIdentifier file_identifier = 11822027;
	Version end;
	Version knownCommittedVersion;

	template <class Ar>
	void serialize( Ar& ar ) {
		serializer(ar, end, knownCommittedVersion);
	}
};

struct TLogConfirmRunningRequest {
	constexpr static FileIdentifier file_identifier = 10929130;
	Optional<UID> debugID;
	ReplyPromise<Void> reply;

	TLogConfirmRunningRequest() {}
	TLogConfirmRunningRequest( Optional<UID> debugID ) : debugID(debugID) {}

	template <class Ar> 
	void serialize( Ar& ar ) {
		serializer(ar, debugID, reply);
	}
};

struct VerUpdateRef {
	Version version;
	VectorRef<MutationRef> mutations;
	bool isPrivateData;

	VerUpdateRef() : isPrivateData(false), version(invalidVersion) {}
	VerUpdateRef( Arena& to, const VerUpdateRef& from ) : version(from.version), mutations( to, from.mutations ), isPrivateData( from.isPrivateData ) {}
	int expectedSize() const { return mutations.expectedSize(); }

	MutationRef push_back_deep(Arena& arena, const MutationRef& m) {
		mutations.push_back_deep(arena, m);
		return mutations.back();
	}

	template <class Ar>
	void serialize( Ar& ar ) {
		serializer(ar, version, mutations, isPrivateData);
	}
};

struct TLogPeekReply {
	constexpr static FileIdentifier file_identifier = 11365689;
	Arena arena;
	StringRef messages;
	Version end;
	Optional<Version> popped;
	Version maxKnownVersion;
	Version minKnownCommittedVersion;
	Optional<Version> begin;
	bool onlySpilled = false;

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, arena, messages, end, popped, maxKnownVersion, minKnownCommittedVersion, begin, onlySpilled);
	}
};

struct TLogPeekRequest {
	constexpr static FileIdentifier file_identifier = 11001131;
	Arena arena;
	Version begin;
	Tag tag;
	bool returnIfBlocked;
	bool onlySpilled;
	Optional<std::pair<UID, int>> sequence;
	ReplyPromise<TLogPeekReply> reply;

	TLogPeekRequest( Version begin, Tag tag, bool returnIfBlocked, bool onlySpilled, Optional<std::pair<UID, int>> sequence = Optional<std::pair<UID, int>>() ) : begin(begin), tag(tag), returnIfBlocked(returnIfBlocked), sequence(sequence), onlySpilled(onlySpilled) {}
	TLogPeekRequest() {}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, arena, begin, tag, returnIfBlocked, onlySpilled, sequence, reply);
	}
};

struct TLogPopRequest {
	constexpr static FileIdentifier file_identifier = 5556423;
	Arena arena;
	Version to;
	Version durableKnownCommittedVersion;
	Tag tag;
	ReplyPromise<Void> reply;

	TLogPopRequest( Version to, Version durableKnownCommittedVersion, Tag tag ) : to(to), durableKnownCommittedVersion(durableKnownCommittedVersion), tag(tag) {}
	TLogPopRequest() {}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, arena, to, durableKnownCommittedVersion, tag, reply);
	}
};

struct TagMessagesRef {
	Tag tag;
	VectorRef<int> messageOffsets;

	TagMessagesRef() {}
	TagMessagesRef(Arena &a, const TagMessagesRef &from) : tag(from.tag), messageOffsets(a, from.messageOffsets) {}

	size_t expectedSize() const {
		return messageOffsets.expectedSize();
	}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, tag, messageOffsets);
	}
};

struct TLogCommitReply {
	constexpr static FileIdentifier file_identifier = 3;

	Version version;
	TLogCommitReply() = default;
	explicit TLogCommitReply(Version version) : version(version) {}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, version);
	}
};

struct TLogCommitRequest {
	constexpr static FileIdentifier file_identifier = 4022206;
	Arena arena;
	Version prevVersion, version, knownCommittedVersion, minKnownCommittedVersion;

	StringRef messages;// Each message prefixed by a 4-byte length

	ReplyPromise<TLogCommitReply> reply;
	Optional<UID> debugID;
	Optional<SplitTransaction> splitTransaction;

	TLogCommitRequest() {}
	TLogCommitRequest( const Arena& a, Version prevVersion, Version version, Version knownCommittedVersion, Version minKnownCommittedVersion, StringRef messages, Optional<UID> debugID, Optional<SplitTransaction> splitTransaction_ )
		: arena(a), prevVersion(prevVersion), version(version), knownCommittedVersion(knownCommittedVersion), minKnownCommittedVersion(minKnownCommittedVersion), messages(messages), debugID(debugID), splitTransaction(splitTransaction_) {}
	template <class Ar>
	void serialize( Ar& ar ) {
		serializer(ar, prevVersion, version, knownCommittedVersion, minKnownCommittedVersion, messages, reply, arena, debugID, splitTransaction);
	}
};


struct TLogQueuingMetricsReply {
	constexpr static FileIdentifier file_identifier = 12206626;
	double localTime;
	int64_t instanceID;  // changes if bytesDurable and bytesInput reset
	int64_t bytesDurable, bytesInput;
	StorageBytes storageBytes;
	Version v; // committed version

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, localTime, instanceID, bytesDurable, bytesInput, storageBytes, v);
	}
};

struct TLogQueuingMetricsRequest {
	constexpr static FileIdentifier file_identifier = 7798476;
	ReplyPromise<struct TLogQueuingMetricsReply> reply;

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, reply);
	}
};

struct TLogDisablePopRequest {
	constexpr static FileIdentifier file_identifier = 4022806;
	Arena arena;
	UID snapUID;
	ReplyPromise<Void> reply;
	Optional<UID> debugID;

	TLogDisablePopRequest() = default;
	TLogDisablePopRequest(const UID uid) : snapUID(uid) {}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, snapUID, reply, arena, debugID);
	}
};

struct TLogEnablePopRequest {
	constexpr static FileIdentifier file_identifier = 4022809;
	Arena arena;
	UID snapUID;
	ReplyPromise<Void> reply;
	Optional<UID> debugID;

	TLogEnablePopRequest() = default;
	TLogEnablePopRequest(const UID uid) : snapUID(uid) {}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, snapUID, reply, arena, debugID);
	}
};

struct TLogSnapRequest {
	constexpr static FileIdentifier file_identifier = 8184128;
	ReplyPromise<Void> reply;
	Arena arena;
	StringRef snapPayload;
	UID snapUID;
	StringRef role;

	TLogSnapRequest(StringRef snapPayload, UID snapUID, StringRef role) : snapPayload(snapPayload), snapUID(snapUID), role(role) {}
	TLogSnapRequest() = default;

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, reply, snapPayload, snapUID, role, arena);
	}
};

#endif
