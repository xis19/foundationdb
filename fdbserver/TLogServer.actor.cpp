/*
 * TLogServer.actor.cpp
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

#include <chrono>
#include <type_traits>
#include <vector>

#include "fdbclient/FDBTypes.h"
#include "fdbclient/KeyRangeMap.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/Notified.h"
#include "fdbclient/RunTransaction.actor.h"
#include "fdbclient/SystemData.h"
#include "fdbrpc/FailureMonitor.h"
#include "fdbrpc/Stats.h"
#include "fdbrpc/sim_validation.h"
#include "fdbrpc/simulator.h"
#include "fdbserver/FDBExecHelper.actor.h"
#include "fdbserver/IDiskQueue.h"
#include "fdbserver/IKeyValueStore.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/LogProtocolMessage.h"
#include "fdbserver/LogSystem.h"
#include "fdbserver/MutationTracking.h"
#include "fdbserver/PartMerger.h"
#include "fdbserver/RecoveryState.h"
#include "fdbserver/ServerDBInfo.h"
#include "fdbserver/TimedKVCache.h"
#include "fdbserver/TLogInterface.h"
#include "fdbserver/WaitFailure.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "flow/ActorCollection.h"
#include "flow/Hash3.h"
#include "flow/UnitTest.h"

#include "flow/actorcompiler.h"  // This must be the last #include.

using std::pair;
using std::make_pair;
using std::min;
using std::max;

class TLogCommitRequestMerger : public PartMerger<UID, TLogCommitRequest> {
protected:
	virtual void merge(value_t& value, const value_t& incomingRequest) override {
		ASSERT(value.prevVersion == incomingRequest.prevVersion);
		ASSERT(value.version == incomingRequest.version);

		// FIXME -- think about this
		// ASSERT(value.knownCommittedVersion == incomingRequest.knownCommittedVersion);
		// ASSERT(value.minKnownCommittedVersion == incomingRequest.minKnownCommittedVersion);
		value.knownCommittedVersion = std::max(value.knownCommittedVersion, incomingRequest.knownCommittedVersion);
		value.minKnownCommittedVersion =
		    std::max(value.minKnownCommittedVersion, incomingRequest.minKnownCommittedVersion);

		if (!value.additionalMessages.present()) {
			value.additionalMessages = std::vector<std::pair<Arena, StringRef>>();
		}
		value.additionalMessages.get().emplace_back(std::make_pair(incomingRequest.arena, incomingRequest.messages));
	}

public:
	using PartMerger::PartMerger;
};

const auto SPLIT_TRANSACTION_HISTORY = std::chrono::duration_cast<std::chrono::seconds>(
    std::chrono::microseconds(SERVER_KNOBS->SPLIT_TRANSACTION_HISTORY_LENGTH));

struct TLogQueueEntryRef {
	UID id;
	Version version;
	Version knownCommittedVersion;
	StringRef messages;

	TLogQueueEntryRef() : version(0), knownCommittedVersion(0) {}
	TLogQueueEntryRef(Arena &a, TLogQueueEntryRef const &from)
	  : version(from.version), knownCommittedVersion(from.knownCommittedVersion), id(from.id), messages(a, from.messages) {
	}

	//To change this serialization, ProtocolVersion::TLogQueueEntryRef must be updated, and downgrades need to be considered
	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, version, messages, knownCommittedVersion, id);
	}
	size_t expectedSize() const {
		return messages.expectedSize();
	}
};

struct AlternativeTLogQueueEntryRef {
	UID id;
	Version version;
	Version knownCommittedVersion;
	std::vector<TagsAndMessage>* alternativeMessages;

	AlternativeTLogQueueEntryRef() : version(0), knownCommittedVersion(0), alternativeMessages(NULL) {}

	template <class Ar>
	void serialize(Ar& ar) {
		ASSERT(!ar.isDeserializing && alternativeMessages);
		uint32_t msgSize = expectedSize();
		serializer(ar, version, msgSize);
		for(auto& msg : *alternativeMessages) {
			ar.serializeBytes( msg.message );
		}
		serializer(ar, knownCommittedVersion, id);
	}

	uint32_t expectedSize() const {
		uint32_t msgSize = 0;
		for(auto& msg : *alternativeMessages) {
			msgSize += msg.message.size();
		}
		return msgSize;
	}
};

typedef Standalone<TLogQueueEntryRef> TLogQueueEntry;
struct LogData;
struct TLogData;

struct TLogQueue : public IClosable {
public:
	TLogQueue( IDiskQueue* queue, UID dbgid ) : queue(queue), dbgid(dbgid) {}

	// Each packet in the queue is
	//    uint32_t payloadSize
	//    uint8_t payload[payloadSize]  (begins with uint64_t protocolVersion via IncludeVersion)
	//    uint8_t validFlag

	// TLogQueue is a durable queue of TLogQueueEntry objects with an interface similar to IDiskQueue

	// TLogQueue pushes (but not commits) are atomic - after commit fails to return, a prefix of entire calls to push are durable.  This is
	//    implemented on top of the weaker guarantee of IDiskQueue::commit (that a prefix of bytes is durable) using validFlag and by
	//    padding any incomplete packet with zeros after recovery.

	// Before calling push, pop, or commit, the user must call readNext() until it throws
	//    end_of_stream(). It may not be called again thereafter.
	Future<TLogQueueEntry> readNext( TLogData* tLog ) {
		return readNext( this, tLog );
	}

	Future<bool> initializeRecovery( IDiskQueue::location recoverAt ) {
		return queue->initializeRecovery( recoverAt );
	}

	template <class T>
	void push( T const& qe, Reference<LogData> logData );
	void forgetBefore( Version upToVersion, Reference<LogData> logData );
	void pop( IDiskQueue::location upToLocation );
	Future<Void> commit() { return queue->commit(); }

	// Implements IClosable
	virtual Future<Void> getError() { return queue->getError(); }
	virtual Future<Void> onClosed() { return queue->onClosed(); }
	virtual void dispose() { queue->dispose(); delete this; }
	virtual void close() { queue->close(); delete this; }

private:
	IDiskQueue* queue;
	UID dbgid;

	void updateVersionSizes( const TLogQueueEntry& result, TLogData* tLog, IDiskQueue::location start, IDiskQueue::location end );

	ACTOR static Future<TLogQueueEntry> readNext( TLogQueue* self, TLogData* tLog ) {
		state TLogQueueEntry result;
		state int zeroFillSize = 0;

		loop {
			state IDiskQueue::location startloc = self->queue->getNextReadLocation();
			Standalone<StringRef> h = wait( self->queue->readNext( sizeof(uint32_t) ) );
			if (h.size() != sizeof(uint32_t)) {
				if (h.size()) {
					TEST( true );  // Zero fill within size field
					int payloadSize = 0;
					memcpy(&payloadSize, h.begin(), h.size());
					zeroFillSize = sizeof(uint32_t)-h.size(); // zero fill the size itself
					zeroFillSize += payloadSize+1;  // and then the contents and valid flag
				}
				break;
			}

			state uint32_t payloadSize = *(uint32_t*)h.begin();
			ASSERT( payloadSize < (100<<20) );

			Standalone<StringRef> e = wait( self->queue->readNext( payloadSize+1 ) );
			if (e.size() != payloadSize+1) {
				TEST( true ); // Zero fill within payload
				zeroFillSize = payloadSize+1 - e.size();
				break;
			}

			if (e[payloadSize]) {
				ASSERT( e[payloadSize] == 1 );
				Arena a = e.arena();
				ArenaReader ar( a, e.substr(0, payloadSize), IncludeVersion() );
				ar >> result;
				const IDiskQueue::location endloc = self->queue->getNextReadLocation();
				self->updateVersionSizes(result, tLog, startloc, endloc);
				return result;
			}
		}
		if (zeroFillSize) {
			TEST( true );  // Fixing a partial commit at the end of the tlog queue
			for(int i=0; i<zeroFillSize; i++)
				self->queue->push( StringRef((const uint8_t*)"",1) );
		}
		throw end_of_stream();
	}
};

////// Persistence format (for self->persistentData)

// Immutable keys
// persistFormat has been mostly invalidated by TLogVersion, and can probably be removed when
// 4.6's TLog code is removed.
static const KeyValueRef persistFormat( LiteralStringRef( "Format" ), LiteralStringRef("FoundationDB/LogServer/3/0") );
static const KeyRangeRef persistFormatReadableRange( LiteralStringRef("FoundationDB/LogServer/3/0"), LiteralStringRef("FoundationDB/LogServer/4/0") );
static const KeyRangeRef persistProtocolVersionKeys( LiteralStringRef( "ProtocolVersion/" ), LiteralStringRef( "ProtocolVersion0" ) );
static const KeyRangeRef persistTLogSpillTypeKeys( LiteralStringRef( "TLogSpillType/" ), LiteralStringRef( "TLogSpillType0" ) );
static const KeyRangeRef persistRecoveryCountKeys = KeyRangeRef( LiteralStringRef( "DbRecoveryCount/" ), LiteralStringRef( "DbRecoveryCount0" ) );

// Updated on updatePersistentData()
static const KeyRangeRef persistCurrentVersionKeys = KeyRangeRef( LiteralStringRef( "version/" ), LiteralStringRef( "version0" ) );
static const KeyRangeRef persistKnownCommittedVersionKeys = KeyRangeRef( LiteralStringRef( "knownCommitted/" ), LiteralStringRef( "knownCommitted0" ) );
static const KeyRef persistRecoveryLocationKey = KeyRef( LiteralStringRef( "recoveryLocation" ) );
static const KeyRangeRef persistLocalityKeys = KeyRangeRef( LiteralStringRef( "Locality/" ), LiteralStringRef( "Locality0" ) );
static const KeyRangeRef persistLogRouterTagsKeys = KeyRangeRef( LiteralStringRef( "LogRouterTags/" ), LiteralStringRef( "LogRouterTags0" ) );
static const KeyRangeRef persistTxsTagsKeys = KeyRangeRef( LiteralStringRef( "TxsTags/" ), LiteralStringRef( "TxsTags0" ) );
static const KeyRange persistTagMessagesKeys = prefixRange(LiteralStringRef("TagMsg/"));
static const KeyRange persistTagMessageRefsKeys = prefixRange(LiteralStringRef("TagMsgRef/"));
static const KeyRange persistTagPoppedKeys = prefixRange(LiteralStringRef("TagPop/"));

static Key persistTagMessagesKey( UID id, Tag tag, Version version ) {
	BinaryWriter wr( Unversioned() );
	wr.serializeBytes(persistTagMessagesKeys.begin);
	wr << id;
	wr << tag;
	wr << bigEndian64( version );
	return wr.toValue();
}

static Key persistTagMessageRefsKey( UID id, Tag tag, Version version ) {
	BinaryWriter wr( Unversioned() );
	wr.serializeBytes(persistTagMessageRefsKeys.begin);
	wr << id;
	wr << tag;
	wr << bigEndian64( version );
	return wr.toValue();
}

static Key persistTagPoppedKey( UID id, Tag tag ) {
	BinaryWriter wr(Unversioned());
	wr.serializeBytes( persistTagPoppedKeys.begin );
	wr << id;
	wr << tag;
	return wr.toValue();
}

static Value persistTagPoppedValue( Version popped ) {
	return BinaryWriter::toValue( popped, Unversioned() );
}

static Tag decodeTagPoppedKey( KeyRef id, KeyRef key ) {
	Tag s;
	BinaryReader rd( key.removePrefix(persistTagPoppedKeys.begin).removePrefix(id), Unversioned() );
	rd >> s;
	return s;
}

static Version decodeTagPoppedValue( ValueRef value ) {
	return BinaryReader::fromStringRef<Version>( value, Unversioned() );
}

static StringRef stripTagMessagesKey( StringRef key ) {
	return key.substr( sizeof(UID) + sizeof(Tag) + persistTagMessagesKeys.begin.size() );
}

static Version decodeTagMessagesKey( StringRef key ) {
	return bigEndian64( BinaryReader::fromStringRef<Version>( stripTagMessagesKey(key), Unversioned() ) );
}

struct SpilledData {
	SpilledData() = default;
	SpilledData(Version version, IDiskQueue::location start, uint32_t length, uint32_t mutationBytes)
		: version(version), start(start), length(length), mutationBytes(mutationBytes) {
	}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, version, start, length, mutationBytes);
	}

	Version version = 0;
	IDiskQueue::location start = 0;
	uint32_t length = 0;
	uint32_t mutationBytes = 0;
};

struct TLogData : NonCopyable {
	AsyncTrigger newLogData;
	// A process has only 1 SharedTLog, which holds data for multiple logs, so that it obeys its assigned memory limit.
	// A process has only 1 active log and multiple non-active log from old generations.
	// In the figure below, TLog [1-4] are logs from old generations.
	// Because SS may need to pull data from old generation log, we keep Tlog [1-4].
	//
	//  We always pop the disk queue from the oldest TLog, spill from the oldest TLog that still has
	//  data in memory, and commits to the disk queue come from the most recent TLog.
	//
	//                    SharedTLog
	//  +--------+--------+--------+--------+--------+
	//  | TLog 1 | TLog 2 | TLog 3 | TLog 4 | TLog 5 |
	//  +--------+--------+--------+--------+--------+
	//    ^ popOrder         ^spillOrder         ^committing
	// 
	// ^popOrder is the location where SS reads the to-be-read data from tlog.
	// ^committing is the location where the active TLog accepts the pushed data.
	Deque<UID> popOrder;
	Deque<UID> spillOrder;
	std::map<UID, Reference<struct LogData>> id_data;

	UID dbgid;
	UID workerID;

	TLogCommitRequestMerger splitTransactionMerger;
	TimedKVCache<UID, Promise<Version>> splitTransactionResponse;

	IKeyValueStore* persistentData; // Durable data on disk that were spilled.
	IDiskQueue* rawPersistentQueue; // The physical queue the persistentQueue below stores its data. Ideally, log interface should work without directly accessing rawPersistentQueue
	TLogQueue *persistentQueue;	// Logical queue the log operates on and persist its data.

	int64_t diskQueueCommitBytes;
	AsyncVar<bool> largeDiskQueueCommitBytes; //becomes true when diskQueueCommitBytes is greater than MAX_QUEUE_COMMIT_BYTES

	Reference<AsyncVar<ServerDBInfo>> dbInfo;
	Database cx;

	NotifiedVersion queueCommitEnd;
	Version queueCommitBegin;

	int64_t instanceID;
	int64_t bytesInput;
	int64_t bytesDurable;
	int64_t targetVolatileBytes;  // The number of bytes of mutations this TLog should hold in memory before spilling.
	int64_t overheadBytesInput;
	int64_t overheadBytesDurable;

	WorkerCache<TLogInterface> tlogCache;
	FlowLock peekMemoryLimiter;

	PromiseStream<Future<Void>> sharedActors;
	Promise<Void> terminated;
	FlowLock concurrentLogRouterReads;
	FlowLock persistentDataCommitLock;

	bool ignorePopRequest;    // ignore pop request from storage servers
	double ignorePopDeadline; // time until which the ignorePopRequest will be
                              // honored
	std::string ignorePopUid; // callers that set ignorePopRequest will set this
                              // extra state, used to validate the ownership of
                              // the set and for callers that unset will
                              // be able to match it up
	std::string dataFolder; // folder where data is stored
	std::map<Tag, Version> toBePopped; // map of Tag->Version for all the pops
                                       // that came when ignorePopRequest was set
	Reference<AsyncVar<bool>> degraded;
	std::vector<TagsAndMessage> tempTagMessages;

	TLogData(UID dbgid, UID workerID, IKeyValueStore* persistentData, IDiskQueue* persistentQueue,
	         Reference<AsyncVar<ServerDBInfo>> dbInfo, Reference<AsyncVar<bool>> degraded, std::string folder)
	  : dbgid(dbgid), workerID(workerID), splitTransactionMerger(SPLIT_TRANSACTION_HISTORY),
	    splitTransactionResponse(SPLIT_TRANSACTION_HISTORY),
	    instanceID(deterministicRandom()->randomUniqueID().first()), persistentData(persistentData),
	    rawPersistentQueue(persistentQueue), persistentQueue(new TLogQueue(persistentQueue, dbgid)), dbInfo(dbInfo),
	    degraded(degraded), queueCommitBegin(0), queueCommitEnd(0), diskQueueCommitBytes(0),
	    largeDiskQueueCommitBytes(false), bytesInput(0), bytesDurable(0),
	    targetVolatileBytes(SERVER_KNOBS->TLOG_SPILL_THRESHOLD), overheadBytesInput(0), overheadBytesDurable(0),
	    peekMemoryLimiter(SERVER_KNOBS->TLOG_SPILL_REFERENCE_MAX_PEEK_MEMORY_BYTES),
	    concurrentLogRouterReads(SERVER_KNOBS->CONCURRENT_LOG_ROUTER_READS), ignorePopRequest(false),
	    ignorePopDeadline(), ignorePopUid(), dataFolder(folder), toBePopped() {
		cx = openDBOnServer(dbInfo, TaskPriority::DefaultEndpoint, true, true);
	}
};

struct LogData : NonCopyable, public ReferenceCounted<LogData> {
	struct TagData : NonCopyable, public ReferenceCounted<TagData> {
		std::deque<std::pair<Version, LengthPrefixedStringRef>> versionMessages;
		bool nothingPersistent;				// true means tag is *known* to have no messages in persistentData.  false means nothing.
		bool poppedRecently;					// `popped` has changed since last updatePersistentData
		Version popped;				// see popped version tracking contract below
		Version persistentPopped;  // The popped version recorded in the btree.
		Version versionForPoppedLocation;  // `poppedLocation` was calculated at this popped version
		IDiskQueue::location poppedLocation;  // The location of the earliest commit with data for this tag.
		bool unpoppedRecovered;
		Tag tag;

		TagData( Tag tag, Version popped, IDiskQueue::location poppedLocation, bool nothingPersistent, bool poppedRecently, bool unpoppedRecovered ) : tag(tag), nothingPersistent(nothingPersistent), poppedRecently(poppedRecently), popped(popped), persistentPopped(0), versionForPoppedLocation(0), poppedLocation(poppedLocation), unpoppedRecovered(unpoppedRecovered) {}

		TagData(TagData&& r) noexcept
		  : versionMessages(std::move(r.versionMessages)), nothingPersistent(r.nothingPersistent),
		    poppedRecently(r.poppedRecently), popped(r.popped), persistentPopped(r.persistentPopped),
		    versionForPoppedLocation(r.versionForPoppedLocation), poppedLocation(r.poppedLocation), tag(r.tag),
		    unpoppedRecovered(r.unpoppedRecovered) {}

		void operator=(TagData&& r) noexcept {
			versionMessages = std::move(r.versionMessages);
			nothingPersistent = r.nothingPersistent;
			poppedRecently = r.poppedRecently;
			popped = r.popped;
			persistentPopped = r.persistentPopped;
			versionForPoppedLocation = r.versionForPoppedLocation;
			poppedLocation = r.poppedLocation;
			tag = r.tag;
			unpoppedRecovered = r.unpoppedRecovered;
		}

		// Erase messages not needed to update *from* versions >= before (thus, messages with toversion <= before)
		ACTOR Future<Void> eraseMessagesBefore( TagData *self, Version before, TLogData *tlogData, Reference<LogData> logData, TaskPriority taskID ) {
			while(!self->versionMessages.empty() && self->versionMessages.front().first < before) {
				Version version = self->versionMessages.front().first;
				std::pair<int,int> &sizes = logData->version_sizes[version];
				int64_t messagesErased = 0;

				while(!self->versionMessages.empty() && self->versionMessages.front().first == version) {
					auto const& m = self->versionMessages.front();
					++messagesErased;

					if(self->tag.locality != tagLocalityTxs && self->tag != txsTag) {
						sizes.first -= m.second.expectedSize();
					} else {
						sizes.second -= m.second.expectedSize();
					}

					self->versionMessages.pop_front();
				}

				int64_t bytesErased = messagesErased * SERVER_KNOBS->VERSION_MESSAGES_ENTRY_BYTES_WITH_OVERHEAD;
				logData->bytesDurable += bytesErased;
				tlogData->bytesDurable += bytesErased;
				tlogData->overheadBytesDurable += bytesErased;
				wait(yield(taskID));
			}

			return Void();
		}

		Future<Void> eraseMessagesBefore(Version before, TLogData *tlogData, Reference<LogData> logData, TaskPriority taskID) {
			return eraseMessagesBefore(this, before, tlogData, logData, taskID);
		}
	};

	Map<Version, std::pair<IDiskQueue::location, IDiskQueue::location>> versionLocation;  // For the version of each entry that was push()ed, the [start, end) location of the serialized bytes

	/*
	Popped version tracking contract needed by log system to implement ILogCursor::popped():

		- Log server tracks for each (possible) tag a popped_version
		Impl: TagData::popped (in memory) and persistTagPoppedKeys (in persistentData)
		- popped_version(tag) is <= the maximum version for which log server (or a predecessor) is ever asked to pop the tag
		Impl: Only increased by tLogPop() in response to either a pop request or recovery from a predecessor
		- popped_version(tag) is > the maximum version for which log server is unable to peek messages due to previous pops (on this server or a predecessor)
		Impl: Increased by tLogPop() atomically with erasing messages from memory; persisted by updatePersistentData() atomically with erasing messages from store; messages are not erased from queue where popped_version is not persisted
		- LockTLogReply returns all tags which either have messages, or which have nonzero popped_versions
		Impl: tag_data is present for all such tags
		- peek(tag, v) returns the popped_version for tag if that is greater than v
		Impl: Check tag_data->popped (after all waits)
	*/

	AsyncTrigger stopCommit;
	bool stopped, initialized;
	DBRecoveryCount recoveryCount;

	VersionMetricHandle persistentDataVersion, persistentDataDurableVersion;  // The last version number in the portion of the log (written|durable) to persistentData
	NotifiedVersion version, queueCommittedVersion;
	Version queueCommittingVersion;
	Version knownCommittedVersion, durableKnownCommittedVersion, minKnownCommittedVersion;
	Version queuePoppedVersion;
	Version minPoppedTagVersion;
	Tag minPoppedTag;

	Deque<std::pair<Version, Standalone<VectorRef<uint8_t>>>> messageBlocks;
	std::vector<std::vector<Reference<TagData>>> tag_data; //tag.locality | tag.id
	int unpoppedRecoveredTags;

	Reference<TagData> getTagData(Tag tag) {
		int idx = tag.toTagDataIndex();
		if(idx >= tag_data.size()) {
			tag_data.resize(idx+1);
		}
		if(tag.id >= tag_data[idx].size()) {
			tag_data[idx].resize(tag.id+1);
		}
		return tag_data[idx][tag.id];
	}

	//only callable after getTagData returns a null reference
	Reference<TagData> createTagData(Tag tag, Version popped, bool nothingPersistent, bool poppedRecently, bool unpoppedRecovered) {
		if(tag.locality != tagLocalityLogRouter && tag.locality != tagLocalityTxs && tag != txsTag && allTags.size() && !allTags.count(tag) && popped <= recoveredAt) {
			popped = recoveredAt + 1;
		}
		Reference<TagData> newTagData = Reference<TagData>( new TagData(tag, popped, 0, nothingPersistent, poppedRecently, unpoppedRecovered) );
		tag_data[tag.toTagDataIndex()][tag.id] = newTagData;
		return newTagData;
	}

	Map<Version, std::pair<int,int>> version_sizes;

	CounterCollection cc;
	Counter bytesInput;
	Counter bytesDurable;

	UID logId;
	ProtocolVersion protocolVersion;
	Version newPersistentDataVersion;
	Future<Void> removed;
	PromiseStream<Future<Void>> addActor;
	TLogData* tLogData;
	Promise<Void> recoveryComplete, committingQueue;
	Version unrecoveredBefore, recoveredAt;

	struct PeekTrackerData {
		std::map<int, Promise<std::pair<Version, bool>>> sequence_version;
		double lastUpdate;

		Tag tag;
		
		double lastLogged;
		int64_t totalPeeks;
		int64_t replyBytes;
		int64_t duplicatePeeks;
		double queueTime;
		double queueMax;
		double blockTime;
		double blockMax;
		double workTime;
		double workMax;

		int64_t unblockedPeeks;
		double idleTime;
		double idleMax;

		PeekTrackerData() : lastUpdate(0) {
			resetMetrics();
		}

		void resetMetrics() {
			lastLogged = now();
			totalPeeks = 0;
			replyBytes = 0;
			duplicatePeeks = 0;
			queueTime = 0;
			queueMax = 0;
			blockTime = 0;
			blockMax = 0;
			workTime = 0;
			workMax = 0;
			unblockedPeeks = 0;
			idleTime = 0;
			idleMax = 0;
		}
	};

	std::map<UID, PeekTrackerData> peekTracker;

	Reference<AsyncVar<Reference<ILogSystem>>> logSystem;
	Tag remoteTag;
	bool isPrimary;
	int logRouterTags;
	Version logRouterPoppedVersion, logRouterPopToVersion;
	int8_t locality;
	UID recruitmentID;
	TLogSpillType logSpillType;
	std::set<Tag> allTags;
	Future<Void> terminated;
	FlowLock execOpLock;
	bool execOpCommitInProgress;
	int txsTags;

	explicit LogData(TLogData* tLogData, TLogInterface interf, Tag remoteTag, bool isPrimary, int logRouterTags, int txsTags, UID recruitmentID, ProtocolVersion protocolVersion, TLogSpillType logSpillType, std::vector<Tag> tags, std::string context) 
			: tLogData(tLogData), knownCommittedVersion(0), logId(interf.id()),
			  cc("TLog", interf.id().toString()), bytesInput("BytesInput", cc), bytesDurable("BytesDurable", cc), remoteTag(remoteTag), isPrimary(isPrimary), logRouterTags(logRouterTags), txsTags(txsTags), recruitmentID(recruitmentID), protocolVersion(protocolVersion), logSpillType(logSpillType),
			  logSystem(new AsyncVar<Reference<ILogSystem>>()), logRouterPoppedVersion(0), durableKnownCommittedVersion(0), minKnownCommittedVersion(0), queuePoppedVersion(0), allTags(tags.begin(), tags.end()), terminated(tLogData->terminated.getFuture()),
			  minPoppedTagVersion(0), minPoppedTag(invalidTag),
			// These are initialized differently on init() or recovery
			recoveryCount(), stopped(false), initialized(false), queueCommittingVersion(0), newPersistentDataVersion(invalidVersion), unrecoveredBefore(1), recoveredAt(1), unpoppedRecoveredTags(0),
			logRouterPopToVersion(0), locality(tagLocalityInvalid), execOpCommitInProgress(false)
	{
		startRole(Role::TRANSACTION_LOG, interf.id(), tLogData->workerID, {{"SharedTLog", tLogData->dbgid.shortString()}}, context);
		addActor.send(traceRole(Role::TRANSACTION_LOG, interf.id()));

		persistentDataVersion.init(LiteralStringRef("TLog.PersistentDataVersion"), cc.id);
		persistentDataDurableVersion.init(LiteralStringRef("TLog.PersistentDataDurableVersion"), cc.id);
		version.initMetric(LiteralStringRef("TLog.Version"), cc.id);
		queueCommittedVersion.initMetric(LiteralStringRef("TLog.QueueCommittedVersion"), cc.id);

		specialCounter(cc, "Version", [this](){ return this->version.get(); });
		specialCounter(cc, "QueueCommittedVersion", [this](){ return this->queueCommittedVersion.get(); });
		specialCounter(cc, "PersistentDataVersion", [this](){ return this->persistentDataVersion; });
		specialCounter(cc, "PersistentDataDurableVersion", [this](){ return this->persistentDataDurableVersion; });
		specialCounter(cc, "KnownCommittedVersion", [this](){ return this->knownCommittedVersion; });
		specialCounter(cc, "QueuePoppedVersion", [this](){ return this->queuePoppedVersion; });
		specialCounter(cc, "MinPoppedTagVersion", [this](){ return this->minPoppedTagVersion; });
		specialCounter(cc, "MinPoppedTagLocality", [this](){ return this->minPoppedTag.locality; });
		specialCounter(cc, "MinPoppedTagId", [this](){ return this->minPoppedTag.id; });
		specialCounter(cc, "SharedBytesInput", [tLogData](){ return tLogData->bytesInput; });
		specialCounter(cc, "SharedBytesDurable", [tLogData](){ return tLogData->bytesDurable; });
		specialCounter(cc, "SharedOverheadBytesInput", [tLogData](){ return tLogData->overheadBytesInput; });
		specialCounter(cc, "SharedOverheadBytesDurable", [tLogData](){ return tLogData->overheadBytesDurable; });
		specialCounter(cc, "KvstoreBytesUsed", [tLogData](){ return tLogData->persistentData->getStorageBytes().used; });
		specialCounter(cc, "KvstoreBytesFree", [tLogData](){ return tLogData->persistentData->getStorageBytes().free; });
		specialCounter(cc, "KvstoreBytesAvailable", [tLogData](){ return tLogData->persistentData->getStorageBytes().available; });
		specialCounter(cc, "KvstoreBytesTotal", [tLogData](){ return tLogData->persistentData->getStorageBytes().total; });
		specialCounter(cc, "QueueDiskBytesUsed", [tLogData](){ return tLogData->rawPersistentQueue->getStorageBytes().used; });
		specialCounter(cc, "QueueDiskBytesFree", [tLogData](){ return tLogData->rawPersistentQueue->getStorageBytes().free; });
		specialCounter(cc, "QueueDiskBytesAvailable", [tLogData](){ return tLogData->rawPersistentQueue->getStorageBytes().available; });
		specialCounter(cc, "QueueDiskBytesTotal", [tLogData](){ return tLogData->rawPersistentQueue->getStorageBytes().total; });
		specialCounter(cc, "PeekMemoryReserved", [tLogData]() { return tLogData->peekMemoryLimiter.activePermits(); });
		specialCounter(cc, "PeekMemoryRequestsStalled", [tLogData]() { return tLogData->peekMemoryLimiter.waiters(); });
	}

	~LogData() {
		endRole(Role::TRANSACTION_LOG, logId, "Error", true);

		if(!terminated.isReady()) {
			tLogData->bytesDurable += bytesInput.getValue() - bytesDurable.getValue();
			TraceEvent("TLogBytesWhenRemoved", logId).detail("SharedBytesInput", tLogData->bytesInput).detail("SharedBytesDurable", tLogData->bytesDurable).detail("LocalBytesInput", bytesInput.getValue()).detail("LocalBytesDurable", bytesDurable.getValue());

			ASSERT_ABORT(tLogData->bytesDurable <= tLogData->bytesInput);

			Key logIdKey = BinaryWriter::toValue(logId,Unversioned());
			tLogData->persistentData->clear( singleKeyRange(logIdKey.withPrefix(persistCurrentVersionKeys.begin)) );
			tLogData->persistentData->clear( singleKeyRange(logIdKey.withPrefix(persistKnownCommittedVersionKeys.begin)) );
			tLogData->persistentData->clear( singleKeyRange(logIdKey.withPrefix(persistLocalityKeys.begin)) );
			tLogData->persistentData->clear( singleKeyRange(logIdKey.withPrefix(persistLogRouterTagsKeys.begin)) );
			tLogData->persistentData->clear( singleKeyRange(logIdKey.withPrefix(persistTxsTagsKeys.begin)) );
			tLogData->persistentData->clear( singleKeyRange(logIdKey.withPrefix(persistRecoveryCountKeys.begin)) );
			tLogData->persistentData->clear( singleKeyRange(logIdKey.withPrefix(persistProtocolVersionKeys.begin)) );
			tLogData->persistentData->clear( singleKeyRange(logIdKey.withPrefix(persistTLogSpillTypeKeys.begin)) );
			tLogData->persistentData->clear( singleKeyRange(logIdKey.withPrefix(persistRecoveryLocationKey)) );
			Key msgKey = logIdKey.withPrefix(persistTagMessagesKeys.begin);
			tLogData->persistentData->clear( KeyRangeRef( msgKey, strinc(msgKey) ) );
			Key msgRefKey = logIdKey.withPrefix(persistTagMessageRefsKeys.begin);
			tLogData->persistentData->clear( KeyRangeRef( msgRefKey, strinc(msgRefKey) ) );
			Key poppedKey = logIdKey.withPrefix(persistTagPoppedKeys.begin);
			tLogData->persistentData->clear( KeyRangeRef( poppedKey, strinc(poppedKey) ) );
		}

		for ( auto it = peekTracker.begin(); it != peekTracker.end(); ++it ) {
			for(auto seq : it->second.sequence_version) {
				if(!seq.second.isSet()) {
					seq.second.sendError(timed_out());
				}
			}
		}
	}

	LogEpoch epoch() const { return recoveryCount; }

	bool shouldSpillByValue( Tag t ) const {
		switch (logSpillType) {
		case TLogSpillType::VALUE:
			return true;
		case TLogSpillType::REFERENCE:
			return t.locality == tagLocalityTxs || t == txsTag;
		default:
			ASSERT(false);
			return false;
		}
	}

	bool shouldSpillByReference( Tag t ) const {
		return !shouldSpillByValue( t );
	}
};

template <class T>
void TLogQueue::push( T const& qe, Reference<LogData> logData ) {
	BinaryWriter wr( Unversioned() );  // outer framing is not versioned
	wr << uint32_t(0);
	IncludeVersion(ProtocolVersion::withTLogQueueEntryRef()).write(wr);  // payload is versioned
	wr << qe;
	wr << uint8_t(1);
	*(uint32_t*)wr.getData() = wr.getLength() - sizeof(uint32_t) - sizeof(uint8_t);
	const IDiskQueue::location startloc = queue->getNextPushLocation();
	// FIXME: push shouldn't return anything.  We should call getNextPushLocation() again.
	const IDiskQueue::location endloc = queue->push( wr.toValue() );
	//TraceEvent("TLogQueueVersionWritten", dbgid).detail("Size", wr.getLength() - sizeof(uint32_t) - sizeof(uint8_t)).detail("Loc", loc);
	logData->versionLocation[qe.version] = std::make_pair(startloc, endloc);
}

void TLogQueue::forgetBefore( Version upToVersion, Reference<LogData> logData ) {
	// Keep only the given and all subsequent version numbers
	// Find the first version >= upTo
	auto v = logData->versionLocation.lower_bound(upToVersion);
	if (v == logData->versionLocation.begin()) return;

	if(v == logData->versionLocation.end()) {
		v = logData->versionLocation.lastItem();
	}
	else {
		v.decrementNonEnd();
	}

	logData->versionLocation.erase( logData->versionLocation.begin(), v );  // ... and then we erase that previous version and all prior versions
}

void TLogQueue::pop( IDiskQueue::location upToLocation ) {
	queue->pop( upToLocation );
}

void TLogQueue::updateVersionSizes( const TLogQueueEntry& result, TLogData* tLog,
                                    IDiskQueue::location start, IDiskQueue::location end) {
	auto it = tLog->id_data.find(result.id);
	if(it != tLog->id_data.end()) {
		it->second->versionLocation[result.version] = std::make_pair(start, end);
	}
}

ACTOR Future<Void> tLogLock( TLogData* self, ReplyPromise< TLogLockResult > reply, Reference<LogData> logData ) {
	state Version stopVersion = logData->version.get();

	TEST(true); // TLog stopped by recovering master
	TEST( logData->stopped );
	TEST( !logData->stopped );

	TraceEvent("TLogStop", logData->logId).detail("Ver", stopVersion).detail("IsStopped", logData->stopped).detail("QueueCommitted", logData->queueCommittedVersion.get());

	logData->stopped = true;
	if(!logData->recoveryComplete.isSet()) {
		logData->recoveryComplete.sendError(end_of_stream());
	}

	// Lock once the current version has been committed
	wait( logData->queueCommittedVersion.whenAtLeast( stopVersion ) );

	ASSERT(stopVersion == logData->version.get());

	TLogLockResult result;
	result.end = stopVersion;
	result.knownCommittedVersion = logData->knownCommittedVersion;

	TraceEvent("TLogStop2", self->dbgid).detail("LogId", logData->logId).detail("Ver", stopVersion).detail("IsStopped", logData->stopped).detail("QueueCommitted", logData->queueCommittedVersion.get()).detail("KnownCommitted", result.knownCommittedVersion);

	reply.send( result );
	return Void();
}

void updatePersistentPopped( TLogData* self, Reference<LogData> logData, Reference<LogData::TagData> data ) {
	if (!data->poppedRecently) return;
	self->persistentData->set(KeyValueRef( persistTagPoppedKey(logData->logId, data->tag), persistTagPoppedValue(data->popped) ));
	data->poppedRecently = false;
	data->persistentPopped = data->popped;

	if (data->nothingPersistent) return;

	if (logData->shouldSpillByValue(data->tag)) {
		self->persistentData->clear( KeyRangeRef(
					persistTagMessagesKey( logData->logId, data->tag, Version(0) ),
					persistTagMessagesKey( logData->logId, data->tag, data->popped ) ) );
	} else {
		self->persistentData->clear( KeyRangeRef(
					persistTagMessageRefsKey( logData->logId, data->tag, Version(0) ),
					persistTagMessageRefsKey( logData->logId, data->tag, data->popped ) ) );
	}

	if (data->popped > logData->persistentDataVersion) {
		data->nothingPersistent = true;
	}
}

ACTOR Future<Void> updatePoppedLocation( TLogData* self, Reference<LogData> logData, Reference<LogData::TagData> data ) {
	// For anything spilled by value, we do not need to track its popped location.
	if (logData->shouldSpillByValue(data->tag)) {
		return Void();
	}

	if (data->versionForPoppedLocation >= data->persistentPopped) return Void();
	data->versionForPoppedLocation = data->persistentPopped;

	// Use persistentPopped and not popped, so that a pop update received after spilling doesn't cause
	// us to remove data that still is pointed to by SpilledData in the btree.
	if (data->persistentPopped <= logData->persistentDataVersion) {
		// Recover the next needed location in the Disk Queue from the index.
		Standalone<RangeResultRef> kvrefs = wait(
				self->persistentData->readRange(KeyRangeRef(
						persistTagMessageRefsKey(logData->logId, data->tag, data->persistentPopped),
						persistTagMessageRefsKey(logData->logId, data->tag, logData->persistentDataVersion + 1)), 1));

		if (kvrefs.empty()) {
			// Nothing was persistent after all.
			data->nothingPersistent = true;
		} else {
			VectorRef<SpilledData> spilledData;
			BinaryReader r(kvrefs[0].value, AssumeVersion(logData->protocolVersion));
			r >> spilledData;

			for (const SpilledData& sd : spilledData) {
				if (sd.version >= data->persistentPopped) {
					data->poppedLocation = sd.start;
					data->versionForPoppedLocation = sd.version;
					break;
				}
			}
		}
	}

	if (data->persistentPopped >= logData->persistentDataVersion || data->nothingPersistent) {
		// Then the location must be in memory.
		auto locationIter = logData->versionLocation.lower_bound(data->persistentPopped);
		if (locationIter != logData->versionLocation.end()) {
			data->poppedLocation = locationIter->value.first;
			data->versionForPoppedLocation = locationIter->key;
		} else {
			// No data on disk and no data in RAM.
			// This TLog instance will be removed soon anyway, so we temporarily freeze our poppedLocation
			// to avoid trying to track what the ending location of this TLog instance was.
		}
	}

	return Void();
}

ACTOR Future<Void> popDiskQueue( TLogData* self, Reference<LogData> logData ) {
	if (!logData->initialized) return Void();

	std::vector<Future<Void>> updates;
	for(int tagLocality = 0; tagLocality < logData->tag_data.size(); tagLocality++) {
		for(int tagId = 0; tagId < logData->tag_data[tagLocality].size(); tagId++) {
			Reference<LogData::TagData> tagData = logData->tag_data[tagLocality][tagId];
			if (tagData) {
				updates.push_back( updatePoppedLocation( self, logData, tagData ) );
			}
		}
	}
	wait(waitForAll(updates));

	IDiskQueue::location minLocation = 0;
	Version minVersion = 0;
	auto locationIter = logData->versionLocation.lower_bound(logData->persistentDataVersion);
	if (locationIter != logData->versionLocation.end()) {
		minLocation = locationIter->value.first;
		minVersion = locationIter->key;
	}
	logData->minPoppedTagVersion = std::numeric_limits<Version>::max();

	for(int tagLocality = 0; tagLocality < logData->tag_data.size(); tagLocality++) {
		for(int tagId = 0; tagId < logData->tag_data[tagLocality].size(); tagId++) {
			Reference<LogData::TagData> tagData = logData->tag_data[tagLocality][tagId];
			if (tagData && logData->shouldSpillByReference(tagData->tag)) {
				if(!tagData->nothingPersistent) {
					minLocation = std::min(minLocation, tagData->poppedLocation);
					minVersion = std::min(minVersion, tagData->popped);
				}
				if((!tagData->nothingPersistent || tagData->versionMessages.size()) && tagData->popped < logData->minPoppedTagVersion) {
					logData->minPoppedTagVersion = tagData->popped;
					logData->minPoppedTag = tagData->tag;
				}
			}
		}
	}

	if( self->queueCommitEnd.get() > 0 ) {
		Version lastCommittedVersion = logData->queueCommittedVersion.get();
		IDiskQueue::location lastCommittedLocation = minLocation;
		auto locationIter = logData->versionLocation.lower_bound(lastCommittedVersion);
		if (locationIter != logData->versionLocation.end()) {
			lastCommittedLocation = locationIter->value.first;
		}
		self->persistentQueue->pop( std::min(minLocation, lastCommittedLocation) );
		logData->queuePoppedVersion = std::max(logData->queuePoppedVersion, minVersion);
	}

	return Void();
}

ACTOR Future<Void> updatePersistentData( TLogData* self, Reference<LogData> logData, Version newPersistentDataVersion ) {
	state BinaryWriter wr( Unversioned() );
	// PERSIST: Changes self->persistentDataVersion and writes and commits the relevant changes
	ASSERT( newPersistentDataVersion <= logData->version.get() );
	ASSERT( newPersistentDataVersion <= logData->queueCommittedVersion.get() );
	ASSERT( newPersistentDataVersion > logData->persistentDataVersion );
	ASSERT( logData->persistentDataVersion == logData->persistentDataDurableVersion );
	logData->newPersistentDataVersion = newPersistentDataVersion;

	//TraceEvent("UpdatePersistentData", self->dbgid).detail("Seq", newPersistentDataSeq);

	state bool anyData = false;

	// For all existing tags
	state int tagLocality = 0;
	state int tagId = 0;

	for(tagLocality = 0; tagLocality < logData->tag_data.size(); tagLocality++) {
		for(tagId = 0; tagId < logData->tag_data[tagLocality].size(); tagId++) {
			state Reference<LogData::TagData> tagData = logData->tag_data[tagLocality][tagId];
			if(tagData) {
				wait(tagData->eraseMessagesBefore( tagData->popped, self, logData, TaskPriority::UpdateStorage ));
				state Version currentVersion = 0;
				// Clear recently popped versions from persistentData if necessary
				updatePersistentPopped( self, logData, tagData );
				state Version lastVersion = std::numeric_limits<Version>::min();
				state IDiskQueue::location firstLocation = std::numeric_limits<IDiskQueue::location>::max();
				// Transfer unpopped messages with version numbers less than newPersistentDataVersion to persistentData
				state std::deque<std::pair<Version, LengthPrefixedStringRef>>::iterator msg = tagData->versionMessages.begin();
				state int refSpilledTagCount = 0;
				wr = BinaryWriter( AssumeVersion(logData->protocolVersion) );
				// We prefix our spilled locations with a count, so that we can read this back out as a VectorRef.
				wr << uint32_t(0);
				while(msg != tagData->versionMessages.end() && msg->first <= newPersistentDataVersion) {
					currentVersion = msg->first;
					anyData = true;
					tagData->nothingPersistent = false;

					if (logData->shouldSpillByValue(tagData->tag)) {
						wr = BinaryWriter( Unversioned() );
						for(; msg != tagData->versionMessages.end() && msg->first == currentVersion; ++msg) {
							wr << msg->second.toStringRef();
						}
						self->persistentData->set( KeyValueRef( persistTagMessagesKey( logData->logId, tagData->tag, currentVersion ), wr.toValue() ) );
					} else {
						// spill everything else by reference
						const IDiskQueue::location begin = logData->versionLocation[currentVersion].first;
						const IDiskQueue::location end = logData->versionLocation[currentVersion].second;
						ASSERT(end > begin && end.lo - begin.lo < std::numeric_limits<uint32_t>::max());
						uint32_t length = static_cast<uint32_t>(end.lo - begin.lo);
						refSpilledTagCount++;

						uint32_t size = 0;
						for(; msg != tagData->versionMessages.end() && msg->first == currentVersion; ++msg) {
							// Fast forward until we find a new version.
							size += msg->second.expectedSize();
						}

						SpilledData spilledData( currentVersion, begin, length, size );
						wr << spilledData;

						lastVersion = std::max(currentVersion, lastVersion);
						firstLocation = std::min(begin, firstLocation);

						if ((wr.getLength() + sizeof(SpilledData) > SERVER_KNOBS->TLOG_SPILL_REFERENCE_MAX_BYTES_PER_BATCH) ) {
							*(uint32_t*)wr.getData() = refSpilledTagCount;
							self->persistentData->set( KeyValueRef( persistTagMessageRefsKey( logData->logId, tagData->tag, lastVersion ), wr.toValue() ) );
							tagData->poppedLocation = std::min(tagData->poppedLocation, firstLocation);
							refSpilledTagCount = 0;
							wr = BinaryWriter( AssumeVersion(logData->protocolVersion) );
							wr << uint32_t(0);
						}

						Future<Void> f = yield(TaskPriority::UpdateStorage);
						if(!f.isReady()) {
							wait(f);
							msg = std::upper_bound(tagData->versionMessages.begin(), tagData->versionMessages.end(), std::make_pair(currentVersion, LengthPrefixedStringRef()), CompareFirst<std::pair<Version, LengthPrefixedStringRef>>());
						}
					}
				}
				if (refSpilledTagCount > 0) {
					*(uint32_t*)wr.getData() = refSpilledTagCount;
					self->persistentData->set( KeyValueRef( persistTagMessageRefsKey( logData->logId, tagData->tag, lastVersion ), wr.toValue() ) );
					tagData->poppedLocation = std::min(tagData->poppedLocation, firstLocation);
				}

				wait(yield(TaskPriority::UpdateStorage));
			}
		}
	}

	auto locationIter = logData->versionLocation.lower_bound(newPersistentDataVersion);
	if (locationIter != logData->versionLocation.end()) {
		self->persistentData->set( KeyValueRef( persistRecoveryLocationKey, BinaryWriter::toValue(locationIter->value.first,Unversioned()) ) );
	}

	self->persistentData->set( KeyValueRef( BinaryWriter::toValue(logData->logId,Unversioned()).withPrefix(persistCurrentVersionKeys.begin), BinaryWriter::toValue(newPersistentDataVersion, Unversioned()) ) );
	self->persistentData->set( KeyValueRef( BinaryWriter::toValue(logData->logId,Unversioned()).withPrefix(persistKnownCommittedVersionKeys.begin), BinaryWriter::toValue(logData->knownCommittedVersion, Unversioned()) ) );
	logData->persistentDataVersion = newPersistentDataVersion;

	wait( self->persistentData->commit() ); // SOMEDAY: This seems to be running pretty often, should we slow it down???
	wait( delay(0, TaskPriority::UpdateStorage) );

	// Now that the changes we made to persistentData are durable, erase the data we moved from memory and the queue, increase bytesDurable accordingly, and update persistentDataDurableVersion.

	TEST(anyData);  // TLog moved data to persistentData
	logData->persistentDataDurableVersion = newPersistentDataVersion;

	for(tagLocality = 0; tagLocality < logData->tag_data.size(); tagLocality++) {
		for(tagId = 0; tagId < logData->tag_data[tagLocality].size(); tagId++) {
			if(logData->tag_data[tagLocality][tagId]) {
				wait(logData->tag_data[tagLocality][tagId]->eraseMessagesBefore( newPersistentDataVersion+1, self, logData, TaskPriority::UpdateStorage ));
				wait(yield(TaskPriority::UpdateStorage));
			}
		}
	}

	logData->version_sizes.erase(logData->version_sizes.begin(), logData->version_sizes.lower_bound(logData->persistentDataDurableVersion));

	wait(yield(TaskPriority::UpdateStorage));

	while(!logData->messageBlocks.empty() && logData->messageBlocks.front().first <= newPersistentDataVersion) {
		int64_t bytesErased = int64_t(logData->messageBlocks.front().second.size()) * SERVER_KNOBS->TLOG_MESSAGE_BLOCK_OVERHEAD_FACTOR;
		logData->bytesDurable += bytesErased;
		self->bytesDurable += bytesErased;
		logData->messageBlocks.pop_front();
		wait(yield(TaskPriority::UpdateStorage));
	}

	if(logData->bytesDurable.getValue() > logData->bytesInput.getValue() || self->bytesDurable > self->bytesInput) {
		TraceEvent(SevError, "BytesDurableTooLarge", logData->logId).detail("SharedBytesInput", self->bytesInput).detail("SharedBytesDurable", self->bytesDurable).detail("LocalBytesInput", logData->bytesInput.getValue()).detail("LocalBytesDurable", logData->bytesDurable.getValue());
	}

	ASSERT(logData->bytesDurable.getValue() <= logData->bytesInput.getValue());
	ASSERT(self->bytesDurable <= self->bytesInput);

	if( self->queueCommitEnd.get() > 0 ) {
		// FIXME: Maintain a heap of tags ordered by version to make this O(1) instead of O(n).
		Version minVersion = std::numeric_limits<Version>::max();
		for(tagLocality = 0; tagLocality < logData->tag_data.size(); tagLocality++) {
			for(tagId = 0; tagId < logData->tag_data[tagLocality].size(); tagId++) {
				Reference<LogData::TagData> tagData = logData->tag_data[tagLocality][tagId];
				if (tagData) {
					if (logData->shouldSpillByValue(tagData->tag)) {
						minVersion = std::min(minVersion, newPersistentDataVersion);
					} else {
						minVersion = std::min(minVersion, tagData->popped);
					}
				}
			}
		}
		if (minVersion != std::numeric_limits<Version>::max()) {
			self->persistentQueue->forgetBefore( newPersistentDataVersion, logData ); // SOMEDAY: this can cause a slow task (~0.5ms), presumably from erasing too many versions. Should we limit the number of versions cleared at a time?
		}
	}
	logData->newPersistentDataVersion = invalidVersion;

	return Void();
}

ACTOR Future<Void> tLogPopCore( TLogData* self, Tag inputTag, Version to, Reference<LogData> logData ) {
	if (self->ignorePopRequest) {
		TraceEvent(SevDebug, "IgnoringPopRequest").detail("IgnorePopDeadline", self->ignorePopDeadline);

		if (self->toBePopped.find(inputTag) == self->toBePopped.end()
			|| to > self->toBePopped[inputTag]) {
			self->toBePopped[inputTag] = to;
		}
		// add the pop to the toBePopped map
		TraceEvent(SevDebug, "IgnoringPopRequest")
			.detail("IgnorePopDeadline", self->ignorePopDeadline)
			.detail("Tag", inputTag.toString())
			.detail("Version", to);
		return Void();
	}
	state Version upTo = to;
	int8_t tagLocality = inputTag.locality;
	if (isPseudoLocality(tagLocality)) {
		if (logData->logSystem->get().isValid()) {
			upTo = logData->logSystem->get()->popPseudoLocalityTag(inputTag, to);
			tagLocality = tagLocalityLogRouter;
		} else {
			TraceEvent(SevWarn, "TLogPopNoLogSystem", self->dbgid).detail("Locality", tagLocality).detail("Version", upTo);
			return Void();
		}
	}
	state Tag tag(tagLocality, inputTag.id);
	auto tagData = logData->getTagData(tag);
	if (!tagData) {
		tagData = logData->createTagData(tag, upTo, true, true, false);
	} else if (upTo > tagData->popped) {
		tagData->popped = upTo;
		tagData->poppedRecently = true;

		if(tagData->unpoppedRecovered && upTo > logData->recoveredAt) {
			tagData->unpoppedRecovered = false;
			logData->unpoppedRecoveredTags--;
			TraceEvent("TLogPoppedTag", logData->logId).detail("Tags", logData->unpoppedRecoveredTags).detail("Tag", tag.toString()).detail("DurableKCVer", logData->durableKnownCommittedVersion).detail("RecoveredAt", logData->recoveredAt);
			if(logData->unpoppedRecoveredTags == 0 && logData->durableKnownCommittedVersion >= logData->recoveredAt && logData->recoveryComplete.canBeSet()) {
				logData->recoveryComplete.send(Void());
			}
		}

		if (upTo > logData->persistentDataDurableVersion)
			wait(tagData->eraseMessagesBefore(upTo, self, logData, TaskPriority::TLogPop));
		//TraceEvent("TLogPop", logData->logId).detail("Tag", tag.toString()).detail("To", upTo);
	}
	return Void();
}

ACTOR Future<Void> tLogPop( TLogData* self, TLogPopRequest req, Reference<LogData> logData ) {
	// timeout check for ignorePopRequest
	if (self->ignorePopRequest && (g_network->now() > self->ignorePopDeadline)) {

		TraceEvent("EnableTLogPlayAllIgnoredPops");
		// use toBePopped and issue all the pops
		std::map<Tag, Version>::iterator it;
		vector<Future<Void>> ignoredPops;
		self->ignorePopRequest = false;
		self->ignorePopUid = "";
		self->ignorePopDeadline = 0.0;
		for (it = self->toBePopped.begin(); it != self->toBePopped.end(); it++) {
			TraceEvent("PlayIgnoredPop")
				.detail("Tag", it->first.toString())
				.detail("Version", it->second);
			ignoredPops.push_back(tLogPopCore(self, it->first, it->second, logData));
		}
		self->toBePopped.clear();
		wait(waitForAll(ignoredPops));
		TraceEvent("ResetIgnorePopRequest")
		    .detail("Now", g_network->now())
		    .detail("IgnorePopRequest", self->ignorePopRequest)
		    .detail("IgnorePopDeadline", self->ignorePopDeadline);
	}
	wait(tLogPopCore(self, req.tag, req.to, logData));
	req.reply.send(Void());
	return Void();
}

// This function (and updatePersistentData, which is called by this function) run at a low priority and can soak up all CPU resources.
// For this reason, they employ aggressive use of yields to avoid causing slow tasks that could introduce latencies for more important
// work (e.g. commits).
ACTOR Future<Void> updateStorage( TLogData* self ) {
	while(self->spillOrder.size() && !self->id_data.count(self->spillOrder.front())) {
		self->spillOrder.pop_front();
	}

	if(!self->spillOrder.size()) {
		wait( delay(BUGGIFY ? SERVER_KNOBS->BUGGIFY_TLOG_STORAGE_MIN_UPDATE_INTERVAL : SERVER_KNOBS->TLOG_STORAGE_MIN_UPDATE_INTERVAL, TaskPriority::UpdateStorage) );
		return Void();
	}

	state Reference<LogData> logData = self->id_data[self->spillOrder.front()];
	state Version nextVersion = 0;
	state int totalSize = 0;

	state FlowLock::Releaser commitLockReleaser;

	//FIXME: This policy for calculating the cache pop version could end up popping recent data in the remote DC after two consecutive recoveries.
	// It also does not protect against spilling the cache tag directly, so it is theoretically possible to spill this tag; which is not intended to ever happen.
	Optional<Version> cachePopVersion;
	for(auto& it : self->id_data) {
		if(!it.second->stopped) {
			if(it.second->version.get() - it.second->unrecoveredBefore > SERVER_KNOBS->MAX_VERSIONS_IN_FLIGHT + SERVER_KNOBS->MAX_CACHE_VERSIONS) {
				cachePopVersion = it.second->version.get() - SERVER_KNOBS->MAX_CACHE_VERSIONS;
			}
			break;
		}
	}

	if(cachePopVersion.present()) {
		state std::vector<Future<Void>> cachePopFutures;
		for(auto& it : self->id_data) {
			cachePopFutures.push_back(tLogPop(self, TLogPopRequest(cachePopVersion.get(),0,cacheTag), it.second));
		}
		wait( waitForAll(cachePopFutures) );
	}

	if(logData->stopped) {
		if (self->bytesInput - self->bytesDurable >= self->targetVolatileBytes) {
			while(logData->persistentDataDurableVersion != logData->version.get()) {
				totalSize = 0;
				Map<Version, std::pair<int,int>>::iterator sizeItr = logData->version_sizes.begin();
				nextVersion = logData->version.get();
				while( totalSize < SERVER_KNOBS->REFERENCE_SPILL_UPDATE_STORAGE_BYTE_LIMIT &&
				       sizeItr != logData->version_sizes.end() )
				{
					totalSize += sizeItr->value.first + sizeItr->value.second;
					++sizeItr;
					nextVersion = sizeItr == logData->version_sizes.end() ? logData->version.get() : sizeItr->key;
				}

				wait( logData->queueCommittedVersion.whenAtLeast( nextVersion ) );
				wait( delay(0, TaskPriority::UpdateStorage) );

				//TraceEvent("TlogUpdatePersist", self->dbgid).detail("LogId", logData->logId).detail("NextVersion", nextVersion).detail("Version", logData->version.get()).detail("PersistentDataDurableVer", logData->persistentDataDurableVersion).detail("QueueCommitVer", logData->queueCommittedVersion.get()).detail("PersistDataVer", logData->persistentDataVersion);
				if (nextVersion > logData->persistentDataVersion) {
					wait( self->persistentDataCommitLock.take() );
					commitLockReleaser = FlowLock::Releaser(self->persistentDataCommitLock);
					wait( updatePersistentData(self, logData, nextVersion) );
					// Concurrently with this loop, the last stopped TLog could have been removed.
					if (self->popOrder.size()) {
						wait( popDiskQueue(self, self->id_data[self->popOrder.front()]) );
					}
					commitLockReleaser.release();
				} else {
					wait( delay(BUGGIFY ? SERVER_KNOBS->BUGGIFY_TLOG_STORAGE_MIN_UPDATE_INTERVAL : SERVER_KNOBS->TLOG_STORAGE_MIN_UPDATE_INTERVAL, TaskPriority::UpdateStorage) );
				}

				if( logData->removed.isReady() ) {
					break;
				}
			}

			if(logData->persistentDataDurableVersion == logData->version.get()) {
				self->spillOrder.pop_front();
			}
			wait( delay(0.0, TaskPriority::UpdateStorage) );
		} else {
			wait( delay(BUGGIFY ? SERVER_KNOBS->BUGGIFY_TLOG_STORAGE_MIN_UPDATE_INTERVAL : SERVER_KNOBS->TLOG_STORAGE_MIN_UPDATE_INTERVAL, TaskPriority::UpdateStorage) );
		}
	}
	else if(logData->initialized) {
		ASSERT(self->spillOrder.size() == 1);
		if(logData->version_sizes.empty()) {
			nextVersion = logData->version.get();
		} else {
			// Double check that a running TLog wasn't wrongly affected by spilling locked SharedTLogs.
			ASSERT_WE_THINK(self->targetVolatileBytes == SERVER_KNOBS->TLOG_SPILL_THRESHOLD);
			Map<Version, std::pair<int,int>>::iterator sizeItr = logData->version_sizes.begin();
			while( totalSize < SERVER_KNOBS->REFERENCE_SPILL_UPDATE_STORAGE_BYTE_LIMIT &&
			       sizeItr != logData->version_sizes.end()
					&& (logData->bytesInput.getValue() - logData->bytesDurable.getValue() - totalSize >= self->targetVolatileBytes || sizeItr->value.first == 0) )
			{
				totalSize += sizeItr->value.first + sizeItr->value.second;
				++sizeItr;
				nextVersion = sizeItr == logData->version_sizes.end() ? logData->version.get() : sizeItr->key;
			}
		}

		//TraceEvent("UpdateStorageVer", logData->logId).detail("NextVersion", nextVersion).detail("PersistentDataVersion", logData->persistentDataVersion).detail("TotalSize", totalSize);

		wait( logData->queueCommittedVersion.whenAtLeast( nextVersion ) );
		wait( delay(0, TaskPriority::UpdateStorage) );

		if (nextVersion > logData->persistentDataVersion) {
			wait( self->persistentDataCommitLock.take() );
			commitLockReleaser = FlowLock::Releaser(self->persistentDataCommitLock);
			wait( updatePersistentData(self, logData, nextVersion) );
			if (self->popOrder.size()) {
				wait( popDiskQueue(self, self->id_data[self->popOrder.front()]) );
			}
			commitLockReleaser.release();
		}

		if( totalSize < SERVER_KNOBS->REFERENCE_SPILL_UPDATE_STORAGE_BYTE_LIMIT ) {
			wait( delay(BUGGIFY ? SERVER_KNOBS->BUGGIFY_TLOG_STORAGE_MIN_UPDATE_INTERVAL : SERVER_KNOBS->TLOG_STORAGE_MIN_UPDATE_INTERVAL, TaskPriority::UpdateStorage) );
		}
		else {
			//recovery wants to commit to persistant data when updatePersistentData is not active, this delay ensures that immediately after
			//updatePersist returns another one has not been started yet.
			wait( delay(0.0, TaskPriority::UpdateStorage) );
		}
	} else {
		wait( delay(BUGGIFY ? SERVER_KNOBS->BUGGIFY_TLOG_STORAGE_MIN_UPDATE_INTERVAL : SERVER_KNOBS->TLOG_STORAGE_MIN_UPDATE_INTERVAL, TaskPriority::UpdateStorage) );
	}
	return Void();
}

ACTOR Future<Void> updateStorageLoop( TLogData* self ) {
	wait(delay(0, TaskPriority::UpdateStorage));

	loop {
		wait( updateStorage(self) );
	}
}

void commitMessages( TLogData* self, Reference<LogData> logData, Version version, const std::vector<TagsAndMessage>& taggedMessages ) {
	// SOMEDAY: This method of copying messages is reasonably memory efficient, but it's still a lot of bytes copied.  Find a
	// way to do the memory allocation right as we receive the messages in the network layer.

	int64_t addedBytes = 0;
	int64_t overheadBytes = 0;
	int expectedBytes = 0;
	int txsBytes = 0;

	if(!taggedMessages.size()) {
		return;
	}

	int msgSize = 0;
	for(auto& i : taggedMessages) {
		msgSize += i.message.size();
	}

	// Grab the last block in the blocks list so we can share its arena
	// We pop all of the elements of it to create a "fresh" vector that starts at the end of the previous vector
	Standalone<VectorRef<uint8_t>> block;
	if(logData->messageBlocks.empty()) {
		block = Standalone<VectorRef<uint8_t>>();
		block.reserve(block.arena(), std::max<int64_t>(SERVER_KNOBS->TLOG_MESSAGE_BLOCK_BYTES, msgSize));
	}
	else {
		block = logData->messageBlocks.back().second;
	}

	block.pop_front(block.size());

	for(auto& msg : taggedMessages) {
		if(msg.message.size() > block.capacity() - block.size()) {
			logData->messageBlocks.emplace_back(version, block);
			addedBytes += int64_t(block.size()) * SERVER_KNOBS->TLOG_MESSAGE_BLOCK_OVERHEAD_FACTOR;
			block = Standalone<VectorRef<uint8_t>>();
			block.reserve(block.arena(), std::max<int64_t>(SERVER_KNOBS->TLOG_MESSAGE_BLOCK_BYTES, msgSize));
		}

		DEBUG_TAGS_AND_MESSAGE("TLogCommitMessages", version, msg.getRawMessage()).detail("UID", self->dbgid).detail("LogId", logData->logId);
		block.append(block.arena(), msg.message.begin(), msg.message.size());
		for(auto tag : msg.tags) {
			if(logData->locality == tagLocalitySatellite) {
				if(!(tag.locality == tagLocalityTxs || tag.locality == tagLocalityLogRouter || tag == txsTag)) {
					continue;
				}
			} else if(!(logData->locality == tagLocalitySpecial || logData->locality == tag.locality || tag.locality < 0)) {
				continue;
			}

			if(tag.locality == tagLocalityLogRouter) {
				if(!logData->logRouterTags) {
					continue;
				}
				tag.id = tag.id % logData->logRouterTags;
			}
			if(tag.locality == tagLocalityTxs) {
				if (logData->txsTags > 0) {
					tag.id = tag.id % logData->txsTags;
				} else {
					tag = txsTag;
				}
			}
			Reference<LogData::TagData> tagData = logData->getTagData(tag);
			if(!tagData) {
				tagData = logData->createTagData(tag, 0, true, true, false);
			}

			if (version >= tagData->popped) {
				tagData->versionMessages.emplace_back(version, LengthPrefixedStringRef((uint32_t*)(block.end() - msg.message.size())));
				if(tagData->versionMessages.back().second.expectedSize() > SERVER_KNOBS->MAX_MESSAGE_SIZE) {
					TraceEvent(SevWarnAlways, "LargeMessage").detail("Size", tagData->versionMessages.back().second.expectedSize());
				}
				if (tag.locality != tagLocalityTxs && tag != txsTag) {
					expectedBytes += tagData->versionMessages.back().second.expectedSize();
				} else {
					txsBytes += tagData->versionMessages.back().second.expectedSize();
				}

				// The factor of VERSION_MESSAGES_OVERHEAD is intended to be an overestimate of the actual memory used to store this data in a std::deque.
				// In practice, this number is probably something like 528/512 ~= 1.03, but this could vary based on the implementation.
				// There will also be a fixed overhead per std::deque, but its size should be trivial relative to the size of the TLog
				// queue and can be thought of as increasing the capacity of the queue slightly.
				overheadBytes += SERVER_KNOBS->VERSION_MESSAGES_ENTRY_BYTES_WITH_OVERHEAD;
			}
		}

		msgSize -= msg.message.size();
	}
	logData->messageBlocks.emplace_back(version, block);
	addedBytes += int64_t(block.size()) * SERVER_KNOBS->TLOG_MESSAGE_BLOCK_OVERHEAD_FACTOR;
	addedBytes += overheadBytes;

	logData->version_sizes[version] = std::make_pair(expectedBytes, txsBytes);
	logData->bytesInput += addedBytes;
	self->bytesInput += addedBytes;
	self->overheadBytesInput += overheadBytes;

	//TraceEvent("TLogPushed", self->dbgid).detail("Bytes", addedBytes).detail("MessageBytes", messages.size()).detail("Tags", tags.size()).detail("ExpectedBytes", expectedBytes).detail("MCount", mCount).detail("TCount", tCount);
}

void commitMessages( TLogData *self, Reference<LogData> logData, Version version, Arena arena, StringRef messages ) {
	ArenaReader rd( arena, messages, Unversioned() );
	self->tempTagMessages.clear();
	while(!rd.empty()) {
		TagsAndMessage tagsAndMsg;
		tagsAndMsg.loadFromArena(&rd, nullptr);
		self->tempTagMessages.push_back(std::move(tagsAndMsg));
	}
	commitMessages(self, logData, version, self->tempTagMessages);
}

Version poppedVersion( Reference<LogData> self, Tag tag) {
	auto tagData = self->getTagData(tag);
	if (!tagData) {
		if (tag == txsTag || tag.locality == tagLocalityTxs) {
 			return 0;
 		}
		return self->recoveredAt;
	}
	return tagData->popped;
}

std::deque<std::pair<Version, LengthPrefixedStringRef>> & getVersionMessages( Reference<LogData> self, Tag tag ) {
	auto tagData = self->getTagData(tag);
	if (!tagData) {
		static std::deque<std::pair<Version, LengthPrefixedStringRef>> empty;
		return empty;
	}
	return tagData->versionMessages;
};

void peekMessagesFromMemory( Reference<LogData> self, TLogPeekRequest const& req, BinaryWriter& messages, Version& endVersion ) {
	ASSERT( !messages.getLength() );

	auto& deque = getVersionMessages(self, req.tag);
	//TraceEvent("TLogPeekMem", self->dbgid).detail("Tag", req.tag1).detail("PDS", self->persistentDataSequence).detail("PDDS", self->persistentDataDurableSequence).detail("Oldest", map1.empty() ? 0 : map1.begin()->key ).detail("OldestMsgCount", map1.empty() ? 0 : map1.begin()->value.size());

	Version begin = std::max( req.begin, self->persistentDataDurableVersion+1 );
	auto it = std::lower_bound(deque.begin(), deque.end(), std::make_pair(begin, LengthPrefixedStringRef()), CompareFirst<std::pair<Version, LengthPrefixedStringRef>>());

	Version currentVersion = -1;
	for(; it != deque.end(); ++it) {
		if(it->first != currentVersion) {
			if (messages.getLength() >= SERVER_KNOBS->DESIRED_TOTAL_BYTES) {
				endVersion = currentVersion + 1;
				//TraceEvent("TLogPeekMessagesReached2", self->dbgid);
				break;
			}

			currentVersion = it->first;
			messages << VERSION_HEADER << currentVersion;
		}

		// We need the 4 byte length prefix to be a TagsAndMessage format, but that prefix is added as part of StringRef serialization.
		int offset = messages.getLength();
		messages << it->second.toStringRef();
		void* data = messages.getData();
		DEBUG_TAGS_AND_MESSAGE("TLogPeek", currentVersion, StringRef((uint8_t*)data+offset, messages.getLength()-offset))
			.detail("LogId", self->logId).detail("PeekTag", req.tag);
	}
}

ACTOR Future<std::vector<StringRef>> parseMessagesForTag( StringRef commitBlob, Tag tag, int logRouters ) {
	// See the comment in LogSystem.cpp for the binary format of commitBlob.
	state std::vector<StringRef> relevantMessages;
	state BinaryReader rd(commitBlob, AssumeVersion(currentProtocolVersion));
	while (!rd.empty()) {
		TagsAndMessage tagsAndMessage;
		tagsAndMessage.loadFromArena(&rd, nullptr);
		for (Tag t : tagsAndMessage.tags) {
			if (t == tag || (tag.locality == tagLocalityLogRouter && t.locality == tagLocalityLogRouter &&
			                 t.id % logRouters == tag.id)) {
				// Mutations that are in the partially durable span between known comitted version and
				// recovery version get copied to the new log generation.  These commits might have had more
				// log router tags than what now exist, so we mod them down to what we have.
				relevantMessages.push_back(tagsAndMessage.getRawMessage());
				break;
			}
		}
		wait(yield());
	}
	return relevantMessages;
}

ACTOR Future<Void> tLogPeekMessages( TLogData* self, TLogPeekRequest req, Reference<LogData> logData ) {
	state BinaryWriter messages(Unversioned());
	state BinaryWriter messages2(Unversioned());
	state int sequence = -1;
	state UID peekId;
	state double queueStart = now();

	if(req.tag.locality == tagLocalityTxs && req.tag.id >= logData->txsTags && logData->txsTags > 0) {
		req.tag.id = req.tag.id % logData->txsTags;
	}
	
	if(req.sequence.present()) {
		try {
			peekId = req.sequence.get().first;
			sequence = req.sequence.get().second;
			if (sequence >= SERVER_KNOBS->PARALLEL_GET_MORE_REQUESTS && logData->peekTracker.find(peekId) == logData->peekTracker.end()) {
				throw operation_obsolete();
			}
			auto& trackerData = logData->peekTracker[peekId];
			if (sequence == 0 && trackerData.sequence_version.find(0) == trackerData.sequence_version.end()) {
				trackerData.tag = req.tag;
				trackerData.sequence_version[0].send(std::make_pair(req.begin, req.onlySpilled));
			}
			auto seqBegin = trackerData.sequence_version.begin();
			// The peek cursor and this comparison need to agree about the maximum number of in-flight requests.
			while(trackerData.sequence_version.size() && seqBegin->first <= sequence - SERVER_KNOBS->PARALLEL_GET_MORE_REQUESTS) {
				if(seqBegin->second.canBeSet()) {
					seqBegin->second.sendError(operation_obsolete());
				}
				trackerData.sequence_version.erase(seqBegin);
				seqBegin = trackerData.sequence_version.begin();
			}

			if(trackerData.sequence_version.size() && sequence < seqBegin->first) {
				throw operation_obsolete();
			}

			Future<std::pair<Version, bool>> fPrevPeekData = trackerData.sequence_version[sequence].getFuture();
			if(fPrevPeekData.isReady()) {
				trackerData.unblockedPeeks++;
				double t = now() - trackerData.lastUpdate;
				if(t > trackerData.idleMax) trackerData.idleMax = t;
				trackerData.idleTime += t;
			}
			trackerData.lastUpdate = now();
			std::pair<Version, bool> prevPeekData = wait(fPrevPeekData);
			req.begin = std::max(prevPeekData.first, req.begin);
			req.onlySpilled = prevPeekData.second;
			wait(yield());
		} catch( Error &e ) {
			if(e.code() == error_code_timed_out || e.code() == error_code_operation_obsolete) {
				req.reply.sendError(e);
				return Void();
			} else {
				throw;
			}
		}
	}

	state double blockStart = now();

	if( req.returnIfBlocked && logData->version.get() < req.begin ) {
		req.reply.sendError(end_of_stream());
		if(req.sequence.present()) {
			auto& trackerData = logData->peekTracker[peekId];
			auto& sequenceData = trackerData.sequence_version[sequence+1];
			trackerData.lastUpdate = now();
			if (!sequenceData.isSet()) {
				sequenceData.send(std::make_pair(req.begin, req.onlySpilled));
			}
		}
		return Void();
	}

	//TraceEvent("TLogPeekMessages0", self->dbgid).detail("ReqBeginEpoch", req.begin.epoch).detail("ReqBeginSeq", req.begin.sequence).detail("Epoch", self->epoch()).detail("PersistentDataSeq", self->persistentDataSequence).detail("Tag1", req.tag1).detail("Tag2", req.tag2);
	// Wait until we have something to return that the caller doesn't already have
	if( logData->version.get() < req.begin ) {
		wait( logData->version.whenAtLeast( req.begin ) );
		wait( delay(SERVER_KNOBS->TLOG_PEEK_DELAY, g_network->getCurrentTask()) );
	}

	if( logData->locality != tagLocalitySatellite && req.tag.locality == tagLocalityLogRouter ) {
		wait( self->concurrentLogRouterReads.take() );
		state FlowLock::Releaser globalReleaser(self->concurrentLogRouterReads);
		wait( delay(0.0, TaskPriority::Low) );
	}

	if( req.begin <= logData->persistentDataDurableVersion && req.tag.locality != tagLocalityTxs && req.tag != txsTag) {
		// Reading spilled data will almost always imply that the storage server is >5s behind the rest
		// of the cluster.  We shouldn't prioritize spending CPU on helping this server catch up
		// slightly faster over keeping the rest of the cluster operating normally.
		// txsTag is only ever peeked on recovery, and we would still wish to prioritize requests
		// that impact recovery duration.
		wait(delay(0, TaskPriority::TLogSpilledPeekReply));
	}

	state double workStart = now();

	Version poppedVer = poppedVersion(logData, req.tag);
	if(poppedVer > req.begin) {
		TLogPeekReply rep;
		rep.maxKnownVersion = logData->version.get();
		rep.minKnownCommittedVersion = logData->minKnownCommittedVersion;
		rep.popped = poppedVer;
		rep.end = poppedVer;
		rep.onlySpilled = false;

		if(req.sequence.present()) {
			auto& trackerData = logData->peekTracker[peekId];
			auto& sequenceData = trackerData.sequence_version[sequence+1];
			trackerData.lastUpdate = now();
			if(trackerData.sequence_version.size() && sequence+1 < trackerData.sequence_version.begin()->first) {
				req.reply.sendError(operation_obsolete());
				if (!sequenceData.isSet())
					sequenceData.sendError(operation_obsolete());
				return Void();
			}
			if(sequenceData.isSet()) {
				if(sequenceData.getFuture().get().first != rep.end) {
					TEST(true); //tlog peek second attempt ended at a different version
					req.reply.sendError(operation_obsolete());
					return Void();
				}
			} else {
				sequenceData.send(std::make_pair(rep.end, rep.onlySpilled));
			}
			rep.begin = req.begin;
		}

		req.reply.send( rep );
		return Void();
	}

	state Version endVersion = logData->version.get() + 1;
	state bool onlySpilled = false;

	//grab messages from disk
	//TraceEvent("TLogPeekMessages", self->dbgid).detail("ReqBeginEpoch", req.begin.epoch).detail("ReqBeginSeq", req.begin.sequence).detail("Epoch", self->epoch()).detail("PersistentDataSeq", self->persistentDataSequence).detail("Tag1", req.tag1).detail("Tag2", req.tag2);
	if( req.begin <= logData->persistentDataDurableVersion ) {
		// Just in case the durable version changes while we are waiting for the read, we grab this data from memory.  We may or may not actually send it depending on
		// whether we get enough data from disk.
		// SOMEDAY: Only do this if an initial attempt to read from disk results in insufficient data and the required data is no longer in memory
		// SOMEDAY: Should we only send part of the messages we collected, to actually limit the size of the result?

		if (req.onlySpilled) {
			endVersion = logData->persistentDataDurableVersion + 1;
		} else {
			peekMessagesFromMemory( logData, req, messages2, endVersion );
		}

		if ( logData->shouldSpillByValue(req.tag) ) {
			Standalone<RangeResultRef> kvs = wait(
					self->persistentData->readRange(KeyRangeRef(
							persistTagMessagesKey(logData->logId, req.tag, req.begin),
							persistTagMessagesKey(logData->logId, req.tag, logData->persistentDataDurableVersion + 1)), SERVER_KNOBS->DESIRED_TOTAL_BYTES, SERVER_KNOBS->DESIRED_TOTAL_BYTES));

			for (auto &kv : kvs) {
				auto ver = decodeTagMessagesKey(kv.key);
				messages << VERSION_HEADER << ver;
				messages.serializeBytes(kv.value);
			}

			if (kvs.expectedSize() >= SERVER_KNOBS->DESIRED_TOTAL_BYTES) {
				endVersion = decodeTagMessagesKey(kvs.end()[-1].key) + 1;
				onlySpilled = true;
			} else {
				messages.serializeBytes( messages2.toValue() );
			}
		} else {
			// FIXME: Limit to approximately DESIRED_TOTATL_BYTES somehow.
			Standalone<RangeResultRef> kvrefs = wait(
					self->persistentData->readRange(KeyRangeRef(
							persistTagMessageRefsKey(logData->logId, req.tag, req.begin),
							persistTagMessageRefsKey(logData->logId, req.tag, logData->persistentDataDurableVersion + 1)),
						  SERVER_KNOBS->TLOG_SPILL_REFERENCE_MAX_BATCHES_PER_PEEK+1));

			//TraceEvent("TLogPeekResults", self->dbgid).detail("ForAddress", req.reply.getEndpoint().getPrimaryAddress()).detail("Tag1Results", s1).detail("Tag2Results", s2).detail("Tag1ResultsLim", kv1.size()).detail("Tag2ResultsLim", kv2.size()).detail("Tag1ResultsLast", kv1.size() ? kv1[0].key : "").detail("Tag2ResultsLast", kv2.size() ? kv2[0].key : "").detail("Limited", limited).detail("NextEpoch", next_pos.epoch).detail("NextSeq", next_pos.sequence).detail("NowEpoch", self->epoch()).detail("NowSeq", self->sequence.getNextSequence());

			state std::vector<std::pair<IDiskQueue::location, IDiskQueue::location>> commitLocations;
			state bool earlyEnd = false;
			uint32_t mutationBytes = 0;
			state uint64_t commitBytes = 0;
			state Version firstVersion = std::numeric_limits<Version>::max();
			for (int i = 0; i < kvrefs.size() && i < SERVER_KNOBS->TLOG_SPILL_REFERENCE_MAX_BATCHES_PER_PEEK; i++) {
				auto& kv = kvrefs[i];
				VectorRef<SpilledData> spilledData;
				BinaryReader r(kv.value, AssumeVersion(logData->protocolVersion));
				r >> spilledData;
				for (const SpilledData& sd : spilledData) {
					if (mutationBytes >= SERVER_KNOBS->DESIRED_TOTAL_BYTES) {
						earlyEnd = true;
						break;
					}
					if (sd.version >= req.begin) {
						firstVersion = std::min(firstVersion, sd.version);
						const IDiskQueue::location end = sd.start.lo + sd.length;
						commitLocations.emplace_back(sd.start, end);
						// This isn't perfect, because we aren't accounting for page boundaries, but should be
						// close enough.
						commitBytes += sd.length;
						mutationBytes += sd.mutationBytes;
					}
				}
				if (earlyEnd) break;
			}
			earlyEnd = earlyEnd || (kvrefs.size() >= SERVER_KNOBS->TLOG_SPILL_REFERENCE_MAX_BATCHES_PER_PEEK+1);
			wait( self->peekMemoryLimiter.take(TaskPriority::TLogSpilledPeekReply, commitBytes) );
			state FlowLock::Releaser memoryReservation(self->peekMemoryLimiter, commitBytes);
			state std::vector<Future<Standalone<StringRef>>> messageReads;
			messageReads.reserve( commitLocations.size() );
			for (const auto& pair : commitLocations) {
				messageReads.push_back( self->rawPersistentQueue->read(pair.first, pair.second, CheckHashes::YES ) );
			}
			commitLocations.clear();
			wait( waitForAll( messageReads ) );

			state Version lastRefMessageVersion = 0;
			state int index = 0;
			loop {
				if (index >= messageReads.size()) break;
				Standalone<StringRef> queueEntryData = messageReads[index].get();
				uint8_t valid;
				const uint32_t length = *(uint32_t*)queueEntryData.begin();
				queueEntryData = queueEntryData.substr( 4, queueEntryData.size() - 4);
				BinaryReader rd( queueEntryData, IncludeVersion() );
				state TLogQueueEntry entry;
				rd >> entry >> valid;
				ASSERT( valid == 0x01 );
				ASSERT( length + sizeof(valid) == queueEntryData.size() );

				messages << VERSION_HEADER << entry.version;

				std::vector<StringRef> rawMessages =
				    wait(parseMessagesForTag(entry.messages, req.tag, logData->logRouterTags));
				for (const StringRef& msg : rawMessages) {
					messages.serializeBytes(msg);
					DEBUG_TAGS_AND_MESSAGE("TLogPeekFromDisk", entry.version, msg).detail("UID", self->dbgid).detail("LogId", logData->logId).detail("PeekTag", req.tag);
				}

				lastRefMessageVersion = entry.version;
				index++;
			}

			messageReads.clear();
			memoryReservation.release();

			if (earlyEnd) {
				endVersion = lastRefMessageVersion + 1;
				onlySpilled = true;
			} else {
				messages.serializeBytes( messages2.toValue() );
			}
		}
	} else {
		if (req.onlySpilled) {
			endVersion = logData->persistentDataDurableVersion + 1;
		} else {
			peekMessagesFromMemory( logData, req, messages, endVersion );
		}

		//TraceEvent("TLogPeekResults", self->dbgid).detail("ForAddress", req.reply.getEndpoint().getPrimaryAddress()).detail("MessageBytes", messages.getLength()).detail("NextEpoch", next_pos.epoch).detail("NextSeq", next_pos.sequence).detail("NowSeq", self->sequence.getNextSequence());
	}

	TLogPeekReply reply;
	reply.maxKnownVersion = logData->version.get();
	reply.minKnownCommittedVersion = logData->minKnownCommittedVersion;
	reply.messages = messages.toValue();
	reply.end = endVersion;
	reply.onlySpilled = onlySpilled;

	//TraceEvent("TlogPeek", self->dbgid).detail("LogId", logData->logId).detail("Tag", req.tag.toString()).
	//	detail("BeginVer", req.begin).detail("EndVer", reply.end).
	//	detail("MsgBytes", reply.messages.expectedSize()).
	//	detail("ForAddress", req.reply.getEndpoint().getPrimaryAddress());

	if(req.sequence.present()) {
		auto& trackerData = logData->peekTracker[peekId];
		trackerData.lastUpdate = now();

		double queueT = blockStart-queueStart;
		double blockT = workStart-blockStart;
		double workT = now()-workStart;

		trackerData.totalPeeks++;
		trackerData.replyBytes += reply.messages.size();

		if(queueT > trackerData.queueMax) trackerData.queueMax = queueT;
		if(blockT > trackerData.blockMax) trackerData.blockMax = blockT;
		if(workT > trackerData.workMax) trackerData.workMax = workT;

		trackerData.queueTime += queueT;
		trackerData.blockTime += blockT;
		trackerData.workTime += workT;

		auto& sequenceData = trackerData.sequence_version[sequence+1];
		if(trackerData.sequence_version.size() && sequence+1 < trackerData.sequence_version.begin()->first) {
			req.reply.sendError(operation_obsolete());
			if(!sequenceData.isSet()) {
				// It would technically be more correct to .send({req.begin, req.onlySpilled}), as the next
				// request might still be in the window of active requests, but LogSystemPeekCursor will
				// throw away all future responses upon getting an operation_obsolete(), so computing a
				// response will probably be a waste of CPU.
				sequenceData.sendError(operation_obsolete());
			}
			return Void();
		}
		if(sequenceData.isSet()) {
			trackerData.duplicatePeeks++;
			if(sequenceData.getFuture().get().first != reply.end) {
				TEST(true); //tlog peek second attempt ended at a different version
				req.reply.sendError(operation_obsolete());
				return Void();
			}
		} else {
			sequenceData.send(std::make_pair(reply.end, reply.onlySpilled));
		}
		reply.begin = req.begin;
	}

	req.reply.send( reply );
	return Void();
}

ACTOR Future<Void> watchDegraded(TLogData* self) {
	if(g_network->isSimulated() && g_simulator.speedUpSimulation) {
		return Void();
	}
	
	wait(lowPriorityDelay(SERVER_KNOBS->TLOG_DEGRADED_DURATION));
	
	TraceEvent(SevWarnAlways, "TLogDegraded", self->dbgid);
	TEST(true); //TLog degraded
	self->degraded->set(true);
	return Void();
}

ACTOR Future<Void> doQueueCommit( TLogData* self, Reference<LogData> logData, std::vector<Reference<LogData>> missingFinalCommit ) {
	state Version ver = logData->version.get();
	state Version commitNumber = self->queueCommitBegin+1;
	state Version knownCommittedVersion = logData->knownCommittedVersion;
	self->queueCommitBegin = commitNumber;
	logData->queueCommittingVersion = ver;

	g_network->setCurrentTask(TaskPriority::TLogCommitReply);
	Future<Void> c = self->persistentQueue->commit();
	self->diskQueueCommitBytes = 0;
	self->largeDiskQueueCommitBytes.set(false);

	state Future<Void> degraded = watchDegraded(self);
	wait(c);
	if(g_network->isSimulated() && !g_simulator.speedUpSimulation && BUGGIFY_WITH_PROB(0.0001)) {
		wait(delay(6.0));
	}
	degraded.cancel();
	wait(self->queueCommitEnd.whenAtLeast(commitNumber-1));

	//Calling check_yield instead of yield to avoid a destruction ordering problem in simulation
	if(g_network->check_yield(g_network->getCurrentTask())) {
		wait(delay(0, g_network->getCurrentTask()));
	}

	ASSERT( ver > logData->queueCommittedVersion.get() );

	logData->durableKnownCommittedVersion = knownCommittedVersion;
	if(logData->unpoppedRecoveredTags == 0 && knownCommittedVersion >= logData->recoveredAt && logData->recoveryComplete.canBeSet()) {
		TraceEvent("TLogRecoveryComplete", logData->logId).detail("Tags", logData->unpoppedRecoveredTags).detail("DurableKCVer", logData->durableKnownCommittedVersion).detail("RecoveredAt", logData->recoveredAt);
		logData->recoveryComplete.send(Void());
	}

	//TraceEvent("TLogCommitDurable", self->dbgid).detail("Version", ver);
	if(logData->logSystem->get() && (!logData->isPrimary || logData->logRouterPoppedVersion < logData->logRouterPopToVersion)) {
		logData->logRouterPoppedVersion = ver;
		logData->logSystem->get()->pop(ver, logData->remoteTag, knownCommittedVersion, logData->locality);
	}

	logData->queueCommittedVersion.set(ver);
	self->queueCommitEnd.set(commitNumber);

	for(auto& it : missingFinalCommit) {
		TraceEvent("TLogCommitMissingFinalCommit", self->dbgid).detail("LogId", logData->logId).detail("Version", it->version.get()).detail("QueueVer", it->queueCommittedVersion.get());
		TEST(true); //A TLog was replaced before having a chance to commit its queue
		it->queueCommittedVersion.set(it->version.get());
	}
	return Void();
}

ACTOR Future<Void> commitQueue( TLogData* self ) {
	state Reference<LogData> logData;

	loop {
		int foundCount = 0;
		state std::vector<Reference<LogData>> missingFinalCommit;
		for(auto it : self->id_data) {
			if(!it.second->stopped) {
				 logData = it.second;
				 foundCount++;
			} else if(it.second->version.get() > std::max(it.second->queueCommittingVersion, it.second->queueCommittedVersion.get())) {
				missingFinalCommit.push_back(it.second);
			}
		}

		ASSERT(foundCount < 2);
		if(!foundCount) {
			wait( self->newLogData.onTrigger() );
			continue;
		}

		TraceEvent("CommitQueueNewLog", self->dbgid).detail("LogId", logData->logId).detail("Version", logData->version.get()).detail("Committing", logData->queueCommittingVersion).detail("Commmitted", logData->queueCommittedVersion.get());
		if(logData->committingQueue.canBeSet()) {
			logData->committingQueue.send(Void());
		}

		loop {
			if(logData->stopped && logData->version.get() == std::max(logData->queueCommittingVersion, logData->queueCommittedVersion.get())) {
				wait( logData->queueCommittedVersion.whenAtLeast(logData->version.get() ) );
				break;
			}

			choose {
				when(wait( logData->version.whenAtLeast( std::max(logData->queueCommittingVersion, logData->queueCommittedVersion.get()) + 1 ) ) ) {
					while( self->queueCommitBegin != self->queueCommitEnd.get() && !self->largeDiskQueueCommitBytes.get() ) {
						wait( self->queueCommitEnd.whenAtLeast(self->queueCommitBegin) || self->largeDiskQueueCommitBytes.onChange() );
					}
					self->sharedActors.send(doQueueCommit(self, logData, missingFinalCommit));
					missingFinalCommit.clear();
				}
				when(wait(self->newLogData.onTrigger())) {}
			}
		}
	}
}

void sendCommitResponse(TLogData* self, TLogCommitRequest& req, const Version& response) {
	if (!req.splitTransaction.present()) {
		req.reply.send(response);
	} else {
		const UID& splitID = req.splitTransaction.get().id;
		self->splitTransactionResponse.get(splitID).send(response);
	}
}

void sendCommitResponse(TLogData* self, TLogCommitRequest& req, const Error& error) {
	if (!req.splitTransaction.present()) {
		req.reply.sendError(error);
	} else {
		const UID& splitID = req.splitTransaction.get().id;
		self->splitTransactionResponse.get(splitID).sendError(error);
	}
}

ACTOR Future<Void> tLogCommit(
		TLogData* self,
		TLogCommitRequest req,
		Reference<LogData> logData,
		PromiseStream<Void> warningCollectorInput ) {
	state Optional<UID> tlogDebugID;
	if(req.debugID.present()) {
		tlogDebugID = nondeterministicRandom()->randomUniqueID();
		g_traceBatch.addAttach("CommitAttachID", req.debugID.get().first(), tlogDebugID.get().first());
		g_traceBatch.addEvent("CommitDebug", tlogDebugID.get().first(), "TLog.tLogCommit.BeforeWaitForVersion");
	}

	logData->minKnownCommittedVersion = std::max(logData->minKnownCommittedVersion, req.minKnownCommittedVersion);

	wait( logData->version.whenAtLeast( req.prevVersion ) );

	//Calling check_yield instead of yield to avoid a destruction ordering problem in simulation
	if(g_network->check_yield(g_network->getCurrentTask())) {
		wait(delay(0, g_network->getCurrentTask()));
	}

	state double waitStartT = 0;
	while( self->bytesInput - self->bytesDurable >= SERVER_KNOBS->TLOG_HARD_LIMIT_BYTES && !logData->stopped ) {
		if (now() - waitStartT >= 1) {
			TraceEvent(SevWarn, "TLogUpdateLag", logData->logId)
				.detail("Version", logData->version.get())
				.detail("PersistentDataVersion", logData->persistentDataVersion)
				.detail("PersistentDataDurableVersion", logData->persistentDataDurableVersion);
			waitStartT = now();
		}
		wait( delayJittered(.005, TaskPriority::TLogCommit) );
	}

	if(logData->stopped) {
		sendCommitResponse(self, req, tlog_stopped());
		return Void();
	}

	if (logData->version.get() == req.prevVersion) {  // Not a duplicate (check relies on critical section between here self->version.set() below!)
		if(req.debugID.present())
			g_traceBatch.addEvent("CommitDebug", tlogDebugID.get().first(), "TLog.tLogCommit.Before");

		//TraceEvent("TLogCommit", logData->logId).detail("Version", req.version);
		commitMessages(self, logData, req.version, req.arena, req.messages);

		if (req.additionalMessages.present()) {
			const auto& additionalMessages = req.additionalMessages.get();
			for (size_t i = 0; i < additionalMessages.size(); ++i) {
				commitMessages(self, logData, req.version, additionalMessages[i].first, additionalMessages[i].second);
			}
		}

		logData->knownCommittedVersion = std::max(logData->knownCommittedVersion, req.knownCommittedVersion);

		TLogQueueEntryRef qe;
		// Log the changes to the persistent queue, to be committed by commitQueue()
		qe.version = req.version;
		qe.knownCommittedVersion = logData->knownCommittedVersion;
		qe.messages = req.messages;
		qe.id = logData->logId;
		self->persistentQueue->push( qe, logData );

		self->diskQueueCommitBytes += qe.expectedSize();
		if( self->diskQueueCommitBytes > SERVER_KNOBS->MAX_QUEUE_COMMIT_BYTES ) {
			self->largeDiskQueueCommitBytes.set(true);
		}

		// Notifies the commitQueue actor to commit persistentQueue, and also unblocks tLogPeekMessages actors
		logData->version.set( req.version );

		if(req.debugID.present())
			g_traceBatch.addEvent("CommitDebug", tlogDebugID.get().first(), "TLog.tLogCommit.AfterTLogCommit");
	}
	// Send replies only once all prior messages have been received and committed.
	state Future<Void> stopped = logData->stopCommit.onTrigger();
	wait( timeoutWarning( logData->queueCommittedVersion.whenAtLeast( req.version ) || stopped, 0.1, warningCollectorInput ) );

	if(stopped.isReady()) {
		ASSERT(logData->stopped);
		sendCommitResponse(self, req, tlog_stopped());
		return Void();
	}

	if(req.debugID.present())
		g_traceBatch.addEvent("CommitDebug", tlogDebugID.get().first(), "TLog.tLogCommit.After");

	sendCommitResponse(self, req, logData->durableKnownCommittedVersion);
	return Void();
}

ACTOR Future<Void> tLogCommitSplitTransactions(TLogData* self, TLogCommitRequest req, Reference<LogData> logData,
                                               PromiseStream<Void> warningCollectorInput) {

	state const SplitTransaction& splitTransaction = req.splitTransaction.get();
	state const UID splitID = splitTransaction.id;

	const int partIndex = splitTransaction.partIndex;
	const int totalParts = splitTransaction.totalParts;

	if (!self->splitTransactionResponse.exists(splitID)) {
		self->splitTransactionResponse.add(splitID, Promise<Version>());
	}

	if (self->splitTransactionMerger.insert(splitID, partIndex, totalParts, req)) {
		wait(tLogCommit(self, self->splitTransactionMerger.get(splitID), logData, warningCollectorInput));
	}

	try {
		state Version reply = wait(self->splitTransactionResponse.get(splitID).getFuture());
		req.reply.send(reply);
	} catch (Error& err) {
		req.reply.sendError(err);
	}

	return Void();
}

ACTOR Future<Void> initPersistentState( TLogData* self, Reference<LogData> logData ) {
	wait( self->persistentDataCommitLock.take() );
	state FlowLock::Releaser commitLockReleaser(self->persistentDataCommitLock);

	// PERSIST: Initial setup of persistentData for a brand new tLog for a new database
	state IKeyValueStore *storage = self->persistentData;
	wait( ioTimeoutError( storage->init(), SERVER_KNOBS->TLOG_MAX_CREATE_DURATION ) );
	storage->set( persistFormat );
	storage->set( KeyValueRef( BinaryWriter::toValue(logData->logId,Unversioned()).withPrefix(persistCurrentVersionKeys.begin), BinaryWriter::toValue(logData->version.get(), Unversioned()) ) );
	storage->set( KeyValueRef( BinaryWriter::toValue(logData->logId,Unversioned()).withPrefix(persistKnownCommittedVersionKeys.begin), BinaryWriter::toValue(logData->knownCommittedVersion, Unversioned()) ) );
	storage->set( KeyValueRef( BinaryWriter::toValue(logData->logId,Unversioned()).withPrefix(persistLocalityKeys.begin), BinaryWriter::toValue(logData->locality, Unversioned()) ) );
	storage->set( KeyValueRef( BinaryWriter::toValue(logData->logId,Unversioned()).withPrefix(persistLogRouterTagsKeys.begin), BinaryWriter::toValue(logData->logRouterTags, Unversioned()) ) );
	storage->set( KeyValueRef( BinaryWriter::toValue(logData->logId,Unversioned()).withPrefix(persistTxsTagsKeys.begin), BinaryWriter::toValue(logData->txsTags, Unversioned()) ) );
	storage->set( KeyValueRef( BinaryWriter::toValue(logData->logId,Unversioned()).withPrefix(persistRecoveryCountKeys.begin), BinaryWriter::toValue(logData->recoveryCount, Unversioned()) ) );
	storage->set( KeyValueRef( BinaryWriter::toValue(logData->logId,Unversioned()).withPrefix(persistProtocolVersionKeys.begin), BinaryWriter::toValue(logData->protocolVersion, Unversioned()) ) );
	storage->set( KeyValueRef( BinaryWriter::toValue(logData->logId,Unversioned()).withPrefix(persistTLogSpillTypeKeys.begin), BinaryWriter::toValue(logData->logSpillType, AssumeVersion(logData->protocolVersion)) ) );

	for(auto tag : logData->allTags) {
		ASSERT(!logData->getTagData(tag));
		logData->createTagData(tag, 0, true, true, true);
		updatePersistentPopped( self, logData, logData->getTagData(tag) );
	}

	TraceEvent("TLogInitCommit", logData->logId);
	wait( ioTimeoutError( self->persistentData->commit(), SERVER_KNOBS->TLOG_MAX_CREATE_DURATION ) );
	return Void();
}

ACTOR Future<Void> rejoinMasters( TLogData* self, TLogInterface tli, DBRecoveryCount recoveryCount, Future<Void> registerWithMaster, bool isPrimary ) {
	state UID lastMasterID(0,0);
	loop {
		auto const& inf = self->dbInfo->get();
		bool isDisplaced = !std::count( inf.priorCommittedLogServers.begin(), inf.priorCommittedLogServers.end(), tli.id() );
		if(isPrimary) {
			isDisplaced = isDisplaced && inf.recoveryCount >= recoveryCount && inf.recoveryState != RecoveryState::UNINITIALIZED;
		} else {
			isDisplaced = isDisplaced && ( ( inf.recoveryCount > recoveryCount && inf.recoveryState != RecoveryState::UNINITIALIZED ) || ( inf.recoveryCount == recoveryCount && inf.recoveryState == RecoveryState::FULLY_RECOVERED ) );
		}
		isDisplaced = isDisplaced && !inf.logSystemConfig.hasTLog(tli.id());
		if (isDisplaced) {
			TraceEvent("TLogDisplaced", tli.id())
			    .detail("Reason", "DBInfoDoesNotContain")
			    .detail("RecoveryCount", recoveryCount)
			    .detail("InfRecoveryCount", inf.recoveryCount)
			    .detail("RecoveryState", (int)inf.recoveryState)
			    .detail("LogSysConf", describe(inf.logSystemConfig.tLogs))
			    .detail("PriorLogs", describe(inf.priorCommittedLogServers))
			    .detail("OldLogGens", inf.logSystemConfig.oldTLogs.size());
			if (BUGGIFY) wait( delay( SERVER_KNOBS->BUGGIFY_WORKER_REMOVED_MAX_LAG * deterministicRandom()->random01() ) );
			throw worker_removed();
		}

		if( registerWithMaster.isReady() ) {
			if ( self->dbInfo->get().master.id() != lastMasterID) {
				// The TLogRejoinRequest is needed to establish communications with a new master, which doesn't have our TLogInterface
				TLogRejoinRequest req(tli);
				TraceEvent("TLogRejoining", tli.id()).detail("Master", self->dbInfo->get().master.id());
				choose {
					when(TLogRejoinReply rep =
					         wait(brokenPromiseToNever(self->dbInfo->get().master.tlogRejoin.getReply(req)))) {
						if (rep.masterIsRecovered) lastMasterID = self->dbInfo->get().master.id();
					}
					when ( wait( self->dbInfo->onChange() ) ) { }
				}
			} else {
				wait( self->dbInfo->onChange() );
			}
		} else {
			wait( registerWithMaster || self->dbInfo->onChange() );
		}
	}
}

ACTOR Future<Void> respondToRecovered( TLogInterface tli, Promise<Void> recoveryComplete ) {
	state bool finishedRecovery = true;
	try {
		wait( recoveryComplete.getFuture() );
	} catch( Error &e ) {
		if(e.code() != error_code_end_of_stream) {
			throw;
		}
		finishedRecovery = false;
	}
	TraceEvent("TLogRespondToRecovered", tli.id()).detail("Finished", finishedRecovery);
	loop {
		TLogRecoveryFinishedRequest req = waitNext( tli.recoveryFinished.getFuture() );
		if(finishedRecovery) {
			req.reply.send(Void());
		} else {
			req.reply.send(Never());
		}
	}
}

ACTOR Future<Void> cleanupPeekTrackers( LogData* logData ) {
	loop {
		double minTimeUntilExpiration = SERVER_KNOBS->PEEK_TRACKER_EXPIRATION_TIME;
		auto it = logData->peekTracker.begin();
		while(it != logData->peekTracker.end()) {
			double timeUntilExpiration = it->second.lastUpdate + SERVER_KNOBS->PEEK_TRACKER_EXPIRATION_TIME - now();
			if(timeUntilExpiration < 1.0e-6) {
				for(auto seq : it->second.sequence_version) {
					if(!seq.second.isSet()) {
						seq.second.sendError(timed_out());
					}
				}
				it = logData->peekTracker.erase(it);
			} else {
				minTimeUntilExpiration = std::min(minTimeUntilExpiration, timeUntilExpiration);
				++it;
			}
		}

		wait( delay(minTimeUntilExpiration) );
	}
}

ACTOR Future<Void> logPeekTrackers( LogData* logData ) {
	loop {
		int64_t logThreshold = 1;
		if(logData->peekTracker.size() > SERVER_KNOBS->PEEK_LOGGING_AMOUNT) {
			std::vector<int64_t> peekCounts;
			peekCounts.reserve(logData->peekTracker.size());
			for( auto& it : logData->peekTracker ) {
				peekCounts.push_back(it.second.totalPeeks);
			}
			size_t pivot = peekCounts.size()-SERVER_KNOBS->PEEK_LOGGING_AMOUNT;
			std::nth_element(peekCounts.begin(), peekCounts.begin()+pivot, peekCounts.end());
			logThreshold = std::max<int64_t>(1,peekCounts[pivot]);
		}
		int logCount = 0;
		for( auto& it : logData->peekTracker ) {
			if(it.second.totalPeeks >= logThreshold) {
				logCount++;
				TraceEvent("PeekMetrics", logData->logId)
					.detail("Tag", it.second.tag.toString())
					.detail("Elapsed", now() - it.second.lastLogged)
					.detail("MeanReplyBytes", it.second.replyBytes/it.second.totalPeeks)
					.detail("TotalPeeks", it.second.totalPeeks)
					.detail("UnblockedPeeks", it.second.unblockedPeeks)
					.detail("DuplicatePeeks", it.second.duplicatePeeks)
					.detail("Sequence", it.second.sequence_version.size() ? it.second.sequence_version.begin()->first : -1)
					.detail("IdleSeconds", it.second.idleTime)
					.detail("IdleMax", it.second.idleMax)
					.detail("QueueSeconds", it.second.queueTime)
					.detail("QueueMax", it.second.queueMax)
					.detail("BlockSeconds", it.second.blockTime)
					.detail("BlockMax", it.second.blockMax)
					.detail("WorkSeconds", it.second.workTime)
					.detail("WorkMax", it.second.workMax);
				it.second.resetMetrics();
			}
		}

		wait( delay(SERVER_KNOBS->PEEK_LOGGING_DELAY * std::max(1,logCount)) );
	}
}

void getQueuingMetrics( TLogData* self, Reference<LogData> logData, TLogQueuingMetricsRequest const& req ) {
	TLogQueuingMetricsReply reply;
	reply.localTime = now();
	reply.instanceID = self->instanceID;
	reply.bytesInput = self->bytesInput;
	reply.bytesDurable = self->bytesDurable;
	reply.storageBytes = self->persistentData->getStorageBytes();
	//FIXME: Add the knownCommittedVersion to this message and change ratekeeper to use that version.
	reply.v = logData->durableKnownCommittedVersion;
	req.reply.send( reply );
}


ACTOR Future<Void>
tLogSnapCreate(TLogSnapRequest snapReq, TLogData* self, Reference<LogData> logData) {
	if (self->ignorePopUid != snapReq.snapUID.toString()) {
		snapReq.reply.sendError(operation_failed());
		return Void();
	}
	ExecCmdValueString snapArg(snapReq.snapPayload);
	try {
		int err = wait(execHelper(&snapArg, snapReq.snapUID, self->dataFolder, snapReq.role.toString()));

		std::string uidStr = snapReq.snapUID.toString();
		TraceEvent("ExecTraceTLog")
			.detail("Uid", uidStr)
			.detail("Status", err)
			.detail("Role", snapReq.role)
			.detail("Value", self->dataFolder)
			.detail("ExecPayload", snapReq.snapPayload)
			.detail("PersistentDataVersion", logData->persistentDataVersion)
			.detail("PersistentDatadurableVersion", logData->persistentDataDurableVersion)
			.detail("QueueCommittedVersion", logData->queueCommittedVersion.get())
			.detail("Version", logData->version.get());

		if (err != 0) {
			throw operation_failed();
		}
		snapReq.reply.send(Void());
	} catch (Error& e) {
		TraceEvent("TLogExecHelperError").error(e, true /*includeCancelled */);
		if (e.code() != error_code_operation_cancelled) {
			snapReq.reply.sendError(e);
		} else {
			throw e;
		}
	}
	return Void();
}


ACTOR Future<Void>
tLogEnablePopReq(TLogEnablePopRequest enablePopReq, TLogData* self, Reference<LogData> logData) {
	if (self->ignorePopUid != enablePopReq.snapUID.toString()) {
		TraceEvent(SevWarn, "TLogPopDisableEnableUidMismatch")
			.detail("IgnorePopUid", self->ignorePopUid)
			.detail("UidStr", enablePopReq.snapUID.toString());
		enablePopReq.reply.sendError(operation_failed());
		return Void();
	}
	TraceEvent("EnableTLogPlayAllIgnoredPops2");
	// use toBePopped and issue all the pops
	std::map<Tag, Version>::iterator it;
	state vector<Future<Void>> ignoredPops;
	self->ignorePopRequest = false;
	self->ignorePopDeadline = 0.0;
	self->ignorePopUid = "";
	for (it = self->toBePopped.begin(); it != self->toBePopped.end(); it++) {
		TraceEvent("PlayIgnoredPop")
			.detail("Tag", it->first.toString())
			.detail("Version", it->second);
		ignoredPops.push_back(tLogPopCore(self, it->first, it->second, logData));
	}
	TraceEvent("TLogExecCmdPopEnable")
		.detail("UidStr", enablePopReq.snapUID.toString())
		.detail("IgnorePopUid", self->ignorePopUid)
		.detail("IgnporePopRequest", self->ignorePopRequest)
		.detail("IgnporePopDeadline", self->ignorePopDeadline)
		.detail("PersistentDataVersion", logData->persistentDataVersion)
		.detail("PersistentDatadurableVersion", logData->persistentDataDurableVersion)
		.detail("QueueCommittedVersion", logData->queueCommittedVersion.get())
		.detail("Version", logData->version.get());
	wait(waitForAll(ignoredPops));
	self->toBePopped.clear();
	enablePopReq.reply.send(Void());
	return Void();
}

ACTOR Future<Void> serveTLogInterface( TLogData* self, TLogInterface tli, Reference<LogData> logData, PromiseStream<Void> warningCollectorInput ) {
	state Future<Void> dbInfoChange = Void();

	loop choose {
		when( wait( dbInfoChange ) ) {
			dbInfoChange = self->dbInfo->onChange();
			bool found = false;
			if(self->dbInfo->get().recoveryState >= RecoveryState::ACCEPTING_COMMITS) {
				for(auto& logs : self->dbInfo->get().logSystemConfig.tLogs) {
					if( std::count( logs.tLogs.begin(), logs.tLogs.end(), logData->logId ) ) {
						found = true;
						break;
					}
				}
			}
			if(found && self->dbInfo->get().logSystemConfig.recruitmentID == logData->recruitmentID) {
				logData->logSystem->set(ILogSystem::fromServerDBInfo( self->dbgid, self->dbInfo->get() ));
				if(!logData->isPrimary) {
					logData->logSystem->get()->pop(logData->logRouterPoppedVersion, logData->remoteTag, logData->durableKnownCommittedVersion, logData->locality);
				}

				if(!logData->isPrimary && logData->stopped) {
					TraceEvent("TLogAlreadyStopped", self->dbgid).detail("LogId", logData->logId);
					logData->removed = logData->removed && logData->logSystem->get()->endEpoch();
				}
			} else {
				logData->logSystem->set(Reference<ILogSystem>());
			}
		}
		when( TLogPeekRequest req = waitNext( tli.peekMessages.getFuture() ) ) {
			logData->addActor.send( tLogPeekMessages( self, req, logData ) );
		}
		when( TLogPopRequest req = waitNext( tli.popMessages.getFuture() ) ) {
			logData->addActor.send(tLogPop(self, req, logData));
		}
		when( TLogCommitRequest req = waitNext( tli.commit.getFuture() ) ) {
			//TraceEvent("TLogCommitReq", logData->logId).detail("Ver", req.version).detail("PrevVer", req.prevVersion).detail("LogVer", logData->version.get());
			ASSERT(logData->isPrimary);
			TEST(logData->stopped); // TLogCommitRequest while stopped
			if (!logData->stopped) {
				if (!req.splitTransaction.present()) {
					logData->addActor.send( tLogCommit( self, req, logData, warningCollectorInput ) );
				} else {
					logData->addActor.send(tLogCommitSplitTransactions(self, req, logData, warningCollectorInput));
				}
			} else {
				req.reply.sendError( tlog_stopped() );
			}
		}
		when( ReplyPromise< TLogLockResult > reply = waitNext( tli.lock.getFuture() ) ) {
			logData->addActor.send( tLogLock(self, reply, logData) );
		}
		when (TLogQueuingMetricsRequest req = waitNext(tli.getQueuingMetrics.getFuture())) {
			getQueuingMetrics(self, logData, req);
		}
		when (TLogConfirmRunningRequest req = waitNext(tli.confirmRunning.getFuture())){
			if (req.debugID.present() ) {
				UID tlogDebugID = nondeterministicRandom()->randomUniqueID();
				g_traceBatch.addAttach("TransactionAttachID", req.debugID.get().first(), tlogDebugID.first());
				g_traceBatch.addEvent("TransactionDebug", tlogDebugID.first(), "TLogServer.TLogConfirmRunningRequest");
			}
			if (!logData->stopped)
				req.reply.send(Void());
			else
				req.reply.sendError( tlog_stopped() );
		}
		when( TLogDisablePopRequest req = waitNext( tli.disablePopRequest.getFuture() ) ) {
			if (self->ignorePopUid != "") {
				TraceEvent(SevWarn, "TLogPopDisableonDisable")
					.detail("IgnorePopUid", self->ignorePopUid)
					.detail("UidStr", req.snapUID.toString())
					.detail("PersistentDataVersion", logData->persistentDataVersion)
					.detail("PersistentDatadurableVersion", logData->persistentDataDurableVersion)
					.detail("QueueCommittedVersion", logData->queueCommittedVersion.get())
					.detail("Version", logData->version.get());
				req.reply.sendError(operation_failed());
			} else {
				//FIXME: As part of reverting snapshot V1, make ignorePopUid a UID instead of string
				self->ignorePopRequest = true;
				self->ignorePopUid = req.snapUID.toString();
				self->ignorePopDeadline = g_network->now() + SERVER_KNOBS->TLOG_IGNORE_POP_AUTO_ENABLE_DELAY;
				req.reply.send(Void());
			}
		}
		when( TLogEnablePopRequest enablePopReq = waitNext( tli.enablePopRequest.getFuture() ) ) {
			logData->addActor.send( tLogEnablePopReq( enablePopReq, self, logData) );
		}
		when( TLogSnapRequest snapReq = waitNext( tli.snapRequest.getFuture() ) ) {
			logData->addActor.send( tLogSnapCreate( snapReq, self, logData) );
		}
	}
}

void removeLog( TLogData* self, Reference<LogData> logData ) {
	TraceEvent("TLogRemoved", self->dbgid).detail("LogId", logData->logId).detail("Input", logData->bytesInput.getValue()).detail("Durable", logData->bytesDurable.getValue());
	logData->stopped = true;
	if(!logData->recoveryComplete.isSet()) {
		logData->recoveryComplete.sendError(end_of_stream());
	}

	logData->addActor = PromiseStream<Future<Void>>(); //there could be items still in the promise stream if one of the actors threw an error immediately
	self->id_data.erase(logData->logId);

	while (self->popOrder.size() && !self->id_data.count(self->popOrder.front())) {
			self->popOrder.pop_front();
	}

	if (self->id_data.size() == 0) {
		throw worker_removed();
	}
}

ACTOR Future<Void> pullAsyncData( TLogData* self, Reference<LogData> logData, std::vector<Tag> tags, Version beginVersion, Optional<Version> endVersion, bool poppedIsKnownCommitted ) {
	state Future<Void> dbInfoChange = Void();
	state Reference<ILogSystem::IPeekCursor> r;
	state Version tagAt = beginVersion;
	state Version lastVer = 0;

	if (endVersion.present()) {
		TraceEvent("TLogRestoreReplicationFactor", self->dbgid).detail("LogId", logData->logId).detail("Locality", logData->locality).detail("RecoverFrom", beginVersion).detail("RecoverTo", endVersion.get());
	}

	while (!endVersion.present() || logData->version.get() < endVersion.get()) {
		loop {
			choose {
				when(wait( r ? r->getMore(TaskPriority::TLogCommit) : Never() ) ) {
					break;
				}
				when( wait( dbInfoChange ) ) {
					if( logData->logSystem->get() ) {
						r = logData->logSystem->get()->peek( logData->logId, tagAt, endVersion, tags, true );
					} else {
						r = Reference<ILogSystem::IPeekCursor>();
					}
					dbInfoChange = logData->logSystem->onChange();
				}
			}
		}

		state double waitStartT = 0;
		while( self->bytesInput - self->bytesDurable >= SERVER_KNOBS->TLOG_HARD_LIMIT_BYTES && !logData->stopped ) {
			if (now() - waitStartT >= 1) {
				TraceEvent(SevWarn, "TLogUpdateLag", logData->logId)
					.detail("Version", logData->version.get())
					.detail("PersistentDataVersion", logData->persistentDataVersion)
					.detail("PersistentDataDurableVersion", logData->persistentDataDurableVersion);
				waitStartT = now();
			}
			wait( delayJittered(.005, TaskPriority::TLogCommit) );
		}

		state Version ver = 0;
		state std::vector<TagsAndMessage> messages;
		loop {
			state bool foundMessage = r->hasMessage();
			if (!foundMessage || r->version().version != ver) {
				ASSERT(r->version().version > lastVer);
				if (ver) {
					if(logData->stopped || (endVersion.present() && ver > endVersion.get())) {
						return Void();
					}

					if(poppedIsKnownCommitted) {
						logData->knownCommittedVersion = std::max(logData->knownCommittedVersion, r->popped());
						logData->minKnownCommittedVersion = std::max(logData->minKnownCommittedVersion, r->getMinKnownCommittedVersion());
					}

					commitMessages(self, logData, ver, messages);

					if(self->terminated.isSet()) {
						return Void();
					}

					// Log the changes to the persistent queue, to be committed by commitQueue()
					AlternativeTLogQueueEntryRef qe;
					qe.version = ver;
					qe.knownCommittedVersion = logData->knownCommittedVersion;
					qe.alternativeMessages = &messages;
					qe.id = logData->logId;
					self->persistentQueue->push( qe, logData );

					self->diskQueueCommitBytes += qe.expectedSize();
					if( self->diskQueueCommitBytes > SERVER_KNOBS->MAX_QUEUE_COMMIT_BYTES ) {
						self->largeDiskQueueCommitBytes.set(true);
					}

					// Notifies the commitQueue actor to commit persistentQueue, and also unblocks tLogPeekMessages actors
					logData->version.set( ver );
					wait( yield(TaskPriority::TLogCommit) );
				}
				lastVer = ver;
				ver = r->version().version;
				messages.clear();

				if (!foundMessage) {
					ver--;
					if(ver > logData->version.get()) {
						if(logData->stopped || (endVersion.present() && ver > endVersion.get())) {
							return Void();
						}

						if(poppedIsKnownCommitted) {
							logData->knownCommittedVersion = std::max(logData->knownCommittedVersion, r->popped());
							logData->minKnownCommittedVersion = std::max(logData->minKnownCommittedVersion, r->getMinKnownCommittedVersion());
						}

						if(self->terminated.isSet()) {
							return Void();
						}

						// Log the changes to the persistent queue, to be committed by commitQueue()
						TLogQueueEntryRef qe;
						qe.version = ver;
						qe.knownCommittedVersion = logData->knownCommittedVersion;
						qe.messages = StringRef();
						qe.id = logData->logId;
						self->persistentQueue->push( qe, logData );

						self->diskQueueCommitBytes += qe.expectedSize();
						if( self->diskQueueCommitBytes > SERVER_KNOBS->MAX_QUEUE_COMMIT_BYTES ) {
							self->largeDiskQueueCommitBytes.set(true);
						}

						// Notifies the commitQueue actor to commit persistentQueue, and also unblocks tLogPeekMessages actors
						logData->version.set( ver );
						wait( yield(TaskPriority::TLogCommit) );
					}
					break;
				}
			}

			messages.emplace_back(r->getMessageWithTags(), r->getTags());
			r->nextMessage();
		}

		tagAt = std::max( r->version().version, logData->version.get() + 1 );
	}
	return Void();
}

ACTOR Future<Void> tLogCore( TLogData* self, Reference<LogData> logData, TLogInterface tli, bool pulledRecoveryVersions ) {
	if(logData->removed.isReady()) {
		wait(delay(0)); //to avoid iterator invalidation in restorePersistentState when removed is already ready
		ASSERT(logData->removed.isError());

		if(logData->removed.getError().code() != error_code_worker_removed) {
			throw logData->removed.getError();
		}

		removeLog(self, logData);
		return Void();
	}

	state PromiseStream<Void> warningCollectorInput;
	state Future<Void> warningCollector = timeoutWarningCollector( warningCollectorInput.getFuture(), 1.0, "TLogQueueCommitSlow", self->dbgid );
	state Future<Void> error = actorCollection( logData->addActor.getFuture() );

	logData->addActor.send( waitFailureServer( tli.waitFailure.getFuture()) );
	logData->addActor.send( logData->removed );
	//FIXME: update tlogMetrics to include new information, or possibly only have one copy for the shared instance
	logData->addActor.send( traceCounters("TLogMetrics", logData->logId, SERVER_KNOBS->STORAGE_LOGGING_DELAY, &logData->cc, logData->logId.toString() + "/TLogMetrics"));
	logData->addActor.send( serveTLogInterface(self, tli, logData, warningCollectorInput) );
	logData->addActor.send( cleanupPeekTrackers(logData.getPtr()) );
	logData->addActor.send( logPeekTrackers(logData.getPtr()) );

	if(!logData->isPrimary) {
		std::vector<Tag> tags;
		tags.push_back(logData->remoteTag);
		logData->addActor.send( pullAsyncData(self, logData, tags, pulledRecoveryVersions ? logData->recoveredAt + 1 : logData->unrecoveredBefore, Optional<Version>(), true) );
	}

	try {
		wait( error );
		throw internal_error();
	} catch( Error &e ) {
		if( e.code() != error_code_worker_removed )
			throw;

		removeLog(self, logData);
		return Void();
	}
}

ACTOR Future<Void> checkEmptyQueue(TLogData* self) {
	TraceEvent("TLogCheckEmptyQueueBegin", self->dbgid);
	try {
		bool recoveryFinished = wait( self->persistentQueue->initializeRecovery(0) );
		if (recoveryFinished)
			return Void();
		TLogQueueEntry r = wait( self->persistentQueue->readNext(self) );
		throw internal_error();
	} catch (Error& e) {
		if (e.code() != error_code_end_of_stream) throw;
		TraceEvent("TLogCheckEmptyQueueEnd", self->dbgid);
		return Void();
	}
}

ACTOR Future<Void> checkRecovered(TLogData* self) {
	TraceEvent("TLogCheckRecoveredBegin", self->dbgid);
	Optional<Value> v = wait( self->persistentData->readValue(StringRef()) );
	TraceEvent("TLogCheckRecoveredEnd", self->dbgid);
	return Void();
}

// Recovery persistent state of tLog from disk
ACTOR Future<Void> restorePersistentState( TLogData* self, LocalityData locality, Promise<Void> oldLog, Promise<Void> recovered, PromiseStream<InitializeTLogRequest> tlogRequests ) {
	state double startt = now();
	state Reference<LogData> logData;
	state KeyRange tagKeys;
	// PERSIST: Read basic state from persistentData; replay persistentQueue but don't erase it

	TraceEvent("TLogRestorePersistentState", self->dbgid);

	state IKeyValueStore *storage = self->persistentData;
	wait(storage->init());
	state Future<Optional<Value>> fFormat = storage->readValue(persistFormat.key);
	state Future<Optional<Value>> fRecoveryLocation = storage->readValue(persistRecoveryLocationKey);
	state Future<Standalone<RangeResultRef>> fVers = storage->readRange(persistCurrentVersionKeys);
	state Future<Standalone<RangeResultRef>> fKnownCommitted = storage->readRange(persistKnownCommittedVersionKeys);
	state Future<Standalone<RangeResultRef>> fLocality = storage->readRange(persistLocalityKeys);
	state Future<Standalone<RangeResultRef>> fLogRouterTags = storage->readRange(persistLogRouterTagsKeys);
	state Future<Standalone<RangeResultRef>> fTxsTags = storage->readRange(persistTxsTagsKeys);
	state Future<Standalone<RangeResultRef>> fRecoverCounts = storage->readRange(persistRecoveryCountKeys);
	state Future<Standalone<RangeResultRef>> fProtocolVersions = storage->readRange(persistProtocolVersionKeys);
	state Future<Standalone<RangeResultRef>> fTLogSpillTypes = storage->readRange(persistTLogSpillTypeKeys);

	// FIXME: metadata in queue?

	wait( waitForAll( std::vector{fFormat, fRecoveryLocation} ) );
	wait( waitForAll( std::vector{fVers, fKnownCommitted, fLocality, fLogRouterTags, fTxsTags, fRecoverCounts, fProtocolVersions, fTLogSpillTypes} ) );

	if (fFormat.get().present() && !persistFormatReadableRange.contains( fFormat.get().get() )) {
		//FIXME: remove when we no longer need to test upgrades from 4.X releases
		if(g_network->isSimulated()) {
			TraceEvent("ElapsedTime").detail("SimTime", now()).detail("RealTime", 0).detail("RandomUnseed", 0);
			flushAndExit(0);
		}

		TraceEvent(SevError, "UnsupportedDBFormat", self->dbgid).detail("Format", fFormat.get().get()).detail("Expected", persistFormat.value.toString());
		throw worker_recovery_failed();
	}

	if (!fFormat.get().present()) {
		Standalone<RangeResultRef> v = wait( self->persistentData->readRange( KeyRangeRef(StringRef(), LiteralStringRef("\xff")), 1 ) );
		if (!v.size()) {
			TEST(true); // The DB is completely empty, so it was never initialized.  Delete it.
			throw worker_removed();
		} else {
			// This should never happen
			TraceEvent(SevError, "NoDBFormatKey", self->dbgid).detail("FirstKey", v[0].key);
			ASSERT( false );
			throw worker_recovery_failed();
		}
	}

	state std::vector<Future<ErrorOr<Void>>> removed;

	ASSERT(fFormat.get().get() == LiteralStringRef("FoundationDB/LogServer/3/0"));

	ASSERT(fVers.get().size() == fRecoverCounts.get().size());

	state std::map<UID, int8_t> id_locality;
	for(auto it : fLocality.get()) {
		id_locality[ BinaryReader::fromStringRef<UID>(it.key.removePrefix(persistLocalityKeys.begin), Unversioned())] = BinaryReader::fromStringRef<int8_t>( it.value, Unversioned() );
	}

	state std::map<UID, int> id_logRouterTags;
	for(auto it : fLogRouterTags.get()) {
		id_logRouterTags[ BinaryReader::fromStringRef<UID>(it.key.removePrefix(persistLogRouterTagsKeys.begin), Unversioned())] = BinaryReader::fromStringRef<int>( it.value, Unversioned() );
	}

	state std::map<UID, int> id_txsTags;
	for(auto it : fTxsTags.get()) {
		id_txsTags[ BinaryReader::fromStringRef<UID>(it.key.removePrefix(persistTxsTagsKeys.begin), Unversioned())] = BinaryReader::fromStringRef<int>( it.value, Unversioned() );
	}

	state std::map<UID, Version> id_knownCommitted;
	for(auto it : fKnownCommitted.get()) {
		id_knownCommitted[ BinaryReader::fromStringRef<UID>(it.key.removePrefix(persistKnownCommittedVersionKeys.begin), Unversioned())] = BinaryReader::fromStringRef<Version>( it.value, Unversioned() );
	}

	state IDiskQueue::location minimumRecoveryLocation = 0;
	if (fRecoveryLocation.get().present()) {
		minimumRecoveryLocation = BinaryReader::fromStringRef<IDiskQueue::location>(fRecoveryLocation.get().get(), Unversioned());
	}

	state int idx = 0;
	state Promise<Void> registerWithMaster;
	state std::map<UID, TLogInterface> id_interf;
	state std::vector<std::pair<Version, UID>> logsByVersion;
	for(idx = 0; idx < fVers.get().size(); idx++) {
		state KeyRef rawId = fVers.get()[idx].key.removePrefix(persistCurrentVersionKeys.begin);
		UID id1 = BinaryReader::fromStringRef<UID>( rawId, Unversioned() );
		UID id2 = BinaryReader::fromStringRef<UID>( fRecoverCounts.get()[idx].key.removePrefix(persistRecoveryCountKeys.begin), Unversioned() );
		ASSERT(id1 == id2);

		TLogInterface recruited(id1, self->dbgid, locality);
		recruited.initEndpoints();

		DUMPTOKEN( recruited.peekMessages );
		DUMPTOKEN( recruited.popMessages );
		DUMPTOKEN( recruited.commit );
		DUMPTOKEN( recruited.lock );
		DUMPTOKEN( recruited.getQueuingMetrics );
		DUMPTOKEN( recruited.confirmRunning );

		ProtocolVersion protocolVersion = BinaryReader::fromStringRef<ProtocolVersion>( fProtocolVersions.get()[idx].value, Unversioned() );
		TLogSpillType logSpillType = BinaryReader::fromStringRef<TLogSpillType>( fTLogSpillTypes.get()[idx].value, AssumeVersion(protocolVersion) );

		//We do not need the remoteTag, because we will not be loading any additional data
		logData = Reference<LogData>( new LogData(self, recruited, Tag(), true, id_logRouterTags[id1], id_txsTags[id1], UID(), protocolVersion, logSpillType, std::vector<Tag>(), "Restored") );
		logData->locality = id_locality[id1];
		logData->stopped = true;
		self->id_data[id1] = logData;
		id_interf[id1] = recruited;

		logData->knownCommittedVersion = id_knownCommitted[id1];
		Version ver = BinaryReader::fromStringRef<Version>( fVers.get()[idx].value, Unversioned() );
		logData->persistentDataVersion = ver;
		logData->persistentDataDurableVersion = ver;
		logData->version.set(ver);
		logData->recoveryCount = BinaryReader::fromStringRef<DBRecoveryCount>( fRecoverCounts.get()[idx].value, Unversioned() );
		logData->removed = rejoinMasters(self, recruited, logData->recoveryCount, registerWithMaster.getFuture(), false);
		removed.push_back(errorOr(logData->removed));
		logsByVersion.emplace_back(ver, id1);

		TraceEvent("TLogPersistentStateRestore", self->dbgid).detail("LogId", logData->logId).detail("Ver", ver);
		// Restore popped keys.  Pop operations that took place after the last (committed) updatePersistentDataVersion might be lost, but
		// that is fine because we will get the corresponding data back, too.
		tagKeys = prefixRange( rawId.withPrefix(persistTagPoppedKeys.begin) );
		loop {
			if(logData->removed.isReady()) break;
			Standalone<RangeResultRef> data = wait( self->persistentData->readRange( tagKeys, BUGGIFY ? 3 : 1<<30, 1<<20 ) );
			if (!data.size()) break;
			((KeyRangeRef&)tagKeys) = KeyRangeRef( keyAfter(data.back().key, tagKeys.arena()), tagKeys.end );

			for(auto &kv : data) {
				Tag tag = decodeTagPoppedKey(rawId, kv.key);
				Version popped = decodeTagPoppedValue(kv.value);
				TraceEvent("TLogRestorePopped", logData->logId).detail("Tag", tag.toString()).detail("To", popped);
				auto tagData = logData->getTagData(tag);
				ASSERT( !tagData );
				logData->createTagData(tag, popped, false, false, false);
				logData->getTagData(tag)->persistentPopped = popped;
			}
		}
	}

	std::sort(logsByVersion.begin(), logsByVersion.end());
	for (const auto& pair : logsByVersion) {
		// TLogs that have been fully spilled won't have queue entries read in the loop below.
		self->popOrder.push_back(pair.second);
	}
	logsByVersion.clear();

	state Future<Void> allRemoved = waitForAll(removed);
	state UID lastId = UID(1,1); //initialized so it will not compare equal to a default UID
	state double recoverMemoryLimit = SERVER_KNOBS->TLOG_RECOVER_MEMORY_LIMIT;
	if (BUGGIFY) recoverMemoryLimit = std::max<double>(
			SERVER_KNOBS->BUGGIFY_RECOVER_MEMORY_LIMIT,
			(double)SERVER_KNOBS->TLOG_SPILL_THRESHOLD);

	try {
		bool recoveryFinished = wait( self->persistentQueue->initializeRecovery(minimumRecoveryLocation) );
		if (recoveryFinished)
			throw end_of_stream();
		loop {
			if(allRemoved.isReady()) {
				TEST(true); //all tlogs removed during queue recovery
				throw worker_removed();
			}
			choose {
				when( TLogQueueEntry qe = wait( self->persistentQueue->readNext(self) ) ) {
					if(qe.id != lastId) {
						lastId = qe.id;
						auto it = self->id_data.find(qe.id);
						if(it != self->id_data.end()) {
							logData = it->second;
						} else {
							logData = Reference<LogData>();
						}
					}

					//TraceEvent("TLogRecoveredQE", self->dbgid).detail("LogId", qe.id).detail("Ver", qe.version).detail("MessageBytes", qe.messages.size()).detail("Tags", qe.tags.size())
					//	.detail("Tag0", qe.tags.size() ? qe.tags[0].tag : invalidTag).detail("Version", logData->version.get());

					if(logData) {
						if(!self->spillOrder.size() || self->spillOrder.back() != qe.id) {
							self->spillOrder.push_back(qe.id);
						}
						logData->knownCommittedVersion = std::max(logData->knownCommittedVersion, qe.knownCommittedVersion);
						if( qe.version > logData->version.get() ) {
							commitMessages(self, logData, qe.version, qe.arena(), qe.messages);
							logData->version.set( qe.version );
							logData->queueCommittedVersion.set( qe.version );

							while (self->bytesInput - self->bytesDurable >= recoverMemoryLimit) {
								TEST(true);  // Flush excess data during TLog queue recovery
								TraceEvent("FlushLargeQueueDuringRecovery", self->dbgid).detail("LogId", logData->logId).detail("BytesInput", self->bytesInput).detail("BytesDurable", self->bytesDurable).detail("Version", logData->version.get()).detail("PVer", logData->persistentDataVersion);

								choose {
									when( wait( updateStorage(self) ) ) {}
									when( wait( allRemoved ) ) { throw worker_removed(); }
								}
							}
						} else {
							// Updating persistRecoveryLocation and persistCurrentVersion at the same time,
							// transactionally, should mean that we never read any TLogQueueEntry that has already
							// been spilled.
							ASSERT_WE_THINK(qe.version == logData->version.get());
						}
					}
				}
				when( wait( allRemoved ) ) { throw worker_removed(); }
			}
		}
	} catch (Error& e) {
		if (e.code() != error_code_end_of_stream) throw;
	}

	TraceEvent("TLogRestorePersistentStateDone", self->dbgid).detail("Took", now()-startt);
	TEST( now()-startt >= 1.0 );  // TLog recovery took more than 1 second

	for(auto it : self->id_data) {
		if(it.second->queueCommittedVersion.get() == 0) {
			TraceEvent("TLogZeroVersion", self->dbgid).detail("LogId", it.first);
			it.second->queueCommittedVersion.set(it.second->version.get());
		}
		it.second->recoveryComplete.sendError(end_of_stream());
		self->sharedActors.send( tLogCore( self, it.second, id_interf[it.first], false ) );
	}

	if(registerWithMaster.canBeSet()) registerWithMaster.send(Void());
	return Void();
}

bool tlogTerminated( TLogData* self, IKeyValueStore* persistentData, TLogQueue* persistentQueue, Error const& e ) {
	// Dispose the IKVS (destroying its data permanently) only if this shutdown is definitely permanent.  Otherwise just close it.
	if (e.code() == error_code_worker_removed || e.code() == error_code_recruitment_failed) {
		persistentData->dispose();
		persistentQueue->dispose();
	} else {
		persistentData->close();
		persistentQueue->close();
	}

	if ( e.code() == error_code_worker_removed ||
		 e.code() == error_code_recruitment_failed ||
		 e.code() == error_code_file_not_found )
	{
		TraceEvent("TLogTerminated", self->dbgid).error(e, true);
		return true;
	} else
		return false;
}

ACTOR Future<Void> updateLogSystem(TLogData* self, Reference<LogData> logData, LogSystemConfig recoverFrom, Reference<AsyncVar<Reference<ILogSystem>>> logSystem) {
	loop {
		bool found = self->dbInfo->get().logSystemConfig.recruitmentID == logData->recruitmentID;
		if (found) {
			if (self->dbInfo->get().logSystemConfig.isNextGenerationOf(recoverFrom)) {
				logSystem->set(ILogSystem::fromOldLogSystemConfig( logData->logId, self->dbInfo->get().myLocality, self->dbInfo->get().logSystemConfig ));
			} else if (self->dbInfo->get().logSystemConfig.isEqualIds(recoverFrom)) {
				logSystem->set(ILogSystem::fromLogSystemConfig( logData->logId, self->dbInfo->get().myLocality, self->dbInfo->get().logSystemConfig, false, true ));
			} else if (self->dbInfo->get().recoveryState >= RecoveryState::ACCEPTING_COMMITS) {
				logSystem->set(ILogSystem::fromLogSystemConfig( logData->logId, self->dbInfo->get().myLocality, self->dbInfo->get().logSystemConfig, true ));
			} else {
				found = false;
			}
		}
		if (!found) {
			logSystem->set(Reference<ILogSystem>());
		} else {
			logData->logSystem->get()->pop(logData->logRouterPoppedVersion, logData->remoteTag, logData->durableKnownCommittedVersion, logData->locality);
		}
		TraceEvent("TLogUpdate", self->dbgid).detail("LogId", logData->logId).detail("RecruitmentID", logData->recruitmentID).detail("DbRecruitmentID", self->dbInfo->get().logSystemConfig.recruitmentID).detail("RecoverFrom", recoverFrom.toString()).detail("DbInfo", self->dbInfo->get().logSystemConfig.toString()).detail("Found", found).detail("LogSystem", (bool) logSystem->get() ).detail("RecoveryState", (int)self->dbInfo->get().recoveryState);
		for (const auto& it : self->dbInfo->get().logSystemConfig.oldTLogs) {
			TraceEvent("TLogUpdateOld", self->dbgid).detail("LogId", logData->logId).detail("DbInfo", it.toString());
		}
		wait( self->dbInfo->onChange() );
	}
}

void stopAllTLogs( TLogData* self, UID newLogId ) {
	for(auto it : self->id_data) {
		if( !it.second->stopped ) {
			TraceEvent("TLogStoppedByNewRecruitment", self->dbgid).detail("LogId", it.second->logId).detail("StoppedId", it.first.toString()).detail("RecruitedId", newLogId).detail("EndEpoch", it.second->logSystem->get().getPtr() != 0);
			if(!it.second->isPrimary && it.second->logSystem->get()) {
				it.second->removed = it.second->removed && it.second->logSystem->get()->endEpoch();
			}
			if(it.second->committingQueue.canBeSet()) {
				it.second->committingQueue.sendError(worker_removed());
			}
		}
		it.second->stopped = true;
		if(!it.second->recoveryComplete.isSet()) {
			it.second->recoveryComplete.sendError(end_of_stream());
		}
		it.second->stopCommit.trigger();
	}
}

// Start the tLog role for a worker
ACTOR Future<Void> tLogStart( TLogData* self, InitializeTLogRequest req, LocalityData locality ) {
	state TLogInterface recruited(self->dbgid, locality);
	recruited.initEndpoints();

	DUMPTOKEN( recruited.peekMessages );
	DUMPTOKEN( recruited.popMessages );
	DUMPTOKEN( recruited.commit );
	DUMPTOKEN( recruited.lock );
	DUMPTOKEN( recruited.getQueuingMetrics );
	DUMPTOKEN( recruited.confirmRunning );

	stopAllTLogs(self, recruited.id());

	bool recovering = (req.recoverFrom.logSystemType == LogSystemType::tagPartitioned);
	state Reference<LogData> logData = Reference<LogData>( new LogData(self, recruited, req.remoteTag, req.isPrimary, req.logRouterTags, req.txsTags, req.recruitmentID, currentProtocolVersion, req.spillType, req.allTags, recovering ? "Recovered" : "Recruited") );
	self->id_data[recruited.id()] = logData;
	logData->locality = req.locality;
	logData->recoveryCount = req.epoch;
	logData->removed = rejoinMasters(self, recruited, req.epoch, Future<Void>(Void()), req.isPrimary);
	self->popOrder.push_back(recruited.id());
	self->spillOrder.push_back(recruited.id());

	TraceEvent("TLogStart", logData->logId);

	state Future<Void> updater;
	state bool pulledRecoveryVersions = false;
	try {
		if( logData->removed.isReady() ) {
			throw logData->removed.getError();
		}

		if (recovering) {
			logData->unrecoveredBefore = req.startVersion;
			logData->recoveredAt = req.recoverAt;
			logData->knownCommittedVersion = req.startVersion - 1;
			logData->persistentDataVersion = logData->unrecoveredBefore - 1;
			logData->persistentDataDurableVersion = logData->unrecoveredBefore - 1;
			logData->queueCommittedVersion.set( logData->unrecoveredBefore - 1 );
			logData->version.set( logData->unrecoveredBefore - 1 );

			logData->unpoppedRecoveredTags = req.allTags.size();
			wait( initPersistentState( self, logData ) || logData->removed );

			TraceEvent("TLogRecover", self->dbgid).detail("LogId", logData->logId).detail("At", req.recoverAt).detail("Known", req.knownCommittedVersion).detail("Unrecovered", logData->unrecoveredBefore).detail("Tags", describe(req.recoverTags)).detail("Locality", req.locality).detail("LogRouterTags", logData->logRouterTags);

			if(logData->recoveryComplete.isSet()) {
				throw worker_removed();
			}

			updater = updateLogSystem(self, logData, req.recoverFrom, logData->logSystem);

			logData->initialized = true;
			self->newLogData.trigger();

			if((req.isPrimary || req.recoverFrom.logRouterTags == 0) && !logData->stopped && logData->unrecoveredBefore <= req.recoverAt) {
				if(req.recoverFrom.logRouterTags > 0 && req.locality != tagLocalitySatellite) {
					logData->logRouterPopToVersion = req.recoverAt;
					std::vector<Tag> tags;
					tags.push_back(logData->remoteTag);
					wait(pullAsyncData(self, logData, tags, logData->unrecoveredBefore, req.recoverAt, true) || logData->removed);
				} else if(!req.recoverTags.empty()) {
					ASSERT(logData->unrecoveredBefore > req.knownCommittedVersion);
					wait(pullAsyncData(self, logData, req.recoverTags, req.knownCommittedVersion + 1, req.recoverAt, false) || logData->removed);
				}
				pulledRecoveryVersions = true;
				logData->knownCommittedVersion = req.recoverAt;
			}

			if((req.isPrimary || req.recoverFrom.logRouterTags == 0) && logData->version.get() < req.recoverAt && !logData->stopped) {
				// Log the changes to the persistent queue, to be committed by commitQueue()
				TLogQueueEntryRef qe;
				qe.version = req.recoverAt;
				qe.knownCommittedVersion = logData->knownCommittedVersion;
				qe.messages = StringRef();
				qe.id = logData->logId;
				self->persistentQueue->push( qe, logData );

				self->diskQueueCommitBytes += qe.expectedSize();
				if( self->diskQueueCommitBytes > SERVER_KNOBS->MAX_QUEUE_COMMIT_BYTES ) {
					self->largeDiskQueueCommitBytes.set(true);
				}

				logData->version.set( req.recoverAt );
			}

			if(logData->recoveryComplete.isSet()) {
				throw worker_removed();
			}

			logData->addActor.send( respondToRecovered( recruited, logData->recoveryComplete ) );
		} else {
			// Brand new tlog, initialization has already been done by caller
			wait( initPersistentState( self, logData ) || logData->removed );

			if(logData->recoveryComplete.isSet()) {
				throw worker_removed();
			}

			logData->initialized = true;
			self->newLogData.trigger();

			logData->recoveryComplete.send(Void());
		}
		wait(logData->committingQueue.getFuture() || logData->removed );
	} catch( Error &e ) {
		req.reply.sendError(recruitment_failed());

		if( e.code() != error_code_worker_removed ) {
			throw;
		}

		wait( delay(0.0) ); // if multiple recruitment requests were already in the promise stream make sure they are all started before any are removed

		removeLog(self, logData);
		return Void();
	}

	req.reply.send( recruited );

	TraceEvent("TLogReady", logData->logId).detail("AllTags", describe(req.allTags)).detail("Locality", logData->locality);

	updater = Void();
	wait( tLogCore( self, logData, recruited, pulledRecoveryVersions ) );
	return Void();
}

ACTOR Future<Void> startSpillingInTenSeconds(TLogData* self, UID tlogId, Reference<AsyncVar<UID>> activeSharedTLog) {
	wait(delay(10));
	if (activeSharedTLog->get() != tlogId) {
		// TODO: This should fully spill, but currently doing so will cause us to no longer update poppedVersion
		// and QuietDatabase will hang thinking our TLog is behind.
		TraceEvent("SharedTLogBeginSpilling", self->dbgid).detail("NowActive", activeSharedTLog->get());
		self->targetVolatileBytes = SERVER_KNOBS->REFERENCE_SPILL_UPDATE_STORAGE_BYTE_LIMIT * 2;
	} else {
		TraceEvent("SharedTLogSkipSpilling", self->dbgid).detail("NowActive", activeSharedTLog->get());
	}
	return Void();
}

// New tLog (if !recoverFrom.size()) or restore from network
ACTOR Future<Void> tLog( IKeyValueStore* persistentData, IDiskQueue* persistentQueue, Reference<AsyncVar<ServerDBInfo>> db, LocalityData locality, PromiseStream<InitializeTLogRequest> tlogRequests, UID tlogId, UID workerID, bool restoreFromDisk, Promise<Void> oldLog, Promise<Void> recovered, std::string folder, Reference<AsyncVar<bool>> degraded, Reference<AsyncVar<UID>> activeSharedTLog ) {
	state TLogData self( tlogId, workerID, persistentData, persistentQueue, db, degraded, folder );
	state Future<Void> error = actorCollection( self.sharedActors.getFuture() );

	TraceEvent("SharedTlog", tlogId);
	try {
		if(restoreFromDisk) {
			wait( restorePersistentState( &self, locality, oldLog, recovered, tlogRequests ) );
		} else {
			wait( ioTimeoutError( checkEmptyQueue(&self) && checkRecovered(&self), SERVER_KNOBS->TLOG_MAX_CREATE_DURATION ) );
		}

		//Disk errors need a chance to kill this actor.
		wait(delay(0.000001));

		if(recovered.canBeSet()) recovered.send(Void());

		self.sharedActors.send( commitQueue(&self) );
		self.sharedActors.send( updateStorageLoop(&self) );
		self.sharedActors.send( traceRole(Role::SHARED_TRANSACTION_LOG, tlogId) );
		state Future<Void> activeSharedChange = Void();

		loop {
			choose {
				when ( InitializeTLogRequest req = waitNext(tlogRequests.getFuture() ) ) {
					if( !self.tlogCache.exists( req.recruitmentID ) ) {
						self.tlogCache.set( req.recruitmentID, req.reply.getFuture() );
						self.sharedActors.send( self.tlogCache.removeOnReady( req.recruitmentID, tLogStart( &self, req, locality ) ) );
					} else {
						forwardPromise( req.reply, self.tlogCache.get( req.recruitmentID ) );
					}
				}
				when ( wait( error ) ) { throw internal_error(); }
				when ( wait( activeSharedChange ) ) {
					if (activeSharedTLog->get() == tlogId) {
						TraceEvent("SharedTLogNowActive", self.dbgid).detail("NowActive", activeSharedTLog->get());
						self.targetVolatileBytes = SERVER_KNOBS->TLOG_SPILL_THRESHOLD;
					} else {
						stopAllTLogs(&self, tlogId);
						TraceEvent("SharedTLogQueueSpilling", self.dbgid).detail("NowActive", activeSharedTLog->get());
						self.sharedActors.send( startSpillingInTenSeconds(&self, tlogId, activeSharedTLog) );
					}
					activeSharedChange = activeSharedTLog->onChange();
				}
			}
		}
	} catch (Error& e) {
		self.terminated.send(Void());
		TraceEvent("TLogError", tlogId).error(e, true);
		if(recovered.canBeSet()) recovered.send(Void());

		while(!tlogRequests.isEmpty()) {
			tlogRequests.getFuture().pop().reply.sendError(recruitment_failed());
		}

		for( auto& it : self.id_data ) {
			if(!it.second->recoveryComplete.isSet()) {
				it.second->recoveryComplete.sendError(end_of_stream());
			}
		}

		if (tlogTerminated( &self, persistentData, self.persistentQueue, e )) {
			return Void();
		} else {
			throw;
		}
	}
}

// UNIT TESTS
struct DequeAllocatorStats {
	static int64_t allocatedBytes;
};

int64_t DequeAllocatorStats::allocatedBytes = 0;

template <class T>
struct DequeAllocator : std::allocator<T> {
	template<typename U>
	struct rebind {
		typedef DequeAllocator<U> other;
	};

	DequeAllocator() {}

	template<typename U>
	DequeAllocator(DequeAllocator<U> const& u) : std::allocator<T>(u) {}

	T* allocate(std::size_t n, std::allocator<void>::const_pointer hint = 0) {
		DequeAllocatorStats::allocatedBytes += n * sizeof(T);
		//fprintf(stderr, "Allocating %lld objects for %lld bytes (total allocated: %lld)\n", n, n * sizeof(T), DequeAllocatorStats::allocatedBytes);
		return std::allocator<T>::allocate(n, hint);
	}
	void deallocate(T* p, std::size_t n) {
		DequeAllocatorStats::allocatedBytes -= n * sizeof(T);
		//fprintf(stderr, "Deallocating %lld objects for %lld bytes (total allocated: %lld)\n", n, n * sizeof(T), DequeAllocatorStats::allocatedBytes);
		return std::allocator<T>::deallocate(p, n);
	}
};

TEST_CASE("/fdbserver/tlogserver/VersionMessagesOverheadFactor" ) {

	typedef std::pair<Version, LengthPrefixedStringRef> TestType; // type used by versionMessages

	for(int i = 1; i < 9; ++i) {
		for(int j = 0; j < 20; ++j) {
			DequeAllocatorStats::allocatedBytes = 0;
			DequeAllocator<TestType> allocator;
			std::deque<TestType, DequeAllocator<TestType>> d(allocator);

			int numElements = deterministicRandom()->randomInt(pow(10, i-1), pow(10, i));
			for(int k = 0; k < numElements; ++k) {
				d.push_back(TestType());
			}

			int removedElements = 0;//deterministicRandom()->randomInt(0, numElements); // FIXME: the overhead factor does not accurately account for removal!
			for(int k = 0; k < removedElements; ++k) {
				d.pop_front();
			}

			int64_t dequeBytes = DequeAllocatorStats::allocatedBytes + sizeof(std::deque<TestType>);
			int64_t insertedBytes = (numElements-removedElements) * sizeof(TestType);
			double overheadFactor = std::max<double>(insertedBytes, dequeBytes-10000) / insertedBytes; // We subtract 10K here as an estimated upper bound for the fixed cost of an std::deque
			//fprintf(stderr, "%d elements (%d inserted, %d removed):\n", numElements-removedElements, numElements, removedElements);
			//fprintf(stderr, "Allocated %lld bytes to store %lld bytes (%lf overhead factor)\n", dequeBytes, insertedBytes, overheadFactor);
			ASSERT(overheadFactor * 1024 <= SERVER_KNOBS->VERSION_MESSAGES_OVERHEAD_FACTOR_1024THS);
		}
	}

	return Void();
}
