/*
 * experiment.actor.cpp

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

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/ReadYourWrites.h"
#include "flow/DeterministicRandom.h"
#include "flow/flow.h"
#include "flow/Platform.h"
#include "flow/TLSConfig.actor.h"

#include "flow/actorcompiler.h"

NetworkAddress serverAddress;

void arenaTest() {
	Arena arena;

	char* test = new (arena) char[100];
	strcpy(test, "test string");

	StringRef ref(reinterpret_cast<uint8_t*>(test), strlen(test));

	std::vector<std::pair<Arena, StringRef>> vec;

	std::cout << "size of arena: " << arena.getSize() << std::endl;

	vec.emplace_back(std::make_pair(arena, ref));

	std::cout << vec.back().second.toString() << std::endl;
}

ACTOR Future<Void> experimental() {
	std::cout << "Hello," << std::flush;
	arenaTest();
	wait(delay(1.0));
	std::cout << " world!" << std::endl;

	return Void();
}

Future<int> setFuture() {
	Promise<int> promise;
	promise.send(Never());
	return promise.getFuture();
}

ACTOR Future<Void> testSetFuture() {
	try {
		state int k = wait(setFuture());
		std::cout << k << std::endl;
	} catch (Error& err) {
		std::cout << err.what() << std::endl;
	}
	return Void();
}

int main(int argc, char* argv[]) {
	bool isServer = false;
	std::string port;
	std::vector<std::function<Future<Void>()>> toRun;

	platformInit();
	g_network = newNet2(TLSConfig(), false, true);

	FlowTransport::createInstance(!isServer, 0);
	NetworkAddress publicAddress = NetworkAddress::parse("0.0.0.0:0");
	if (isServer) {
		publicAddress = NetworkAddress::parse("0.0.0.0:" + port);
	}

	try {
		if (isServer) {
			auto listenError = FlowTransport::transport().bind(publicAddress, publicAddress);
			if (listenError.isError()) {
				listenError.get();
			}
		}
	} catch (Error& e) {
		std::cout << format("Error while binding to address (%d): %s\n", e.code(), e.what());
	}
	// now we start the actors
	std::vector<Future<Void>> all;
	all.emplace_back(experimental());

	auto f = stopAfter(waitForAll(all));
	g_network->run();

	return 0;
}
