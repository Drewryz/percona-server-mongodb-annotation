/**
 *    Copyright (C) 2008 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <deque>
#include <memory>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {

class Database;
class OperationContext;
struct MultikeyPathInfo;

namespace repl {
class BackgroundSync;
class ReplicationCoordinator;
class OpTime;

/**
 * Used for oplog application on a replica set secondary.
 * Primarily used to apply batches of operations fetched from a sync source during steady state
 * replication and initial sync.
 *
 * When used for steady state replication, runs a thread that reads batches of operations from
 * an oplog buffer (through the BackgroundSync interface) and applies the batch of operations.
 */
class SyncTail {
public:
    using MultiSyncApplyFunc = stdx::function<void(MultiApplier::OperationPtrs* ops,
                                                   SyncTail* st,
                                                   WorkerMultikeyPathInfo* workerMultikeyPathInfo)>;

    /**
     * Type of function to increment "repl.apply.ops" server status metric.
     */
    using IncrementOpsAppliedStatsFn = stdx::function<void()>;

    /**
     * Type of function that takes a non-command op and applies it locally.
     * Used for applying from an oplog.
     * 'db' is the database where the op will be applied.
     * 'opObj' is a BSONObj describing the op to be applied.
     * 'alwaysUpsert' indicates to convert updates to upserts for idempotency reasons.
     * 'mode' indicates the oplog application mode.
     * 'opCounter' is used to update server status metrics.
     * Returns failure status if the op was an update that could not be applied.
     */
    using ApplyOperationInLockFn =
        stdx::function<Status(OperationContext* opCtx,
                              Database* db,
                              const BSONObj& opObj,
                              bool alwaysUpsert,
                              OplogApplication::Mode oplogApplicationMode,
                              IncrementOpsAppliedStatsFn opCounter)>;

    /**
     * Type of function that takes a command op and applies it locally.
     * Used for applying from an oplog.
     * 'mode' indicates the oplog application mode.
     * Returns failure status if the op that could not be applied.
     */
    using ApplyCommandInLockFn = stdx::function<Status(
        OperationContext*, const BSONObj&, OplogApplication::Mode oplogApplicationMode)>;

    /**
     *
     * Constructs a SyncTail.
     * During steady state replication, oplogApplication() obtains batches of operations to apply
     * from 'bgsync'. It is not required to provide 'bgsync' at construction if we do not plan on
     * using oplogApplication(). During the oplog application phase, the batch of operations is
     * distributed across writer threads in 'writerPool'. Each writer thread applies its own vector
     * of operations using 'func'. The writer thread pool is not owned by us.
     */
    SyncTail(BackgroundSync* bgsync, MultiSyncApplyFunc func, ThreadPool* writerPool);
    virtual ~SyncTail();

    /**
     * Creates thread pool for writer tasks.
     */
    static std::unique_ptr<ThreadPool> makeWriterPool();
    static std::unique_ptr<ThreadPool> makeWriterPool(int threadCount);

    /**
     * Applies the operation that is in param o.
     * Functions for applying operations/commands and increment server status counters may
     * be overridden for testing.
     */
    static Status syncApply(OperationContext* opCtx,
                            const BSONObj& o,
                            OplogApplication::Mode oplogApplicationMode,
                            ApplyOperationInLockFn applyOperationInLock,
                            ApplyCommandInLockFn applyCommandInLock,
                            IncrementOpsAppliedStatsFn incrementOpsAppliedStats);

    static Status syncApply(OperationContext* opCtx,
                            const BSONObj& o,
                            OplogApplication::Mode oplogApplicationMode);

    void oplogApplication(ReplicationCoordinator* replCoord);
    bool peek(OperationContext* opCtx, BSONObj* obj);

    class OpQueue {
    public:
        OpQueue() : _bytes(0) {
            _batch.reserve(replBatchLimitOperations.load());
        }

        size_t getBytes() const {
            return _bytes;
        }
        size_t getCount() const {
            return _batch.size();
        }
        bool empty() const {
            return _batch.empty();
        }
        const OplogEntry& front() const {
            invariant(!_batch.empty());
            return _batch.front();
        }
        const OplogEntry& back() const {
            invariant(!_batch.empty());
            return _batch.back();
        }
        const std::vector<OplogEntry>& getBatch() const {
            return _batch;
        }

        void emplace_back(BSONObj obj) {
            invariant(!_mustShutdown);
            _bytes += obj.objsize();
            _batch.emplace_back(std::move(obj));
        }
        void pop_back() {
            _bytes -= back().getRawObjSizeBytes();
            _batch.pop_back();
        }

        /**
         * A batch with this set indicates that the upstream stages of the pipeline are shutdown and
         * no more batches will be coming.
         *
         * This can only happen with empty batches.
         *
         * TODO replace the empty object used to signal draining with this.
         */
        bool mustShutdown() const {
            return _mustShutdown;
        }
        void setMustShutdownFlag() {
            invariant(empty());
            _mustShutdown = true;
        }

        /**
         * Leaves this object in an unspecified state. Only assignment and destruction are valid.
         */
        std::vector<OplogEntry> releaseBatch() {
            return std::move(_batch);
        }

    private:
        std::vector<OplogEntry> _batch;
        size_t _bytes;
        bool _mustShutdown = false;
    };

    struct BatchLimits {
        size_t bytes = replBatchLimitBytes;
        size_t ops = replBatchLimitOperations.load();

        // If provided, the batch will not include any operations with timestamps after this point.
        // This is intended for implementing slaveDelay, so it should be some number of seconds
        // before now.
        boost::optional<Date_t> slaveDelayLatestTimestamp = {};
    };

    /**
     * Attempts to pop an OplogEntry off the BGSync queue and add it to ops.
     *
     * Returns true if the (possibly empty) batch in ops should be ended and a new one started.
     * If ops is empty on entry and nothing can be added yet, will wait up to a second before
     * returning true.
     */
    bool tryPopAndWaitForMore(OperationContext* opCtx, OpQueue* ops, const BatchLimits& limits);

    /**
     * Fetch a single document referenced in the operation from the sync source.
     */
    virtual BSONObj getMissingDoc(OperationContext* opCtx, const OplogEntry& oplogEntry);

    /**
     * If an update fails, fetches the missing document and inserts it into the local collection.
     *
     * Returns true if the document was fetched and inserted successfully.
     */
    virtual bool fetchAndInsertMissingDocument(OperationContext* opCtx,
                                               const OplogEntry& oplogEntry);

    void setHostname(const std::string& hostname);

    static AtomicInt32 replBatchLimitOperations;

    /**
     * Apply a batch of operations, using multiple threads.
     * Returns the last OpTime applied during the apply batch, ops.end["ts"] basically.
     */
    StatusWith<OpTime> multiApply(OperationContext* opCtx, MultiApplier::Operations ops);

protected:
    static const unsigned int replBatchLimitBytes = 100 * 1024 * 1024;
    static const int replBatchLimitSeconds = 1;

private:
    class OpQueueBatcher;

    std::string _hostname;

    BackgroundSync* _bgsync;

    // Function to use during applyOps
    MultiSyncApplyFunc _applyFunc;

    // Pool of worker threads for writing ops to the databases.
    // Not owned by us.
    ThreadPool* const _writerPool;
};

/**
 * Applies the operations described in the oplog entries contained in "ops" using the
 * "applyOperation" function.
 *
 * Returns ErrorCodes::CannotApplyOplogWhilePrimary if the node has become primary, and the OpTime
 * of the final operation applied otherwise.
 *
 * Shared between here and MultiApplier.
 */
StatusWith<OpTime> multiApply(OperationContext* opCtx,
                              ThreadPool* workerPool,
                              MultiApplier::Operations ops,
                              MultiApplier::ApplyOperationFn applyOperation);

// These free functions are used by the thread pool workers to write ops to the db.
// They consume the passed in OperationPtrs and callers should not make any assumptions about the
// state of the container after calling. However, these functions cannot modify the pointed-to
// operations because the OperationPtrs container contains const pointers.
void multiSyncApply(MultiApplier::OperationPtrs* ops,
                    SyncTail* st,
                    WorkerMultikeyPathInfo* workerMultikeyPathInfo);

// Used by 3.4 initial sync.
Status multiInitialSyncApply(MultiApplier::OperationPtrs* ops,
                             SyncTail* st,
                             AtomicUInt32* fetchCount,
                             WorkerMultikeyPathInfo* workerMultikeyPathInfo);

/**
 * Testing-only version of multiSyncApply that returns an error instead of aborting.
 * Accepts an external operation context and a function with the same argument list as
 * SyncTail::syncApply.
 */
using SyncApplyFn = stdx::function<Status(
    OperationContext* opCtx, const BSONObj& o, OplogApplication::Mode oplogApplicationMode)>;
Status multiSyncApply_noAbort(OperationContext* opCtx,
                              MultiApplier::OperationPtrs* ops,
                              WorkerMultikeyPathInfo* workerMultikeyPathInfo,
                              SyncApplyFn syncApply);

/**
 * Testing-only version of multiInitialSyncApply that accepts an external operation context and
 * returns an error instead of aborting.
 */
Status multiInitialSyncApply_noAbort(OperationContext* opCtx,
                                     MultiApplier::OperationPtrs* ops,
                                     SyncTail* st,
                                     AtomicUInt32* fetchCount,
                                     WorkerMultikeyPathInfo* workerMultikeyPathInfo);

}  // namespace repl
}  // namespace mongo
