/*
 * LogSystemPeekCursor.actor.cpp
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

#include "fdbserver/LogSystem.h"
#include "fdbrpc/FailureMonitor.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/MutationTracking.h"
#include "fdbrpc/ReplicationUtils.h"
#include "flow/actorcompiler.h" // has to be last include

// create a peek stream for cursor when it's possible
void tryEstablishPeekStream(ILogSystem::ServerPeekCursor* self) {
	if (self->peekReplyStream.present())
		return;
	else if (!self->interf || !self->interf->get().present()) {
		self->peekReplyStream.reset();
		return;
	}
	self->peekReplyStream = self->interf->get().interf().peekStreamMessages.getReplyStream(TLogPeekStreamRequest(
	    self->messageVersion.version, self->tag, self->returnIfBlocked, std::numeric_limits<int>::max()));
	TraceEvent(SevDebug, "StreamCreated");
}

ILogSystem::ServerPeekCursor::ServerPeekCursor(Reference<AsyncVar<OptionalInterface<TLogInterface>>> const& interf,
                                               Tag tag,
                                               Version begin,
                                               Version end,
                                               bool returnIfBlocked,
                                               bool parallelGetMore)
  : interf(interf), tag(tag), messageVersion(begin), end(end), hasMsg(false),
    rd(results.arena, results.messages, Unversioned()), randomID(deterministicRandom()->randomUniqueID()),
    poppedVersion(0), returnIfBlocked(returnIfBlocked), sequence(0), onlySpilled(false),
    parallelGetMore(parallelGetMore), lastReset(0), slowReplies(0), fastReplies(0), unknownReplies(0),
    resetCheck(Void()), usePeekStream(SERVER_KNOBS->PEEK_USEING_STREAMING) {
	this->results.maxKnownVersion = 0;
	this->results.minKnownCommittedVersion = 0;
	//TraceEvent("SPC_Starting", randomID).detail("Tag", tag.toString()).detail("Begin", begin).detail("End", end).backtrace();
}

ILogSystem::ServerPeekCursor::ServerPeekCursor(TLogPeekReply const& results,
                                               LogMessageVersion const& messageVersion,
                                               LogMessageVersion const& end,
                                               TagsAndMessage const& message,
                                               bool hasMsg,
                                               Version poppedVersion,
                                               Tag tag)
  : results(results), tag(tag), rd(results.arena, results.messages, Unversioned()), messageVersion(messageVersion),
    end(end), messageAndTags(message), hasMsg(hasMsg), randomID(deterministicRandom()->randomUniqueID()),
    poppedVersion(poppedVersion), returnIfBlocked(false), sequence(0), onlySpilled(false), parallelGetMore(false),
    lastReset(0), slowReplies(0), fastReplies(0), unknownReplies(0), resetCheck(Void()), usePeekStream(false) {
	//TraceEvent("SPC_Clone", randomID);
	this->results.maxKnownVersion = 0;
	this->results.minKnownCommittedVersion = 0;
	if (hasMsg)
		nextMessage();

	advanceTo(messageVersion);
}

Reference<ILogSystem::IPeekCursor> ILogSystem::ServerPeekCursor::cloneNoMore() {
	return makeReference<ILogSystem::ServerPeekCursor>(
	    results, messageVersion, end, messageAndTags, hasMsg, poppedVersion, tag);
}

void ILogSystem::ServerPeekCursor::setProtocolVersion(ProtocolVersion version) {
	rd.setProtocolVersion(version);
}

Arena& ILogSystem::ServerPeekCursor::arena() {
	return results.arena;
}

ArenaReader* ILogSystem::ServerPeekCursor::reader() {
	return &rd;
}

bool ILogSystem::ServerPeekCursor::hasMessage() const {
	//TraceEvent("SPC_HasMessage", randomID).detail("HasMsg", hasMsg);
	return hasMsg;
}

void ILogSystem::ServerPeekCursor::nextMessage() {
	//TraceEvent("SPC_NextMessage", randomID).detail("MessageVersion", messageVersion.toString());
	ASSERT(hasMsg);
	if (rd.empty()) {
		messageVersion.reset(std::min(results.end, end.version));
		hasMsg = false;
		return;
	}
	if (*(int32_t*)rd.peekBytes(4) == VERSION_HEADER) {
		// A version
		int32_t dummy;
		Version ver;
		rd >> dummy >> ver;

		//TraceEvent("SPC_ProcessSeq", randomID).detail("MessageVersion", messageVersion.toString()).detail("Ver", ver).detail("Tag", tag.toString());
		// ASSERT( ver >= messageVersion.version );

		messageVersion.reset(ver);

		if (messageVersion >= end) {
			messageVersion = end;
			hasMsg = false;
			return;
		}
		ASSERT(!rd.empty());
	}

	messageAndTags.loadFromArena(&rd, &messageVersion.sub);
	DEBUG_TAGS_AND_MESSAGE("ServerPeekCursor", messageVersion.version, messageAndTags.getRawMessage())
	    .detail("CursorID", this->randomID);
	// Rewind and consume the header so that reader() starts from the message.
	rd.rewind();
	rd.readBytes(messageAndTags.getHeaderSize());
	hasMsg = true;
	//TraceEvent("SPC_NextMessageB", randomID).detail("MessageVersion", messageVersion.toString());
}

StringRef ILogSystem::ServerPeekCursor::getMessage() {
	//TraceEvent("SPC_GetMessage", randomID);
	StringRef message = messageAndTags.getMessageWithoutTags();
	rd.readBytes(message.size()); // Consumes the message.
	return message;
}

StringRef ILogSystem::ServerPeekCursor::getMessageWithTags() {
	StringRef rawMessage = messageAndTags.getRawMessage();
	rd.readBytes(rawMessage.size() - messageAndTags.getHeaderSize()); // Consumes the message.
	return rawMessage;
}

VectorRef<Tag> ILogSystem::ServerPeekCursor::getTags() const {
	return messageAndTags.tags;
}

void ILogSystem::ServerPeekCursor::advanceTo(LogMessageVersion n) {
	//TraceEvent("SPC_AdvanceTo", randomID).detail("N", n.toString());
	while (messageVersion < n && hasMessage()) {
		getMessage();
		nextMessage();
	}

	if (hasMessage())
		return;

	// if( more.isValid() && !more.isReady() ) more.cancel();

	if (messageVersion < n) {
		messageVersion = n;
	}
}

// This function is called after the cursor received one TLogPeekReply to update its members, which is the common logic
// in getMore helper functions.
void updateCursorWithReply(ILogSystem::ServerPeekCursor* self, const TLogPeekReply& res) {
	self->results = res;
	self->onlySpilled = res.onlySpilled;
	if (res.popped.present())
		self->poppedVersion = std::min(std::max(self->poppedVersion, res.popped.get()), self->end.version);
	self->rd = ArenaReader(self->results.arena, self->results.messages, Unversioned());
	LogMessageVersion skipSeq = self->messageVersion;
	self->hasMsg = true;
	self->nextMessage();
	self->advanceTo(skipSeq);
}

ACTOR Future<Void> resetChecker(ILogSystem::ServerPeekCursor* self, NetworkAddress addr) {
	self->slowReplies = 0;
	self->unknownReplies = 0;
	self->fastReplies = 0;
	wait(delay(SERVER_KNOBS->PEEK_STATS_INTERVAL));
	TraceEvent("SlowPeekStats", self->randomID)
	    .detail("PeerAddress", addr)
	    .detail("SlowReplies", self->slowReplies)
	    .detail("FastReplies", self->fastReplies)
	    .detail("UnknownReplies", self->unknownReplies);

	if (self->slowReplies >= SERVER_KNOBS->PEEK_STATS_SLOW_AMOUNT &&
	    self->slowReplies / double(self->slowReplies + self->fastReplies) >= SERVER_KNOBS->PEEK_STATS_SLOW_RATIO) {

		TraceEvent("ConnectionResetSlowPeek", self->randomID)
		    .detail("PeerAddress", addr)
		    .detail("SlowReplies", self->slowReplies)
		    .detail("FastReplies", self->fastReplies)
		    .detail("UnknownReplies", self->unknownReplies);
		FlowTransport::transport().resetConnection(addr);
		self->lastReset = now();
	}
	return Void();
}

ACTOR Future<TLogPeekReply> recordRequestMetrics(ILogSystem::ServerPeekCursor* self,
                                                 NetworkAddress addr,
                                                 Future<TLogPeekReply> in) {
	try {
		state double startTime = now();
		TLogPeekReply t = wait(in);
		if (now() - self->lastReset > SERVER_KNOBS->PEEK_RESET_INTERVAL) {
			if (now() - startTime > SERVER_KNOBS->PEEK_MAX_LATENCY) {
				if (t.messages.size() >= SERVER_KNOBS->DESIRED_TOTAL_BYTES || SERVER_KNOBS->PEEK_COUNT_SMALL_MESSAGES) {
					if (self->resetCheck.isReady()) {
						self->resetCheck = resetChecker(self, addr);
					}
					self->slowReplies++;
				} else {
					self->unknownReplies++;
				}
			} else {
				self->fastReplies++;
			}
		}
		return t;
	} catch (Error& e) {
		if (e.code() != error_code_broken_promise)
			throw;
		wait(Never()); // never return
		throw internal_error(); // does not happen
	}
}

ACTOR Future<Void> serverPeekParallelGetMore(ILogSystem::ServerPeekCursor* self, TaskPriority taskID) {
	if (!self->interf || self->isExhausted()) {
		if (self->hasMessage())
			return Void();
		return Never();
	}

	if (!self->interfaceChanged.isValid()) {
		self->interfaceChanged = self->interf->onChange();
	}

	loop {
		state Version expectedBegin = self->messageVersion.version;
		try {
			if (self->parallelGetMore || self->onlySpilled) {
				while (self->futureResults.size() < SERVER_KNOBS->PARALLEL_GET_MORE_REQUESTS &&
				       self->interf->get().present()) {
					self->futureResults.push_back(recordRequestMetrics(
					    self,
					    self->interf->get().interf().peekMessages.getEndpoint().getPrimaryAddress(),
					    self->interf->get().interf().peekMessages.getReply(
					        TLogPeekRequest(self->messageVersion.version,
					                        self->tag,
					                        self->returnIfBlocked,
					                        self->onlySpilled,
					                        std::make_pair(self->randomID, self->sequence++)),
					        taskID)));
				}
				if (self->sequence == std::numeric_limits<decltype(self->sequence)>::max()) {
					throw operation_obsolete();
				}
			} else if (self->futureResults.size() == 0) {
				return Void();
			}

			if (self->hasMessage())
				return Void();

			choose {
				when(TLogPeekReply res = wait(self->interf->get().present() ? self->futureResults.front() : Never())) {
					if (res.begin.get() != expectedBegin) {
						throw operation_obsolete();
					}
					expectedBegin = res.end;
					self->futureResults.pop_front();
					updateCursorWithReply(self, res);
					//TraceEvent("SPC_GetMoreB", self->randomID).detail("Has", self->hasMessage()).detail("End", res.end).detail("Popped", res.popped.present() ? res.popped.get() : 0);
					return Void();
				}
				when(wait(self->interfaceChanged)) {
					self->interfaceChanged = self->interf->onChange();
					self->randomID = deterministicRandom()->randomUniqueID();
					self->sequence = 0;
					self->onlySpilled = false;
					self->futureResults.clear();
				}
			}
		} catch (Error& e) {
			if (e.code() == error_code_end_of_stream) {
				self->end.reset(self->messageVersion.version);
				return Void();
			} else if (e.code() == error_code_timed_out || e.code() == error_code_operation_obsolete) {
				TraceEvent("PeekCursorTimedOut", self->randomID).error(e);
				// We *should* never get timed_out(), as it means the TLog got stuck while handling a parallel peek,
				// and thus we've likely just wasted 10min.
				// timed_out() is sent by cleanupPeekTrackers as value PEEK_TRACKER_EXPIRATION_TIME
				ASSERT_WE_THINK(e.code() == error_code_operation_obsolete ||
				                SERVER_KNOBS->PEEK_TRACKER_EXPIRATION_TIME < 10);
				self->interfaceChanged = self->interf->onChange();
				self->randomID = deterministicRandom()->randomUniqueID();
				self->sequence = 0;
				self->futureResults.clear();
			} else {
				throw e;
			}
		}
	}
}

ACTOR Future<Void> serverPeekStreamGetMore(ILogSystem::ServerPeekCursor* self, TaskPriority taskID) {
	if (!self->interf || self->isExhausted()) {
		if (self->hasMessage())
			return Void();
		return Never();
	}

	tryEstablishPeekStream(self);
	loop {
		try {
			choose {
				when(wait(self->interf->onChange())) {
					self->onlySpilled = false;
					self->peekReplyStream.reset();
					tryEstablishPeekStream(self);
				}
				when(TLogPeekStreamReply res =
				         wait(self->peekReplyStream.present()
				                  ? brokenPromiseToNever(waitAndForward(self->peekReplyStream.get().getFuture()))
				                  : Never())) {
					updateCursorWithReply(self, res.rep);
					TraceEvent("SPC_GetMoreB", self->randomID)
					    .detail("Has", self->hasMessage())
					    .detail("End", res.rep.end)
					    .detail("Popped", res.rep.popped.present() ? res.rep.popped.get() : 0);
					return Void();
				}
			}
		} catch (Error& e) {
			if (e.code() == error_code_connection_failed) {
				self->peekReplyStream.reset();
			} else if (e.code() == error_code_end_of_stream) {
				self->end.reset(self->messageVersion.version);
				return Void();
			} else {
				throw;
			}
		}
	}
}

ACTOR Future<Void> serverPeekGetMore(ILogSystem::ServerPeekCursor* self, TaskPriority taskID) {
	if (!self->interf || self->isExhausted()) {
		return Never();
	}
	try {
		loop {
			choose {
				when(TLogPeekReply res =
				         wait(self->interf->get().present()
				                  ? brokenPromiseToNever(self->interf->get().interf().peekMessages.getReply(
				                        TLogPeekRequest(self->messageVersion.version,
				                                        self->tag,
				                                        self->returnIfBlocked,
				                                        self->onlySpilled),
				                        taskID))
				                  : Never())) {
					updateCursorWithReply(self, res);
					//TraceEvent("SPC_GetMoreB", self->randomID).detail("Has", self->hasMessage()).detail("End", res.end).detail("Popped", res.popped.present() ? res.popped.get() : 0);
					return Void();
				}
				when(wait(self->interf->onChange())) { self->onlySpilled = false; }
			}
		}
	} catch (Error& e) {
		if (e.code() == error_code_end_of_stream) {
			self->end.reset(self->messageVersion.version);
			return Void();
		}
		throw e;
	}
}

Future<Void> ILogSystem::ServerPeekCursor::getMore(TaskPriority taskID) {
	//TraceEvent("SPC_GetMore", randomID).detail("HasMessage", hasMessage()).detail("More", !more.isValid() || more.isReady()).detail("MessageVersion", messageVersion.toString()).detail("End", end.toString());
	if (hasMessage() && !parallelGetMore)
		return Void();
	if (!more.isValid() || more.isReady()) {
		if (usePeekStream && taskID == TaskPriority::TLogPeekReply) {
			more = serverPeekStreamGetMore(this, taskID);
		}
		//        if (parallelGetMore || onlySpilled || futureResults.size()) {
		//            more = serverPeekParallelGetMore(this, taskID);
		//        }
		else {
			more = serverPeekGetMore(this, taskID);
		}
	}
	return more;
}

ACTOR Future<Void> serverPeekOnFailed(ILogSystem::ServerPeekCursor* self) {
	loop {
		choose {
			when(wait(self->interf->get().present()
			              ? IFailureMonitor::failureMonitor().onStateEqual(
			                    self->interf->get().interf().peekMessages.getEndpoint(), FailureStatus())
			              : Never())) {
				return Void();
			}
			when(wait(self->interf->get().present()
			              ? IFailureMonitor::failureMonitor().onStateEqual(
			                    self->interf->get().interf().peekStreamMessages.getEndpoint(), FailureStatus())
			              : Never())) {
				return Void();
			}
			when(wait(self->interf->onChange())) {}
		}
	}
}

Future<Void> ILogSystem::ServerPeekCursor::onFailed() {
	return serverPeekOnFailed(this);
}

bool ILogSystem::ServerPeekCursor::isActive() const {
	if (!interf->get().present())
		return false;
	if (isExhausted())
		return false;
	return IFailureMonitor::failureMonitor()
	           .getState(interf->get().interf().peekMessages.getEndpoint())
	           .isAvailable() &&
	       IFailureMonitor::failureMonitor()
	           .getState(interf->get().interf().peekStreamMessages.getEndpoint())
	           .isAvailable();
}

bool ILogSystem::ServerPeekCursor::isExhausted() const {
	return messageVersion >= end;
}

const LogMessageVersion& ILogSystem::ServerPeekCursor::version() const {
	return messageVersion;
} // Call only after nextMessage().  The sequence of the current message, or results.end if nextMessage() has returned
  // false.

Version ILogSystem::ServerPeekCursor::getMinKnownCommittedVersion() const {
	return results.minKnownCommittedVersion;
}

Optional<UID> ILogSystem::ServerPeekCursor::getPrimaryPeekLocation() const {
	if (interf && interf->get().present()) {
		return interf->get().id();
	}
	return Optional<UID>();
}

Optional<UID> ILogSystem::ServerPeekCursor::getCurrentPeekLocation() const {
	return ILogSystem::ServerPeekCursor::getPrimaryPeekLocation();
}

Version ILogSystem::ServerPeekCursor::popped() const {
	return poppedVersion;
}

ILogSystem::MergedPeekCursor::MergedPeekCursor(vector<Reference<ILogSystem::IPeekCursor>> const& serverCursors,
                                               Version begin)
  : serverCursors(serverCursors), bestServer(-1), readQuorum(serverCursors.size()), tag(invalidTag), currentCursor(0),
    hasNextMessage(false), messageVersion(begin), randomID(deterministicRandom()->randomUniqueID()),
    tLogReplicationFactor(0) {
	sortedVersions.resize(serverCursors.size());
}

ILogSystem::MergedPeekCursor::MergedPeekCursor(
    std::vector<Reference<AsyncVar<OptionalInterface<TLogInterface>>>> const& logServers,
    int bestServer,
    int readQuorum,
    Tag tag,
    Version begin,
    Version end,
    bool parallelGetMore,
    std::vector<LocalityData> const& tLogLocalities,
    Reference<IReplicationPolicy> const tLogPolicy,
    int tLogReplicationFactor)
  : bestServer(bestServer), readQuorum(readQuorum), tag(tag), currentCursor(0), hasNextMessage(false),
    messageVersion(begin), randomID(deterministicRandom()->randomUniqueID()),
    tLogReplicationFactor(tLogReplicationFactor) {
	if (tLogPolicy) {
		logSet = makeReference<LogSet>();
		logSet->tLogPolicy = tLogPolicy;
		logSet->tLogLocalities = tLogLocalities;
		filterLocalityDataForPolicy(logSet->tLogPolicy, &logSet->tLogLocalities);
		logSet->updateLocalitySet(logSet->tLogLocalities);
	}

	for (int i = 0; i < logServers.size(); i++) {
		auto cursor = makeReference<ILogSystem::ServerPeekCursor>(
		    logServers[i], tag, begin, end, bestServer >= 0, parallelGetMore);
		//TraceEvent("MPC_Starting", randomID).detail("Cursor", cursor->randomID).detail("End", end);
		serverCursors.push_back(cursor);
	}
	sortedVersions.resize(serverCursors.size());
}

ILogSystem::MergedPeekCursor::MergedPeekCursor(vector<Reference<ILogSystem::IPeekCursor>> const& serverCursors,
                                               LogMessageVersion const& messageVersion,
                                               int bestServer,
                                               int readQuorum,
                                               Optional<LogMessageVersion> nextVersion,
                                               Reference<LogSet> logSet,
                                               int tLogReplicationFactor)
  : serverCursors(serverCursors), bestServer(bestServer), readQuorum(readQuorum), currentCursor(0),
    hasNextMessage(false), messageVersion(messageVersion), nextVersion(nextVersion), logSet(logSet),
    randomID(deterministicRandom()->randomUniqueID()), tLogReplicationFactor(tLogReplicationFactor) {
	sortedVersions.resize(serverCursors.size());
	calcHasMessage();
}

Reference<ILogSystem::IPeekCursor> ILogSystem::MergedPeekCursor::cloneNoMore() {
	vector<Reference<ILogSystem::IPeekCursor>> cursors;
	for (auto it : serverCursors) {
		cursors.push_back(it->cloneNoMore());
	}
	return makeReference<ILogSystem::MergedPeekCursor>(
	    cursors, messageVersion, bestServer, readQuorum, nextVersion, logSet, tLogReplicationFactor);
}

void ILogSystem::MergedPeekCursor::setProtocolVersion(ProtocolVersion version) {
	for (auto it : serverCursors)
		if (it->hasMessage())
			it->setProtocolVersion(version);
}

Arena& ILogSystem::MergedPeekCursor::arena() {
	return serverCursors[currentCursor]->arena();
}

ArenaReader* ILogSystem::MergedPeekCursor::reader() {
	return serverCursors[currentCursor]->reader();
}

void ILogSystem::MergedPeekCursor::calcHasMessage() {
	if (bestServer >= 0) {
		if (nextVersion.present())
			serverCursors[bestServer]->advanceTo(nextVersion.get());
		if (serverCursors[bestServer]->hasMessage()) {
			messageVersion = serverCursors[bestServer]->version();
			currentCursor = bestServer;
			hasNextMessage = true;

			for (auto& c : serverCursors)
				c->advanceTo(messageVersion);

			return;
		}

		auto bestVersion = serverCursors[bestServer]->version();
		for (auto& c : serverCursors)
			c->advanceTo(bestVersion);
	}

	hasNextMessage = false;
	updateMessage(false);

	if (!hasNextMessage && logSet) {
		updateMessage(true);
	}
}

void ILogSystem::MergedPeekCursor::updateMessage(bool usePolicy) {
	loop {
		bool advancedPast = false;
		sortedVersions.clear();
		for (int i = 0; i < serverCursors.size(); i++) {
			auto& serverCursor = serverCursors[i];
			if (nextVersion.present())
				serverCursor->advanceTo(nextVersion.get());
			sortedVersions.push_back(std::pair<LogMessageVersion, int>(serverCursor->version(), i));
		}

		if (usePolicy) {
			ASSERT(logSet->tLogPolicy);
			std::sort(sortedVersions.begin(), sortedVersions.end());

			locations.clear();
			for (auto sortedVersion : sortedVersions) {
				locations.push_back(logSet->logEntryArray[sortedVersion.second]);
				if (locations.size() >= tLogReplicationFactor && logSet->satisfiesPolicy(locations)) {
					messageVersion = sortedVersion.first;
					break;
				}
			}
		} else {
			std::nth_element(sortedVersions.begin(), sortedVersions.end() - readQuorum, sortedVersions.end());
			messageVersion = sortedVersions[sortedVersions.size() - readQuorum].first;
		}

		for (int i = 0; i < serverCursors.size(); i++) {
			auto& c = serverCursors[i];
			auto start = c->version();
			c->advanceTo(messageVersion);
			if (start <= messageVersion && messageVersion < c->version()) {
				advancedPast = true;
				TEST(true); // Merge peek cursor advanced past desired sequence
			}
		}

		if (!advancedPast)
			break;
	}

	for (int i = 0; i < serverCursors.size(); i++) {
		auto& c = serverCursors[i];
		ASSERT_WE_THINK(!c->hasMessage() ||
		                c->version() >= messageVersion); // Seems like the loop above makes this unconditionally true
		if (c->version() == messageVersion && c->hasMessage()) {
			hasNextMessage = true;
			currentCursor = i;
			break;
		}
	}
}

bool ILogSystem::MergedPeekCursor::hasMessage() const {
	return hasNextMessage;
}

void ILogSystem::MergedPeekCursor::nextMessage() {
	nextVersion = version();
	nextVersion.get().sub++;
	serverCursors[currentCursor]->nextMessage();
	calcHasMessage();
	ASSERT(hasMessage() || !version().sub);
}

StringRef ILogSystem::MergedPeekCursor::getMessage() {
	return serverCursors[currentCursor]->getMessage();
}

StringRef ILogSystem::MergedPeekCursor::getMessageWithTags() {
	return serverCursors[currentCursor]->getMessageWithTags();
}

VectorRef<Tag> ILogSystem::MergedPeekCursor::getTags() const {
	return serverCursors[currentCursor]->getTags();
}

void ILogSystem::MergedPeekCursor::advanceTo(LogMessageVersion n) {
	bool canChange = false;
	for (auto& c : serverCursors) {
		if (c->version() < n) {
			canChange = true;
			c->advanceTo(n);
		}
	}
	if (canChange) {
		calcHasMessage();
	}
}

ACTOR Future<Void> mergedPeekGetMore(ILogSystem::MergedPeekCursor* self,
                                     LogMessageVersion startVersion,
                                     TaskPriority taskID) {
	loop {
		//TraceEvent("MPC_GetMoreA", self->randomID).detail("Start", startVersion.toString());
		if (self->bestServer >= 0 && self->serverCursors[self->bestServer]->isActive()) {
			ASSERT(!self->serverCursors[self->bestServer]->hasMessage());
			wait(self->serverCursors[self->bestServer]->getMore(taskID) ||
			     self->serverCursors[self->bestServer]->onFailed());
		} else {
			vector<Future<Void>> q;
			for (auto& c : self->serverCursors)
				if (!c->hasMessage())
					q.push_back(c->getMore(taskID));
			wait(quorum(q, 1));
		}
		self->calcHasMessage();
		//TraceEvent("MPC_GetMoreB", self->randomID).detail("HasMessage", self->hasMessage()).detail("Start", startVersion.toString()).detail("Seq", self->version().toString());
		if (self->hasMessage() || self->version() > startVersion) {
			return Void();
		}
	}
}

Future<Void> ILogSystem::MergedPeekCursor::getMore(TaskPriority taskID) {
	if (more.isValid() && !more.isReady()) {
		return more;
	}

	if (!serverCursors.size())
		return Never();

	auto startVersion = version();
	calcHasMessage();
	if (hasMessage())
		return Void();
	if (nextVersion.present())
		advanceTo(nextVersion.get());
	ASSERT(!hasMessage());
	if (version() > startVersion)
		return Void();

	more = mergedPeekGetMore(this, startVersion, taskID);
	return more;
}

Future<Void> ILogSystem::MergedPeekCursor::onFailed() {
	ASSERT(false);
	return Never();
}

bool ILogSystem::MergedPeekCursor::isActive() const {
	ASSERT(false);
	return false;
}

bool ILogSystem::MergedPeekCursor::isExhausted() const {
	return serverCursors[currentCursor]->isExhausted();
}

const LogMessageVersion& ILogSystem::MergedPeekCursor::version() const {
	return messageVersion;
}

Version ILogSystem::MergedPeekCursor::getMinKnownCommittedVersion() const {
	return serverCursors[currentCursor]->getMinKnownCommittedVersion();
}

Optional<UID> ILogSystem::MergedPeekCursor::getPrimaryPeekLocation() const {
	if (bestServer >= 0) {
		return serverCursors[bestServer]->getPrimaryPeekLocation();
	}
	return Optional<UID>();
}

Optional<UID> ILogSystem::MergedPeekCursor::getCurrentPeekLocation() const {
	if (currentCursor >= 0) {
		return serverCursors[currentCursor]->getPrimaryPeekLocation();
	}
	return Optional<UID>();
}

Version ILogSystem::MergedPeekCursor::popped() const {
	Version poppedVersion = 0;
	for (auto& c : serverCursors)
		poppedVersion = std::max(poppedVersion, c->popped());
	return poppedVersion;
}

ILogSystem::SetPeekCursor::SetPeekCursor(std::vector<Reference<LogSet>> const& logSets,
                                         int bestSet,
                                         int bestServer,
                                         Tag tag,
                                         Version begin,
                                         Version end,
                                         bool parallelGetMore)
  : logSets(logSets), bestSet(bestSet), bestServer(bestServer), tag(tag), currentCursor(0), currentSet(bestSet),
    hasNextMessage(false), messageVersion(begin), useBestSet(true), randomID(deterministicRandom()->randomUniqueID()) {
	serverCursors.resize(logSets.size());
	int maxServers = 0;
	for (int i = 0; i < logSets.size(); i++) {
		for (int j = 0; j < logSets[i]->logServers.size(); j++) {
			auto cursor = makeReference<ILogSystem::ServerPeekCursor>(
			    logSets[i]->logServers[j], tag, begin, end, true, parallelGetMore);
			serverCursors[i].push_back(cursor);
		}
		maxServers = std::max<int>(maxServers, serverCursors[i].size());
	}
	sortedVersions.resize(maxServers);
}

ILogSystem::SetPeekCursor::SetPeekCursor(std::vector<Reference<LogSet>> const& logSets,
                                         std::vector<std::vector<Reference<IPeekCursor>>> const& serverCursors,
                                         LogMessageVersion const& messageVersion,
                                         int bestSet,
                                         int bestServer,
                                         Optional<LogMessageVersion> nextVersion,
                                         bool useBestSet)
  : logSets(logSets), serverCursors(serverCursors), messageVersion(messageVersion), bestSet(bestSet),
    bestServer(bestServer), nextVersion(nextVersion), currentSet(bestSet), currentCursor(0), hasNextMessage(false),
    useBestSet(useBestSet), randomID(deterministicRandom()->randomUniqueID()) {
	int maxServers = 0;
	for (int i = 0; i < logSets.size(); i++) {
		maxServers = std::max<int>(maxServers, serverCursors[i].size());
	}
	sortedVersions.resize(maxServers);
	calcHasMessage();
}

Reference<ILogSystem::IPeekCursor> ILogSystem::SetPeekCursor::cloneNoMore() {
	vector<vector<Reference<ILogSystem::IPeekCursor>>> cursors;
	cursors.resize(logSets.size());
	for (int i = 0; i < logSets.size(); i++) {
		for (int j = 0; j < logSets[i]->logServers.size(); j++) {
			cursors[i].push_back(serverCursors[i][j]->cloneNoMore());
		}
	}
	return makeReference<ILogSystem::SetPeekCursor>(
	    logSets, cursors, messageVersion, bestSet, bestServer, nextVersion, useBestSet);
}

void ILogSystem::SetPeekCursor::setProtocolVersion(ProtocolVersion version) {
	for (auto& cursors : serverCursors) {
		for (auto& it : cursors) {
			if (it->hasMessage()) {
				it->setProtocolVersion(version);
			}
		}
	}
}

Arena& ILogSystem::SetPeekCursor::arena() {
	return serverCursors[currentSet][currentCursor]->arena();
}

ArenaReader* ILogSystem::SetPeekCursor::reader() {
	return serverCursors[currentSet][currentCursor]->reader();
}

void ILogSystem::SetPeekCursor::calcHasMessage() {
	if (bestSet >= 0 && bestServer >= 0) {
		if (nextVersion.present()) {
			//TraceEvent("LPC_CalcNext").detail("Ver", messageVersion.toString()).detail("Tag", tag.toString()).detail("HasNextMessage", hasNextMessage).detail("NextVersion", nextVersion.get().toString());
			serverCursors[bestSet][bestServer]->advanceTo(nextVersion.get());
		}
		if (serverCursors[bestSet][bestServer]->hasMessage()) {
			messageVersion = serverCursors[bestSet][bestServer]->version();
			currentSet = bestSet;
			currentCursor = bestServer;
			hasNextMessage = true;

			//TraceEvent("LPC_Calc1").detail("Ver", messageVersion.toString()).detail("Tag", tag.toString()).detail("HasNextMessage", hasNextMessage);

			for (auto& cursors : serverCursors) {
				for (auto& c : cursors) {
					c->advanceTo(messageVersion);
				}
			}

			return;
		}

		auto bestVersion = serverCursors[bestSet][bestServer]->version();
		for (auto& cursors : serverCursors) {
			for (auto& c : cursors) {
				c->advanceTo(bestVersion);
			}
		}
	}

	hasNextMessage = false;
	if (useBestSet) {
		updateMessage(bestSet, false); // Use Quorum logic

		//TraceEvent("LPC_Calc2").detail("Ver", messageVersion.toString()).detail("Tag", tag.toString()).detail("HasNextMessage", hasNextMessage);
		if (!hasNextMessage) {
			updateMessage(bestSet, true);
			//TraceEvent("LPC_Calc3").detail("Ver", messageVersion.toString()).detail("Tag", tag.toString()).detail("HasNextMessage", hasNextMessage);
		}
	} else {
		for (int i = 0; i < logSets.size() && !hasNextMessage; i++) {
			if (i != bestSet) {
				updateMessage(i, false); // Use Quorum logic
			}
		}
		//TraceEvent("LPC_Calc4").detail("Ver", messageVersion.toString()).detail("Tag", tag.toString()).detail("HasNextMessage", hasNextMessage);
		for (int i = 0; i < logSets.size() && !hasNextMessage; i++) {
			if (i != bestSet) {
				updateMessage(i, true);
			}
		}
		//TraceEvent("LPC_Calc5").detail("Ver", messageVersion.toString()).detail("Tag", tag.toString()).detail("HasNextMessage", hasNextMessage);
	}
}

void ILogSystem::SetPeekCursor::updateMessage(int logIdx, bool usePolicy) {
	loop {
		bool advancedPast = false;
		sortedVersions.clear();
		for (int i = 0; i < serverCursors[logIdx].size(); i++) {
			auto& serverCursor = serverCursors[logIdx][i];
			if (nextVersion.present())
				serverCursor->advanceTo(nextVersion.get());
			sortedVersions.push_back(std::pair<LogMessageVersion, int>(serverCursor->version(), i));
			//TraceEvent("LPC_Update1").detail("Ver", messageVersion.toString()).detail("Tag", tag.toString()).detail("HasNextMessage", hasNextMessage).detail("ServerVer", serverCursor->version().toString()).detail("I", i);
		}

		if (usePolicy) {
			std::sort(sortedVersions.begin(), sortedVersions.end());
			locations.clear();
			for (auto sortedVersion : sortedVersions) {
				locations.push_back(logSets[logIdx]->logEntryArray[sortedVersion.second]);
				if (locations.size() >= logSets[logIdx]->tLogReplicationFactor &&
				    logSets[logIdx]->satisfiesPolicy(locations)) {
					messageVersion = sortedVersion.first;
					break;
				}
			}
		} else {
			//(int)oldLogData[i].logServers.size() + 1 - oldLogData[i].tLogReplicationFactor
			std::nth_element(sortedVersions.begin(),
			                 sortedVersions.end() -
			                     (logSets[logIdx]->logServers.size() + 1 - logSets[logIdx]->tLogReplicationFactor),
			                 sortedVersions.end());
			messageVersion = sortedVersions[sortedVersions.size() - (logSets[logIdx]->logServers.size() + 1 -
			                                                         logSets[logIdx]->tLogReplicationFactor)]
			                     .first;
		}

		for (auto& cursors : serverCursors) {
			for (auto& c : cursors) {
				auto start = c->version();
				c->advanceTo(messageVersion);
				if (start <= messageVersion && messageVersion < c->version()) {
					advancedPast = true;
					TEST(true); // Merge peek cursor with logIdx advanced past desired sequence
				}
			}
		}

		if (!advancedPast)
			break;
	}

	for (int i = 0; i < serverCursors[logIdx].size(); i++) {
		auto& c = serverCursors[logIdx][i];
		ASSERT_WE_THINK(!c->hasMessage() ||
		                c->version() >= messageVersion); // Seems like the loop above makes this unconditionally true
		if (c->version() == messageVersion && c->hasMessage()) {
			hasNextMessage = true;
			currentSet = logIdx;
			currentCursor = i;
			break;
		}
	}
}

bool ILogSystem::SetPeekCursor::hasMessage() const {
	return hasNextMessage;
}

void ILogSystem::SetPeekCursor::nextMessage() {
	nextVersion = version();
	nextVersion.get().sub++;
	serverCursors[currentSet][currentCursor]->nextMessage();
	calcHasMessage();
	ASSERT(hasMessage() || !version().sub);
}

StringRef ILogSystem::SetPeekCursor::getMessage() {
	return serverCursors[currentSet][currentCursor]->getMessage();
}

StringRef ILogSystem::SetPeekCursor::getMessageWithTags() {
	return serverCursors[currentSet][currentCursor]->getMessageWithTags();
}

VectorRef<Tag> ILogSystem::SetPeekCursor::getTags() const {
	return serverCursors[currentSet][currentCursor]->getTags();
}

void ILogSystem::SetPeekCursor::advanceTo(LogMessageVersion n) {
	bool canChange = false;
	for (auto& cursors : serverCursors) {
		for (auto& c : cursors) {
			if (c->version() < n) {
				canChange = true;
				c->advanceTo(n);
			}
		}
	}
	if (canChange) {
		calcHasMessage();
	}
}

ACTOR Future<Void> setPeekGetMore(ILogSystem::SetPeekCursor* self,
                                  LogMessageVersion startVersion,
                                  TaskPriority taskID) {
	loop {
		//TraceEvent("LPC_GetMore1", self->randomID).detail("Start", startVersion.toString()).detail("Tag", self->tag.toString());
		if (self->bestServer >= 0 && self->bestSet >= 0 &&
		    self->serverCursors[self->bestSet][self->bestServer]->isActive()) {
			ASSERT(!self->serverCursors[self->bestSet][self->bestServer]->hasMessage());
			//TraceEvent("LPC_GetMore2", self->randomID).detail("Start", startVersion.toString()).detail("Tag", self->tag.toString());
			wait(self->serverCursors[self->bestSet][self->bestServer]->getMore(taskID) ||
			     self->serverCursors[self->bestSet][self->bestServer]->onFailed());
			self->useBestSet = true;
		} else {
			// FIXME: if best set is exhausted, do not peek remote servers
			bool bestSetValid = self->bestSet >= 0;
			if (bestSetValid) {
				self->locations.clear();
				for (int i = 0; i < self->serverCursors[self->bestSet].size(); i++) {
					if (!self->serverCursors[self->bestSet][i]->isActive() &&
					    self->serverCursors[self->bestSet][i]->version() <= self->messageVersion) {
						self->locations.push_back(self->logSets[self->bestSet]->logEntryArray[i]);
					}
				}
				bestSetValid = self->locations.size() < self->logSets[self->bestSet]->tLogReplicationFactor ||
				               !self->logSets[self->bestSet]->satisfiesPolicy(self->locations);
			}
			if (bestSetValid || self->logSets.size() == 1) {
				if (!self->useBestSet) {
					self->useBestSet = true;
					self->calcHasMessage();
					if (self->hasMessage() || self->version() > startVersion)
						return Void();
				}

				//TraceEvent("LPC_GetMore3", self->randomID).detail("Start", startVersion.toString()).detail("Tag", self->tag.toString()).detail("BestSetSize", self->serverCursors[self->bestSet].size());
				vector<Future<Void>> q;
				for (auto& c : self->serverCursors[self->bestSet]) {
					if (!c->hasMessage()) {
						q.push_back(c->getMore(taskID));
						if (c->isActive()) {
							q.push_back(c->onFailed());
						}
					}
				}
				wait(quorum(q, 1));
			} else {
				// FIXME: this will peeking way too many cursors when satellites exist, and does not need to peek
				// bestSet cursors since we cannot get anymore data from them
				vector<Future<Void>> q;
				//TraceEvent("LPC_GetMore4", self->randomID).detail("Start", startVersion.toString()).detail("Tag", self->tag.toString());
				for (auto& cursors : self->serverCursors) {
					for (auto& c : cursors) {
						if (!c->hasMessage()) {
							q.push_back(c->getMore(taskID));
						}
					}
				}
				wait(quorum(q, 1));
				self->useBestSet = false;
			}
		}
		self->calcHasMessage();
		//TraceEvent("LPC_GetMoreB", self->randomID).detail("HasMessage", self->hasMessage()).detail("Start", startVersion.toString()).detail("Seq", self->version().toString());
		if (self->hasMessage() || self->version() > startVersion)
			return Void();
	}
}

Future<Void> ILogSystem::SetPeekCursor::getMore(TaskPriority taskID) {
	if (more.isValid() && !more.isReady()) {
		return more;
	}

	auto startVersion = version();
	calcHasMessage();
	if (hasMessage())
		return Void();
	if (nextVersion.present())
		advanceTo(nextVersion.get());
	ASSERT(!hasMessage());
	if (version() > startVersion)
		return Void();

	more = setPeekGetMore(this, startVersion, taskID);
	return more;
}

Future<Void> ILogSystem::SetPeekCursor::onFailed() {
	ASSERT(false);
	return Never();
}

bool ILogSystem::SetPeekCursor::isActive() const {
	ASSERT(false);
	return false;
}

bool ILogSystem::SetPeekCursor::isExhausted() const {
	return serverCursors[currentSet][currentCursor]->isExhausted();
}

const LogMessageVersion& ILogSystem::SetPeekCursor::version() const {
	return messageVersion;
}

Version ILogSystem::SetPeekCursor::getMinKnownCommittedVersion() const {
	return serverCursors[currentSet][currentCursor]->getMinKnownCommittedVersion();
}

Optional<UID> ILogSystem::SetPeekCursor::getPrimaryPeekLocation() const {
	if (bestServer >= 0 && bestSet >= 0) {
		return serverCursors[bestSet][bestServer]->getPrimaryPeekLocation();
	}
	return Optional<UID>();
}

Optional<UID> ILogSystem::SetPeekCursor::getCurrentPeekLocation() const {
	if (currentCursor >= 0 && currentSet >= 0) {
		return serverCursors[currentSet][currentCursor]->getPrimaryPeekLocation();
	}
	return Optional<UID>();
}

Version ILogSystem::SetPeekCursor::popped() const {
	Version poppedVersion = 0;
	for (auto& cursors : serverCursors) {
		for (auto& c : cursors) {
			poppedVersion = std::max(poppedVersion, c->popped());
		}
	}
	return poppedVersion;
}

ILogSystem::MultiCursor::MultiCursor(std::vector<Reference<IPeekCursor>> cursors,
                                     std::vector<LogMessageVersion> epochEnds)
  : cursors(cursors), epochEnds(epochEnds), poppedVersion(0) {
	for (int i = 0; i < std::min<int>(cursors.size(), SERVER_KNOBS->MULTI_CURSOR_PRE_FETCH_LIMIT); i++) {
		cursors[cursors.size() - i - 1]->getMore();
	}
}

Reference<ILogSystem::IPeekCursor> ILogSystem::MultiCursor::cloneNoMore() {
	return cursors.back()->cloneNoMore();
}

void ILogSystem::MultiCursor::setProtocolVersion(ProtocolVersion version) {
	cursors.back()->setProtocolVersion(version);
}

Arena& ILogSystem::MultiCursor::arena() {
	return cursors.back()->arena();
}

ArenaReader* ILogSystem::MultiCursor::reader() {
	return cursors.back()->reader();
}

bool ILogSystem::MultiCursor::hasMessage() const {
	return cursors.back()->hasMessage();
}

void ILogSystem::MultiCursor::nextMessage() {
	cursors.back()->nextMessage();
}

StringRef ILogSystem::MultiCursor::getMessage() {
	return cursors.back()->getMessage();
}

StringRef ILogSystem::MultiCursor::getMessageWithTags() {
	return cursors.back()->getMessageWithTags();
}

VectorRef<Tag> ILogSystem::MultiCursor::getTags() const {
	return cursors.back()->getTags();
}

void ILogSystem::MultiCursor::advanceTo(LogMessageVersion n) {
	while (cursors.size() > 1 && n >= epochEnds.back()) {
		poppedVersion = std::max(poppedVersion, cursors.back()->popped());
		cursors.pop_back();
		epochEnds.pop_back();
	}
	cursors.back()->advanceTo(n);
}

Future<Void> ILogSystem::MultiCursor::getMore(TaskPriority taskID) {
	LogMessageVersion startVersion = cursors.back()->version();
	while (cursors.size() > 1 && cursors.back()->version() >= epochEnds.back()) {
		poppedVersion = std::max(poppedVersion, cursors.back()->popped());
		cursors.pop_back();
		epochEnds.pop_back();
	}
	if (cursors.back()->version() > startVersion) {
		return Void();
	}
	return cursors.back()->getMore(taskID);
}

Future<Void> ILogSystem::MultiCursor::onFailed() {
	return cursors.back()->onFailed();
}

bool ILogSystem::MultiCursor::isActive() const {
	return cursors.back()->isActive();
}

bool ILogSystem::MultiCursor::isExhausted() const {
	return cursors.back()->isExhausted();
}

const LogMessageVersion& ILogSystem::MultiCursor::version() const {
	return cursors.back()->version();
}

Version ILogSystem::MultiCursor::getMinKnownCommittedVersion() const {
	return cursors.back()->getMinKnownCommittedVersion();
}

Optional<UID> ILogSystem::MultiCursor::getPrimaryPeekLocation() const {
	return cursors.back()->getPrimaryPeekLocation();
}

Optional<UID> ILogSystem::MultiCursor::getCurrentPeekLocation() const {
	return cursors.back()->getCurrentPeekLocation();
}

Version ILogSystem::MultiCursor::popped() const {
	return std::max(poppedVersion, cursors.back()->popped());
}

ILogSystem::BufferedCursor::BufferedCursor(std::vector<Reference<IPeekCursor>> cursors,
                                           Version begin,
                                           Version end,
                                           bool withTags,
                                           bool collectTags,
                                           bool canDiscardPopped)
  : cursors(cursors), messageVersion(begin), end(end), withTags(withTags), collectTags(collectTags),
    hasNextMessage(false), messageIndex(0), poppedVersion(0), initialPoppedVersion(0),
    canDiscardPopped(canDiscardPopped), knownUnique(false), minKnownCommittedVersion(0),
    randomID(deterministicRandom()->randomUniqueID()) {
	targetQueueSize = SERVER_KNOBS->DESIRED_OUTSTANDING_MESSAGES / cursors.size();
	messages.reserve(SERVER_KNOBS->DESIRED_OUTSTANDING_MESSAGES);
	cursorMessages.resize(cursors.size());
}

ILogSystem::BufferedCursor::BufferedCursor(
    std::vector<Reference<AsyncVar<OptionalInterface<TLogInterface>>>> const& logServers,
    Tag tag,
    Version begin,
    Version end,
    bool parallelGetMore)
  : messageVersion(begin), end(end), withTags(true), collectTags(false), hasNextMessage(false), messageIndex(0),
    poppedVersion(0), initialPoppedVersion(0), canDiscardPopped(false), knownUnique(true), minKnownCommittedVersion(0),
    randomID(deterministicRandom()->randomUniqueID()) {
	targetQueueSize = SERVER_KNOBS->DESIRED_OUTSTANDING_MESSAGES / logServers.size();
	messages.reserve(SERVER_KNOBS->DESIRED_OUTSTANDING_MESSAGES);
	cursorMessages.resize(logServers.size());
	for (int i = 0; i < logServers.size(); i++) {
		auto cursor =
		    makeReference<ILogSystem::ServerPeekCursor>(logServers[i], tag, begin, end, false, parallelGetMore);
		cursors.push_back(cursor);
	}
}

void ILogSystem::BufferedCursor::combineMessages() {
	if (!hasNextMessage) {
		return;
	}

	std::vector<Tag> tags;
	tags.push_back(messages[messageIndex].tags[0]);
	for (int i = messageIndex + 1; i < messages.size() && messages[messageIndex].version == messages[i].version; i++) {
		tags.push_back(messages[i].tags[0]);
		messageIndex = i;
	}
	auto& msg = messages[messageIndex];
	BinaryWriter messageWriter(Unversioned());
	messageWriter << uint32_t(msg.message.size() + sizeof(uint32_t) + sizeof(uint16_t) + tags.size() * sizeof(Tag))
	              << msg.version.sub << uint16_t(tags.size());
	for (auto t : tags) {
		messageWriter << t;
	}
	messageWriter.serializeBytes(msg.message);
	Standalone<StringRef> val = messageWriter.toValue();
	msg.arena = val.arena();
	msg.message = val;
	msg.tags = VectorRef<Tag>();
	for (auto t : tags) {
		msg.tags.push_back(msg.arena, t);
	}
}

Reference<ILogSystem::IPeekCursor> ILogSystem::BufferedCursor::cloneNoMore() {
	ASSERT(false);
	return Reference<ILogSystem::IPeekCursor>();
}

void ILogSystem::BufferedCursor::setProtocolVersion(ProtocolVersion version) {
	for (auto& c : cursors) {
		c->setProtocolVersion(version);
	}
}

Arena& ILogSystem::BufferedCursor::arena() {
	return messages[messageIndex].arena;
}

ArenaReader* ILogSystem::BufferedCursor::reader() {
	ASSERT(false);
	return cursors[0]->reader();
}

bool ILogSystem::BufferedCursor::hasMessage() const {
	return hasNextMessage;
}

void ILogSystem::BufferedCursor::nextMessage() {
	messageIndex++;
	if (messageIndex == messages.size()) {
		hasNextMessage = false;
	}
	if (collectTags) {
		combineMessages();
	}
}

StringRef ILogSystem::BufferedCursor::getMessage() {
	ASSERT(!withTags);
	return messages[messageIndex].message;
}

StringRef ILogSystem::BufferedCursor::getMessageWithTags() {
	ASSERT(withTags);
	return messages[messageIndex].message;
}

VectorRef<Tag> ILogSystem::BufferedCursor::getTags() const {
	ASSERT(withTags);
	return messages[messageIndex].tags;
}

void ILogSystem::BufferedCursor::advanceTo(LogMessageVersion n) {
	ASSERT(false);
}

ACTOR Future<Void> bufferedGetMoreLoader(ILogSystem::BufferedCursor* self,
                                         Reference<ILogSystem::IPeekCursor> cursor,
                                         int idx,
                                         TaskPriority taskID) {
	loop {
		wait(yield());
		if (cursor->version().version >= self->end || self->cursorMessages[idx].size() > self->targetQueueSize) {
			return Void();
		}
		wait(cursor->getMore(taskID));
		self->poppedVersion = std::max(self->poppedVersion, cursor->popped());
		self->minKnownCommittedVersion =
		    std::max(self->minKnownCommittedVersion, cursor->getMinKnownCommittedVersion());
		if (self->canDiscardPopped) {
			self->initialPoppedVersion = std::max(self->initialPoppedVersion, cursor->popped());
		}
		if (cursor->version().version >= self->end) {
			return Void();
		}
		while (cursor->hasMessage()) {
			self->cursorMessages[idx].push_back(ILogSystem::BufferedCursor::BufferedMessage(
			    cursor->arena(),
			    (!self->withTags || self->collectTags) ? cursor->getMessage() : cursor->getMessageWithTags(),
			    !self->withTags ? VectorRef<Tag>() : cursor->getTags(),
			    cursor->version()));
			cursor->nextMessage();
		}
	}
}

ACTOR Future<Void> bufferedGetMore(ILogSystem::BufferedCursor* self, TaskPriority taskID) {
	if (self->messageVersion.version >= self->end) {
		wait(Future<Void>(Never()));
		throw internal_error();
	}

	self->messages.clear();

	std::vector<Future<Void>> loaders;
	loaders.reserve(self->cursors.size());

	for (int i = 0; i < self->cursors.size(); i++) {
		loaders.push_back(bufferedGetMoreLoader(self, self->cursors[i], i, taskID));
	}

	state Future<Void> allLoaders = waitForAll(loaders);
	state Version minVersion;
	loop {
		wait(allLoaders || delay(SERVER_KNOBS->DESIRED_GET_MORE_DELAY, taskID));
		minVersion = self->end;
		for (int i = 0; i < self->cursors.size(); i++) {
			auto cursor = self->cursors[i];
			while (cursor->hasMessage()) {
				self->cursorMessages[i].push_back(ILogSystem::BufferedCursor::BufferedMessage(
				    cursor->arena(),
				    (!self->withTags || self->collectTags) ? cursor->getMessage() : cursor->getMessageWithTags(),
				    !self->withTags ? VectorRef<Tag>() : cursor->getTags(),
				    cursor->version()));
				cursor->nextMessage();
			}
			minVersion = std::min(minVersion, cursor->version().version);
		}
		if (minVersion > self->messageVersion.version) {
			break;
		}
		if (allLoaders.isReady()) {
			wait(Future<Void>(Never()));
		}
	}
	wait(yield());

	for (auto& it : self->cursorMessages) {
		while (!it.empty() && it.front().version.version < minVersion) {
			self->messages.push_back(it.front());
			it.pop_front();
		}
	}
	if (self->collectTags || self->knownUnique) {
		std::sort(self->messages.begin(), self->messages.end());
	} else {
		uniquify(self->messages);
	}

	self->messageVersion = LogMessageVersion(minVersion);
	self->messageIndex = 0;
	self->hasNextMessage = self->messages.size() > 0;

	if (self->collectTags) {
		self->combineMessages();
	}

	wait(yield());
	if (self->canDiscardPopped && self->poppedVersion > self->version().version) {
		TraceEvent(SevWarn, "DiscardingPoppedData", self->randomID)
		    .detail("Version", self->version().version)
		    .detail("Popped", self->poppedVersion);
		self->messageVersion = std::max(self->messageVersion, LogMessageVersion(self->poppedVersion));
		for (auto cursor : self->cursors) {
			cursor->advanceTo(self->messageVersion);
		}
		self->messageIndex = self->messages.size();
		if (self->messages.size() > 0 &&
		    self->messages[self->messages.size() - 1].version.version < self->poppedVersion) {
			self->hasNextMessage = false;
		} else {
			auto iter = std::lower_bound(self->messages.begin(),
			                             self->messages.end(),
			                             ILogSystem::BufferedCursor::BufferedMessage(self->poppedVersion));
			self->hasNextMessage = iter != self->messages.end();
			if (self->hasNextMessage) {
				self->messageIndex = iter - self->messages.begin();
			}
		}
	}
	if (self->hasNextMessage) {
		self->canDiscardPopped = false;
	}
	return Void();
}

Future<Void> ILogSystem::BufferedCursor::getMore(TaskPriority taskID) {
	if (hasMessage()) {
		return Void();
	}

	if (!more.isValid() || more.isReady()) {
		more = bufferedGetMore(this, taskID);
	}
	return more;
}

Future<Void> ILogSystem::BufferedCursor::onFailed() {
	ASSERT(false);
	return Never();
}

bool ILogSystem::BufferedCursor::isActive() const {
	ASSERT(false);
	return false;
}

bool ILogSystem::BufferedCursor::isExhausted() const {
	ASSERT(false);
	return false;
}

const LogMessageVersion& ILogSystem::BufferedCursor::version() const {
	if (hasNextMessage) {
		return messages[messageIndex].version;
	}
	return messageVersion;
}

Version ILogSystem::BufferedCursor::getMinKnownCommittedVersion() const {
	return minKnownCommittedVersion;
}

Optional<UID> ILogSystem::BufferedCursor::getPrimaryPeekLocation() const {
	return Optional<UID>();
}

Optional<UID> ILogSystem::BufferedCursor::getCurrentPeekLocation() const {
	return Optional<UID>();
}

Version ILogSystem::BufferedCursor::popped() const {
	if (initialPoppedVersion == poppedVersion) {
		return 0;
	}
	return poppedVersion;
}
