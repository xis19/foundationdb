/*
 * MasterProxyServer.actor.cpp
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

#include <algorithm>
#include <tuple>

#include "fdbclient/Atomic.h"
#include "fdbclient/DatabaseConfiguration.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/KeyRangeMap.h"
#include "fdbclient/Knobs.h"
#include "fdbclient/MasterProxyInterface.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/Notified.h"
#include "fdbclient/SystemData.h"
#include "fdbrpc/sim_validation.h"
#include "fdbrpc/Stats.h"
#include "fdbserver/ApplyMetadataMutation.h"
#include "fdbserver/ConflictSet.h"
#include "fdbserver/DataDistributorInterface.h"
#include "fdbserver/FDBExecHelper.actor.h"
#include "fdbserver/IKeyValueStore.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/LatencyBandConfig.h"
#include "fdbserver/LogSystem.h"
#include "fdbserver/LogSystemDiskQueueAdapter.h"
#include "fdbserver/MasterInterface.h"
#include "fdbserver/MutationTracking.h"
#include "fdbserver/ProxyCommitData.actor.h"
#include "fdbserver/RecoveryState.h"
#include "fdbserver/ServerDBInfo.h"
#include "fdbserver/WaitFailure.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "flow/ActorCollection.h"
#include "flow/IRandom.h"
#include "flow/Knobs.h"
#include "flow/TDMetric.actor.h"
#include "flow/Tracing.h"

#include "debug.h"

#include "flow/actorcompiler.h"  // This must be the last #include.

ACTOR Future<Void> broadcastTxnRequest(TxnStateRequest req, int sendAmount, bool sendReply) {
	state ReplyPromise<Void> reply = req.reply;
	resetReply( req );
	std::vector<Future<Void>> replies;
	int currentStream = 0;
	std::vector<Endpoint> broadcastEndpoints = req.broadcastInfo;
	for(int i = 0; i < sendAmount && currentStream < broadcastEndpoints.size(); i++) {
		std::vector<Endpoint> endpoints;
		RequestStream<TxnStateRequest> cur(broadcastEndpoints[currentStream++]);
		while(currentStream < broadcastEndpoints.size()*(i+1)/sendAmount) {
			endpoints.push_back(broadcastEndpoints[currentStream++]);
		}
		req.broadcastInfo = endpoints;
		replies.push_back(brokenPromiseToNever( cur.getReply( req ) ));
		resetReply( req );
	}
	wait( waitForAll(replies) );
	if(sendReply) {
		reply.send(Void());
	}
	return Void();
}

struct TransactionRateInfo {
	double rate;
	double limit;
	double budget;

	bool disabled;

	Smoother smoothRate;
	Smoother smoothReleased;

	TransactionRateInfo(double rate) : rate(rate), limit(0), budget(0), disabled(true), smoothRate(SERVER_KNOBS->START_TRANSACTION_RATE_WINDOW),
	                                   smoothReleased(SERVER_KNOBS->START_TRANSACTION_RATE_WINDOW) {}

	void reset() {
		// Determine the number of transactions that this proxy is allowed to release
		// Roughly speaking, this is done by computing the number of transactions over some historical window that we could
		// have started but didn't, and making that our limit. More precisely, we track a smoothed rate limit and release rate,
		// the difference of which is the rate of additional transactions that we could have released based on that window.
		// Then we multiply by the window size to get a number of transactions.
		//
		// Limit can be negative in the event that we are releasing more transactions than we are allowed (due to the use of
		// our budget or because of higher priority transactions).
		double releaseRate = smoothRate.smoothTotal() - smoothReleased.smoothRate();
		limit = SERVER_KNOBS->START_TRANSACTION_RATE_WINDOW * releaseRate;
	}

	bool canStart(int64_t numAlreadyStarted, int64_t count) {
		return numAlreadyStarted + count <= std::min(limit + budget, SERVER_KNOBS->START_TRANSACTION_MAX_TRANSACTIONS_TO_START);
	}

	void updateBudget(int64_t numStartedAtPriority, bool queueEmptyAtPriority, double elapsed) {
		// Update the budget to accumulate any extra capacity available or remove any excess that was used.
		// The actual delta is the portion of the limit we didn't use multiplied by the fraction of the window that elapsed.
		//
		// We may have exceeded our limit due to the budget or because of higher priority transactions, in which case this
		// delta will be negative. The delta can also be negative in the event that our limit was negative, which can happen
		// if we had already started more transactions in our window than our rate would have allowed.
		//
		// This budget has the property that when the budget is required to start transactions (because batches are big),
		// the sum limit+budget will increase linearly from 0 to the batch size over time and decrease by the batch size
		// upon starting a batch. In other words, this works equivalently to a model where we linearly accumulate budget over
		// time in the case that our batches are too big to take advantage of the window based limits.
		budget = std::max(0.0, budget + elapsed * (limit - numStartedAtPriority) / SERVER_KNOBS->START_TRANSACTION_RATE_WINDOW);

		// If we are emptying out the queue of requests, then we don't need to carry much budget forward
		// If we did keep accumulating budget, then our responsiveness to changes in workflow could be compromised
		if(queueEmptyAtPriority) {
			budget = std::min(budget, SERVER_KNOBS->START_TRANSACTION_MAX_EMPTY_QUEUE_BUDGET);
		}

		smoothReleased.addDelta(numStartedAtPriority);
	}

	void disable() {
		disabled = true;
		rate = 0;
		smoothRate.reset(0);
	}

	void setRate(double rate) {
		ASSERT(rate >= 0 && rate != std::numeric_limits<double>::infinity() && !std::isnan(rate));

		this->rate = rate;
		if(disabled) {
			smoothRate.reset(rate);
			disabled = false;
		}
		else {
			smoothRate.setTotal(rate);
		}
	}
};

ACTOR Future<Void> getRate(UID myID, Reference<AsyncVar<ServerDBInfo>> db, int64_t* inTransactionCount,
                           int64_t* inBatchTransactionCount, TransactionRateInfo* transactionRateInfo,
                           TransactionRateInfo* batchTransactionRateInfo, GetHealthMetricsReply* healthMetricsReply,
                           GetHealthMetricsReply* detailedHealthMetricsReply,
                           TransactionTagMap<uint64_t>* transactionTagCounter,
                           PrioritizedTransactionTagMap<ClientTagThrottleLimits>* throttledTags,
                           TransactionTagMap<TransactionCommitCostEstimation>* transactionTagCommitCostEst) {
	state Future<Void> nextRequestTimer = Never();
	state Future<Void> leaseTimeout = Never();
	state Future<GetRateInfoReply> reply = Never();
	state double lastDetailedReply = 0.0; // request detailed metrics immediately
	state bool expectingDetailedReply = false;
	state int64_t lastTC = 0;

	if (db->get().ratekeeper.present()) nextRequestTimer = Void();
	loop choose {
		when ( wait( db->onChange() ) ) {
			if ( db->get().ratekeeper.present() ) {
				TraceEvent("ProxyRatekeeperChanged", myID)
				.detail("RKID", db->get().ratekeeper.get().id());
				nextRequestTimer = Void();  // trigger GetRate request
			} else {
				TraceEvent("ProxyRatekeeperDied", myID);
				nextRequestTimer = Never();
				reply = Never();
			}
		}
		when ( wait( nextRequestTimer ) ) {
			nextRequestTimer = Never();
			bool detailed = now() - lastDetailedReply > SERVER_KNOBS->DETAILED_METRIC_UPDATE_RATE;

			TransactionTagMap<uint64_t> tagCounts;
			for(auto itr : *throttledTags) {
				for(auto priorityThrottles : itr.second) {
					tagCounts[priorityThrottles.first] = (*transactionTagCounter)[priorityThrottles.first];
				}
			}
			reply = brokenPromiseToNever(db->get().ratekeeper.get().getRateInfo.getReply(
			    GetRateInfoRequest(myID, *inTransactionCount, *inBatchTransactionCount, tagCounts,
			                       *transactionTagCommitCostEst, detailed)));
			transactionTagCounter->clear();
			transactionTagCommitCostEst->clear();
			expectingDetailedReply = detailed;
		}
		when ( GetRateInfoReply rep = wait(reply) ) {
			reply = Never();

			transactionRateInfo->setRate(rep.transactionRate);
			batchTransactionRateInfo->setRate(rep.batchTransactionRate);
			//TraceEvent("MasterProxyRate", myID).detail("Rate", rep.transactionRate).detail("BatchRate", rep.batchTransactionRate).detail("Lease", rep.leaseDuration).detail("ReleasedTransactions", *inTransactionCount - lastTC);
			lastTC = *inTransactionCount;
			leaseTimeout = delay(rep.leaseDuration);
			nextRequestTimer = delayJittered(rep.leaseDuration / 2);
			healthMetricsReply->update(rep.healthMetrics, expectingDetailedReply, true);
			if (expectingDetailedReply) {
				detailedHealthMetricsReply->update(rep.healthMetrics, true, true);
				lastDetailedReply = now();
			}

			// Replace our throttles with what was sent by ratekeeper. Because we do this,
			// we are not required to expire tags out of the map
			if(rep.throttledTags.present()) {
				*throttledTags = std::move(rep.throttledTags.get());
			}
		}
		when ( wait( leaseTimeout ) ) {
			transactionRateInfo->disable();
			batchTransactionRateInfo->disable();
			TraceEvent(SevWarn, "MasterProxyRateLeaseExpired", myID).suppressFor(5.0);
			//TraceEvent("MasterProxyRate", myID).detail("Rate", 0.0).detail("BatchRate", 0.0).detail("Lease", 0);
			leaseTimeout = Never();
		}
	}
}

ACTOR Future<Void> queueTransactionStartRequests(
	Reference<AsyncVar<ServerDBInfo>> db,
	SpannedDeque<GetReadVersionRequest> *systemQueue,
	SpannedDeque<GetReadVersionRequest> *defaultQueue,
	SpannedDeque<GetReadVersionRequest> *batchQueue,
	FutureStream<GetReadVersionRequest> readVersionRequests,
	PromiseStream<Void> GRVTimer, double *lastGRVTime,
	double *GRVBatchTime, FutureStream<double> replyTimes,
	ProxyStats* stats, TransactionRateInfo* batchRateInfo,
	TransactionTagMap<uint64_t>* transactionTagCounter)
{
	loop choose{
		when(GetReadVersionRequest req = waitNext(readVersionRequests)) {
			//WARNING: this code is run at a high priority, so it needs to do as little work as possible
			stats->addRequest();
			if( stats->txnRequestIn.getValue() - stats->txnRequestOut.getValue() > SERVER_KNOBS->START_TRANSACTION_MAX_QUEUE_SIZE ) {
				++stats->txnRequestErrors;
				//FIXME: send an error instead of giving an unreadable version when the client can support the error: req.reply.sendError(proxy_memory_limit_exceeded());
				GetReadVersionReply rep;
				rep.version = 1;
				rep.locked = true;
				req.reply.send(rep);
				TraceEvent(SevWarnAlways, "ProxyGRVThresholdExceeded").suppressFor(60);
			} else {
				// TODO: check whether this is reasonable to do in the fast path
				for(auto tag : req.tags) {
					(*transactionTagCounter)[tag.first] += tag.second;
				}

				if (req.debugID.present())
					g_traceBatch.addEvent("TransactionDebug", req.debugID.get().first(), "MasterProxyServer.queueTransactionStartRequests.Before");

				if (systemQueue->empty() && defaultQueue->empty() && batchQueue->empty()) {
					forwardPromise(GRVTimer, delayJittered(std::max(0.0, *GRVBatchTime - (now() - *lastGRVTime)), TaskPriority::ProxyGRVTimer));
				}

				++stats->txnRequestIn;
				stats->txnStartIn += req.transactionCount;
				if (req.priority >= TransactionPriority::IMMEDIATE) {
					stats->txnSystemPriorityStartIn += req.transactionCount;
					systemQueue->push_back(req);
					systemQueue->span.addParent(req.spanContext);
				} else if (req.priority >= TransactionPriority::DEFAULT) {
					stats->txnDefaultPriorityStartIn += req.transactionCount;
					defaultQueue->push_back(req);
					defaultQueue->span.addParent(req.spanContext);
				} else {
					// Return error for batch_priority GRV requests
					int64_t proxiesCount = std::max((int)db->get().client.proxies.size(), 1);
					if (batchRateInfo->rate <= (1.0 / proxiesCount)) {
						req.reply.sendError(batch_transaction_throttled());
						stats->txnThrottled += req.transactionCount;
						continue;
					}

					stats->txnBatchPriorityStartIn += req.transactionCount;
					batchQueue->push_back(req);
					batchQueue->span.addParent(req.spanContext);
				}
			}
		}
		// dynamic batching monitors reply latencies
		when(double reply_latency = waitNext(replyTimes)) {
			double target_latency = reply_latency * SERVER_KNOBS->START_TRANSACTION_BATCH_INTERVAL_LATENCY_FRACTION;
			*GRVBatchTime = std::max(
			    SERVER_KNOBS->START_TRANSACTION_BATCH_INTERVAL_MIN,
			    std::min(SERVER_KNOBS->START_TRANSACTION_BATCH_INTERVAL_MAX,
			             target_latency * SERVER_KNOBS->START_TRANSACTION_BATCH_INTERVAL_SMOOTHER_ALPHA +
			                 *GRVBatchTime * (1 - SERVER_KNOBS->START_TRANSACTION_BATCH_INTERVAL_SMOOTHER_ALPHA)));
		}
	}
}

ACTOR void discardCommit(UID id, Future<LogSystemDiskQueueAdapter::CommitMessage> fcm, Future<Void> dummyCommitState) {
	ASSERT(!dummyCommitState.isReady());
	LogSystemDiskQueueAdapter::CommitMessage cm = wait(fcm);
	TraceEvent("Discarding", id).detail("Count", cm.messages.size());
	cm.acknowledge.send(Void());
	ASSERT(dummyCommitState.isReady());
}

struct ResolutionRequestBuilder {
	ProxyCommitData* self;
	vector<ResolveTransactionBatchRequest> requests;
	vector<vector<int>> transactionResolverMap;
	vector<CommitTransactionRef*> outTr;
	std::vector<std::vector<std::vector<int>>>
	    txReadConflictRangeIndexMap; // Used to report conflicting keys, the format is
	                                 // [CommitTransactionRef_Index][Resolver_Index][Read_Conflict_Range_Index_on_Resolver]
	                                 // -> read_conflict_range's original index in the commitTransactionRef

	ResolutionRequestBuilder(ProxyCommitData* self, Version version, Version prevVersion, Version lastReceivedVersion,
	                         Span& parentSpan)
	  : self(self), requests(self->resolvers.size()) {
		for (auto& req : requests) {
			req.spanContext = parentSpan.context;
			req.prevVersion = prevVersion;
			req.version = version;
			req.lastReceivedVersion = lastReceivedVersion;
		}
	}

	void setSplitTransaction(const SplitTransaction& splitTransaction) {
		for (auto& request : requests) {
			request.splitTransaction = splitTransaction;
		}
	}

	CommitTransactionRef& getOutTransaction(int resolver, Version read_snapshot) {
		CommitTransactionRef *& out = outTr[resolver];
		if (!out) {
			ResolveTransactionBatchRequest& request = requests[resolver];
			request.transactions.resize(request.arena, request.transactions.size() + 1);
			out = &request.transactions.back();
			out->read_snapshot = read_snapshot;
		}
		return *out;
	}

	void addTransaction(CommitTransactionRef& trIn, int transactionNumberInBatch) {
		// SOMEDAY: There are a couple of unnecessary O( # resolvers ) steps here
		outTr.assign(requests.size(), NULL);
		ASSERT( transactionNumberInBatch >= 0 && transactionNumberInBatch < 32768 );

		bool isTXNStateTransaction = false;
		for (auto & m : trIn.mutations) {
			if (m.type == MutationRef::SetVersionstampedKey) {
				transformVersionstampMutation( m, &MutationRef::param1, requests[0].version, transactionNumberInBatch );
				trIn.write_conflict_ranges.push_back( requests[0].arena, singleKeyRange( m.param1, requests[0].arena ) );
			} else if (m.type == MutationRef::SetVersionstampedValue) {
				transformVersionstampMutation( m, &MutationRef::param2, requests[0].version, transactionNumberInBatch );
			}
			if (isMetadataMutation(m)) {
				isTXNStateTransaction = true;
				getOutTransaction(0, trIn.read_snapshot).mutations.push_back(requests[0].arena, m);
			}
		}
		std::vector<std::vector<int>> rCRIndexMap(
		    requests.size()); // [resolver_index][read_conflict_range_index_on_the_resolver]
		                      // -> read_conflict_range's original index
		for (int idx = 0; idx < trIn.read_conflict_ranges.size(); ++idx) {
			const auto& r = trIn.read_conflict_ranges[idx];
			auto ranges = self->keyResolvers.intersectingRanges( r );
			std::set<int> resolvers;
			for(auto &ir : ranges) {
				auto& version_resolver = ir.value();
				for(int i = version_resolver.size()-1; i >= 0; i--) {
					resolvers.insert(version_resolver[i].second);
					if( version_resolver[i].first < trIn.read_snapshot )
						break;
				}
			}
			ASSERT(resolvers.size());
			for (int resolver : resolvers) {
				getOutTransaction( resolver, trIn.read_snapshot ).read_conflict_ranges.push_back( requests[resolver].arena, r );
				rCRIndexMap[resolver].push_back(idx);
			}
		}
		txReadConflictRangeIndexMap.push_back(std::move(rCRIndexMap));
		for(auto& r : trIn.write_conflict_ranges) {
			auto ranges = self->keyResolvers.intersectingRanges( r );
			std::set<int> resolvers;
			for(auto &ir : ranges)
				resolvers.insert(ir.value().back().second);
			ASSERT(resolvers.size());
			for(int resolver : resolvers)
				getOutTransaction( resolver, trIn.read_snapshot ).write_conflict_ranges.push_back( requests[resolver].arena, r );
		}
		if (isTXNStateTransaction)
			for (int r = 0; r<requests.size(); r++) {
				int transactionNumberInRequest = &getOutTransaction(r, trIn.read_snapshot) - requests[r].transactions.begin();
				requests[r].txnStateTransactions.push_back(requests[r].arena, transactionNumberInRequest);
			}

		vector<int> resolversUsed;
		for (int r = 0; r<outTr.size(); r++)
			if (outTr[r]) {
				resolversUsed.push_back(r);
				outTr[r]->report_conflicting_keys = trIn.report_conflicting_keys;
			}
		transactionResolverMap.emplace_back(std::move(resolversUsed));
	}
};

ACTOR Future<Void> commitBatcher(ProxyCommitData *commitData, PromiseStream<std::pair<std::vector<CommitTransactionRequest>, int> > out, FutureStream<CommitTransactionRequest> in, int desiredBytes, int64_t memBytesLimit) {
	wait(delayJittered(commitData->commitBatchInterval, TaskPriority::ProxyCommitBatcher));

	state double lastBatch = 0;

	loop{
		state Future<Void> timeout;
		state std::vector<CommitTransactionRequest> batch;
		state int batchBytes = 0;

		if(SERVER_KNOBS->MAX_COMMIT_BATCH_INTERVAL <= 0) {
			timeout = Never();
		}
		else {
			timeout = delayJittered(SERVER_KNOBS->MAX_COMMIT_BATCH_INTERVAL, TaskPriority::ProxyCommitBatcher);
		}

		while(!timeout.isReady() && !(batch.size() == SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_COUNT_MAX || batchBytes >= desiredBytes)) {
			choose{
				when(CommitTransactionRequest req = waitNext(in)) {
					//WARNING: this code is run at a high priority, so it needs to do as little work as possible
					commitData->stats.addRequest();
					int bytes = getBytes(req);

					// Drop requests if memory is under severe pressure
					if(commitData->commitBatchesMemBytesCount + bytes > memBytesLimit) {
						++commitData->stats.txnCommitErrors;
						req.reply.sendError(proxy_memory_limit_exceeded());
						TraceEvent(SevWarnAlways, "ProxyCommitBatchMemoryThresholdExceeded").suppressFor(60).detail("MemBytesCount", commitData->commitBatchesMemBytesCount).detail("MemLimit", memBytesLimit);
						continue;
					}

					if (bytes > FLOW_KNOBS->PACKET_WARNING) {
						TraceEvent(!g_network->isSimulated() ? SevWarnAlways : SevWarn, "LargeTransaction")
						    .suppressFor(1.0)
						    .detail("Size", bytes)
						    .detail("Client", req.reply.getEndpoint().getPrimaryAddress());
					}
					++commitData->stats.txnCommitIn;

					if(req.debugID.present()) {
						g_traceBatch.addEvent("CommitDebug", req.debugID.get().first(), "MasterProxyServer.batcher");
					}

					if(!batch.size()) {
						if(now() - lastBatch > commitData->commitBatchInterval) {
							timeout = delayJittered(SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_INTERVAL_FROM_IDLE, TaskPriority::ProxyCommitBatcher);
						}
						else {
							timeout = delayJittered(commitData->commitBatchInterval - (now() - lastBatch), TaskPriority::ProxyCommitBatcher);
						}
					}

					if((batchBytes + bytes > CLIENT_KNOBS->TRANSACTION_SIZE_LIMIT || req.firstInBatch()) && batch.size()) {
						out.send({ std::move(batch), batchBytes });
						lastBatch = now();
						timeout = delayJittered(commitData->commitBatchInterval, TaskPriority::ProxyCommitBatcher);
						batch.clear();
						batchBytes = 0;
					}

					if (req.splitTransaction.present()) {
						// The transaction is a split transaction, or part of
						// a big transaction. In this case, all parts of the
						// transactions should have the same version. In some
						// cases. Consider the following schema:
						//                   /-   TS1 -- Proxy1 (TS1P1)
						// 	 Transaction 1  ---   TS1 -- Proxy2 (TS1P2)
						//                   \-   TS1 -- Proxy3 (TS1P3)
						// Assume TS1P1 is processed with Version TS1V, it is
						// expected that TS1V will be used for TS1P2 and TS1P3.
						// Now assume there is another transaction TS2, having
						// similar distribution TS2P1, TS2P2 and TS2P3, with
						// version TS2V.
						// If TS1P2 and TS2P2 are batched now, then the master
						// will be confused -- should it return TS1V or TS2V?
						// A race condition is then introduced. The easiest way
						// is simple -- do *NOT* batch any split transaction.
						// This is acceptable since for split transaction, each
						// part is already big enough.
						//
						// NOTE: In fdbclient/MasterProxyInterface.cpp,
						// prepareSplitTransactions, it is guaranteed that the
						// split transaction has flag FLAG_FIRST_IN_BATCH.
						COUT << "commitBatcher: SplitID found: " << req.splitTransaction.get().id.toString()
						     << std::endl;

						// NOTE: Since the bytes from the split request is not
						// part of the batch, we have to manually add bytes
						// to total batches count.
						out.send({ { req }, bytes });
						commitData->commitBatchesMemBytesCount += bytes;

						lastBatch = now();
						timeout = delayJittered(commitData->commitBatchInterval, TaskPriority::ProxyCommitBatcher);

						batch.clear();
						batchBytes = 0;
					} else {
						batch.push_back(req);
						batchBytes += bytes;
						commitData->commitBatchesMemBytesCount += bytes;
					}
				}
				when(wait(timeout)) {}
			}
		}
		out.send({ std::move(batch), batchBytes });
		lastBatch = now();
	}
}

void createWhitelistBinPathVec(const std::string& binPath, vector<Standalone<StringRef>>& binPathVec) {
	TraceEvent(SevDebug, "BinPathConverter").detail("Input", binPath);
	StringRef input(binPath);
	while (input != StringRef()) {
		StringRef token = input.eat(LiteralStringRef(","));
		if (token != StringRef()) {
			const uint8_t* ptr = token.begin();
			while (ptr != token.end() && *ptr == ' ') {
				ptr++;
			}
			if (ptr != token.end()) {
				Standalone<StringRef> newElement(token.substr(ptr - token.begin()));
				TraceEvent(SevDebug, "BinPathItem").detail("Element", newElement);
				binPathVec.push_back(newElement);
			}
		}
	}
	return;
}

bool isWhitelisted(const vector<Standalone<StringRef>>& binPathVec, StringRef binPath) {
	TraceEvent("BinPath").detail("Value", binPath);
	for (const auto& item : binPathVec) {
		TraceEvent("Element").detail("Value", item);
	}
	return std::find(binPathVec.begin(), binPathVec.end(), binPath) != binPathVec.end();
}

ACTOR Future<Void> addBackupMutations(ProxyCommitData* self, std::map<Key, MutationListRef>* logRangeMutations,
                                      LogPushData* toCommit, Version commitVersion, double* computeDuration, double* computeStart) {
	state std::map<Key, MutationListRef>::iterator logRangeMutation = logRangeMutations->begin();
	state int32_t version = commitVersion / CLIENT_KNOBS->LOG_RANGE_BLOCK_SIZE;
	state int yieldBytes = 0;
	state BinaryWriter valueWriter(Unversioned());

	// Serialize the log range mutations within the map
	for (; logRangeMutation != logRangeMutations->end(); ++logRangeMutation)
	{
		//FIXME: this is re-implementing the serialize function of MutationListRef in order to have a yield
		valueWriter = BinaryWriter(IncludeVersion(ProtocolVersion::withBackupMutations()));
		valueWriter << logRangeMutation->second.totalSize();

		state MutationListRef::Blob* blobIter = logRangeMutation->second.blob_begin;
		while(blobIter) {
			if(yieldBytes > SERVER_KNOBS->DESIRED_TOTAL_BYTES) {
				yieldBytes = 0;
				if(g_network->check_yield(TaskPriority::ProxyCommitYield1)) {
					*computeDuration += g_network->timer() - *computeStart;
					wait(delay(0, TaskPriority::ProxyCommitYield1));
					*computeStart = g_network->timer();
				}
			}
			valueWriter.serializeBytes(blobIter->data);
			yieldBytes += blobIter->data.size();
			blobIter = blobIter->next;
		}

		Key val = valueWriter.toValue();

		BinaryWriter wr(Unversioned());

		// Serialize the log destination
		wr.serializeBytes( logRangeMutation->first );

		// Write the log keys and version information
		wr << (uint8_t)hashlittle(&version, sizeof(version), 0);
		wr << bigEndian64(commitVersion);

		MutationRef backupMutation;
		backupMutation.type = MutationRef::SetValue;
		uint32_t* partBuffer = NULL;

		for (int part = 0; part * CLIENT_KNOBS->MUTATION_BLOCK_SIZE < val.size(); part++) {

			// Assign the second parameter as the part
			backupMutation.param2 = val.substr(part * CLIENT_KNOBS->MUTATION_BLOCK_SIZE,
				std::min(val.size() - part * CLIENT_KNOBS->MUTATION_BLOCK_SIZE, CLIENT_KNOBS->MUTATION_BLOCK_SIZE));

			// Write the last part of the mutation to the serialization, if the buffer is not defined
			if (!partBuffer) {
				// Serialize the part to the writer
				wr << bigEndian32(part);

				// Define the last buffer part
				partBuffer = (uint32_t*) ((char*) wr.getData() + wr.getLength() - sizeof(uint32_t));
			}
			else {
				*partBuffer = bigEndian32(part);
			}

			// Define the mutation type and and location
			backupMutation.param1 = wr.toValue();
			ASSERT( backupMutation.param1.startsWith(logRangeMutation->first) );  // We are writing into the configured destination

			auto& tags = self->tagsForKey(backupMutation.param1);
			toCommit->addTags(tags);
			toCommit->addTypedMessage(backupMutation);

//			if (DEBUG_MUTATION("BackupProxyCommit", commitVersion, backupMutation)) {
//				TraceEvent("BackupProxyCommitTo", self->dbgid).detail("To", describe(tags)).detail("BackupMutation", backupMutation.toString())
//					.detail("BackupMutationSize", val.size()).detail("Version", commitVersion).detail("DestPath", logRangeMutation.first)
//					.detail("PartIndex", part).detail("PartIndexEndian", bigEndian32(part)).detail("PartData", backupMutation.param1);
//			}
		}
	}
	return Void();
}

ACTOR Future<Void> releaseResolvingAfter(ProxyCommitData* self, Future<Void> releaseDelay, int64_t localBatchNumber) {
	wait(releaseDelay);
	ASSERT(self->latestLocalCommitBatchResolving.get() == localBatchNumber-1);
	self->latestLocalCommitBatchResolving.set(localBatchNumber);
	return Void();
}

namespace CommitBatch {

struct CommitBatchContext {
	using StoreCommit_t = std::vector<std::pair<Future<LogSystemDiskQueueAdapter::CommitMessage>, Future<Void>>>;

	ProxyCommitData* const pProxyCommitData;
	std::vector<CommitTransactionRequest> trs;
	int currentBatchMemBytesCount;

	double startTime;

	Optional<UID> debugID;

	bool forceRecovery = false;

	int64_t localBatchNumber;
	LogPushData toCommit;

	int batchOperations = 0;

	Span span = Span("MP:commitBatch"_loc);

	int64_t batchBytes = 0;

	int latencyBucket = 0;

	Version commitVersion;
	Version prevVersion;

	int64_t maxTransactionBytes;
	std::vector<std::vector<int>> transactionResolverMap;
	std::vector<std::vector<std::vector<int>>> txReadConflictRangeIndexMap;

	Future<Void> releaseDelay;
	Future<Void> releaseFuture;

	std::vector<ResolveTransactionBatchReply> resolution;

	double computeStart;
	double computeDuration = 0;

	Arena arena;

	/// true if the batch is the 1st batch for this proxy, additional metadata
	/// processing is involved for this batch.
	bool isMyFirstBatch;
	bool firstStateMutations;

	Optional<Value> oldCoordinators;

	StoreCommit_t storeCommits;

	std::vector<uint8_t> committed;

	Optional<Key> lockedKey;
	bool locked;

	int commitCount = 0;

	std::vector<int> nextTr;

	bool lockedAfter;

	Optional<Value> metadataVersionAfter;

	int mutationCount = 0;
	int mutationBytes = 0;

	std::map<Key, MutationListRef> logRangeMutations;
	Arena logRangeMutationsArena;

	int transactionNum = 0;
	int yieldBytes = 0;

	LogSystemDiskQueueAdapter::CommitMessage msg;

	Future<Version> loggingComplete;

	double commitStartTime;

	CommitBatchContext(ProxyCommitData*, const std::vector<CommitTransactionRequest>*, const int);

	void setupTraceBatch();

private:
	void evaluateBatchSize();
};

CommitBatchContext::CommitBatchContext(ProxyCommitData* const pProxyCommitData_,
                                       const std::vector<CommitTransactionRequest>* trs_,
                                       const int currentBatchMemBytesCount)
  :

    pProxyCommitData(pProxyCommitData_), trs(std::move(*const_cast<std::vector<CommitTransactionRequest>*>(trs_))),
    currentBatchMemBytesCount(currentBatchMemBytesCount),

    startTime(g_network->now()),

    localBatchNumber(++pProxyCommitData->localCommitBatchesStarted), toCommit(pProxyCommitData->logSystem),

    committed(trs.size()) {

	evaluateBatchSize();

	if (batchOperations != 0) {
		latencyBucket = std::min<int>(
			SERVER_KNOBS->PROXY_COMPUTE_BUCKETS - 1,
			SERVER_KNOBS->PROXY_COMPUTE_BUCKETS * batchBytes /
				(batchOperations * (
					CLIENT_KNOBS->VALUE_SIZE_LIMIT +
					CLIENT_KNOBS->KEY_SIZE_LIMIT
				))
		);
	}

	// since we are using just the former to limit the number of versions actually in flight!
	ASSERT(SERVER_KNOBS->MAX_READ_TRANSACTION_LIFE_VERSIONS <= SERVER_KNOBS->MAX_VERSIONS_IN_FLIGHT);
}

void CommitBatchContext::setupTraceBatch() {
	for (const auto& tr : trs) {
		if (tr.debugID.present()) {
			if (!debugID.present()) {
				debugID = nondeterministicRandom()->randomUniqueID();
			}

			g_traceBatch.addAttach(
				"CommitAttachID",
				tr.debugID.get().first(),
				debugID.get().first()
			);
		}
		span.addParent(tr.spanContext);
	}

	if (debugID.present()) {
		g_traceBatch.addEvent(
			"CommitDebug",
			debugID.get().first(),
			"MasterProxyServer.commitBatch.Before"
		);
	}
}

void CommitBatchContext::evaluateBatchSize() {
	for (const auto& tr : trs) {
		const auto& mutations = tr.transaction.mutations;
		batchOperations += mutations.size();
		batchBytes += mutations.expectedSize();
	}
}

ACTOR Future<Void> preresolutionProcessing(CommitBatchContext* self) {

	state ProxyCommitData* const pProxyCommitData = self->pProxyCommitData;
	state std::vector<CommitTransactionRequest>& trs = self->trs;
	state const int64_t localBatchNumber = self->localBatchNumber;
	state const int latencyBucket = self->latencyBucket;
	state const Optional<UID>& debugID = self->debugID;

	// Pre-resolution the commits
	TEST(pProxyCommitData->latestLocalCommitBatchResolving.get() < localBatchNumber - 1);
	wait(pProxyCommitData->latestLocalCommitBatchResolving.whenAtLeast(localBatchNumber - 1));
	self->releaseDelay = delay(
		std::min(SERVER_KNOBS->MAX_PROXY_COMPUTE,
			self->batchOperations * pProxyCommitData->commitComputePerOperation[latencyBucket]),
		TaskPriority::ProxyMasterVersionReply
	);

	if (debugID.present()) {
		g_traceBatch.addEvent(
			"CommitDebug", debugID.get().first(),
			"MasterProxyServer.commitBatch.GettingCommitVersion"
		);
	}

	GetCommitVersionRequest req(self->span.context, pProxyCommitData->commitVersionRequestNumber++,
	                            pProxyCommitData->mostRecentProcessedRequestNumber, pProxyCommitData->dbgid);
	// NOTE A split transaction will only have a single item -- it will not be
	// batched.
	if (trs.size() == 1 && trs.front().splitTransaction.present()) {
		req.splitID = trs.front().splitTransaction.get().id;
		COUT << "Split ID: " << req.splitID.get().toString() << std::endl;

		for (int i = 0; i < trs[0].transaction.mutations.size(); ++i) {
			auto& m = trs[0].transaction.mutations[i];
			std::cout << "  key(" << m.param1.toString() << ")   value(" << m.param2.toString() << ")" << std::endl;
		}
	}
	GetCommitVersionReply versionReply = wait(brokenPromiseToNever(
		pProxyCommitData->master.getCommitVersion.getReply(
			req, TaskPriority::ProxyMasterVersionReply
		)
	));

	COUT << "Commit version: " << versionReply.version << std::endl;

	pProxyCommitData->mostRecentProcessedRequestNumber = versionReply.requestNum;

	pProxyCommitData->stats.txnCommitVersionAssigned += trs.size();
	pProxyCommitData->stats.lastCommitVersionAssigned = versionReply.version;

	self->commitVersion = versionReply.version;
	self->prevVersion = versionReply.prevVersion;

	for(auto it : versionReply.resolverChanges) {
		auto rs = pProxyCommitData->keyResolvers.modify(it.range);
		for(auto r = rs.begin(); r != rs.end(); ++r)
			r->value().emplace_back(versionReply.resolverChangesVersion,it.dest);
	}

	//TraceEvent("ProxyGotVer", pProxyContext->dbgid).detail("Commit", commitVersion).detail("Prev", prevVersion);

	if (debugID.present()) {
		g_traceBatch.addEvent(
			"CommitDebug", debugID.get().first(),
			"MasterProxyServer.commitBatch.GotCommitVersion"
		);
	}

	return Void();
}

ACTOR Future<Void> getResolution(CommitBatchContext* self) {
	// Sending these requests is the fuzzy border between phase 1 and phase 2; it could conceivably overlap with
	// resolution processing but is still using CPU
	ProxyCommitData* pProxyCommitData = self->pProxyCommitData;
	std::vector<CommitTransactionRequest>& trs = self->trs;

	ResolutionRequestBuilder requests(
		pProxyCommitData,
		self->commitVersion,
		self->prevVersion,
		pProxyCommitData->version,
        self->span
	);

	if (trs.size() > 0 && trs[0].splitTransaction.present()) {
		// Transaction should not be batched if the split flag is on
		ASSERT(trs.size() == 1);
		requests.setSplitTransaction(trs[0].splitTransaction.get());
	}

	int conflictRangeCount = 0;
	self->maxTransactionBytes = 0;
	for (int t = 0; t < trs.size(); t++) {
		requests.addTransaction(trs[t].transaction, t);
		conflictRangeCount +=
		    trs[t].transaction.read_conflict_ranges.size() + trs[t].transaction.write_conflict_ranges.size();
		//TraceEvent("MPTransactionDump", self->dbgid).detail("Snapshot", trs[t].transaction.read_snapshot);
		//for(auto& m : trs[t].transaction.mutations)
		self->maxTransactionBytes = std::max<int64_t>(
			self->maxTransactionBytes, trs[t].transaction.expectedSize()
		);
		//	TraceEvent("MPTransactionsDump", self->dbgid).detail("Mutation", m.toString());
	}
	pProxyCommitData->stats.conflictRanges += conflictRangeCount;

	for (int r = 1; r < pProxyCommitData->resolvers.size(); r++)
		ASSERT(requests.requests[r].txnStateTransactions.size() ==
			requests.requests[0].txnStateTransactions.size());

	pProxyCommitData->stats.txnCommitResolving += trs.size();
	std::vector<Future<ResolveTransactionBatchReply>> replies;
	for (int r = 0; r < pProxyCommitData->resolvers.size(); r++) {
		requests.requests[r].debugID = self->debugID;
		replies.push_back(brokenPromiseToNever(
			pProxyCommitData->resolvers[r].resolve.getReply(
				requests.requests[r], TaskPriority::ProxyResolverReply)));
	}

	self->transactionResolverMap.swap(requests.transactionResolverMap);
	// Used to report conflicting keys
	self->txReadConflictRangeIndexMap.swap(requests.txReadConflictRangeIndexMap);
	self->releaseFuture = releaseResolvingAfter(
		pProxyCommitData, self->releaseDelay, self->localBatchNumber
	);

	// Wait for the final resolution
	std::vector<ResolveTransactionBatchReply> resolutionResp = wait(getAll(replies));
	self->resolution.swap(*const_cast<std::vector<ResolveTransactionBatchReply>*>(&resolutionResp));

	if (self->debugID.present()) {
		g_traceBatch.addEvent(
			"CommitDebug", self->debugID.get().first(),
			"MasterProxyServer.commitBatch.AfterResolution"
		);
	}

	return Void();
}

void assertResolutionStateMutationsSizeConsistent(
		const std::vector<ResolveTransactionBatchReply>& resolution) {

	for (int r = 1; r < resolution.size(); r++) {
		ASSERT(resolution[r].stateMutations.size() == resolution[0].stateMutations.size());
		for(int s = 0; s < resolution[r].stateMutations.size(); s++) {
			ASSERT(resolution[r].stateMutations[s].size() == resolution[0].stateMutations[s].size());
		}
	}
}

// Compute and apply "metadata" effects of each other proxy's most recent batch
void applyMetadataEffect(CommitBatchContext* self) {
	bool initialState = self->isMyFirstBatch;
	self->firstStateMutations = self->isMyFirstBatch;
	for (int versionIndex = 0; versionIndex < self->resolution[0].stateMutations.size(); versionIndex++) {
		// pProxyCommitData->logAdapter->setNextVersion( ??? );  << Ideally we would be telling the log adapter that the pushes in this commit will be in the version at which these state mutations were committed by another proxy, but at present we don't have that information here.  So the disk queue may be unnecessarily conservative about popping.

		for (int transactionIndex = 0; transactionIndex < self->resolution[0].stateMutations[versionIndex].size() && !self->forceRecovery; transactionIndex++) {
			bool committed = true;
			for (int resolver = 0; resolver < self->resolution.size(); resolver++)
				committed = committed && self->resolution[resolver].stateMutations[versionIndex][transactionIndex].committed;
			if (committed) {
				applyMetadataMutations(*self->pProxyCommitData, self->arena, self->pProxyCommitData->logSystem,
				                       self->resolution[0].stateMutations[versionIndex][transactionIndex].mutations,
				                       /* pToCommit= */ nullptr, self->forceRecovery,
				                       /* popVersion= */ 0, /* initialCommit */ false);
			}
			if( self->resolution[0].stateMutations[versionIndex][transactionIndex].mutations.size() && self->firstStateMutations ) {
				ASSERT(committed);
				self->firstStateMutations = false;
				self->forceRecovery = false;
			}
		}

		// These changes to txnStateStore will be committed by the other proxy, so we simply discard the commit message
		auto fcm = self->pProxyCommitData->logAdapter->getCommitMessage();
		self->storeCommits.emplace_back(fcm, self->pProxyCommitData->txnStateStore->commit());

		if (initialState) {
			initialState = false;
			self->forceRecovery = false;
			self->pProxyCommitData->txnStateStore->resyncLog();

			for (auto &p : self->storeCommits) {
				ASSERT(!p.second.isReady());
				p.first.get().acknowledge.send(Void());
				ASSERT(p.second.isReady());
			}
			self->storeCommits.clear();
		}
	}
}

/// Determine which transactions actually committed (conservatively) by combining results from the resolvers
void determineCommittedTransactions(CommitBatchContext* self) {
	auto pProxyCommitData = self->pProxyCommitData;
	const auto& trs = self->trs;

	ASSERT(self->transactionResolverMap.size() == self->committed.size());
	// For each commitTransactionRef, it is only sent to resolvers specified in transactionResolverMap
	// Thus, we use this nextTr to track the correct transaction index on each resolver.
	self->nextTr.resize(self->resolution.size());
	for (int t = 0; t < trs.size(); t++) {
		uint8_t commit = ConflictBatch::TransactionCommitted;
		for (int r : self->transactionResolverMap[t]) {
			commit = std::min(self->resolution[r].committed[self->nextTr[r]++], commit);
		}
		self->committed[t] = commit;
	}
	for (int r = 0; r < self->resolution.size(); r++)
		ASSERT(self->nextTr[r] == self->resolution[r].committed.size());

	pProxyCommitData->logAdapter->setNextVersion(self->commitVersion);

	self->lockedKey = pProxyCommitData->txnStateStore->readValue(databaseLockedKey).get();
	self->locked = self->lockedKey.present() && self->lockedKey.get().size();

	const Optional<Value> mustContainSystemKey = pProxyCommitData->txnStateStore->readValue(mustContainSystemMutationsKey).get();
	if (mustContainSystemKey.present() && mustContainSystemKey.get().size()) {
		for (int t = 0; t < trs.size(); t++) {
			if( self->committed[t] == ConflictBatch::TransactionCommitted ) {
				bool foundSystem = false;
				for(auto& m : trs[t].transaction.mutations) {
					if( ( m.type == MutationRef::ClearRange ? m.param2 : m.param1 ) >= nonMetadataSystemKeys.end) {
						foundSystem = true;
						break;
					}
				}
				if(!foundSystem) {
					self->committed[t] = ConflictBatch::TransactionConflict;
				}
			}
		}
	}
}

// This first pass through committed transactions deals with "metadata" effects (modifications of txnStateStore, changes to storage servers' responsibilities)
ACTOR Future<Void> applyMetadataToCommittedTransactions(CommitBatchContext* self) {
	auto pProxyCommitData = self->pProxyCommitData;
	const auto& trs = self->trs;

	int t;
	for (t = 0; t < trs.size() && !self->forceRecovery; t++) {
		if (self->committed[t] == ConflictBatch::TransactionCommitted && (!self->locked || trs[t].isLockAware())) {
			self->commitCount++;
			applyMetadataMutations(*pProxyCommitData, self->arena, pProxyCommitData->logSystem,
			                       trs[t].transaction.mutations, &self->toCommit, self->forceRecovery,
			                       self->commitVersion + 1, /* initialCommit= */ false);
		}
		if(self->firstStateMutations) {
			ASSERT(self->committed[t] == ConflictBatch::TransactionCommitted);
			self->firstStateMutations = false;
			self->forceRecovery = false;
		}
	}
	if (self->forceRecovery) {
		for (; t < trs.size(); t++)
			self->committed[t] = ConflictBatch::TransactionConflict;
		TraceEvent(SevWarn, "RestartingTxnSubsystem", pProxyCommitData->dbgid).detail("Stage", "AwaitCommit");
	}

	self->lockedKey = pProxyCommitData->txnStateStore->readValue(databaseLockedKey).get();
	self->lockedAfter = self->lockedKey.present() && self->lockedKey.get().size();

	self->metadataVersionAfter = pProxyCommitData->txnStateStore->readValue(metadataVersionKey).get();

	auto fcm = pProxyCommitData->logAdapter->getCommitMessage();
	self->storeCommits.emplace_back(fcm, pProxyCommitData->txnStateStore->commit());
	pProxyCommitData->version = self->commitVersion;
	if (!pProxyCommitData->validState.isSet()) pProxyCommitData->validState.send(Void());
	ASSERT(self->commitVersion);

	if (!self->isMyFirstBatch && pProxyCommitData->txnStateStore->readValue( coordinatorsKey ).get().get() != self->oldCoordinators.get()) {
		wait( brokenPromiseToNever( pProxyCommitData->master.changeCoordinators.getReply( ChangeCoordinatorsRequest( pProxyCommitData->txnStateStore->readValue( coordinatorsKey ).get().get() ) ) ) );
		ASSERT(false);   // ChangeCoordinatorsRequest should always throw
	}

	return Void();
}

/// This second pass through committed transactions assigns the actual mutations to the appropriate storage servers' tags
ACTOR Future<Void> assignMutationsToStorageServers(CommitBatchContext* self) {
	state ProxyCommitData* const pProxyCommitData = self->pProxyCommitData;
	state std::vector<CommitTransactionRequest>& trs = self->trs;

	for (; self->transactionNum < trs.size(); self->transactionNum++) {
		if (!(self->committed[self->transactionNum] == ConflictBatch::TransactionCommitted && (!self->locked || trs[self->transactionNum].isLockAware()))) {
			continue;
		}

		state int mutationNum = 0;
		state VectorRef<MutationRef>* pMutations = &trs[self->transactionNum].transaction.mutations;
		for (; mutationNum < pMutations->size(); mutationNum++) {
			if(self->yieldBytes > SERVER_KNOBS->DESIRED_TOTAL_BYTES) {
				self->yieldBytes = 0;
				if(g_network->check_yield(TaskPriority::ProxyCommitYield1)) {
					self->computeDuration += g_network->timer() - self->computeStart;
					wait(delay(0, TaskPriority::ProxyCommitYield1));
					self->computeStart = g_network->timer();
				}
			}

			auto& m = (*pMutations)[mutationNum];
			self->mutationCount++;
			self->mutationBytes += m.expectedSize();
			self->yieldBytes += m.expectedSize();
			// Determine the set of tags (responsible storage servers) for the mutation, splitting it
			// if necessary.  Serialize (splits of) the mutation into the message buffer and add the tags.

			if (isSingleKeyMutation((MutationRef::Type) m.type)) {
				auto& tags = pProxyCommitData->tagsForKey(m.param1);

				if(pProxyCommitData->singleKeyMutationEvent->enabled) {
					KeyRangeRef shard = pProxyCommitData->keyInfo.rangeContaining(m.param1).range();
					pProxyCommitData->singleKeyMutationEvent->tag1 = (int64_t)tags[0].id;
					pProxyCommitData->singleKeyMutationEvent->tag2 = (int64_t)tags[1].id;
					pProxyCommitData->singleKeyMutationEvent->tag3 = (int64_t)tags[2].id;
					pProxyCommitData->singleKeyMutationEvent->shardBegin = shard.begin;
					pProxyCommitData->singleKeyMutationEvent->shardEnd = shard.end;
					pProxyCommitData->singleKeyMutationEvent->log();
				}

				DEBUG_MUTATION("ProxyCommit", self->commitVersion, m).detail("Dbgid", pProxyCommitData->dbgid).detail("To", tags).detail("Mutation", m);
				self->toCommit.addTags(tags);
				if(pProxyCommitData->cacheInfo[m.param1]) {
					self->toCommit.addTag(cacheTag);
				}
				self->toCommit.addTypedMessage(m);
			}
			else if (m.type == MutationRef::ClearRange) {
				KeyRangeRef clearRange(KeyRangeRef(m.param1, m.param2));
				auto ranges = pProxyCommitData->keyInfo.intersectingRanges(clearRange);
				auto firstRange = ranges.begin();
				++firstRange;
				if (firstRange == ranges.end()) {
					// Fast path
					DEBUG_MUTATION("ProxyCommit", self->commitVersion, m).detail("Dbgid", pProxyCommitData->dbgid).detail("To", ranges.begin().value().tags).detail("Mutation", m);

					ranges.begin().value().populateTags();
					self->toCommit.addTags(ranges.begin().value().tags);
				}
				else {
					TEST(true); //A clear range extends past a shard boundary
					std::set<Tag> allSources;
					for (auto r : ranges) {
						r.value().populateTags();
						allSources.insert(r.value().tags.begin(), r.value().tags.end());
					}
					DEBUG_MUTATION("ProxyCommit", self->commitVersion, m).detail("Dbgid", pProxyCommitData->dbgid).detail("To", allSources).detail("Mutation", m);

					self->toCommit.addTags(allSources);
				}

				if(pProxyCommitData->needsCacheTag(clearRange)) {
					self->toCommit.addTag(cacheTag);
				}
				self->toCommit.addTypedMessage(m);
			} else {
				UNREACHABLE();
			}

			// Check on backing up key, if backup ranges are defined and a normal key
			if (!(pProxyCommitData->vecBackupKeys.size() > 1 && (normalKeys.contains(m.param1) || m.param1 == metadataVersionKey))) {
				continue;
			}

			if (m.type != MutationRef::Type::ClearRange) {
				// Add the mutation to the relevant backup tag
				for (auto backupName : pProxyCommitData->vecBackupKeys[m.param1]) {
					self->logRangeMutations[backupName].push_back_deep(self->logRangeMutationsArena, m);
				}
			}
			else {
				KeyRangeRef mutationRange(m.param1, m.param2);
				KeyRangeRef intersectionRange;

				// Identify and add the intersecting ranges of the mutation to the array of mutations to serialize
				for (auto backupRange : pProxyCommitData->vecBackupKeys.intersectingRanges(mutationRange))
				{
					// Get the backup sub range
					const auto&		backupSubrange = backupRange.range();

					// Determine the intersecting range
					intersectionRange = mutationRange & backupSubrange;

					// Create the custom mutation for the specific backup tag
					MutationRef		backupMutation(MutationRef::Type::ClearRange, intersectionRange.begin, intersectionRange.end);

					// Add the mutation to the relevant backup tag
					for (auto backupName : backupRange.value()) {
						self->logRangeMutations[backupName].push_back_deep(self->logRangeMutationsArena, backupMutation);
					}
				}
			}
		}
	}

	return Void();
}

ACTOR Future<Void> postResolution(CommitBatchContext* self) {
	state ProxyCommitData* const pProxyCommitData = self->pProxyCommitData;
	state std::vector<CommitTransactionRequest>& trs = self->trs;
	state const int64_t localBatchNumber = self->localBatchNumber;
	state const Optional<UID>& debugID = self->debugID;

	TEST(pProxyCommitData->latestLocalCommitBatchLogging.get() < localBatchNumber - 1); // Queuing post-resolution commit processing
	wait(pProxyCommitData->latestLocalCommitBatchLogging.whenAtLeast(localBatchNumber - 1));
	wait(yield(TaskPriority::ProxyCommitYield1));

	self->computeStart = g_network->timer();

	pProxyCommitData->stats.txnCommitResolved += trs.size();

	if (debugID.present()) {
		g_traceBatch.addEvent(
			"CommitDebug", debugID.get().first(),
			"MasterProxyServer.commitBatch.ProcessingMutations"
		);
	}

	self->isMyFirstBatch = !pProxyCommitData->version;
	self->oldCoordinators = pProxyCommitData->txnStateStore->readValue(coordinatorsKey).get();

	assertResolutionStateMutationsSizeConsistent(self->resolution);

	applyMetadataEffect(self);

	determineCommittedTransactions(self);

	if(self->forceRecovery) {
		wait( Future<Void>(Never()) );
	}

	// First pass
	wait(applyMetadataToCommittedTransactions(self));

	// Second pass
	wait(assignMutationsToStorageServers(self));

	// Serialize and backup the mutations as a single mutation
	if ((pProxyCommitData->vecBackupKeys.size() > 1) && self->logRangeMutations.size()) {
		wait( addBackupMutations(pProxyCommitData, &self->logRangeMutations, &self->toCommit, self->commitVersion, &self->computeDuration, &self->computeStart) );
	}

	pProxyCommitData->stats.mutations += self->mutationCount;
	pProxyCommitData->stats.mutationBytes += self->mutationBytes;

	// Storage servers mustn't make durable versions which are not fully committed (because then they are impossible to roll back)
	// We prevent this by limiting the number of versions which are semi-committed but not fully committed to be less than the MVCC window
	if (pProxyCommitData->committedVersion.get() < self->commitVersion - SERVER_KNOBS->MAX_READ_TRANSACTION_LIFE_VERSIONS) {
		self->computeDuration += g_network->timer() - self->computeStart;
		state Span waitVersionSpan;
		while (pProxyCommitData->committedVersion.get() < self->commitVersion - SERVER_KNOBS->MAX_READ_TRANSACTION_LIFE_VERSIONS) {
			// This should be *extremely* rare in the real world, but knob buggification should make it happen in simulation
			TEST(true);  // Semi-committed pipeline limited by MVCC window
			//TraceEvent("ProxyWaitingForCommitted", pProxyCommitData->dbgid).detail("CommittedVersion", pProxyCommitData->committedVersion.get()).detail("NeedToCommit", commitVersion);
			waitVersionSpan = Span(deterministicRandom()->randomUniqueID(), "MP:overMaxReadTransactionLifeVersions"_loc, {self->span.context});
			choose{
				when(wait(pProxyCommitData->committedVersion.whenAtLeast(self->commitVersion - SERVER_KNOBS->MAX_READ_TRANSACTION_LIFE_VERSIONS))) {
					wait(yield());
					break;
				}
				when(GetReadVersionReply v = wait(pProxyCommitData->getConsistentReadVersion.getReply(
                        GetReadVersionRequest(waitVersionSpan.context, 0, TransactionPriority::IMMEDIATE,
                                              GetReadVersionRequest::FLAG_CAUSAL_READ_RISKY)))) {
					if(v.version > pProxyCommitData->committedVersion.get()) {
						pProxyCommitData->locked = v.locked;
						pProxyCommitData->metadataVersion = v.metadataVersion;
						pProxyCommitData->committedVersion.set(v.version);
					}

					if (pProxyCommitData->committedVersion.get() < self->commitVersion - SERVER_KNOBS->MAX_READ_TRANSACTION_LIFE_VERSIONS)
						wait(delay(SERVER_KNOBS->PROXY_SPIN_DELAY));
				}
			}
		}
		waitVersionSpan = Span{};
		self->computeStart = g_network->timer();
	}

	self->msg = self->storeCommits.back().first.get();

	if (self->debugID.present())
		g_traceBatch.addEvent("CommitDebug", self->debugID.get().first(), "MasterProxyServer.commitBatch.AfterStoreCommits");

	// txnState (transaction subsystem state) tag: message extracted from log adapter
	bool firstMessage = true;
	for(auto m : self->msg.messages) {
		if(firstMessage) {
			self->toCommit.addTxsTag();
		}
		self->toCommit.addMessage(StringRef(m.begin(), m.size()), !firstMessage);
		firstMessage = false;
	}

	if ( self->prevVersion && self->commitVersion - self->prevVersion < SERVER_KNOBS->MAX_VERSIONS_IN_FLIGHT/2 )
		debug_advanceMaxCommittedVersion( UID(), self->commitVersion );  //< Is this valid?

	//TraceEvent("ProxyPush", pProxyCommitData->dbgid).detail("PrevVersion", prevVersion).detail("Version", commitVersion)
	//	.detail("TransactionsSubmitted", trs.size()).detail("TransactionsCommitted", commitCount).detail("TxsPopTo", msg.popTo);

	if ( self->prevVersion && self->commitVersion - self->prevVersion < SERVER_KNOBS->MAX_VERSIONS_IN_FLIGHT/2 )
		debug_advanceMaxCommittedVersion(UID(), self->commitVersion);

	self->commitStartTime = now();
	pProxyCommitData->lastStartCommit = self->commitStartTime;
	self->loggingComplete = pProxyCommitData->logSystem->push( self->prevVersion, self->commitVersion, pProxyCommitData->committedVersion.get(), pProxyCommitData->minKnownCommittedVersion, self->toCommit, self->debugID );

	if (!self->forceRecovery) {
		ASSERT(pProxyCommitData->latestLocalCommitBatchLogging.get() == self->localBatchNumber-1);
		pProxyCommitData->latestLocalCommitBatchLogging.set(self->localBatchNumber);
	}

	self->computeDuration += g_network->timer() - self->computeStart;
	if(self->computeDuration > SERVER_KNOBS->MIN_PROXY_COMPUTE && self->batchOperations > 0) {
		double computePerOperation = self->computeDuration / self->batchOperations;
		if(computePerOperation <= pProxyCommitData->commitComputePerOperation[self->latencyBucket]) {
			pProxyCommitData->commitComputePerOperation[self->latencyBucket] = computePerOperation;
		} else {
			pProxyCommitData->commitComputePerOperation[self->latencyBucket] = SERVER_KNOBS->PROXY_COMPUTE_GROWTH_RATE*computePerOperation + ((1.0-SERVER_KNOBS->PROXY_COMPUTE_GROWTH_RATE)*pProxyCommitData->commitComputePerOperation[self->latencyBucket]);
		}
	}

	return Void();
}

ACTOR Future<Void> transactionLogging(CommitBatchContext* self) {
	state ProxyCommitData* const pProxyCommitData = self->pProxyCommitData;

	try {
		choose {
			when(Version ver = wait(self->loggingComplete)) {
				pProxyCommitData->minKnownCommittedVersion = std::max(pProxyCommitData->minKnownCommittedVersion, ver);
			}
			when(wait(pProxyCommitData->committedVersion.whenAtLeast( self->commitVersion + 1 ))) {}
		}
	} catch(Error &e) {
		if(e.code() == error_code_broken_promise) {
			throw master_tlog_failed();
		}
		throw;
	}

	pProxyCommitData->lastCommitLatency = now() - self->commitStartTime;
	pProxyCommitData->lastCommitTime = std::max(pProxyCommitData->lastCommitTime.get(), self->commitStartTime);

	wait(yield(TaskPriority::ProxyCommitYield2));

	if( pProxyCommitData->popRemoteTxs && self->msg.popTo > ( pProxyCommitData->txsPopVersions.size() ? pProxyCommitData->txsPopVersions.back().second : pProxyCommitData->lastTxsPop ) ) {
		if(pProxyCommitData->txsPopVersions.size() >= SERVER_KNOBS->MAX_TXS_POP_VERSION_HISTORY) {
			TraceEvent(SevWarnAlways, "DiscardingTxsPopHistory").suppressFor(1.0);
			pProxyCommitData->txsPopVersions.pop_front();
		}

		pProxyCommitData->txsPopVersions.emplace_back(self->commitVersion, self->msg.popTo);
	}
	pProxyCommitData->logSystem->popTxs(self->msg.popTo);

	return Void();
}

ACTOR Future<Void> reply(CommitBatchContext* self) {
	state ProxyCommitData* const pProxyCommitData = self->pProxyCommitData;

	const Optional<UID>& debugID = self->debugID;

	if ( self->prevVersion && self->commitVersion - self->prevVersion < SERVER_KNOBS->MAX_VERSIONS_IN_FLIGHT/2 )
		debug_advanceMinCommittedVersion(UID(), self->commitVersion);

	//TraceEvent("ProxyPushed", pProxyCommitData->dbgid).detail("PrevVersion", prevVersion).detail("Version", commitVersion);
	if (debugID.present())
		g_traceBatch.addEvent("CommitDebug", debugID.get().first(), "MasterProxyServer.commitBatch.AfterLogPush");

	for (auto &p : self->storeCommits) {
		ASSERT(!p.second.isReady());
		p.first.get().acknowledge.send(Void());
		ASSERT(p.second.isReady());
	}

	// After logging finishes, we report the commit version to master so that every other proxy can get the most
	// up-to-date live committed version. We also maintain the invariant that master's committed version >= self->committedVersion
	// by reporting commit version first before updating self->committedVersion. Otherwise, a client may get a commit
	// version that the master is not aware of, and next GRV request may get a version less than self->committedVersion.
	TEST(pProxyCommitData->committedVersion.get() > self->commitVersion);   // A later version was reported committed first
	if ( self->commitVersion > pProxyCommitData->committedVersion.get()) {
		wait(pProxyCommitData->master.reportLiveCommittedVersion.getReply(
			ReportRawCommittedVersionRequest(
				self->commitVersion,
				self->lockedAfter,
				self->metadataVersionAfter), TaskPriority::ProxyMasterVersionReply));
	}
	if( self->commitVersion > pProxyCommitData->committedVersion.get() ) {
		pProxyCommitData->locked = self->lockedAfter;
		pProxyCommitData->metadataVersion = self->metadataVersionAfter;
		pProxyCommitData->committedVersion.set(self->commitVersion);
	}

	if (self->forceRecovery) {
		TraceEvent(SevWarn, "RestartingTxnSubsystem", pProxyCommitData->dbgid).detail("Stage", "ProxyShutdown");
		throw worker_removed();
	}

	// Send replies to clients
	double endTime = g_network->timer();
	// Reset all to zero, used to track the correct index of each commitTransacitonRef on each resolver

	std::fill(self->nextTr.begin(), self->nextTr.end(), 0);
	for (int t = 0; t < self->trs.size(); t++) {
		auto& tr = self->trs[t];
		if (self->committed[t] == ConflictBatch::TransactionCommitted && (!self->locked || tr.isLockAware())) {
			ASSERT_WE_THINK(self->commitVersion != invalidVersion);
			tr.reply.send(CommitID(self->commitVersion, t, self->metadataVersionAfter));

			// aggregate commit cost estimation if committed
			ASSERT(tr.commitCostEstimation.present() == tr.tagSet.present());
			if (tr.tagSet.present()) {
				TransactionCommitCostEstimation& costEstimation = tr.commitCostEstimation.get();
				for (auto& tag : tr.tagSet.get()) {
					pProxyCommitData->transactionTagCommitCostEst[tag] += costEstimation;
				}
			}
		}
		else if (self->committed[t] == ConflictBatch::TransactionTooOld) {
			tr.reply.sendError(transaction_too_old());
		}
		else {
			// If enable the option to report conflicting keys from resolvers, we send back all keyranges' indices
			// through CommitID
			if (tr.transaction.report_conflicting_keys) {
				Standalone<VectorRef<int>> conflictingKRIndices;
				for (int resolverInd : self->transactionResolverMap[t]) {
					auto const& cKRs =
					    self->resolution[resolverInd]
					        .conflictingKeyRangeMap[self->nextTr[resolverInd]]; // nextTr[resolverInd] -> index of this trs[t]
					                                                      // on the resolver
					for (auto const& rCRIndex : cKRs)
						// read_conflict_range can change when sent to resolvers, mapping the index from resolver-side
						// to original index in commitTransactionRef
						conflictingKRIndices.push_back(conflictingKRIndices.arena(),
						                               self->txReadConflictRangeIndexMap[t][resolverInd][rCRIndex]);
				}
				// At least one keyRange index should be returned
				ASSERT(conflictingKRIndices.size());
				tr.reply.send(CommitID(invalidVersion, t, Optional<Value>(),
				                           Optional<Standalone<VectorRef<int>>>(conflictingKRIndices)));
			} else {
				tr.reply.sendError(not_committed());
			}
		}

		// Update corresponding transaction indices on each resolver
		for (int resolverInd : self->transactionResolverMap[t]) self->nextTr[resolverInd]++;

		// TODO: filter if pipelined with large commit
		const double duration = endTime - tr.requestTime();
		pProxyCommitData->stats.commitLatencySample.addMeasurement(duration);
		if(pProxyCommitData->latencyBandConfig.present()) {
			bool filter = self->maxTransactionBytes > pProxyCommitData->latencyBandConfig.get().commitConfig.maxCommitBytes.orDefault(std::numeric_limits<int>::max());
			pProxyCommitData->stats.commitLatencyBands.addMeasurement(duration, filter);
		}
	}

	++pProxyCommitData->stats.commitBatchOut;
	pProxyCommitData->stats.txnCommitOut += self->trs.size();
	pProxyCommitData->stats.txnConflicts += self->trs.size() - self->commitCount;
	pProxyCommitData->stats.txnCommitOutSuccess += self->commitCount;

	if(now() - pProxyCommitData->lastCoalesceTime > SERVER_KNOBS->RESOLVER_COALESCE_TIME) {
		pProxyCommitData->lastCoalesceTime = now();
		int lastSize = pProxyCommitData->keyResolvers.size();
		auto rs = pProxyCommitData->keyResolvers.ranges();
		Version oldestVersion = self->prevVersion - SERVER_KNOBS->MAX_WRITE_TRANSACTION_LIFE_VERSIONS;
		for(auto r = rs.begin(); r != rs.end(); ++r) {
			while(r->value().size() > 1 && r->value()[1].first < oldestVersion)
				r->value().pop_front();
			if(r->value().size() && r->value().front().first < oldestVersion)
				r->value().front().first = 0;
		}
		pProxyCommitData->keyResolvers.coalesce(allKeys);
		if(pProxyCommitData->keyResolvers.size() != lastSize)
			TraceEvent("KeyResolverSize", pProxyCommitData->dbgid).detail("Size", pProxyCommitData->keyResolvers.size());
	}

	// Dynamic batching for commits
	double target_latency = (now() - self->startTime) * SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_INTERVAL_LATENCY_FRACTION;
	pProxyCommitData->commitBatchInterval = std::max(
	    SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_INTERVAL_MIN,
	    std::min(SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_INTERVAL_MAX,
	             target_latency * SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_INTERVAL_SMOOTHER_ALPHA +
	                 pProxyCommitData->commitBatchInterval * (1 - SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_INTERVAL_SMOOTHER_ALPHA)));

	pProxyCommitData->commitBatchesMemBytesCount -= self->currentBatchMemBytesCount;
	ASSERT_ABORT(pProxyCommitData->commitBatchesMemBytesCount >= 0);
	wait(self->releaseFuture);

	return Void();
}

}	// namespace CommitBatch

// Commit one batch of transactions trs
ACTOR Future<Void> commitBatch(
		ProxyCommitData* self,
		vector<CommitTransactionRequest>* trs,
		int currentBatchMemBytesCount) {
	//WARNING: this code is run at a high priority (until the first delay(0)), so it needs to do as little work as possible
	state CommitBatch::CommitBatchContext context(self, trs, currentBatchMemBytesCount);

	for (int i = 0; i < context.trs.size(); ++i) {
		COUT << " item=" << i << std::endl;
		auto& tr = context.trs[i];
		if (!tr.splitTransaction.present()) {
			continue;
		}
		auto& split = tr.splitTransaction.get();
		std::cout << "  SPLIT id=" << split.id.toString() << "  part=" << split.partIndex << "/" << split.totalParts
		          << std::endl;
		auto& txn = tr.transaction;
		auto& mxn = txn.mutations;
		for (int j = 0; j < mxn.size(); ++j) {
			auto& m = mxn[j];
			std::cout << m.param1.toString() << "\t" << m.param2.toString() << std::endl;
		}
	}

	// Active load balancing runs at a very high priority (to obtain accurate estimate of memory used by commit batches) so we need to downgrade here
	wait(delay(0, TaskPriority::ProxyCommit));

	context.pProxyCommitData->lastVersionTime = context.startTime;
	++context.pProxyCommitData->stats.commitBatchIn;

	/////// Phase 1: Pre-resolution processing (CPU bound except waiting for a version # which is separately pipelined and *should* be available by now (unless empty commit); ordered; currently atomic but could yield)
	wait(CommitBatch::preresolutionProcessing(&context));

	/////// Phase 2: Resolution (waiting on the network; pipelined)
	wait(CommitBatch::getResolution(&context));

	////// Phase 3: Post-resolution processing (CPU bound except for very rare situations; ordered; currently atomic but doesn't need to be)
	wait(CommitBatch::postResolution(&context));

	/////// Phase 4: Logging (network bound; pipelined up to MAX_READ_TRANSACTION_LIFE_VERSIONS (limited by loop above))
	wait(CommitBatch::transactionLogging(&context));

	/////// Phase 5: Replies (CPU bound; no particular order required, though ordered execution would be best for latency)
	wait(CommitBatch::reply(&context));

	return Void();
}

ACTOR Future<Void> updateLastCommit(ProxyCommitData* self, Optional<UID> debugID = Optional<UID>()) {
	state double confirmStart = now();
	self->lastStartCommit = confirmStart;
	self->updateCommitRequests++;
	wait(self->logSystem->confirmEpochLive(debugID));
	self->updateCommitRequests--;
	self->lastCommitLatency = now()-confirmStart;
	self->lastCommitTime = std::max(self->lastCommitTime.get(), confirmStart);
	return Void();
}

ACTOR Future<GetReadVersionReply> getLiveCommittedVersion(SpanID parentSpan, ProxyCommitData* commitData, uint32_t flags, Optional<UID> debugID,
                                                          int transactionCount, int systemTransactionCount, int defaultPriTransactionCount, int batchPriTransactionCount)
{
	// Returns a version which (1) is committed, and (2) is >= the latest version reported committed (by a commit response) when this request was sent
	// (1) The version returned is the committedVersion of some proxy at some point before the request returns, so it is committed.
	// (2) No proxy on our list reported committed a higher version before this request was received, because then its committedVersion would have been higher,
	//     and no other proxy could have already committed anything without first ending the epoch
	state Span span("MP:getLiveCommittedVersion"_loc, parentSpan);
	++commitData->stats.txnStartBatch;
	state Future<GetReadVersionReply> replyFromMasterFuture = commitData->master.getLiveCommittedVersion.getReply(
	    GetRawCommittedVersionRequest(span.context, debugID), TaskPriority::GetLiveCommittedVersionReply);

	if (!SERVER_KNOBS->ALWAYS_CAUSAL_READ_RISKY && !(flags&GetReadVersionRequest::FLAG_CAUSAL_READ_RISKY)) {
		wait(updateLastCommit(commitData, debugID));
	} else if (SERVER_KNOBS->REQUIRED_MIN_RECOVERY_DURATION > 0 && now() - SERVER_KNOBS->REQUIRED_MIN_RECOVERY_DURATION > commitData->lastCommitTime.get()) {
		wait(commitData->lastCommitTime.whenAtLeast(now() - SERVER_KNOBS->REQUIRED_MIN_RECOVERY_DURATION));
	}

	if (debugID.present()) {
		g_traceBatch.addEvent("TransactionDebug", debugID.get().first(), "MasterProxyServer.getLiveCommittedVersion.confirmEpochLive");
	}

	state GetReadVersionReply rep;
	rep.locked = commitData->locked;
	rep.metadataVersion = commitData->metadataVersion;
	rep.version = commitData->committedVersion.get();

	GetReadVersionReply replyFromMaster = wait(replyFromMasterFuture);
	if (replyFromMaster.version > rep.version) {
		rep = replyFromMaster;
	}
	rep.recentRequests = commitData->stats.getRecentRequests();

	if (debugID.present()) {
		g_traceBatch.addEvent("TransactionDebug", debugID.get().first(), "MasterProxyServer.getLiveCommittedVersion.After");
	}

	commitData->stats.txnStartOut += transactionCount;
	commitData->stats.txnSystemPriorityStartOut += systemTransactionCount;
	commitData->stats.txnDefaultPriorityStartOut += defaultPriTransactionCount;
	commitData->stats.txnBatchPriorityStartOut += batchPriTransactionCount;

	return rep;
}

ACTOR Future<Void> sendGrvReplies(Future<GetReadVersionReply> replyFuture, std::vector<GetReadVersionRequest> requests,
                                  ProxyStats* stats, Version minKnownCommittedVersion, PrioritizedTransactionTagMap<ClientTagThrottleLimits> throttledTags) {
	GetReadVersionReply _reply = wait(replyFuture);
	GetReadVersionReply reply = _reply;
	Version replyVersion = reply.version;

	double end = g_network->timer();
	for(GetReadVersionRequest const& request : requests) {
		double duration = end - request.requestTime();
		if(request.priority == TransactionPriority::DEFAULT) {
			stats->grvLatencySample.addMeasurement(duration);
		}
		if(request.priority >= TransactionPriority::DEFAULT) {
			stats->grvLatencyBands.addMeasurement(duration);
		}

		if (request.flags & GetReadVersionRequest::FLAG_USE_MIN_KNOWN_COMMITTED_VERSION) {
			// Only backup worker may infrequently use this flag.
			reply.version = minKnownCommittedVersion;
		}
		else {
			reply.version = replyVersion;
		}

		reply.tagThrottleInfo.clear();

		if(!request.tags.empty()) {
			auto& priorityThrottledTags = throttledTags[request.priority];
			for(auto tag : request.tags) {
				auto tagItr = priorityThrottledTags.find(tag.first);
				if(tagItr != priorityThrottledTags.end()) {
					if(tagItr->second.expiration > now()) {
						if(tagItr->second.tpsRate == std::numeric_limits<double>::max()) {
							TEST(true); // Auto TPS rate is unlimited
						}
						else {
							TEST(true); // Proxy returning tag throttle
							reply.tagThrottleInfo[tag.first] = tagItr->second;
						}
					}
					else {
						// This isn't required, but we might as well
						TEST(true); // Proxy expiring tag throttle
						priorityThrottledTags.erase(tagItr);
					}
				}
			}
		}

		request.reply.send(reply);
		++stats->txnRequestOut;
	}

	return Void();
}

ACTOR static Future<Void> transactionStarter(
	MasterProxyInterface proxy,
	Reference<AsyncVar<ServerDBInfo>> db,
	PromiseStream<Future<Void>> addActor,
	ProxyCommitData* commitData, GetHealthMetricsReply* healthMetricsReply,
	GetHealthMetricsReply* detailedHealthMetricsReply)
{
	state double lastGRVTime = 0;
	state PromiseStream<Void> GRVTimer;
	state double GRVBatchTime = SERVER_KNOBS->START_TRANSACTION_BATCH_INTERVAL_MIN;

	state int64_t transactionCount = 0;
	state int64_t batchTransactionCount = 0;
	state TransactionRateInfo normalRateInfo(10);
	state TransactionRateInfo batchRateInfo(0);

	state SpannedDeque<GetReadVersionRequest> systemQueue("MP:transactionStarterSystemQueue"_loc);
	state SpannedDeque<GetReadVersionRequest> defaultQueue("MP:transactionStarterDefaultQueue"_loc);
	state SpannedDeque<GetReadVersionRequest> batchQueue("MP:transactionStarterBatchQueue"_loc);

	state TransactionTagMap<uint64_t> transactionTagCounter;
	state PrioritizedTransactionTagMap<ClientTagThrottleLimits> throttledTags;

	state PromiseStream<double> replyTimes;
	state Span span;

	addActor.send(getRate(proxy.id(), db, &transactionCount, &batchTransactionCount, &normalRateInfo, &batchRateInfo,
	                      healthMetricsReply, detailedHealthMetricsReply, &transactionTagCounter, &throttledTags,
	                      &(commitData->transactionTagCommitCostEst)));
	addActor.send(queueTransactionStartRequests(db, &systemQueue, &defaultQueue, &batchQueue, proxy.getConsistentReadVersion.getFuture(),
	                                            GRVTimer, &lastGRVTime, &GRVBatchTime, replyTimes.getFuture(), &commitData->stats, &batchRateInfo,
	                                            &transactionTagCounter));

	// Get a list of the other proxies that go together with us
	while (std::find(db->get().client.proxies.begin(), db->get().client.proxies.end(), proxy) == db->get().client.proxies.end())
		wait(db->onChange());

	ASSERT(db->get().recoveryState >= RecoveryState::ACCEPTING_COMMITS);  // else potentially we could return uncommitted read versions (since self->committedVersion is only a committed version if this recovery succeeds)

	TraceEvent("ProxyReadyForTxnStarts", proxy.id());

	loop{
		waitNext(GRVTimer.getFuture());
		// Select zero or more transactions to start
		double t = now();
		double elapsed = now() - lastGRVTime;
		lastGRVTime = t;

		if(elapsed == 0) elapsed = 1e-15; // resolve a possible indeterminant multiplication with infinite transaction rate

		normalRateInfo.reset();
		batchRateInfo.reset();

		int transactionsStarted[2] = {0,0};
		int systemTransactionsStarted[2] = {0,0};
		int defaultPriTransactionsStarted[2] = { 0, 0 };
		int batchPriTransactionsStarted[2] = { 0, 0 };

		vector<vector<GetReadVersionRequest>> start(2);  // start[0] is transactions starting with !(flags&CAUSAL_READ_RISKY), start[1] is transactions starting with flags&CAUSAL_READ_RISKY
		Optional<UID> debugID;

		int requestsToStart = 0;

		while (requestsToStart < SERVER_KNOBS->START_TRANSACTION_MAX_REQUESTS_TO_START) {
			SpannedDeque<GetReadVersionRequest>* transactionQueue;
			if(!systemQueue.empty()) {
				transactionQueue = &systemQueue;
			} else if(!defaultQueue.empty()) {
				transactionQueue = &defaultQueue;
			} else if(!batchQueue.empty()) {
				transactionQueue = &batchQueue;
			} else {
				break;
			}
			transactionQueue->span.swap(span);

			auto& req = transactionQueue->front();
			int tc = req.transactionCount;

			if(req.priority < TransactionPriority::DEFAULT && !batchRateInfo.canStart(transactionsStarted[0] + transactionsStarted[1], tc)) {
				break;
			}
			else if(req.priority < TransactionPriority::IMMEDIATE && !normalRateInfo.canStart(transactionsStarted[0] + transactionsStarted[1], tc)) {
				break;
			}

			if (req.debugID.present()) {
				if (!debugID.present()) debugID = nondeterministicRandom()->randomUniqueID();
				g_traceBatch.addAttach("TransactionAttachID", req.debugID.get().first(), debugID.get().first());
			}

			transactionsStarted[req.flags&1] += tc;
			if (req.priority >= TransactionPriority::IMMEDIATE)
				systemTransactionsStarted[req.flags & 1] += tc;
			else if (req.priority >= TransactionPriority::DEFAULT)
				defaultPriTransactionsStarted[req.flags & 1] += tc;
			else
				batchPriTransactionsStarted[req.flags & 1] += tc;

			start[req.flags & 1].emplace_back(std::move(req));
			static_assert(GetReadVersionRequest::FLAG_CAUSAL_READ_RISKY == 1, "Implementation dependent on flag value");
			transactionQueue->pop_front();
			requestsToStart++;
		}

		if (!systemQueue.empty() || !defaultQueue.empty() || !batchQueue.empty()) {
			forwardPromise(GRVTimer, delayJittered(SERVER_KNOBS->START_TRANSACTION_BATCH_QUEUE_CHECK_INTERVAL, TaskPriority::ProxyGRVTimer));
		}

		/*TraceEvent("GRVBatch", proxy.id())
		.detail("Elapsed", elapsed)
		.detail("NTransactionToStart", nTransactionsToStart)
		.detail("TransactionRate", transactionRate)
		.detail("TransactionQueueSize", transactionQueue.size())
		.detail("NumTransactionsStarted", transactionsStarted[0] + transactionsStarted[1])
		.detail("NumSystemTransactionsStarted", systemTransactionsStarted[0] + systemTransactionsStarted[1])
		.detail("NumNonSystemTransactionsStarted", transactionsStarted[0] + transactionsStarted[1] -
		systemTransactionsStarted[0] - systemTransactionsStarted[1])
		.detail("TransactionBudget", transactionBudget)
		.detail("BatchTransactionBudget", batchTransactionBudget);*/

		int systemTotalStarted = systemTransactionsStarted[0] + systemTransactionsStarted[1];
		int normalTotalStarted = defaultPriTransactionsStarted[0] + defaultPriTransactionsStarted[1];
		int batchTotalStarted = batchPriTransactionsStarted[0] + batchPriTransactionsStarted[1];

		transactionCount += transactionsStarted[0] + transactionsStarted[1];
		batchTransactionCount += batchTotalStarted;

		normalRateInfo.updateBudget(systemTotalStarted + normalTotalStarted, systemQueue.empty() && defaultQueue.empty(), elapsed);
		batchRateInfo.updateBudget(systemTotalStarted + normalTotalStarted + batchTotalStarted, systemQueue.empty() && defaultQueue.empty() && batchQueue.empty(), elapsed);

		if (debugID.present()) {
			g_traceBatch.addEvent("TransactionDebug", debugID.get().first(), "MasterProxyServer.masterProxyServerCore.Broadcast");
		}

		for (int i = 0; i < start.size(); i++) {
			if (start[i].size()) {
				Future<GetReadVersionReply> readVersionReply = getLiveCommittedVersion(
				    span.context, commitData, i, debugID, transactionsStarted[i], systemTransactionsStarted[i],
				    defaultPriTransactionsStarted[i], batchPriTransactionsStarted[i]);
				addActor.send(sendGrvReplies(readVersionReply, start[i], &commitData->stats,
				                             commitData->minKnownCommittedVersion, throttledTags));

				// for now, base dynamic batching on the time for normal requests (not read_risky)
				if (i == 0) {
					addActor.send(timeReply(readVersionReply, replyTimes));
				}
			}
		}
		span = Span(span.location);
	}
}

ACTOR static Future<Void> doKeyServerLocationRequest( GetKeyServerLocationsRequest req, ProxyCommitData* commitData ) {
	// We can't respond to these requests until we have valid txnStateStore
	wait(commitData->validState.getFuture());
	wait(delay(0, TaskPriority::DefaultEndpoint));

	GetKeyServerLocationsReply rep;
	if(!req.end.present()) {
		auto r = req.reverse ? commitData->keyInfo.rangeContainingKeyBefore(req.begin) : commitData->keyInfo.rangeContaining(req.begin);
		vector<StorageServerInterface> ssis;
		ssis.reserve(r.value().src_info.size());
		for(auto& it : r.value().src_info) {
			ssis.push_back(it->interf);
		}
		rep.results.push_back(std::make_pair(r.range(), ssis));
	} else if(!req.reverse) {
		int count = 0;
		for(auto r = commitData->keyInfo.rangeContaining(req.begin); r != commitData->keyInfo.ranges().end() && count < req.limit && r.begin() < req.end.get(); ++r) {
			vector<StorageServerInterface> ssis;
			ssis.reserve(r.value().src_info.size());
			for(auto& it : r.value().src_info) {
				ssis.push_back(it->interf);
			}
			rep.results.push_back(std::make_pair(r.range(), ssis));
			count++;
		}
	} else {
		int count = 0;
		auto r = commitData->keyInfo.rangeContainingKeyBefore(req.end.get());
		while( count < req.limit && req.begin < r.end() ) {
			vector<StorageServerInterface> ssis;
			ssis.reserve(r.value().src_info.size());
			for(auto& it : r.value().src_info) {
				ssis.push_back(it->interf);
			}
			rep.results.push_back(std::make_pair(r.range(), ssis));
			if(r == commitData->keyInfo.ranges().begin()) {
				break;
			}
			count++;
			--r;
		}
	}
	req.reply.send(rep);
	++commitData->stats.keyServerLocationOut;
	return Void();
}

ACTOR static Future<Void> readRequestServer( MasterProxyInterface proxy, PromiseStream<Future<Void>> addActor, ProxyCommitData* commitData ) {
	loop {
		GetKeyServerLocationsRequest req = waitNext(proxy.getKeyServersLocations.getFuture());
		//WARNING: this code is run at a high priority, so it needs to do as little work as possible
		commitData->stats.addRequest();
		if(req.limit != CLIENT_KNOBS->STORAGE_METRICS_SHARD_LIMIT && //Always do data distribution requests
		   commitData->stats.keyServerLocationIn.getValue() - commitData->stats.keyServerLocationOut.getValue() > SERVER_KNOBS->KEY_LOCATION_MAX_QUEUE_SIZE) {
			++commitData->stats.keyServerLocationErrors;
			req.reply.sendError(proxy_memory_limit_exceeded());
			TraceEvent(SevWarnAlways, "ProxyLocationRequestThresholdExceeded").suppressFor(60);
		} else {
			++commitData->stats.keyServerLocationIn;
			addActor.send(doKeyServerLocationRequest(req, commitData));
		}
	}
}

ACTOR static Future<Void> rejoinServer( MasterProxyInterface proxy, ProxyCommitData* commitData ) {
	// We can't respond to these requests until we have valid txnStateStore
	wait(commitData->validState.getFuture());

	TraceEvent("ProxyReadyForReads", proxy.id());

	loop {
		GetStorageServerRejoinInfoRequest req = waitNext(proxy.getStorageServerRejoinInfo.getFuture());
		if (commitData->txnStateStore->readValue(serverListKeyFor(req.id)).get().present()) {
			GetStorageServerRejoinInfoReply rep;
			rep.version = commitData->version;
			rep.tag = decodeServerTagValue( commitData->txnStateStore->readValue(serverTagKeyFor(req.id)).get().get() );
			Standalone<RangeResultRef> history = commitData->txnStateStore->readRange(serverTagHistoryRangeFor(req.id)).get();
			for(int i = history.size()-1; i >= 0; i-- ) {
				rep.history.push_back(std::make_pair(decodeServerTagHistoryKey(history[i].key), decodeServerTagValue(history[i].value)));
			}
			auto localityKey = commitData->txnStateStore->readValue(tagLocalityListKeyFor(req.dcId)).get();
			rep.newLocality = false;
			if( localityKey.present() ) {
				int8_t locality = decodeTagLocalityListValue(localityKey.get());
				if(rep.tag.locality != tagLocalityUpgraded && locality != rep.tag.locality) {
					TraceEvent(SevWarnAlways, "SSRejoinedWithChangedLocality").detail("Tag", rep.tag.toString()).detail("DcId", req.dcId).detail("NewLocality", locality);
				} else if(locality != rep.tag.locality) {
					uint16_t tagId = 0;
					std::vector<uint16_t> usedTags;
					auto tagKeys = commitData->txnStateStore->readRange(serverTagKeys).get();
					for( auto& kv : tagKeys ) {
						Tag t = decodeServerTagValue( kv.value );
						if(t.locality == locality) {
							usedTags.push_back(t.id);
						}
					}
					auto historyKeys = commitData->txnStateStore->readRange(serverTagHistoryKeys).get();
					for( auto& kv : historyKeys ) {
						Tag t = decodeServerTagValue( kv.value );
						if(t.locality == locality) {
							usedTags.push_back(t.id);
						}
					}
					std::sort(usedTags.begin(), usedTags.end());

					int usedIdx = 0;
					for(; usedTags.size() > 0 && tagId <= usedTags.end()[-1]; tagId++) {
						if(tagId < usedTags[usedIdx]) {
							break;
						} else {
							usedIdx++;
						}
					}
					rep.newTag = Tag(locality, tagId);
				}
			} else if(rep.tag.locality != tagLocalityUpgraded) {
				TraceEvent(SevWarnAlways, "SSRejoinedWithUnknownLocality").detail("Tag", rep.tag.toString()).detail("DcId", req.dcId);
			} else {
				rep.newLocality = true;
				int8_t maxTagLocality = -1;
				auto localityKeys = commitData->txnStateStore->readRange(tagLocalityListKeys).get();
				for( auto& kv : localityKeys ) {
					maxTagLocality = std::max(maxTagLocality, decodeTagLocalityListValue( kv.value ));
				}
				rep.newTag = Tag(maxTagLocality+1,0);
			}
			req.reply.send(rep);
		} else {
			req.reply.sendError(worker_removed());
		}
	}
}

ACTOR Future<Void> healthMetricsRequestServer(MasterProxyInterface proxy, GetHealthMetricsReply* healthMetricsReply, GetHealthMetricsReply* detailedHealthMetricsReply)
{
	loop {
		choose {
			when(GetHealthMetricsRequest req =
				 waitNext(proxy.getHealthMetrics.getFuture()))
			{
				if (req.detailed)
					req.reply.send(*detailedHealthMetricsReply);
				else
					req.reply.send(*healthMetricsReply);
			}
		}
	}
}

ACTOR Future<Void> ddMetricsRequestServer(MasterProxyInterface proxy, Reference<AsyncVar<ServerDBInfo>> db)
{
	loop {
		choose {
			when(state GetDDMetricsRequest req = waitNext(proxy.getDDMetrics.getFuture()))
			{
				ErrorOr<GetDataDistributorMetricsReply> reply = wait(errorOr(db->get().distributor.get().dataDistributorMetrics.getReply(GetDataDistributorMetricsRequest(req.keys, req.shardLimit))));
				if ( reply.isError() ) {
					req.reply.sendError(reply.getError());
				} else {
					GetDDMetricsReply newReply;
					newReply.storageMetricsList = reply.get().storageMetricsList;
					req.reply.send(newReply);
				}
			}
		}
	}
}

ACTOR Future<Void> monitorRemoteCommitted(ProxyCommitData* self) {
	loop {
		wait(delay(0)); //allow this actor to be cancelled if we are removed after db changes.
		state Optional<std::vector<OptionalInterface<TLogInterface>>> remoteLogs;
		if(self->db->get().recoveryState >= RecoveryState::ALL_LOGS_RECRUITED) {
			for(auto& logSet : self->db->get().logSystemConfig.tLogs) {
				if(!logSet.isLocal) {
					remoteLogs = logSet.tLogs;
					for(auto& tLog : logSet.tLogs) {
						if(!tLog.present()) {
							remoteLogs = Optional<std::vector<OptionalInterface<TLogInterface>>>();
							break;
						}
					}
					break;
				}
			}
		}

		if(!remoteLogs.present()) {
			wait(self->db->onChange());
			continue;
		}
		self->popRemoteTxs = true;

		state Future<Void> onChange = self->db->onChange();
		loop {
			state std::vector<Future<TLogQueuingMetricsReply>> replies;
			for(auto &it : remoteLogs.get()) {
				replies.push_back(brokenPromiseToNever( it.interf().getQueuingMetrics.getReply( TLogQueuingMetricsRequest() ) ));
			}
			wait( waitForAll(replies) || onChange );

			if(onChange.isReady()) {
				break;
			}

			//FIXME: use the configuration to calculate a more precise minimum recovery version.
			Version minVersion = std::numeric_limits<Version>::max();
			for(auto& it : replies) {
				minVersion = std::min(minVersion, it.get().v);
			}

			while(self->txsPopVersions.size() && self->txsPopVersions.front().first <= minVersion) {
				self->lastTxsPop = self->txsPopVersions.front().second;
				self->logSystem->popTxs(self->txsPopVersions.front().second, tagLocalityRemoteLog);
				self->txsPopVersions.pop_front();
			}

			wait( delay(SERVER_KNOBS->UPDATE_REMOTE_LOG_VERSION_INTERVAL) || onChange );
			if(onChange.isReady()) {
				break;
			}
		}
	}
}

ACTOR Future<Void> lastCommitUpdater(ProxyCommitData* self, PromiseStream<Future<Void>> addActor) {
	loop {
		double interval = std::max(SERVER_KNOBS->MIN_CONFIRM_INTERVAL, (SERVER_KNOBS->REQUIRED_MIN_RECOVERY_DURATION - self->lastCommitLatency)/2.0);
		double elapsed = now()-self->lastStartCommit;
		if(elapsed < interval) {
			wait( delay(interval + 0.0001 - elapsed) );
		} else {
			if(self->updateCommitRequests < SERVER_KNOBS->MAX_COMMIT_UPDATES) {
				addActor.send(updateLastCommit(self));
			} else {
				TraceEvent(g_network->isSimulated() ? SevInfo : SevWarnAlways, "TooManyLastCommitUpdates").suppressFor(1.0);
				self->lastStartCommit = now();
			}
		}
	}
}

ACTOR Future<Void> proxySnapCreate(ProxySnapRequest snapReq, ProxyCommitData* commitData) {
	TraceEvent("SnapMasterProxy_SnapReqEnter")
		.detail("SnapPayload", snapReq.snapPayload)
		.detail("SnapUID", snapReq.snapUID);
	try {
		// whitelist check
		ExecCmdValueString execArg(snapReq.snapPayload);
		StringRef binPath = execArg.getBinaryPath();
		if (!isWhitelisted(commitData->whitelistedBinPathVec, binPath)) {
			TraceEvent("SnapMasterProxy_WhiteListCheckFailed")
				.detail("SnapPayload", snapReq.snapPayload)
				.detail("SnapUID", snapReq.snapUID);
			throw snap_path_not_whitelisted();
		}
		// db fully recovered check
		if (commitData->db->get().recoveryState != RecoveryState::FULLY_RECOVERED)  {
			// Cluster is not fully recovered and needs TLogs
			// from previous generation for full recovery.
			// Currently, snapshot of old tlog generation is not
			// supported and hence failing the snapshot request until
			// cluster is fully_recovered.
			TraceEvent("SnapMasterProxy_ClusterNotFullyRecovered")
				.detail("SnapPayload", snapReq.snapPayload)
				.detail("SnapUID", snapReq.snapUID);
			throw snap_not_fully_recovered_unsupported();
		}

		auto result =
			commitData->txnStateStore->readValue(LiteralStringRef("log_anti_quorum").withPrefix(configKeysPrefix)).get();
		int logAntiQuorum = 0;
		if (result.present()) {
			logAntiQuorum = atoi(result.get().toString().c_str());
		}
		// FIXME: logAntiQuorum not supported, remove it later,
		// In version2, we probably don't need this limtiation, but this needs to be tested.
		if (logAntiQuorum > 0) {
			TraceEvent("SnapMasterProxy_LogAnitQuorumNotSupported")
				.detail("SnapPayload", snapReq.snapPayload)
				.detail("SnapUID", snapReq.snapUID);
			throw snap_log_anti_quorum_unsupported();
		}

		// send a snap request to DD
		if (!commitData->db->get().distributor.present()) {
			TraceEvent(SevWarnAlways, "DataDistributorNotPresent").detail("Operation", "SnapRequest");
			throw operation_failed();
		}
		state Future<ErrorOr<Void>> ddSnapReq =
			commitData->db->get().distributor.get().distributorSnapReq.tryGetReply(DistributorSnapRequest(snapReq.snapPayload, snapReq.snapUID));
		try {
			wait(throwErrorOr(ddSnapReq));
		} catch (Error& e) {
			TraceEvent("SnapMasterProxy_DDSnapResponseError")
				.detail("SnapPayload", snapReq.snapPayload)
				.detail("SnapUID", snapReq.snapUID)
				.error(e, true /*includeCancelled*/ );
			throw e;
		}
		snapReq.reply.send(Void());
	} catch (Error& e) {
		TraceEvent("SnapMasterProxy_SnapReqError")
			.detail("SnapPayload", snapReq.snapPayload)
			.detail("SnapUID", snapReq.snapUID)
			.error(e, true /*includeCancelled*/);
		if (e.code() != error_code_operation_cancelled) {
			snapReq.reply.sendError(e);
		} else {
			throw e;
		}
	}
	TraceEvent("SnapMasterProxy_SnapReqExit")
		.detail("SnapPayload", snapReq.snapPayload)
		.detail("SnapUID", snapReq.snapUID);
	return Void();
}

ACTOR Future<Void> proxyCheckSafeExclusion(Reference<AsyncVar<ServerDBInfo>> db, ExclusionSafetyCheckRequest req) {
	TraceEvent("SafetyCheckMasterProxyBegin");
	state ExclusionSafetyCheckReply reply(false);
	if (!db->get().distributor.present()) {
		TraceEvent(SevWarnAlways, "DataDistributorNotPresent").detail("Operation", "ExclusionSafetyCheck");
		req.reply.send(reply);
		return Void();
	}
	try {
		state Future<ErrorOr<DistributorExclusionSafetyCheckReply>> safeFuture =
		    db->get().distributor.get().distributorExclCheckReq.tryGetReply(
		        DistributorExclusionSafetyCheckRequest(req.exclusions));
		DistributorExclusionSafetyCheckReply _reply = wait(throwErrorOr(safeFuture));
		reply.safe = _reply.safe;
	} catch (Error& e) {
		TraceEvent("SafetyCheckMasterProxyResponseError").error(e);
		if (e.code() != error_code_operation_cancelled) {
			req.reply.sendError(e);
			return Void();
		} else {
			throw e;
		}
	}
	TraceEvent("SafetyCheckMasterProxyFinish");
	req.reply.send(reply);
	return Void();
}

ACTOR Future<Void> masterProxyServerCore(
	MasterProxyInterface proxy,
	MasterInterface master,
	Reference<AsyncVar<ServerDBInfo>> db,
	LogEpoch epoch,
	Version recoveryTransactionVersion,
	bool firstProxy,
	std::string whitelistBinPaths)
{
	state ProxyCommitData commitData(proxy.id(), master, proxy.getConsistentReadVersion, recoveryTransactionVersion, proxy.commit, db, firstProxy);

	state Future<Sequence> sequenceFuture = (Sequence)0;
	state PromiseStream< std::pair<vector<CommitTransactionRequest>, int> > batchedCommits;
	state Future<Void> commitBatcherActor;
	state Future<Void> lastCommitComplete = Void();

	state PromiseStream<Future<Void>> addActor;
	state Future<Void> onError = transformError( actorCollection(addActor.getFuture()), broken_promise(), master_tlog_failed() );
	state double lastCommit = 0;
	state std::set<Sequence> txnSequences;
	state Sequence maxSequence = std::numeric_limits<Sequence>::max();

	state GetHealthMetricsReply healthMetricsReply;
	state GetHealthMetricsReply detailedHealthMetricsReply;

	addActor.send( waitFailureServer(proxy.waitFailure.getFuture()) );
	addActor.send( traceRole(Role::MASTER_PROXY, proxy.id()) );

	//TraceEvent("ProxyInit1", proxy.id());

	// Wait until we can load the "real" logsystem, since we don't support switching them currently
	while (!(commitData.db->get().master.id() == master.id() && commitData.db->get().recoveryState >= RecoveryState::RECOVERY_TRANSACTION)) {
		//TraceEvent("ProxyInit2", proxy.id()).detail("LSEpoch", db->get().logSystemConfig.epoch).detail("Need", epoch);
		wait(commitData.db->onChange());
	}
	state Future<Void> dbInfoChange = commitData.db->onChange();
	//TraceEvent("ProxyInit3", proxy.id());

	commitData.resolvers = commitData.db->get().resolvers;
	ASSERT(commitData.resolvers.size() != 0);

	auto rs = commitData.keyResolvers.modify(allKeys);
	for(auto r = rs.begin(); r != rs.end(); ++r)
		r->value().emplace_back(0,0);

	commitData.logSystem = ILogSystem::fromServerDBInfo(proxy.id(), commitData.db->get(), false, addActor);
	commitData.logAdapter = new LogSystemDiskQueueAdapter(commitData.logSystem, Reference<AsyncVar<PeekTxsInfo>>(), 1, false);
	commitData.txnStateStore = keyValueStoreLogSystem(commitData.logAdapter, proxy.id(), 2e9, true, true, true);
	createWhitelistBinPathVec(whitelistBinPaths, commitData.whitelistedBinPathVec);

	commitData.updateLatencyBandConfig(commitData.db->get().latencyBandConfig);

	// ((SERVER_MEM_LIMIT * COMMIT_BATCHES_MEM_FRACTION_OF_TOTAL) / COMMIT_BATCHES_MEM_TO_TOTAL_MEM_SCALE_FACTOR) is only a approximate formula for limiting the memory used.
	// COMMIT_BATCHES_MEM_TO_TOTAL_MEM_SCALE_FACTOR is an estimate based on experiments and not an accurate one.
	state int64_t commitBatchesMemoryLimit = std::min(SERVER_KNOBS->COMMIT_BATCHES_MEM_BYTES_HARD_LIMIT, static_cast<int64_t>((SERVER_KNOBS->SERVER_MEM_LIMIT * SERVER_KNOBS->COMMIT_BATCHES_MEM_FRACTION_OF_TOTAL) / SERVER_KNOBS->COMMIT_BATCHES_MEM_TO_TOTAL_MEM_SCALE_FACTOR));
	TraceEvent(SevInfo, "CommitBatchesMemoryLimit").detail("BytesLimit", commitBatchesMemoryLimit);

	addActor.send(monitorRemoteCommitted(&commitData));
	addActor.send(transactionStarter(proxy, commitData.db, addActor, &commitData, &healthMetricsReply, &detailedHealthMetricsReply));
	addActor.send(readRequestServer(proxy, addActor, &commitData));
	addActor.send(rejoinServer(proxy, &commitData));
	addActor.send(healthMetricsRequestServer(proxy, &healthMetricsReply, &detailedHealthMetricsReply));
	addActor.send(ddMetricsRequestServer(proxy, db));

	// wait for txnStateStore recovery
	wait(success(commitData.txnStateStore->readValue(StringRef())));

	if(SERVER_KNOBS->REQUIRED_MIN_RECOVERY_DURATION > 0) {
		addActor.send(lastCommitUpdater(&commitData, addActor));
	}

	int commitBatchByteLimit =
	    (int)std::min<double>(SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_BYTES_MAX,
	                          std::max<double>(SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_BYTES_MIN,
	                                           SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_BYTES_SCALE_BASE *
	                                               pow(commitData.db->get().client.proxies.size(),
	                                                   SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_BYTES_SCALE_POWER)));

	commitBatcherActor = commitBatcher(&commitData, batchedCommits, proxy.commit.getFuture(), commitBatchByteLimit, commitBatchesMemoryLimit);
	loop choose{
		when( wait( dbInfoChange ) ) {
			dbInfoChange = commitData.db->onChange();
			if(commitData.db->get().master.id() == master.id() && commitData.db->get().recoveryState >= RecoveryState::RECOVERY_TRANSACTION) {
				commitData.logSystem = ILogSystem::fromServerDBInfo(proxy.id(), commitData.db->get(), false, addActor);
				for(auto it : commitData.tag_popped) {
					commitData.logSystem->pop(it.second, it.first);
				}
				commitData.logSystem->popTxs(commitData.lastTxsPop, tagLocalityRemoteLog);
			}

			commitData.updateLatencyBandConfig(commitData.db->get().latencyBandConfig);
		}
		when(wait(onError)) {}
		when(std::pair<vector<CommitTransactionRequest>, int> batchedRequests = waitNext(batchedCommits.getFuture())) {
			//WARNING: this code is run at a high priority, so it needs to do as little work as possible
			const vector<CommitTransactionRequest> &trs = batchedRequests.first;
			int batchBytes = batchedRequests.second;
			//TraceEvent("MasterProxyCTR", proxy.id()).detail("CommitTransactions", trs.size()).detail("TransactionRate", transactionRate).detail("TransactionQueue", transactionQueue.size()).detail("ReleasedTransactionCount", transactionCount);
			if (trs.size() || (commitData.db->get().recoveryState >= RecoveryState::ACCEPTING_COMMITS && now() - lastCommit >= SERVER_KNOBS->MAX_COMMIT_BATCH_INTERVAL)) {
				lastCommit = now();

				if (trs.size() || lastCommitComplete.isReady()) {
					lastCommitComplete = commitBatch(
						&commitData,
						const_cast<std::vector<CommitTransactionRequest>*>(&batchedRequests.first),
						batchBytes
					);
					addActor.send(lastCommitComplete);
				}
			}
		}
		when(ProxySnapRequest snapReq = waitNext(proxy.proxySnapReq.getFuture())) {
			TraceEvent(SevDebug, "SnapMasterEnqueue");
			addActor.send(proxySnapCreate(snapReq, &commitData));
		}
		when(ExclusionSafetyCheckRequest exclCheckReq = waitNext(proxy.exclusionSafetyCheckReq.getFuture())) {
			addActor.send(proxyCheckSafeExclusion(db, exclCheckReq));
		}
		when(state TxnStateRequest req = waitNext(proxy.txnState.getFuture())) {
			state ReplyPromise<Void> reply = req.reply;
			if(req.last) maxSequence = req.sequence + 1;
			if (!txnSequences.count(req.sequence)) {
				txnSequences.insert(req.sequence);

				ASSERT(!commitData.validState.isSet()); // Although we may receive the CommitTransactionRequest for the recovery transaction before all of the TxnStateRequest, we will not get a resolution result from any resolver until the master has submitted its initial (sequence 0) resolution request, which it doesn't do until we have acknowledged all TxnStateRequests

				for(auto& kv : req.data)
					commitData.txnStateStore->set(kv, &req.arena);
				commitData.txnStateStore->commit(true);

				if(txnSequences.size() == maxSequence) {
					state KeyRange txnKeys = allKeys;
					Standalone<RangeResultRef> UIDtoTagMap = commitData.txnStateStore->readRange( serverTagKeys ).get();
					state std::map<Tag, UID> tag_uid;
					for (const KeyValueRef kv : UIDtoTagMap) {
						tag_uid[decodeServerTagValue(kv.value)] = decodeServerTagKey(kv.key);
					}
					loop {
						wait(yield());
						Standalone<RangeResultRef> data = commitData.txnStateStore->readRange(txnKeys, SERVER_KNOBS->BUGGIFIED_ROW_LIMIT, SERVER_KNOBS->APPLY_MUTATION_BYTES).get();
						if(!data.size()) break;
						((KeyRangeRef&)txnKeys) = KeyRangeRef( keyAfter(data.back().key, txnKeys.arena()), txnKeys.end );

						MutationsVec mutations;
						std::vector<std::pair<MapPair<Key,ServerCacheInfo>,int>> keyInfoData;
						vector<UID> src, dest;
						ServerCacheInfo info;
						for(auto &kv : data) {
							if( kv.key.startsWith(keyServersPrefix) ) {
								KeyRef k = kv.key.removePrefix(keyServersPrefix);
								if(k != allKeys.end) {
									decodeKeyServersValue(tag_uid, kv.value, src, dest);
									info.tags.clear();
									info.src_info.clear();
									info.dest_info.clear();
									for (const auto& id : src) {
										auto storageInfo = getStorageInfo(id, &commitData.storageCache, commitData.txnStateStore);
										ASSERT(storageInfo->tag != invalidTag);
										info.tags.push_back( storageInfo->tag );
										info.src_info.push_back( storageInfo );
									}
									for (const auto& id : dest) {
										auto storageInfo = getStorageInfo(id, &commitData.storageCache, commitData.txnStateStore);
										ASSERT(storageInfo->tag != invalidTag);
										info.tags.push_back( storageInfo->tag );
										info.dest_info.push_back( storageInfo );
									}
									uniquify(info.tags);
									keyInfoData.emplace_back(MapPair<Key,ServerCacheInfo>(k, info), 1);
								}
							} else {
								mutations.emplace_back(mutations.arena(), MutationRef::SetValue, kv.key, kv.value);
							}
						}

						//insert keyTag data separately from metadata mutations so that we can do one bulk insert which avoids a lot of map lookups.
						commitData.keyInfo.rawInsert(keyInfoData);

						Arena arena;
						bool confChanges;
						applyMetadataMutations(commitData, arena, Reference<ILogSystem>(), mutations,
						                       /* pToCommit= */ nullptr, confChanges,
						                       /* popVersion= */ 0, /* initialCommit= */ true);
					}

					auto lockedKey = commitData.txnStateStore->readValue(databaseLockedKey).get();
					commitData.locked = lockedKey.present() && lockedKey.get().size();
					commitData.metadataVersion = commitData.txnStateStore->readValue(metadataVersionKey).get();

					commitData.txnStateStore->enableSnapshot();
				}
			}
			addActor.send(broadcastTxnRequest(req, SERVER_KNOBS->TXN_STATE_SEND_AMOUNT, true));
			wait(yield());
		}
	}
}

ACTOR Future<Void> checkRemoved(Reference<AsyncVar<ServerDBInfo>> db, uint64_t recoveryCount, MasterProxyInterface myInterface) {
	loop{
		if (db->get().recoveryCount >= recoveryCount && !std::count(db->get().client.proxies.begin(), db->get().client.proxies.end(), myInterface)) {
			throw worker_removed();
		}
		wait(db->onChange());
	}
}

ACTOR Future<Void> masterProxyServer(
	MasterProxyInterface proxy,
	InitializeMasterProxyRequest req,
	Reference<AsyncVar<ServerDBInfo>> db,
	std::string whitelistBinPaths)
{
	try {
		state Future<Void> core = masterProxyServerCore(proxy, req.master, db, req.recoveryCount, req.recoveryTransactionVersion, req.firstProxy, whitelistBinPaths);
		wait(core || checkRemoved(db, req.recoveryCount, proxy));
	}
	catch (Error& e) {
		TraceEvent("MasterProxyTerminated", proxy.id()).error(e, true);

		if (e.code() != error_code_worker_removed && e.code() != error_code_tlog_stopped &&
			e.code() != error_code_master_tlog_failed && e.code() != error_code_coordinators_changed &&
			e.code() != error_code_coordinated_state_conflict && e.code() != error_code_new_coordinators_timed_out) {
			throw;
		}
	}
	return Void();
}
