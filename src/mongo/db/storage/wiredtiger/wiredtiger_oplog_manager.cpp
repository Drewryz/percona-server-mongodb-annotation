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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <cstring>

#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(WTPausePrimaryOplogDurabilityLoop);

void WiredTigerOplogManager::start(OperationContext* opCtx,
                                   const std::string& uri,
                                   WiredTigerRecordStore* oplogRecordStore) {
    invariant(!_isRunning);
    // Prime the oplog read timestamp.
    std::unique_ptr<SeekableRecordCursor> reverseOplogCursor =
        oplogRecordStore->getCursor(opCtx, false /* false = reverse cursor */);
    auto lastRecord = reverseOplogCursor->next();
    if (lastRecord) {
        // Although the oplog may have holes, using the top of the oplog should be safe. In the
        // event of a secondary crashing, replication recovery will truncate the oplog, resetting
        // visibility to the truncate point. In the event of a primary crashing, it will perform
        // rollback before servicing oplog reads.
        auto oplogVisibility = Timestamp(lastRecord->id.repr());
        setOplogReadTimestamp(oplogVisibility);
        LOGV2_DEBUG(22368,
                    1,
                    "Setting oplog visibility at startup. Val: {oplogVisibility}",
                    "oplogVisibility"_attr = oplogVisibility);
    } else {
        // Avoid setting oplog visibility to 0. That means "everything is visible".
        setOplogReadTimestamp(Timestamp(StorageEngine::kMinimumTimestamp));
    }

    // Need to obtain the mutex before starting the thread, as otherwise it may race ahead
    // see _shuttingDown as true and quit prematurely.
    stdx::lock_guard<Latch> lk(_oplogVisibilityStateMutex);
    _oplogJournalThread = stdx::thread(&WiredTigerOplogManager::_oplogJournalThreadLoop,
                                       this,
                                       WiredTigerRecoveryUnit::get(opCtx)->getSessionCache(),
                                       oplogRecordStore);

    _isRunning = true;
    _shuttingDown = false;
}

void WiredTigerOplogManager::halt() {
    {
        stdx::lock_guard<Latch> lk(_oplogVisibilityStateMutex);
        invariant(_isRunning);
        _shuttingDown = true;
        _isRunning = false;
    }

    if (_oplogJournalThread.joinable()) {
        _opsWaitingForJournalCV.notify_one();
        _oplogJournalThread.join();
    }
}

void WiredTigerOplogManager::waitForAllEarlierOplogWritesToBeVisible(
    const WiredTigerRecordStore* oplogRecordStore, OperationContext* opCtx) {
    invariant(opCtx->lockState()->isNoop() || !opCtx->lockState()->inAWriteUnitOfWork());

    // In order to reliably detect rollback situations, we need to fetch the latestVisibleTimestamp
    // prior to querying the end of the oplog.
    auto currentLatestVisibleTimestamp = getOplogReadTimestamp();

    // Procedure: issue a read on a reverse cursor (which is not subject to the oplog visibility
    // rules), see what is last, and wait for that to become visible.
    std::unique_ptr<SeekableRecordCursor> cursor =
        oplogRecordStore->getCursor(opCtx, false /* false = reverse cursor */);
    auto lastRecord = cursor->next();
    if (!lastRecord) {
        LOGV2_DEBUG(22369, 2, "Trying to query an empty oplog");
        opCtx->recoveryUnit()->abandonSnapshot();
        return;
    }
    const auto waitingFor = lastRecord->id;
    // Close transaction before we wait.
    opCtx->recoveryUnit()->abandonSnapshot();

    stdx::unique_lock<Latch> lk(_oplogVisibilityStateMutex);

    // Prevent any scheduled journal flushes from being delayed and blocking this wait excessively.
    _opsWaitingForVisibility++;
    invariant(_opsWaitingForVisibility > 0);
    auto exitGuard = makeGuard([&] { _opsWaitingForVisibility--; });

    opCtx->waitForConditionOrInterrupt(_opsBecameVisibleCV, lk, [&] {
        auto newLatestVisibleTimestamp = getOplogReadTimestamp();
        if (newLatestVisibleTimestamp < currentLatestVisibleTimestamp) {
            LOGV2_DEBUG(22370,
                        1,
                        "Oplog latest visible timestamp went backwards. newLatestVisibleTimestamp: "
                        "{Timestamp_newLatestVisibleTimestamp} currentLatestVisibleTimestamp: "
                        "{Timestamp_currentLatestVisibleTimestamp}",
                        "Timestamp_newLatestVisibleTimestamp"_attr =
                            Timestamp(newLatestVisibleTimestamp),
                        "Timestamp_currentLatestVisibleTimestamp"_attr =
                            Timestamp(currentLatestVisibleTimestamp));
            // If the visibility went backwards, this means a rollback occurred.
            // Thus, we are finished waiting.
            return true;
        }
        currentLatestVisibleTimestamp = newLatestVisibleTimestamp;
        RecordId latestVisible = RecordId(currentLatestVisibleTimestamp);
        if (latestVisible < waitingFor) {
            LOGV2_DEBUG(22371,
                        2,
                        "Operation is waiting for {Timestamp_waitingFor_repr}; latestVisible is "
                        "{Timestamp_currentLatestVisibleTimestamp}",
                        "Timestamp_waitingFor_repr"_attr = Timestamp(waitingFor.repr()),
                        "Timestamp_currentLatestVisibleTimestamp"_attr =
                            Timestamp(currentLatestVisibleTimestamp));
        }
        return latestVisible >= waitingFor;
    });
}

void WiredTigerOplogManager::triggerOplogVisibilityUpdate() {
    stdx::lock_guard<Latch> lk(_oplogVisibilityStateMutex);
    if (!_opsWaitingForJournal) {
        _opsWaitingForJournal = true;
        _opsWaitingForJournalCV.notify_one();
    }
}

void WiredTigerOplogManager::_oplogJournalThreadLoop(WiredTigerSessionCache* sessionCache,
                                                     WiredTigerRecordStore* oplogRecordStore) {
    Client::initThread("WTOplogJournalThread");

    // This thread updates the oplog read timestamp, the timestamp used to read from the oplog with
    // forward cursors.  The timestamp is used to hide oplog entries that might be committed but
    // have uncommitted entries ahead of them.
    while (true) {
        stdx::unique_lock<Latch> lk(_oplogVisibilityStateMutex);
        {
            MONGO_IDLE_THREAD_BLOCK;
            _opsWaitingForJournalCV.wait(lk,
                                         [&] { return _shuttingDown || _opsWaitingForJournal; });

            // If we're not shutting down and nobody is actively waiting for the oplog to become
            // durable, delay journaling a bit to reduce the sync rate.
            auto journalDelay = Milliseconds(storageGlobalParams.journalCommitIntervalMs.load());
            auto now = Date_t::now();
            auto deadline = now + journalDelay;
            auto shouldSyncOpsWaitingForJournal = [&] {
                return _shuttingDown || _opsWaitingForVisibility ||
                    oplogRecordStore->haveCappedWaiters();
            };

            // Eventually it would be more optimal to merge this with the normal journal flushing
            // and block for either oplog tailers or operations waiting for oplog visibility. For
            // now this loop will poll once a millisecond up to the journalDelay to see if we have
            // any waiters yet. This reduces sync-related I/O on the primary when secondaries are
            // lagged, but will avoid significant delays in confirming majority writes on replica
            // sets with infrequent writes.
            // Callers of waitForAllEarlierOplogWritesToBeVisible() like causally consistent reads
            // will preempt this delay.
            while (now < deadline &&
                   !_opsWaitingForJournalCV.wait_until(
                       lk, now.toSystemTimePoint(), shouldSyncOpsWaitingForJournal)) {
                now += Milliseconds(1);
            }
        }

        while (!_shuttingDown && MONGO_unlikely(WTPausePrimaryOplogDurabilityLoop.shouldFail())) {
            lk.unlock();
            sleepmillis(10);
            lk.lock();
        }

        if (_shuttingDown) {
            LOGV2(22372, "Oplog journal thread loop shutting down");
            return;
        }
        invariant(_opsWaitingForJournal);
        _opsWaitingForJournal = false;
        lk.unlock();

        const uint64_t newTimestamp = fetchAllDurableValue(sessionCache->conn());

        // The newTimestamp may actually go backward during secondary batch application,
        // where we commit data file changes separately from oplog changes, so ignore
        // a non-incrementing timestamp.
        if (newTimestamp <= _oplogReadTimestamp.load()) {
            LOGV2_DEBUG(22373,
                        2,
                        "No new oplog entries were made visible: {Timestamp_newTimestamp}",
                        "Timestamp_newTimestamp"_attr = Timestamp(newTimestamp));
            continue;
        }

        lk.lock();
        // Publish the new timestamp value.  Avoid going backward.
        auto oldTimestamp = getOplogReadTimestamp();
        if (newTimestamp > oldTimestamp) {
            _setOplogReadTimestamp(lk, newTimestamp);
        }
        lk.unlock();

        // Wake up any await_data cursors and tell them more data might be visible now.
        oplogRecordStore->notifyCappedWaitersIfNeeded();
    }
}

std::uint64_t WiredTigerOplogManager::getOplogReadTimestamp() const {
    return _oplogReadTimestamp.load();
}

void WiredTigerOplogManager::setOplogReadTimestamp(Timestamp ts) {
    stdx::lock_guard<Latch> lk(_oplogVisibilityStateMutex);
    _setOplogReadTimestamp(lk, ts.asULL());
}

void WiredTigerOplogManager::_setOplogReadTimestamp(WithLock, uint64_t newTimestamp) {
    _oplogReadTimestamp.store(newTimestamp);
    _opsBecameVisibleCV.notify_all();
    LOGV2_DEBUG(22374,
                2,
                "Setting new oplogReadTimestamp: {Timestamp_newTimestamp}",
                "Timestamp_newTimestamp"_attr = Timestamp(newTimestamp));
}

uint64_t WiredTigerOplogManager::fetchAllDurableValue(WT_CONNECTION* conn) {
    // Fetch the latest all_durable value from the storage engine. This value will be a timestamp
    // that has no holes (uncommitted transactions with lower timestamps) behind it.
    char buf[(2 * 8 /*bytes in hex*/) + 1 /*nul terminator*/];
    auto wtstatus = conn->query_timestamp(conn, buf, "get=all_durable");
    if (wtstatus == WT_NOTFOUND) {
        // Treat this as lowest possible timestamp; we need to see all preexisting data but no new
        // (timestamped) data.
        return StorageEngine::kMinimumTimestamp;
    } else {
        invariantWTOK(wtstatus);
    }

    uint64_t tmp;
    fassert(38002, NumberParser().base(16)(buf, &tmp));
    return tmp;
}

}  // namespace mongo
