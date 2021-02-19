/*
 * GlobalConfig.actor.cpp
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

#include "fdbclient/GlobalConfig.actor.h"

#include "flow/actorcompiler.h"  // This must be the last #include.

GlobalConfig::GlobalConfig() : lastUpdate(0) {}

void GlobalConfig::create(DatabaseContext* cx, Reference<AsyncVar<ClientDBInfo>> dbInfo) {
	auto config = new GlobalConfig{}; // TODO: memory leak?
	config->cx = Database(cx);
	g_network->setGlobal(INetwork::enGlobalConfig, config);
	config->_updater = updater(config, dbInfo);
}

GlobalConfig& GlobalConfig::globalConfig() {
	void* res = g_network->global(INetwork::enGlobalConfig);
	ASSERT(res);
	return *reinterpret_cast<GlobalConfig*>(res);
}

const std::any GlobalConfig::get(KeyRef name) {
	auto it = data.find(name);
	if (it == data.end()) {
		return std::any{};
	}
	return it->second;
}

const std::map<KeyRef, std::any> GlobalConfig::get(KeyRangeRef range) {
	std::map<KeyRef, std::any> results;
	for (const auto& [key, value] : data) {
		if (range.contains(key)) {
			results[key] = value;
		}
	}
	return results;
}

Future<Void> GlobalConfig::onInitialized() {
	return initialized.getFuture();
}

void GlobalConfig::insert(KeyRef key, ValueRef value) {
	KeyRef stableKey = KeyRef(arena, key);
	Tuple t = Tuple::unpack(value);
	if (t.getType(0) == Tuple::ElementType::UTF8) {
		data[stableKey] = t.getString(0);
	} else if (t.getType(0) == Tuple::ElementType::INT) {
		data[stableKey] = t.getInt(0);
	} else if (t.getType(0) == Tuple::ElementType::FLOAT) {
		data[stableKey] = t.getFloat(0);
	} else if (t.getType(0) == Tuple::ElementType::DOUBLE) {
		data[stableKey] = t.getDouble(0);
	} else {
		ASSERT(false);
	}
}

void GlobalConfig::erase(KeyRef key) {
	erase(KeyRangeRef(key, keyAfter(key)));
}

void GlobalConfig::erase(KeyRangeRef range) {
	// TODO: Memory leak -- memory for key remains allocated in arena
	auto it = data.begin();
	while (it != data.end()) {
		if (range.contains(it->first)) {
			it = data.erase(it);
		} else {
			++it;
		}
	}
}
