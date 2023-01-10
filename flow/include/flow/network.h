/*
 * network.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
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

#ifndef FLOW_OPENNETWORK_H
#define FLOW_OPENNETWORK_H
#pragma once

#include <string>
#include <stdint.h>
#include <atomic>
#include <boost/asio/ip/tcp.hpp>
#include "flow/Arena.h"
#include "flow/BooleanParam.h"
#include "flow/IRandom.h"
#include "flow/ProtocolVersion.h"
#include "flow/Trace.h"
#include "flow/TaskPriority.h"
#include "flow/WriteOnlySet.h"
#include "flow/IPAddress.h"

class Void;


template <>
struct Traceable<IPAddress> : std::true_type {
	static std::string toString(const IPAddress& value) { return value.toString(); }
};

FDB_DECLARE_BOOLEAN_PARAM(NetworkAddressFromHostname);

struct NetworkAddress {
	constexpr static FileIdentifier file_identifier = 14155727;
	// A NetworkAddress identifies a particular running server (i.e. a TCP endpoint).
	IPAddress ip;
	uint16_t port;
	uint16_t flags;
	bool fromHostname;

	enum { FLAG_PRIVATE = 1, FLAG_TLS = 2 };

	NetworkAddress()
	  : ip(IPAddress(0)), port(0), flags(FLAG_PRIVATE), fromHostname(NetworkAddressFromHostname::False) {}
	NetworkAddress(const IPAddress& address,
	               uint16_t port,
	               bool isPublic,
	               bool isTLS,
	               NetworkAddressFromHostname fromHostname = NetworkAddressFromHostname::False)
	  : ip(address), port(port), flags((isPublic ? 0 : FLAG_PRIVATE) | (isTLS ? FLAG_TLS : 0)),
	    fromHostname(fromHostname) {}
	NetworkAddress(uint32_t ip,
	               uint16_t port,
	               bool isPublic,
	               bool isTLS,
	               NetworkAddressFromHostname fromHostname = NetworkAddressFromHostname::False)
	  : NetworkAddress(IPAddress(ip), port, isPublic, isTLS, fromHostname) {}

	NetworkAddress(uint32_t ip, uint16_t port)
	  : NetworkAddress(ip, port, false, false, NetworkAddressFromHostname::False) {}
	NetworkAddress(const IPAddress& ip, uint16_t port)
	  : NetworkAddress(ip, port, false, false, NetworkAddressFromHostname::False) {}

	bool operator==(NetworkAddress const& r) const { return ip == r.ip && port == r.port && flags == r.flags; }
	bool operator!=(NetworkAddress const& r) const { return !(*this == r); }
	bool operator<(NetworkAddress const& r) const {
		if (flags != r.flags)
			return flags < r.flags;
		else if (ip != r.ip)
			return ip < r.ip;
		return port < r.port;
	}
	bool operator>(NetworkAddress const& r) const { return r < *this; }
	bool operator<=(NetworkAddress const& r) const { return !(*this > r); }
	bool operator>=(NetworkAddress const& r) const { return !(*this < r); }

	bool isValid() const { return ip.isValid() || port != 0; }
	bool isPublic() const { return !(flags & FLAG_PRIVATE); }
	bool isTLS() const { return (flags & FLAG_TLS) != 0; }
	bool isV6() const { return ip.isV6(); }

	size_t hash() const {
		size_t result = 0;
		if (ip.isV6()) {
			uint16_t* ptr = (uint16_t*)ip.toV6().data();
			result = ((size_t)ptr[5] << 32) | ((size_t)ptr[6] << 16) | ptr[7];
		} else {
			result = ip.toV4();
		}
		return (result << 16) + port;
	}

	static NetworkAddress parse(std::string const&); // May throw connection_string_invalid
	static Optional<NetworkAddress> parseOptional(std::string const&);
	static std::vector<NetworkAddress> parseList(std::string const&);
	std::string toString() const;

	template <class Ar>
	void serialize(Ar& ar) {
		if constexpr (is_fb_function<Ar>) {
			serializer(ar, ip, port, flags, fromHostname);
		} else {
			if (ar.isDeserializing && !ar.protocolVersion().hasIPv6()) {
				uint32_t ipV4;
				serializer(ar, ipV4, port, flags);
				ip = IPAddress(ipV4);
			} else {
				serializer(ar, ip, port, flags);
			}
			if (ar.protocolVersion().hasNetworkAddressHostnameFlag()) {
				serializer(ar, fromHostname);
			}
		}
	}
};

template <>
struct Traceable<NetworkAddress> : std::true_type {
	static std::string toString(const NetworkAddress& value) { return value.toString(); }
};

namespace std {
template <>
struct hash<NetworkAddress> {
	size_t operator()(const NetworkAddress& na) const { return na.hash(); }
};
} // namespace std

struct NetworkAddressList {
	NetworkAddress address;
	Optional<NetworkAddress> secondaryAddress{};

	bool operator==(NetworkAddressList const& r) const {
		return address == r.address && secondaryAddress == r.secondaryAddress;
	}
	bool operator!=(NetworkAddressList const& r) const {
		return address != r.address || secondaryAddress != r.secondaryAddress;
	}
	bool operator<(NetworkAddressList const& r) const {
		if (address != r.address)
			return address < r.address;
		return secondaryAddress < r.secondaryAddress;
	}

	NetworkAddress getTLSAddress() const {
		if (!secondaryAddress.present() || address.isTLS()) {
			return address;
		}
		return secondaryAddress.get();
	}

	std::string toString() const {
		if (!secondaryAddress.present()) {
			return address.toString();
		}
		return address.toString() + ", " + secondaryAddress.get().toString();
	}

	bool contains(const NetworkAddress& r) const {
		return address == r || (secondaryAddress.present() && secondaryAddress.get() == r);
	}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, address, secondaryAddress);
	}
};

std::string toIPVectorString(std::vector<uint32_t> ips);
std::string toIPVectorString(const std::vector<IPAddress>& ips);
std::string formatIpPort(const IPAddress& ip, uint16_t port);

template <class T>
class Future;
template <class T>
class Promise;

// Metrics which represent various network properties
struct NetworkMetrics {
	enum { SLOW_EVENT_BINS = 16 };
	uint64_t countSlowEvents[SLOW_EVENT_BINS] = {};

	double secSquaredSubmit = 0;
	double secSquaredDiskStall = 0;

	struct PriorityStats {
		TaskPriority priority;

		bool active = false;
		double duration = 0;
		double timer = 0;
		double windowedTimer = 0;
		double maxDuration = 0;

		PriorityStats(TaskPriority priority) : priority(priority) {}
	};

	std::unordered_map<TaskPriority, struct PriorityStats> activeTrackers;
	double lastRunLoopBusyness; // network thread busyness (measured every 5s by default)
	std::atomic<double>
	    networkBusyness; // network thread busyness which is returned to the the client (measured every 1s by default)

	// starvation trackers which keeps track of different task priorities
	std::vector<struct PriorityStats> starvationTrackers;
	struct PriorityStats starvationTrackerNetworkBusyness;

	static const std::vector<int> starvationBins;

	NetworkMetrics()
	  : lastRunLoopBusyness(0), networkBusyness(0),
	    starvationTrackerNetworkBusyness(PriorityStats(static_cast<TaskPriority>(starvationBins.at(0)))) {
		for (int priority : starvationBins) { // initalize starvation trackers with given priorities
			starvationTrackers.emplace_back(static_cast<TaskPriority>(priority));
		}
	}

	// Since networkBusyness is atomic we need to redefine copy assignment operator
	NetworkMetrics& operator=(const NetworkMetrics& rhs) {
		for (int i = 0; i < SLOW_EVENT_BINS; i++) {
			countSlowEvents[i] = rhs.countSlowEvents[i];
		}
		secSquaredSubmit = rhs.secSquaredSubmit;
		secSquaredDiskStall = rhs.secSquaredDiskStall;
		activeTrackers = rhs.activeTrackers;
		lastRunLoopBusyness = rhs.lastRunLoopBusyness;
		networkBusyness = rhs.networkBusyness.load();
		starvationTrackers = rhs.starvationTrackers;
		starvationTrackerNetworkBusyness = rhs.starvationTrackerNetworkBusyness;
		return *this;
	}
};

struct FlowLock;

struct NetworkInfo {
	NetworkMetrics metrics;
	double oldestAlternativesFailure = 0;
	double newestAlternativesFailure = 0;
	double lastAlternativesFailureSkipDelay = 0;

	std::map<std::pair<IPAddress, uint16_t>, std::pair<int, double>> serverTLSConnectionThrottler;
	FlowLock* handshakeLock;

	NetworkInfo();
};

class IEventFD : public ReferenceCounted<IEventFD> {
public:
	virtual ~IEventFD() {}
	virtual int getFD() = 0;
	virtual Future<int64_t> read() = 0;
};

// forward declare SendBuffer, declared in serialize.h
class SendBuffer;

class IConnection {
public:
	// IConnection is reference-counted (use Reference<IConnection>), but the caller must explicitly call close()
	virtual void addref() = 0;
	virtual void delref() = 0;

	// Closes the underlying connection eventually if it is not already closed.
	virtual void close() = 0;

	virtual Future<Void> acceptHandshake() = 0;

	virtual Future<Void> connectHandshake() = 0;

	// Precondition: write() has been called and last returned 0
	// returns when write() can write at least one byte (or may throw an error if the connection dies)
	virtual Future<Void> onWritable() = 0;

	// Precondition: read() has been called and last returned 0
	// returns when read() can read at least one byte (or may throw an error if the connection dies)
	virtual Future<Void> onReadable() = 0;

	// Reads as many bytes as possible from the read buffer into [begin,end) and returns the number of bytes read (might
	// be 0) (or may throw an error if the connection dies)
	virtual int read(uint8_t* begin, uint8_t* end) = 0;

	// Writes as many bytes as possible from the given SendBuffer chain into the write buffer and returns the number of
	// bytes written (might be 0) (or may throw an error if the connection dies) The SendBuffer chain cannot be empty,
	// and the limit must be positive. Important non-obvious behavior:  The caller is committing to write the contents
	// of the buffer chain up to the limit.  If all of those bytes could not be sent in this call to write() then
	// further calls must be made to write the remainder.  An IConnection implementation can make decisions based on the
	// entire byte set that the caller was attempting to write even if it is unable to write all of it immediately. Due
	// to limitations of TLSConnection, callers must also avoid reallocations that reduce the amount of written data in
	// the first buffer in the chain.
	virtual int write(SendBuffer const* buffer, int limit = std::numeric_limits<int>::max()) = 0;

	// Returns the network address and port of the other end of the connection.  In the case of an incoming connection,
	// this may not be an address we can connect to!
	virtual NetworkAddress getPeerAddress() const = 0;

	// Returns whether the peer is trusted.
	// For TLS-enabled connections, this is true if the peer has presented a valid chain of certificates trusted by the
	// local endpoint. For non-TLS connections this is always true for any valid open connection.
	virtual bool hasTrustedPeer() const = 0;

	virtual UID getDebugID() const = 0;

	// At present, implemented by Sim2Conn where we want to disable bits flip for connections between parent process and
	// child process, also reduce latency for this kind of connection
	virtual bool isStableConnection() const { throw unsupported_operation(); }

	virtual boost::asio::ip::tcp::socket& getSocket() = 0;
};

class IListener {
public:
	virtual void addref() = 0;
	virtual void delref() = 0;

	// Returns one incoming connection when it is available.  Do not cancel unless you are done with the listener!
	virtual Future<Reference<IConnection>> accept() = 0;

	virtual NetworkAddress getListenAddress() const = 0;
};

typedef void* flowGlobalType;
typedef NetworkAddress (*NetworkAddressFuncPtr)();
typedef NetworkAddressList (*NetworkAddressesFuncPtr)();

class TLSConfig;
class INetwork;
extern INetwork* g_network;
extern INetwork* newNet2(const TLSConfig& tlsConfig, bool useThreadPool = false, bool useMetrics = false);

class INetwork {
public:
	// This interface abstracts the physical or simulated network, event loop and hardware that FoundationDB is running
	// on. Note that there are tools for disk access, scheduling, etc as well as networking, and that almost all access
	//   to the network should be through FlowTransport, not directly through these low level interfaces!

	// Time instants (e.g. from now()) within TIME_EPS are considered to be equal.
	static constexpr double TIME_EPS = 1e-7; // 100ns

	enum enumGlobal {
		enFailureMonitor = 0,
		enFlowTransport = 1,
		enTDMetrics = 2,
		enNetworkConnections = 3,
		enNetworkAddressFunc = 4,
		enFileSystem = 5,
		enASIOService = 6,
		enEventFD = 7,
		enRunCycleFunc = 8,
		enASIOTimedOut = 9,
		enBlobCredentialFiles = 10,
		enNetworkAddressesFunc = 11,
		enClientFailureMonitor = 12,
		enSQLiteInjectedError = 13,
		enGlobalConfig = 14,
		enChaosMetrics = 15,
		enDiskFailureInjector = 16,
		enBitFlipper = 17,
		enHistogram = 18,
		enTokenCache = 19,
		enMetrics = 20,
		COUNT // Add new fields before this enumerator
	};

	virtual void longTaskCheck(const char* name) {}

	virtual double now() const = 0;
	// Provides a clock that advances at a similar rate on all connected endpoints
	// FIXME: Return a fixed point Time class

	virtual double timer() = 0;
	// A wrapper for directly getting the system time. The time returned by now() only updates in the run loop,
	// so it cannot be used to measure times of functions that do not have wait statements.

	// Simulation version of timer_int for convenience, based on timer()
	// Returns epoch nanoseconds
	uint64_t timer_int() { return (uint64_t)(g_network->timer() * 1e9); }

	virtual double timer_monotonic() = 0;
	// Similar to timer, but monotonic

	virtual Future<class Void> delay(double seconds, TaskPriority taskID) = 0;
	// The given future will be set after seconds have elapsed

	virtual Future<class Void> orderedDelay(double seconds, TaskPriority taskID) = 0;
	// The given future will be set after seconds have elapsed, delays with the same time and TaskPriority will be
	// executed in the order they were issues

	virtual Future<class Void> yield(TaskPriority taskID) = 0;
	// The given future will be set immediately or after higher-priority tasks have executed

	virtual bool check_yield(TaskPriority taskID) = 0;
	// Returns true if a call to yield would result in a delay

	virtual TaskPriority getCurrentTask() const = 0;
	// Gets the taskID/priority of the current task

	virtual void setCurrentTask(TaskPriority taskID) = 0;
	// Sets the taskID/priority of the current task, without yielding

	virtual flowGlobalType global(int id) const = 0;
	virtual void setGlobal(size_t id, flowGlobalType v) = 0;

	virtual void stop() = 0;
	// Terminate the program

	virtual void addStopCallback(std::function<void()> fn) = 0;
	// Calls `fn` when stop() is called.
	// addStopCallback can be called more than once, and each added `fn` will be run once.

	virtual bool isSimulated() const = 0;
	// Returns true if this network is a local simulation

	virtual bool isOnMainThread() const = 0;
	// Returns true if the current thread is the main thread

	virtual void onMainThread(Promise<Void>&& signal, TaskPriority taskID) = 0;
	// Executes signal.send(Void()) on a/the thread belonging to this network in FIFO order

	virtual THREAD_HANDLE startThread(THREAD_FUNC_RETURN (*func)(void*),
	                                  void* arg,
	                                  int stackSize = 0,
	                                  const char* name = nullptr) = 0;
	// Starts a thread and returns a handle to it

	virtual void run() = 0;
	// Devotes this thread to running the network (generally until stop())

	virtual void initMetrics() {}
	// Metrics must be initialized after FlowTransport::createInstance has been called

	// TLS must be initialized before using the network
	enum ETLSInitState { NONE = 0, CONFIG = 1, CONNECT = 2, LISTEN = 3 };
	virtual void initTLS(ETLSInitState targetState = CONFIG) {}

	virtual const TLSConfig& getTLSConfig() const = 0;
	// Return the TLS Configuration

	virtual void getDiskBytes(std::string const& directory, int64_t& free, int64_t& total) = 0;
	// Gets the number of free and total bytes available on the disk which contains directory

	virtual bool isAddressOnThisHost(NetworkAddress const& addr) const = 0;
	// Returns true if it is reasonably certain that a connection to the given address would be a fast loopback
	// connection

	// If the network has not been run and this function has not been previously called, returns true. Otherwise,
	// returns false.
	virtual bool checkRunnable() = 0;

#ifdef ENABLE_SAMPLING
	// Returns the shared memory data structure used to store actor lineages.
	virtual ActorLineageSet& getActorLineageSet() = 0;
#endif

	virtual ProtocolVersion protocolVersion() const = 0;

	// Shorthand for transport().getLocalAddress()
	static NetworkAddress getLocalAddress() {
		flowGlobalType netAddressFuncPtr =
		    reinterpret_cast<flowGlobalType>(g_network->global(INetwork::enNetworkAddressFunc));
		return (netAddressFuncPtr) ? reinterpret_cast<NetworkAddressFuncPtr>(netAddressFuncPtr)() : NetworkAddress();
	}

	// Shorthand for transport().getLocalAddresses()
	static NetworkAddressList getLocalAddresses() {
		flowGlobalType netAddressesFuncPtr =
		    reinterpret_cast<flowGlobalType>(g_network->global(INetwork::enNetworkAddressesFunc));
		return (netAddressesFuncPtr) ? reinterpret_cast<NetworkAddressesFuncPtr>(netAddressesFuncPtr)()
		                             : NetworkAddressList();
	}

	NetworkInfo networkInfo;

protected:
	INetwork() {}

	~INetwork() {} // Please don't try to delete through this interface!
};

// DNSCache is a class maintaining a <hostname, vector<NetworkAddress>> mapping.
class DNSCache {
public:
	DNSCache() = default;
	explicit DNSCache(const std::map<std::string, std::vector<NetworkAddress>>& dnsCache)
	  : hostnameToAddresses(dnsCache) {}

	Optional<std::vector<NetworkAddress>> find(const std::string& host, const std::string& service);
	void add(const std::string& host, const std::string& service, const std::vector<NetworkAddress>& addresses);
	void remove(const std::string& host, const std::string& service);
	void clear();

	// Convert hostnameToAddresses to string. The format is:
	// hostname1,host1Address1,host1Address2;hostname2,host2Address1,host2Address2...
	std::string toString();
	static DNSCache parseFromString(const std::string& s);

private:
	std::map<std::string, std::vector<NetworkAddress>> hostnameToAddresses;
};

class IUDPSocket;

class INetworkConnections {
public:
	// Methods for making and accepting network connections.  Logically this is part of the INetwork abstraction
	// that abstracts all interaction with the physical world; it is separated out to make it easy for e.g. transport
	// security to override only these operations without having to delegate everything in INetwork.

	// Make an outgoing connection to the given address.  May return an error or block indefinitely in case of
	// connection problems!
	virtual Future<Reference<IConnection>> connect(NetworkAddress toAddr,
	                                               boost::asio::ip::tcp::socket* existingSocket = nullptr) = 0;

	virtual Future<Reference<IConnection>> connectExternal(NetworkAddress toAddr) = 0;

	// Make an outgoing udp connection and connect to the passed address.
	virtual Future<Reference<IUDPSocket>> createUDPSocket(NetworkAddress toAddr) = 0;
	// Make an outgoing udp connection without establishing a connection
	virtual Future<Reference<IUDPSocket>> createUDPSocket(bool isV6 = false) = 0;

	virtual void addMockTCPEndpoint(const std::string& host,
	                                const std::string& service,
	                                const std::vector<NetworkAddress>& addresses) = 0;
	virtual void removeMockTCPEndpoint(const std::string& host, const std::string& service) = 0;
	virtual void parseMockDNSFromString(const std::string& s) = 0;
	virtual std::string convertMockDNSToString() = 0;
	// Resolve host name and service name (such as "http" or can be a plain number like "80") to a list of 1 or more
	// NetworkAddresses
	virtual Future<std::vector<NetworkAddress>> resolveTCPEndpoint(const std::string& host,
	                                                               const std::string& service) = 0;
	// Similar to resolveTCPEndpoint(), except that this one uses DNS cache.
	virtual Future<std::vector<NetworkAddress>> resolveTCPEndpointWithDNSCache(const std::string& host,
	                                                                           const std::string& service) = 0;
	// Resolve host name and service name. This one should only be used when resolving asynchronously is impossible. For
	// all other cases, resolveTCPEndpoint() should be preferred.
	virtual std::vector<NetworkAddress> resolveTCPEndpointBlocking(const std::string& host,
	                                                               const std::string& service) = 0;
	// Resolve host name and service name with DNS cache. This one should only be used when resolving asynchronously is
	// impossible. For all other cases, resolveTCPEndpointWithDNSCache() should be preferred.
	virtual std::vector<NetworkAddress> resolveTCPEndpointBlockingWithDNSCache(const std::string& host,
	                                                                           const std::string& service) = 0;

	// Convenience function to resolve host/service and connect to one of its NetworkAddresses randomly
	// isTLS has to be a parameter here because it is passed to connect() as part of the toAddr object.
	virtual Future<Reference<IConnection>> connect(const std::string& host,
	                                               const std::string& service,
	                                               bool isTLS = false);

	// Listen for connections on the given local address
	virtual Reference<IListener> listen(NetworkAddress localAddr) = 0;

	static INetworkConnections* net() {
		return static_cast<INetworkConnections*>((void*)g_network->global(INetwork::enNetworkConnections));
	}

	// If a DNS name can be resolved to both and IPv4 and IPv6 addresses, we want IPv6 addresses when running the
	// clusters on IPv6.
	// This function takes a vector of addresses and return a random one, preferring IPv6 over IPv4.
	static NetworkAddress pickOneAddress(const std::vector<NetworkAddress>& addresses) {
		std::vector<NetworkAddress> ipV6Addresses;
		for (const NetworkAddress& addr : addresses) {
			if (addr.isV6()) {
				ipV6Addresses.push_back(addr);
			}
		}
		if (ipV6Addresses.size() > 0) {
			return ipV6Addresses[deterministicRandom()->randomInt(0, ipV6Addresses.size())];
		}
		return addresses[deterministicRandom()->randomInt(0, addresses.size())];
	}

	void removeCachedDNS(const std::string& host, const std::string& service) { dnsCache.remove(host, service); }

	DNSCache dnsCache;

	// Returns the interface that should be used to make and accept socket connections
};

// Chaos Metrics - We periodically log chaosMetrics to make sure that chaos events are happening
// Only includes DiskDelays which encapsulates all type delays and BitFlips for now
// Expand as per need
struct ChaosMetrics {

	ChaosMetrics() { clear(); }

	void clear() {
		memset(this, 0, sizeof(ChaosMetrics));
		startTime = g_network ? g_network->now() : 0;
	}

	unsigned int diskDelays;
	unsigned int bitFlips;
	double startTime;

	void getFields(TraceEvent* e) {
		std::pair<const char*, unsigned int> metrics[] = { { "DiskDelays", diskDelays }, { "BitFlips", bitFlips } };
		if (e != nullptr) {
			for (auto& m : metrics) {
				char c = m.first[0];
				if (c != 0) {
					e->detail(m.first, m.second);
				}
			}
		}
	}
};

// This class supports injecting two type of disk failures
// 1. Stalls: Every interval seconds, the disk will stall and no IO will complete for x seconds, where x is a randomly
// chosen interval
// 2. Slowdown: Random slowdown is injected to each disk operation for specified period of time
struct DiskFailureInjector {
	static DiskFailureInjector* injector() {
		auto res = g_network->global(INetwork::enDiskFailureInjector);
		if (!res) {
			res = new DiskFailureInjector();
			g_network->setGlobal(INetwork::enDiskFailureInjector, res);
		}
		return static_cast<DiskFailureInjector*>(res);
	}

	void setDiskFailure(double interval, double stallFor, double throttleFor) {
		stallInterval = interval;
		stallPeriod = stallFor;
		stallUntil = std::max(stallUntil, g_network->now() + stallFor);
		// random stall duration in ms (chosen once)
		// TODO: make this delay configurable
		stallDuration = 0.001 * deterministicRandom()->randomInt(1, 5);
		throttlePeriod = throttleFor;
		throttleUntil = std::max(throttleUntil, g_network->now() + throttleFor);
		TraceEvent("SetDiskFailure")
		    .detail("Now", g_network->now())
		    .detail("StallInterval", interval)
		    .detail("StallPeriod", stallFor)
		    .detail("StallUntil", stallUntil)
		    .detail("ThrottlePeriod", throttleFor)
		    .detail("ThrottleUntil", throttleUntil);
	}

	double getStallDelay() {
		// If we are in a stall period and a stallInterval was specified, determine the
		// delay to be inserted
		if (((stallUntil - g_network->now()) > 0.0) && stallInterval) {
			auto timeElapsed = fmod(g_network->now(), stallInterval);
			return std::max(0.0, stallDuration - timeElapsed);
		}
		return 0.0;
	}

	double getThrottleDelay() {
		// If we are in the throttle period, insert a random delay (in ms)
		// TODO: make this delay configurable
		if ((throttleUntil - g_network->now()) > 0.0)
			return (0.001 * deterministicRandom()->randomInt(1, 3));

		return 0.0;
	}

	double getDiskDelay() { return getStallDelay() + getThrottleDelay(); }

private: // members
	double stallInterval = 0.0; // how often should the disk be stalled (0 meaning once, 10 meaning every 10 secs)
	double stallPeriod; // Period of time disk stalls will be injected for
	double stallUntil; // End of disk stall period
	double stallDuration; // Duration of each stall
	double throttlePeriod; // Period of time the disk will be slowed down for
	double throttleUntil; // End of disk slowdown period

private: // construction
	DiskFailureInjector() = default;
	DiskFailureInjector(DiskFailureInjector const&) = delete;
};

struct BitFlipper {
	static BitFlipper* flipper() {
		auto res = g_network->global(INetwork::enBitFlipper);
		if (!res) {
			res = new BitFlipper();
			g_network->setGlobal(INetwork::enBitFlipper, res);
		}
		return static_cast<BitFlipper*>(res);
	}

	double getBitFlipPercentage() { return bitFlipPercentage; }

	void setBitFlipPercentage(double percentage) { bitFlipPercentage = percentage; }

private: // members
	double bitFlipPercentage = 0.0;

private: // construction
	BitFlipper() = default;
	BitFlipper(BitFlipper const&) = delete;
};
#endif
