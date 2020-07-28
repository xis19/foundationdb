/*
 * HealthMetricsApi.actor.cpp
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

#include "fdbserver/TesterInterface.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "flow/actorcompiler.h" // This must be the last #include.

// workload description
// This workload can be attached to other workload to collect health information about the FDB cluster.
struct HealthMetricsApiWorkload : TestWorkload {
	// Performance Metrics
	int64_t worstStorageQueue = 0;
	int64_t worstStorageDurabilityLag = 0;
	int64_t worstTLogQueue = 0;
	int64_t detailedWorstStorageQueue = 0;
	int64_t detailedWorstStorageDurabilityLag = 0;
	int64_t detailedWorstTLogQueue = 0;
	double detailedWorstCpuUsage = 0;
	double detailedWorstDiskUsage = 0;

	// Test configuration
	double testDuration;
	double healthMetricsCheckInterval;
	double maxAllowedStaleness;
	bool sendDetailedHealthMetrics;

	// internal states
	bool healthMetricsStoppedUpdating = false;
	static constexpr const char* NAME = "HealthMetricsApi";

	HealthMetricsApiWorkload(WorkloadContext const& wcx) : TestWorkload(wcx) {
		testDuration = getOption(options, LiteralStringRef("testDuration"), 120.0);
		healthMetricsCheckInterval = getOption(options, LiteralStringRef("healthMetricsCheckInterval"), 1.0);
		sendDetailedHealthMetrics = getOption(options, LiteralStringRef("sendDetailedHealthMetrics"), true);
		maxAllowedStaleness = getOption(options, LiteralStringRef("maxAllowedStaleness"), 60.0);
	}

	virtual std::string description() { return HealthMetricsApiWorkload::NAME; }

	ACTOR static Future<Void> _setup(Database cx, HealthMetricsApiWorkload* self) {
		if (!self->sendDetailedHealthMetrics) {
			// Clear detailed health metrics that are already populated
			wait(delay(2 * CLIENT_KNOBS->DETAILED_HEALTH_METRICS_MAX_STALENESS));
			cx->healthMetrics.storageStats.clear();
			cx->healthMetrics.tLogQueue.clear();
		}
		return Void();
	}
	virtual Future<Void> setup(Database const& cx) { return _setup(cx, this); }
	ACTOR static Future<Void> _start(Database cx, HealthMetricsApiWorkload* self) {
		wait(timeout(healthMetricsChecker(cx, self), self->testDuration, Void()));
		return Void();
	}
	virtual Future<Void> start(Database const& cx) { return _start(cx, this); }

	virtual Future<bool> check(Database const& cx) {
		if (healthMetricsStoppedUpdating) {
			TraceEvent(SevError, "HealthMetricsStoppedUpdating");
			return false;
		}
		bool correctHealthMetricsState = true;
		if (worstStorageQueue == 0 || worstStorageDurabilityLag == 0 || worstTLogQueue == 0)
			correctHealthMetricsState = false;
		if (sendDetailedHealthMetrics) {
			if (detailedWorstStorageQueue == 0 || detailedWorstStorageDurabilityLag == 0 ||
			    detailedWorstTLogQueue == 0 || detailedWorstCpuUsage == 0.0 || detailedWorstDiskUsage == 0.0)
				correctHealthMetricsState = false;
		} else {
			if (detailedWorstStorageQueue != 0 || detailedWorstStorageDurabilityLag != 0 ||
			    detailedWorstTLogQueue != 0 || detailedWorstCpuUsage != 0.0 || detailedWorstDiskUsage != 0.0)
				correctHealthMetricsState = false;
		}
		if (!correctHealthMetricsState) {
			TraceEvent(SevError, "IncorrectHealthMetricsState")
			    .detail("WorstStorageQueue", worstStorageQueue)
			    .detail("WorstStorageDurabilityLag", worstStorageDurabilityLag)
			    .detail("WorstTLogQueue", worstTLogQueue)
			    .detail("DetailedWorstStorageQueue", detailedWorstStorageQueue)
			    .detail("DetailedWorstStorageDurabilityLag", detailedWorstStorageDurabilityLag)
			    .detail("DetailedWorstTLogQueue", detailedWorstTLogQueue)
			    .detail("DetailedWorstCpuUsage", detailedWorstCpuUsage)
			    .detail("DetailedWorstDiskUsage", detailedWorstDiskUsage)
			    .detail("SendingDetailedHealthMetrics", sendDetailedHealthMetrics);
		}
		return correctHealthMetricsState;
	}

	virtual void getMetrics(vector<PerfMetric>& m) {
		m.push_back(PerfMetric("WorstStorageQueue", worstStorageQueue, true));
		m.push_back(PerfMetric("DetailedWorstStorageQueue", detailedWorstStorageQueue, true));
		m.push_back(PerfMetric("WorstStorageDurabilityLag", worstStorageDurabilityLag, true));
		m.push_back(PerfMetric("DetailedWorstStorageDurabilityLag", detailedWorstStorageDurabilityLag, true));
		m.push_back(PerfMetric("WorstTLogQueue", worstTLogQueue, true));
		m.push_back(PerfMetric("DetailedWorstTLogQueue", detailedWorstTLogQueue, true));
		m.push_back(PerfMetric("DetailedWorstCpuUsage", detailedWorstCpuUsage, true));
		m.push_back(PerfMetric("DetailedWorstDiskUsage", detailedWorstDiskUsage, true));
	}

	ACTOR static Future<Void> healthMetricsChecker(Database cx, HealthMetricsApiWorkload* self) {
		state int repeated = 0;
		state HealthMetrics healthMetrics;
		loop {
			wait(delay(self->healthMetricsCheckInterval));
			HealthMetrics newHealthMetrics = wait(cx->getHealthMetrics(self->sendDetailedHealthMetrics));
			if (healthMetrics == newHealthMetrics) {
				if (++repeated > self->maxAllowedStaleness / self->healthMetricsCheckInterval)
					self->healthMetricsStoppedUpdating = true;
			} else
				repeated = 0;
			healthMetrics = newHealthMetrics;

			self->worstStorageQueue = std::max(self->worstStorageQueue, healthMetrics.worstStorageQueue);
			self->worstStorageDurabilityLag =
			    std::max(self->worstStorageDurabilityLag, healthMetrics.worstStorageDurabilityLag);
			self->worstTLogQueue = std::max(self->worstTLogQueue, healthMetrics.worstTLogQueue);

			TraceEvent("HealthMetrics")
			    .detail("WorstStorageQueue", healthMetrics.worstStorageQueue)
			    .detail("WorstStorageDurabilityLag", healthMetrics.worstStorageDurabilityLag)
			    .detail("WorstTLogQueue", healthMetrics.worstTLogQueue)
			    .detail("TpsLimit", healthMetrics.tpsLimit);

			TraceEvent traceStorageQueue("StorageQueue");
			TraceEvent traceStorageDurabilityLag("StorageDurabilityLag");
			TraceEvent traceCpuUsage("CpuUsage");
			TraceEvent traceDiskUsage("DiskUsage");

			// update metrics
			for (const auto& ss : healthMetrics.storageStats) {
				auto storageStats = ss.second;
				self->detailedWorstStorageQueue = std::max(self->detailedWorstStorageQueue, storageStats.storageQueue);
				traceStorageQueue.detail(format("Storage-%s", ss.first.toString().c_str()), storageStats.storageQueue);
				self->detailedWorstStorageDurabilityLag =
				    std::max(self->detailedWorstStorageDurabilityLag, storageStats.storageDurabilityLag);
				traceStorageDurabilityLag.detail(format("Storage-%s", ss.first.toString().c_str()),
				                                 storageStats.storageDurabilityLag);
				self->detailedWorstCpuUsage = std::max(self->detailedWorstCpuUsage, storageStats.cpuUsage);
				traceCpuUsage.detail(format("Storage-%s", ss.first.toString().c_str()), storageStats.cpuUsage);
				self->detailedWorstDiskUsage = std::max(self->detailedWorstDiskUsage, storageStats.diskUsage);
				traceDiskUsage.detail(format("Storage-%s", ss.first.toString().c_str()), storageStats.diskUsage);
			}
			TraceEvent traceTLogQueue("TLogQueue");
			for (const auto& ss : healthMetrics.tLogQueue) {
				self->detailedWorstTLogQueue = std::max(self->detailedWorstTLogQueue, ss.second);
				traceTLogQueue.detail(format("TLog-%s", ss.first.toString().c_str()), ss.second);
			}
		};
	}
};
WorkloadFactory<HealthMetricsApiWorkload> HealthMetricsApiWorkloadFactory(HealthMetricsApiWorkload::NAME);
