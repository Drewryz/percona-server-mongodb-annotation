
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

#pragma once


#include "mongo/base/owned_pointer_vector.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/record_id.h"
#include "mongo/util/timer.h"

namespace mongo {

class RecordCursor;

/**
 * OplogStart walks a collection backwards to find the first object in the collection that matches
 * the timestamp.  It's used by replication to efficiently find where the oplog should be replayed
 * from.
 *
 * The oplog is always a capped collection.  In capped collections, documents are oriented on disk
 * according to insertion order.  The oplog inserts documents with increasing timestamps.  Queries
 * on the oplog look for entries that are after a certain time.  Therefore if we navigate backwards,
 * the first document we encounter that is less than or equal to the timestamp is the first document
 * we should scan.
 *
 * Why isn't this a normal reverse table scan, you may ask?  We could be correct if we used a
 * normal reverse collection scan.  However, that's not fast enough.  Since we know all
 * documents are oriented on disk in insertion order, we know all documents in one extent were
 * inserted before documents in a subsequent extent.  As such we can skip through entire extents
 * looking only at the first document.
 *
 * Why is this a stage?  Because we want to yield, and we want to be notified of RecordId
 * invalidations.  :(
 */
class OplogStart final : public PlanStage {
public:
    // Does not take ownership.
    OplogStart(OperationContext* opCtx,
               const Collection* collection,
               Timestamp timestamp,
               WorkingSet* ws);

    StageState doWork(WorkingSetID* out) final;
    bool isEOF() final;

    void doInvalidate(OperationContext* opCtx, const RecordId& dl, InvalidationType type) final;
    void doSaveState() final;
    void doRestoreState() final;
    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;

    // Returns empty PlanStageStats object
    std::unique_ptr<PlanStageStats> getStats() final;

    //
    // Exec stats -- do not call for the oplog start stage.
    //

    const SpecificStats* getSpecificStats() const final {
        return NULL;
    }

    StageType stageType() const final {
        return STAGE_OPLOG_START;
    }

    // For testing only.
    void setBackwardsScanTime(int newTime) {
        _backwardsScanTime = newTime;
    }
    bool isExtentHopping() {
        return _extentHopping;
    }
    bool isBackwardsScanning() {
        return _backwardsScanning;
    }

    static const char* kStageType;

private:
    StageState workBackwardsScan(WorkingSetID* out);

    void switchToExtentHopping();

    StageState workExtentHopping(WorkingSetID* out);

    // This is only used for the extent hopping scan.
    std::vector<std::unique_ptr<RecordCursor>> _subIterators;

    // Have we done our heavy init yet?
    bool _needInit;

    // Our first state: going backwards via a collscan.
    bool _backwardsScanning;

    // Our second state: hopping backwards extent by extent.
    bool _extentHopping;

    // Our final state: done.
    bool _done;

    const Collection* _collection;

    // We only go backwards via a collscan for a few seconds.
    Timer _timer;

    // WorkingSet is not owned by us.
    WorkingSet* _workingSet;

    std::string _ns;

    // '_filter' matches documents whose "ts" field is less than or equal to 'timestamp'. Once we
    // have found a document matching '_filter', we know that we're at or behind the starting point
    // and can start scanning forwards again.
    BSONObj _filterBSON;
    LTEMatchExpression _filter;

    static int _backwardsScanTime;
};

}  // namespace mongo
