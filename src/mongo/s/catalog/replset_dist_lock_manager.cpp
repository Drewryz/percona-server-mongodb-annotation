/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/replset_dist_lock_manager.h"

#include <memory>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/dist_lock_catalog.h"
#include "mongo/s/catalog/type_lockpings.h"
#include "mongo/s/catalog/type_locks.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/chrono.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(setDistLockTimeout);

using std::string;
using std::unique_ptr;

namespace {

MONGO_FAIL_POINT_DEFINE(disableReplSetDistLockManager);

// How many times to retry acquiring the lock after the first attempt fails
const int kMaxNumLockAcquireRetries = 2;

// How frequently to poll the distributed lock when it is found to be locked
const Milliseconds kLockRetryInterval(500);

}  // namespace

const Seconds ReplSetDistLockManager::kDistLockPingInterval{30};
const Minutes ReplSetDistLockManager::kDistLockExpirationTime{15};

ReplSetDistLockManager::ReplSetDistLockManager(ServiceContext* globalContext,
                                               StringData processID,
                                               unique_ptr<DistLockCatalog> catalog,
                                               Milliseconds pingInterval,
                                               Milliseconds lockExpiration)
    : _serviceContext(globalContext),
      _processID(processID.toString()),
      _catalog(std::move(catalog)),
      _pingInterval(pingInterval),
      _lockExpiration(lockExpiration) {}

ReplSetDistLockManager::~ReplSetDistLockManager() = default;

void ReplSetDistLockManager::startUp() {
    if (!_execThread) {
        _execThread = std::make_unique<stdx::thread>(&ReplSetDistLockManager::doTask, this);
    }
}

void ReplSetDistLockManager::shutDown(OperationContext* opCtx) {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _isShutDown = true;
        _shutDownCV.notify_all();
    }

    // Don't grab _mutex, otherwise will deadlock trying to join. Safe to read
    // _execThread since it is modified only at statrUp().
    if (_execThread && _execThread->joinable()) {
        _execThread->join();
        _execThread.reset();
    }

    auto status = _catalog->stopPing(opCtx, _processID);
    if (!status.isOK()) {
        LOGV2_WARNING(22667,
                      "Error cleaning up distributed ping entry for {processId} caused by {error}",
                      "Error cleaning up distributed ping entry",
                      "processId"_attr = _processID,
                      "error"_attr = redact(status));
    }
}

std::string ReplSetDistLockManager::getProcessID() {
    return _processID;
}

bool ReplSetDistLockManager::isShutDown() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _isShutDown;
}

void ReplSetDistLockManager::doTask() {
    LOGV2(22649,
          "Creating distributed lock ping thread for process {processId} with ping interval "
          "{pingInterval}",
          "Creating distributed lock ping thread",
          "processId"_attr = _processID,
          "pingInterval"_attr = _pingInterval);

    Timer elapsedSincelastPing(_serviceContext->getTickSource());
    Client::initThread("replSetDistLockPinger");

    while (!isShutDown()) {
        if (MONGO_unlikely(disableReplSetDistLockManager.shouldFail())) {
            LOGV2(426321,
                  "The distributed lock ping thread is disabled for testing",
                  "processId"_attr = _processID,
                  "pingInterval"_attr = _pingInterval);
            return;
        }
        {
            auto opCtx = cc().makeOperationContext();
            auto pingStatus = _catalog->ping(opCtx.get(), _processID, Date_t::now());

            if (!pingStatus.isOK() && pingStatus != ErrorCodes::NotMaster) {
                LOGV2_WARNING(22668,
                              "Pinging failed for distributed lock pinger caused by {error}",
                              "Pinging failed for distributed lock pinger",
                              "error"_attr = pingStatus);
            }

            const Milliseconds elapsed(elapsedSincelastPing.millis());
            if (elapsed > 10 * _pingInterval) {
                LOGV2_WARNING(22669,
                              "Lock pinger for process {processId} was inactive for {duration}",
                              "Lock pinger was inactive for multiple intervals",
                              "processId"_attr = _processID,
                              "duration"_attr = elapsed);
            }
            elapsedSincelastPing.reset();

            std::deque<std::pair<DistLockHandle, boost::optional<std::string>>> toUnlockBatch;
            {
                stdx::unique_lock<Latch> lk(_mutex);
                toUnlockBatch.swap(_unlockList);
            }

            for (const auto& toUnlock : toUnlockBatch) {
                Status unlockStatus(ErrorCodes::NotYetInitialized,
                                    "status unlock not initialized!");
                if (toUnlock.second) {
                    // A non-empty _id (name) field was provided, unlock by ts (sessionId) and _id.
                    unlockStatus = _catalog->unlock(opCtx.get(), toUnlock.first, *toUnlock.second);
                } else {
                    unlockStatus = _catalog->unlock(opCtx.get(), toUnlock.first);
                }

                if (!unlockStatus.isOK()) {
                    LOGV2_WARNING(22670,
                                  "Error unlocking distributed lock {lockName} with sessionID "
                                  "{lockSessionId} caused by {error}",
                                  "Error unlocking distributed lock",
                                  "lockName"_attr = toUnlock.second,
                                  "lockSessionId"_attr = toUnlock.first,
                                  "error"_attr = unlockStatus);
                    // Queue another attempt, unless the problem was no longer being primary.
                    if (unlockStatus != ErrorCodes::NotMaster) {
                        queueUnlock(toUnlock.first, toUnlock.second);
                    }
                } else {
                    LOGV2(22650,
                          "Unlocked distributed lock {lockName} with sessionID {lockSessionId}",
                          "Unlocked distributed lock",
                          "lockName"_attr = toUnlock.second,
                          "lockSessionId"_attr = toUnlock.first);
                }

                if (isShutDown()) {
                    return;
                }
            }
        }

        MONGO_IDLE_THREAD_BLOCK;
        stdx::unique_lock<Latch> lk(_mutex);
        _shutDownCV.wait_for(lk, _pingInterval.toSystemDuration(), [this] { return _isShutDown; });
    }
}

StatusWith<bool> ReplSetDistLockManager::isLockExpired(OperationContext* opCtx,
                                                       LocksType lockDoc,
                                                       const Milliseconds& lockExpiration) {
    const auto& processID = lockDoc.getProcess();
    auto pingStatus = _catalog->getPing(opCtx, processID);

    Date_t pingValue;
    if (pingStatus.isOK()) {
        const auto& pingDoc = pingStatus.getValue();
        Status pingDocValidationStatus = pingDoc.validate();
        if (!pingDocValidationStatus.isOK()) {
            return {ErrorCodes::UnsupportedFormat,
                    str::stream() << "invalid ping document for " << processID << ": "
                                  << pingDocValidationStatus.toString()};
        }

        pingValue = pingDoc.getPing();
    } else if (pingStatus.getStatus() != ErrorCodes::NoMatchingDocument) {
        return pingStatus.getStatus();
    }  // else use default pingValue if ping document does not exist.

    Timer timer(_serviceContext->getTickSource());
    auto serverInfoStatus = _catalog->getServerInfo(opCtx);
    if (!serverInfoStatus.isOK()) {
        if (serverInfoStatus.getStatus() == ErrorCodes::NotMaster) {
            return false;
        }

        return serverInfoStatus.getStatus();
    }

    // Be conservative when determining that lock expiration has elapsed by
    // taking into account the roundtrip delay of trying to get the local
    // time from the config server.
    Milliseconds delay(timer.millis() / 2);  // Assuming symmetrical delay.

    const auto& serverInfo = serverInfoStatus.getValue();

    stdx::lock_guard<Latch> lk(_mutex);
    auto pingIter = _pingHistory.find(lockDoc.getName());

    if (pingIter == _pingHistory.end()) {
        // We haven't seen this lock before so we don't have any point of reference
        // to compare and determine the elapsed time. Save the current ping info
        // for this lock.
        _pingHistory.emplace(std::piecewise_construct,
                             std::forward_as_tuple(lockDoc.getName()),
                             std::forward_as_tuple(processID,
                                                   pingValue,
                                                   serverInfo.serverTime,
                                                   lockDoc.getLockID(),
                                                   serverInfo.electionId));
        return false;
    }

    auto configServerLocalTime = serverInfo.serverTime - delay;

    auto* pingInfo = &pingIter->second;

    LOGV2_DEBUG(22651,
                1,
                "Checking last ping for lock {lockName} against last seen process {processId} and "
                "ping {lastPing}",
                "Checking last ping for lock",
                "lockName"_attr = lockDoc.getName(),
                "processId"_attr = pingInfo->processId,
                "lastPing"_attr = pingInfo->lastPing);

    if (pingInfo->lastPing != pingValue ||  // ping is active

        // Owner of this lock is now different from last time so we can't
        // use the ping data.
        pingInfo->lockSessionId != lockDoc.getLockID() ||

        // Primary changed, we can't trust that clocks are synchronized so
        // treat as if this is a new entry.
        pingInfo->electionId != serverInfo.electionId) {
        pingInfo->lastPing = pingValue;
        pingInfo->electionId = serverInfo.electionId;
        pingInfo->configLocalTime = configServerLocalTime;
        pingInfo->lockSessionId = lockDoc.getLockID();
        return false;
    }

    if (configServerLocalTime < pingInfo->configLocalTime) {
        LOGV2_WARNING(22671,
                      "Config server local time went backwards, new value "
                      "{newConfigServerLocalTime}, old value {oldConfigServerLocalTime}",
                      "Config server local time went backwards",
                      "newConfigServerLocalTime"_attr = configServerLocalTime,
                      "oldConfigServerLocalTime"_attr = pingInfo->configLocalTime);
        return false;
    }

    Milliseconds elapsedSinceLastPing(configServerLocalTime - pingInfo->configLocalTime);
    if (elapsedSinceLastPing >= lockExpiration) {
        LOGV2(22652,
              "Forcing lock {lockName} because elapsed time {elapsedSinceLastPing} >= "
              "takeover time {lockExpirationTimeout}",
              "Forcing lock because too much time has passed from last ping",
              "lockName"_attr = lockDoc.getName(),
              "elapsedSinceLastPing"_attr = elapsedSinceLastPing,
              "lockExpirationTimeout"_attr = lockExpiration);
        return true;
    }

    LOGV2_DEBUG(22653,
                1,
                "Could not force lock of {lockName} because elapsed time {elapsedSinceLastPing} < "
                "takeover time {lockExpirationTimeout}",
                "Could not force lock because too little time has passed from last ping",
                "lockName"_attr = lockDoc.getName(),
                "elapsedSinceLastPing"_attr = elapsedSinceLastPing,
                "lockExpirationTimeout"_attr = lockExpiration);
    return false;
}

StatusWith<DistLockHandle> ReplSetDistLockManager::lockWithSessionID(OperationContext* opCtx,
                                                                     StringData name,
                                                                     StringData whyMessage,
                                                                     const OID& lockSessionID,
                                                                     Milliseconds waitFor) {
    Timer timer(_serviceContext->getTickSource());
    Timer msgTimer(_serviceContext->getTickSource());

    // Counts how many attempts have been made to grab the lock, which have failed with network
    // error. This value is reset for each lock acquisition attempt because these are
    // independent write operations.
    int networkErrorRetries = 0;

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    // Distributed lock acquisition works by tring to update the state of the lock to 'taken'. If
    // the lock is currently taken, we will back off and try the acquisition again, repeating this
    // until the lockTryInterval has been reached. If a network error occurs at each lock
    // acquisition attempt, the lock acquisition will be retried immediately.
    while (waitFor <= Milliseconds::zero() || Milliseconds(timer.millis()) < waitFor) {
        const string who = str::stream() << _processID << ":" << getThreadName();

        auto lockExpiration = _lockExpiration;
        setDistLockTimeout.execute([&](const BSONObj& data) {
            lockExpiration = Milliseconds(data["timeoutMs"].numberInt());
        });

        LOGV2_DEBUG(22654,
                    1,
                    "Trying to acquire new distributed lock for {lockName} ( "
                    "lockSessionID: {lockSessionId}, "
                    "process : {processId}, "
                    "lock timeout : {lockExpirationTimeout}, "
                    "ping interval : {pingInterval}, "
                    "reason: {reason} )",
                    "Trying to acquire new distributed lock",
                    "lockName"_attr = name,
                    "lockSessionId"_attr = lockSessionID,
                    "processId"_attr = _processID,
                    "lockExpirationTimeout"_attr = lockExpiration,
                    "pingInterval"_attr = _pingInterval,
                    "reason"_attr = whyMessage);

        auto lockResult = _catalog->grabLock(
            opCtx, name, lockSessionID, who, _processID, Date_t::now(), whyMessage.toString());

        auto status = lockResult.getStatus();

        if (status.isOK()) {
            // Lock is acquired since findAndModify was able to successfully modify
            // the lock document.
            LOGV2(22655,
                  "Acquired distributed lock {lockName} with session ID {lockSessionId} for "
                  "{reason}",
                  "Acquired distributed lock",
                  "lockName"_attr = name,
                  "lockSessionId"_attr = lockSessionID,
                  "reason"_attr = whyMessage);
            return lockSessionID;
        }

        // If a network error occurred, unlock the lock synchronously and try again
        if (configShard->isRetriableError(status.code(), Shard::RetryPolicy::kIdempotent) &&
            networkErrorRetries < kMaxNumLockAcquireRetries) {
            LOGV2_DEBUG(22656,
                        1,
                        "Error acquiring distributed lock because of retryable error. "
                        "Retrying acquisition by first unlocking the stale entry, which possibly "
                        "exists now. Caused by {error}",
                        "Error acquiring distributed lock because of retryable error. "
                        "Retrying acquisition by first unlocking the stale entry, which possibly "
                        "exists now",
                        "error"_attr = redact(status));

            networkErrorRetries++;

            status = _catalog->unlock(opCtx, lockSessionID, name);
            if (status.isOK()) {
                // We certainly do not own the lock, so we can retry
                continue;
            }

            // Fall-through to the error checking logic below
            invariant(status != ErrorCodes::LockStateChangeFailed);

            LOGV2_DEBUG(22657,
                        1,
                        "Last attempt to acquire distributed lock failed with {error}",
                        "Last attempt to acquire distributed lock failed",
                        "error"_attr = redact(status));
        }

        if (status != ErrorCodes::LockStateChangeFailed) {
            // An error occurred but the write might have actually been applied on the
            // other side. Schedule an unlock to clean it up just in case.
            queueUnlock(lockSessionID, name.toString());
            return status;
        }

        // Get info from current lock and check if we can overtake it.
        auto getLockStatusResult = _catalog->getLockByName(opCtx, name);
        const auto& getLockStatus = getLockStatusResult.getStatus();

        if (!getLockStatusResult.isOK() && getLockStatus != ErrorCodes::LockNotFound) {
            return getLockStatus;
        }

        // Note: Only attempt to overtake locks that actually exists. If lock was not
        // found, use the normal grab lock path to acquire it.
        if (getLockStatusResult.isOK()) {
            auto currentLock = getLockStatusResult.getValue();
            auto isLockExpiredResult = isLockExpired(opCtx, currentLock, lockExpiration);

            if (!isLockExpiredResult.isOK()) {
                return isLockExpiredResult.getStatus();
            }

            if (isLockExpiredResult.getValue() || (lockSessionID == currentLock.getLockID())) {
                auto overtakeResult = _catalog->overtakeLock(opCtx,
                                                             name,
                                                             lockSessionID,
                                                             currentLock.getLockID(),
                                                             who,
                                                             _processID,
                                                             Date_t::now(),
                                                             whyMessage);

                const auto& overtakeStatus = overtakeResult.getStatus();

                if (overtakeResult.isOK()) {
                    // Lock is acquired since findAndModify was able to successfully modify
                    // the lock document.

                    LOGV2(22658,
                          "Acquired distributed lock {lockName} with sessionId {lockSessionId}",
                          "Acquired distributed lock",
                          "lockName"_attr = name,
                          "lockSessionId"_attr = lockSessionID);
                    return lockSessionID;
                }

                if (overtakeStatus != ErrorCodes::LockStateChangeFailed) {
                    // An error occurred but the write might have actually been applied on the
                    // other side. Schedule an unlock to clean it up just in case.
                    queueUnlock(lockSessionID, boost::none);
                    return overtakeStatus;
                }
            }
        }

        LOGV2_DEBUG(22660,
                    1,
                    "Distributed lock {lockName} was not acquired",
                    "Distributed lock was not acquired",
                    "lockName"_attr = name);

        if (waitFor == Milliseconds::zero()) {
            break;
        }

        // Periodically message for debugging reasons
        if (msgTimer.seconds() > 10) {
            LOGV2(22661,
                  "Waited {elapsed} for distributed lock {lockName} for {reason}",
                  "Waiting for distributed lock",
                  "lockName"_attr = name,
                  "elapsed"_attr = Seconds(timer.seconds()),
                  "reason"_attr = whyMessage);

            msgTimer.reset();
        }

        // A new lock acquisition attempt will begin now (because the previous found the lock to be
        // busy, so reset the retries counter)
        networkErrorRetries = 0;

        const Milliseconds timeRemaining =
            std::max(Milliseconds::zero(), waitFor - Milliseconds(timer.millis()));
        sleepFor(std::min(kLockRetryInterval, timeRemaining));
    }

    return {ErrorCodes::LockBusy, str::stream() << "timed out waiting for " << name};
}

StatusWith<DistLockHandle> ReplSetDistLockManager::tryLockWithLocalWriteConcern(
    OperationContext* opCtx, StringData name, StringData whyMessage, const OID& lockSessionID) {
    const string who = str::stream() << _processID << ":" << getThreadName();

    LOGV2_DEBUG(22662,
                1,
                "Trying to acquire new distributed lock for {lockName} ( "
                "process : {processId}, "
                "lockSessionID: {lockSessionId}, "
                "lock timeout : {lockExpirationTimeout}, "
                "ping interval : {pingInterval}, "
                "reason: {reason} )",
                "Trying to acquire new distributed lock",
                "lockName"_attr = name,
                "lockSessionId"_attr = lockSessionID,
                "processId"_attr = _processID,
                "lockExpirationTimeout"_attr = _lockExpiration,
                "pingInterval"_attr = _pingInterval,
                "reason"_attr = whyMessage);

    auto lockStatus = _catalog->grabLock(opCtx,
                                         name,
                                         lockSessionID,
                                         who,
                                         _processID,
                                         Date_t::now(),
                                         whyMessage.toString(),
                                         DistLockCatalog::kLocalWriteConcern);

    if (lockStatus.isOK()) {
        LOGV2(22663,
              "Acquired distributed lock {lockName} with session ID {lockSessionId} for "
              "{reason}",
              "Acquired distributed lock",
              "lockName"_attr = name,
              "lockSessionId"_attr = lockSessionID,
              "reason"_attr = whyMessage);
        return lockSessionID;
    }

    LOGV2_DEBUG(22664,
                1,
                "Distributed lock {lockName} was not acquired",
                "Distributed lock was not acquired",
                "lockName"_attr = name);

    if (lockStatus == ErrorCodes::LockStateChangeFailed) {
        return {ErrorCodes::LockBusy, str::stream() << "Unable to acquire " << name};
    }

    return lockStatus.getStatus();
}

void ReplSetDistLockManager::unlock(OperationContext* opCtx, const DistLockHandle& lockSessionID) {
    auto unlockStatus = _catalog->unlock(opCtx, lockSessionID);

    if (!unlockStatus.isOK()) {
        queueUnlock(lockSessionID, boost::none);
    } else {
        LOGV2(22665,
              "Unlocked distributed lock with sessionID {lockSessionId}",
              "Unlocked distributed lock",
              "lockSessionId"_attr = lockSessionID);
    }
}

void ReplSetDistLockManager::unlock(OperationContext* opCtx,
                                    const DistLockHandle& lockSessionID,
                                    StringData name) {
    auto unlockStatus = _catalog->unlock(opCtx, lockSessionID, name);

    if (!unlockStatus.isOK()) {
        queueUnlock(lockSessionID, name.toString());
    } else {
        LOGV2(22666,
              "Unlocked distributed lock {lockName} with sessionID {lockSessionId}",
              "Unlocked distributed lock",
              "lockName"_attr = name,
              "lockSessionId"_attr = lockSessionID);
    }
}

void ReplSetDistLockManager::unlockAll(OperationContext* opCtx, const std::string& processID) {
    Status status = _catalog->unlockAll(opCtx, processID);
    if (!status.isOK()) {
        LOGV2_WARNING(
            22672,
            "Error unlocking all distributed locks for process {processId} caused by {error}",
            "Error unlocking all existing distributed locks for a process",
            "processId"_attr = processID,
            "error"_attr = redact(status));
    }
}

Status ReplSetDistLockManager::checkStatus(OperationContext* opCtx,
                                           const DistLockHandle& lockHandle) {
    return _catalog->getLockByTS(opCtx, lockHandle).getStatus();
}

void ReplSetDistLockManager::queueUnlock(const DistLockHandle& lockSessionID,
                                         const boost::optional<std::string>& name) {
    stdx::unique_lock<Latch> lk(_mutex);
    _unlockList.push_back(std::make_pair(lockSessionID, name));
}

}  // namespace mongo
