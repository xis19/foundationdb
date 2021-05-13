/*
 * TLogPeekCursor.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2021 Apple Inc. and the FoundationDB project authors
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

#include "fdbserver/ptxn/TLogPeekCursor.actor.h"

#include <iterator>
#include <unordered_set>
#include <vector>

#include "fdbserver/ptxn/TLogInterface.h"
#include "flow/Error.h"

#include "flow/actorcompiler.h" // This must be the last #include

namespace ptxn {

namespace {

// The deserializer will always expect the serialized data has a header. This function provides header-only serialized
// data for the consumption of the deserializer.
const Standalone<StringRef>& emptyCursorHeader() {
	static Standalone<StringRef> empty;
	if (empty.size() == 0) {
		TeamID teamID;
		ptxn::TLogStorageServerMessageSerializer serializer(teamID);
		serializer.completeMessageWriting();
		empty = serializer.getSerialized();
	}
	return empty;
};

struct PeekRemoteContext {
	const Optional<UID> debugID;
	const TeamID teamID;

	// The last version being processed, the peek will request lastVersion + 1
	Version* pLastVersion;

	// The interface to the remote TLog server
	const std::vector<TLogInterfaceBase*>& pTLogInterfaces;

	// Deserializer
	TLogStorageServerMessageDeserializer* pDeserializer;

	// Deserializer iterator
	TLogStorageServerMessageDeserializer::iterator* pDeserializerIterator;

	// If not null, attach the arena in the reply to this arena, so the mutations will still be
	// accessible even the deserializer gets destructed.
	Arena* pAttachArena;

	PeekRemoteContext(const Optional<UID>& debugID_,
	                  const TeamID& teamID_,
	                  Version* pLastVersion_,
	                  const std::vector<TLogInterfaceBase*>& pInterfaces_,
	                  TLogStorageServerMessageDeserializer* pDeserializer_,
	                  TLogStorageServerMessageDeserializer::iterator* pDeserializerIterator_,
	                  Arena* pAttachArena_ = nullptr)
	  : debugID(debugID_), teamID(teamID_), pLastVersion(pLastVersion_), pTLogInterfaces(pInterfaces_),
	    pDeserializer(pDeserializer_), pDeserializerIterator(pDeserializerIterator_), pAttachArena(pAttachArena_) {

		for (const auto pTLogInterface : pTLogInterfaces) {
			ASSERT(pTLogInterface != nullptr);
		}

		ASSERT(pDeserializer != nullptr && pDeserializerIterator != nullptr);
	}
};

ACTOR Future<bool> peekRemote(PeekRemoteContext peekRemoteContext) {
	state TLogPeekRequest request;
	// FIXME: use loadBalancer rather than picking up a random one
	state TLogInterfaceBase* pTLogInterface =
	    peekRemoteContext
	        .pTLogInterfaces[deterministicRandom()->randomInt(0, peekRemoteContext.pTLogInterfaces.size())];

	request.debugID = peekRemoteContext.debugID;
	request.beginVersion = *peekRemoteContext.pLastVersion + 1;
	request.endVersion = -1; // we *ALWAYS* try to extract *ALL* data
	request.teamID = peekRemoteContext.teamID;

	try {
		state TLogPeekReply reply = wait(pTLogInterface->peek.getReply(request));

		peekRemoteContext.pDeserializer->reset(reply.arena, reply.data);
		*peekRemoteContext.pLastVersion = peekRemoteContext.pDeserializer->getLastVersion();
		*peekRemoteContext.pDeserializerIterator = peekRemoteContext.pDeserializer->begin();

		if (*peekRemoteContext.pDeserializerIterator == peekRemoteContext.pDeserializer->end()) {
			// No new mutations incoming, and there is no new mutations responded from TLog in this request
			return false;
		}

		if (peekRemoteContext.pAttachArena != nullptr) {
			peekRemoteContext.pAttachArena->dependsOn(reply.arena);
		}

		return true;
	} catch (Error& error) {
		// FIXME deal with possible errors
		return false;
	}
}

struct MergedPeekCursorContext {
	MergedPeekCursor::CursorContainer* pCursorPtrs;
	MergedPeekCursor::CursorHeap* pCursorHeap;
};

// Add a cursor to the CursorHeap, the cursor must hasRemaining()
void addCursorToCursorHeap(MergedPeekCursor::CursorContainer::iterator iter, MergedPeekCursor::CursorHeap& heap) {
	ASSERT((*iter)->hasRemaining());
	const VersionSubsequenceMutation& mutation = (*iter)->get();
	heap.emplace(mutation.version, mutation.subsequence, iter);
}

// Peek remote for multiple cursors, if the cursor is not exhausted, remove it from the incoming cursor list.
ACTOR Future<bool> peekRemoteForMergedCursor(MergedPeekCursorContext context) {
	state MergedPeekCursor::CursorContainer& cursorPtrs(*context.pCursorPtrs);
	state MergedPeekCursor::CursorHeap& cursorHeap(*context.pCursorHeap);
	state int numCursors(cursorPtrs.size());
	state std::vector<MergedPeekCursor::CursorContainer::iterator> peekedCursorIter;
	state std::vector<Future<bool>> peekResult;
	state int numPeeks(0);

	// Only peek for those locally-exhausted cursors
	for (std::list<std::unique_ptr<PeekCursorBase>>::iterator iter = std::begin(cursorPtrs);
	     iter != std::end(cursorPtrs);
	     ++iter) {

		if (!(*iter)->hasRemaining()) {
			// Only if the cursor has exhausted its local mutations, it needs to peek data from remote.
			peekedCursorIter.push_back(iter);
			peekResult.push_back((*iter)->remoteMoreAvailable());
			++numPeeks;
		}
	}

	// TODO what would be the proper behavior if *ONE* or a few of the cursors failed? I suppose we could fail the
	// MergedPeekCursor as a whole but is there a better way? e.g. replace it by a new cursor?
	wait(waitForAll(peekResult));

	for (int i = 0; i < numPeeks; ++i) {
		if (!peekResult[i].get()) {
			// The cursor is exhausted, drop from the list
			cursorPtrs.erase(peekedCursorIter[i]);
		} else {
			addCursorToCursorHeap(peekedCursorIter[i], cursorHeap);
		}
	}

	// If there are *ANY* remaining cursors in the list, it means they still have remaining mutations to be consumed, so
	// the return value is *NOT* related with the peek result.
	return !cursorPtrs.empty();
}

struct MergedPeekServerTeamCursorContext : public MergedPeekCursorContext {
	std::unordered_map<TeamID, CursorContainer::iterator>* pMapper;
};

// In addition to peekRemoteForMergedCursor, update the team ID/cursor mapping
ACTOR Future<bool> peekRemoteForMergedServerTeamCursor(MergedPeekServerTeamCursorContext context) {
	bool result = wait(peekRemoteForMergedCursor(static_cast<MergedPeekCursorContext>(context)));

	// Since peekRemoteForMergedCursor will drop exhausted cursors, the team ID - cursor mapping should drop invalid
	// references to those exhaustd cursors.
	std::unordered_set<TeamID> inactiveTeamIDs;
	for (std::unordered_map<TeamID, CursorContainer::iterator>::iterator iter = std::begin(*context.pMapper);
	     iter != std::end(*context.pMapper);
	     ++iter) {
		inactiveTeamIDs.insert(iter->first);
	}
	for (CursorContainer::iterator iter = std::begin(*context.pCursorPtrs); iter != std::end(*context.pCursorPtrs);
	     ++iter) {
		inactiveTeamIDs.erase(dynamic_cast<ServerTeamPeekCursor*>((*iter).get())->getTeamID());
	}
	for (std::unordered_set<TeamID>::iterator iter = std::begin(inactiveTeamIDs); iter != std::end(inactiveTeamIDs);
	     ++iter) {
		context.pMapper->erase(*iter);
	}

	return result;
}

} // anonymous namespace

PeekCursorBase::iterator::iterator(PeekCursorBase* pCursor_, bool isEndIterator_)
  : pCursor(pCursor_), isEndIterator(isEndIterator_) {}

bool PeekCursorBase::iterator::operator==(const PeekCursorBase::iterator& another) const {
	// Since the iterator is not duplicable, no two iterators equal. This is a a hack to help determining if the
	// iterator is reaching the end of the data. See documents of the constructor.
	return (!pCursor->hasRemaining() && another.isEndIterator && pCursor == another.pCursor);
}

bool PeekCursorBase::iterator::operator!=(const PeekCursorBase::iterator& another) const {
	return !this->operator==(another);
}

PeekCursorBase::iterator::reference PeekCursorBase::iterator::operator*() const {
	return pCursor->get();
}

PeekCursorBase::iterator::pointer PeekCursorBase::iterator::operator->() const {
	return &pCursor->get();
}

void PeekCursorBase::iterator::operator++() {
	pCursor->next();
}

PeekCursorBase::PeekCursorBase() : endIterator(this, true) {}

Future<bool> PeekCursorBase::remoteMoreAvailable() {
	return remoteMoreAvailableImpl();
}

const VersionSubsequenceMutation& PeekCursorBase::get() const {
	return getImpl();
}

void PeekCursorBase::next() {
	nextImpl();
}

bool PeekCursorBase::hasRemaining() const {
	return hasRemainingImpl();
}

ServerTeamPeekCursor::ServerTeamPeekCursor(const Version& beginVersion_,
                                           const TeamID& teamID_,
                                           TLogInterfaceBase* pTLogInterface_,
                                           Arena* pArena_)
  : ServerTeamPeekCursor(beginVersion_, teamID_, std::vector<TLogInterfaceBase*>{ pTLogInterface_ }, pArena_) {}

ServerTeamPeekCursor::ServerTeamPeekCursor(const Version& beginVersion_,
                                           const TeamID& teamID_,
                                           const std::vector<TLogInterfaceBase*>& pTLogInterfaces_,
                                           Arena* pArena_)
  : teamID(teamID_), pTLogInterfaces(pTLogInterfaces_), pAttachArena(pArena_), deserializer(emptyCursorHeader()),
    deserializerIter(deserializer.begin()), beginVersion(beginVersion_), lastVersion(beginVersion_ - 1) {

	for (const auto pTLogInterface : pTLogInterfaces) {
		ASSERT(pTLogInterface != nullptr);
	}
}

const TeamID& ServerTeamPeekCursor::getTeamID() const {
	return teamID;
}

const Version& ServerTeamPeekCursor::getLastVersion() const {
	return lastVersion;
}

const Version& ServerTeamPeekCursor::getBeginVersion() const {
	return beginVersion;
}

Future<bool> ServerTeamPeekCursor::remoteMoreAvailableImpl() {
	// FIXME Put debugID if necessary
	PeekRemoteContext context(
	    Optional<UID>(), getTeamID(), &lastVersion, pTLogInterfaces, &deserializer, &deserializerIter, pAttachArena);

	return peekRemote(context);
}

void ServerTeamPeekCursor::nextImpl() {
	++deserializerIter;
}

const VersionSubsequenceMutation& ServerTeamPeekCursor::getImpl() const {
	return *deserializerIter;
}

bool ServerTeamPeekCursor::hasRemainingImpl() const {
	return deserializerIter != deserializer.end();
}

IndexedCursor::IndexedCursor(const Version& version_,
                             const Subsequence& subsequence_,
                             CursorContainer::iterator pCursorPtr_)
  : version(version_), subsequence(subsequence_), pCursorPtr(pCursorPtr_) {}

MergedPeekCursor::MergedPeekCursor() : PeekCursorBase() {}

void MergedPeekCursor::addCursorToCursorHeap(CursorContainer::iterator iter) {
	ptxn::addCursorToCursorHeap(iter, cursorHeap);
}

size_t MergedPeekCursor::getNumActiveCursors() const {
	return cursorPtrs.size();
}

Future<bool> MergedPeekCursor::remoteMoreAvailableImpl() {
	if (cursorPtrs.empty()) {
		// No cursors are currently active
		return false;
	}

	return peekRemoteForMergedCursor({ &cursorPtrs, &cursorHeap });
}

void MergedPeekCursor::nextImpl() {
	auto top = cursorHeap.top();
	cursorHeap.pop();
	(*top.pCursorPtr)->next();
	if ((*top.pCursorPtr)->hasRemaining()) {
		cursorHeap.push(
		    IndexedCursor((*top.pCursorPtr)->get().version, (*top.pCursorPtr)->get().subsequence, top.pCursorPtr));
	}
}

const VersionSubsequenceMutation& MergedPeekCursor::getImpl() const {
	return (*cursorHeap.top().pCursorPtr)->get();
}

bool MergedPeekCursor::hasRemainingImpl() const {
	if (cursorPtrs.empty()) {
		return false;
	}
	// Since cursorPtrs only have non-exhausted cursors, *ANY* of locally exhausted cursors must trigger a
	// remoteMoreAvailable test. If there are no remote data, then it needs to be dropped.
	for (const auto& pCursor : cursorPtrs) {
		if (!pCursor->hasRemaining()) {
			return false;
		}
	}
	return true;
}

MergedPeekCursor::CursorContainer::iterator MergedPeekCursor::addCursorImpl(std::unique_ptr<PeekCursorBase>&& pCursor) {
	cursorPtrs.emplace_back(std::move(pCursor));
	auto iter = std::prev(std::end(cursorPtrs));
	if ((*iter)->hasRemaining()) {
		addCursorToCursorHeap(iter);
	}
	return iter;
}

MergedServerTeamPeekCursor::MergedServerTeamPeekCursor() : MergedPeekCursor() {}

MergedPeekCursor::CursorContainer::iterator MergedServerTeamPeekCursor::addCursorImpl(
    std::unique_ptr<PeekCursorBase>&& cursor) {

	ASSERT(dynamic_cast<ServerTeamPeekCursor*>(cursor.get()) != nullptr);

	auto iter = MergedPeekCursor::addCursorImpl(std::move(cursor));

	const TeamID& teamID = dynamic_cast<ServerTeamPeekCursor*>((*iter).get())->getTeamID();
	ASSERT(teamIDCursorMapper.find(teamID) == teamIDCursorMapper.end());
	teamIDCursorMapper[teamID] = iter;

	return iter;
}

std::unique_ptr<PeekCursorBase> MergedServerTeamPeekCursor::removeCursor(const TeamID& teamID) {
	auto mapperIter = teamIDCursorMapper.find(teamID);
	if (mapperIter == teamIDCursorMapper.end()) {
		return nullptr;
	}

	// The iterator to the cursor in CursorContainer
	auto iter = mapperIter->second;

	// Remove from mapper
	teamIDCursorMapper.erase(mapperIter);

	// Remove from heap, there is no simple way of removing a specific item from a std::priority_queue, so we
	// re-construct it.
	std::vector<IndexedCursor> itemsInHeap;
	while (!cursorHeap.empty()) {
		itemsInHeap.emplace_back(cursorHeap.top());
		cursorHeap.pop();
	}
	for (const auto& item : itemsInHeap) {
		if (item.pCursorPtr == iter) {
			continue;
		}
		cursorHeap.emplace(item);
	}

	std::unique_ptr<PeekCursorBase> pCursor(std::move(*iter));
	cursorPtrs.erase(iter);

	return pCursor;
}

Future<bool> MergedServerTeamPeekCursor::remoteMoreAvailableImpl() {
	if (cursorPtrs.empty()) {
		// No cursors are currently active
		return false;
	}

	return peekRemoteForMergedServerTeamCursor({ &cursorPtrs, &cursorHeap, &teamIDCursorMapper });
}

std::vector<TeamID> MergedServerTeamPeekCursor::getCursorTeamIDs() {
	std::vector<TeamID> result;
	result.reserve(teamIDCursorMapper.size());
	for (const auto& [teamID, _] : teamIDCursorMapper) {
		result.push_back(teamID);
	}
	return result;
}

// Moves the cursor so it locates to the given version/subsequence. If the version/subsequence does not exist, moves the
// cursor to the closest next mutation. If the version/subsequence is earlier than the current version/subsequence the
// cursor is located, then the code will do nothing.
ACTOR Future<Void> advanceTo(PeekCursorBase* cursor, Version version, Subsequence subsequence) {
	state PeekCursorBase::iterator iter = cursor->begin();

	loop {
		while (iter != cursor->end()) {
			// Is iter already past the version?
			if (iter->version > version) {
				return Void();
			}
			// Is iter current at the given version
			if (iter->version == version) {
				while (iter != cursor->end() && iter->subsequence < subsequence)
					++iter;
				if (iter->version > version || iter->subsequence >= subsequence) {
					return Void();
				}
			}
			++iter;
		}

		// Consumed local data, need to check remote TLog
		bool remoteAvailable = wait(cursor->remoteMoreAvailable());
		if (!remoteAvailable) {
			// The version/subsequence should be in the future
			// Throw error?
			return Void();
		}
	}
}

} // namespace ptxn