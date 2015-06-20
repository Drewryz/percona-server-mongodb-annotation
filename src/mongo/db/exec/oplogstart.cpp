/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
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

#include "mongo/db/exec/oplogstart.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"

namespace mongo {

using std::vector;

const char* OplogStart::kStageType = "OPLOG_START";

// Does not take ownership.
OplogStart::OplogStart(OperationContext* txn,
                       const Collection* collection,
                       MatchExpression* filter,
                       WorkingSet* ws)
    : _txn(txn),
      _needInit(true),
      _backwardsScanning(false),
      _extentHopping(false),
      _done(false),
      _collection(collection),
      _workingSet(ws),
      _filter(filter) {}

OplogStart::~OplogStart() {}

PlanStage::StageState OplogStart::work(WorkingSetID* out) {
    // We do our (heavy) init in a work(), where work is expected.
    if (_needInit) {
        CollectionScanParams params;
        params.collection = _collection;
        params.direction = CollectionScanParams::BACKWARD;
        _cs.reset(new CollectionScan(_txn, params, _workingSet, NULL));

        _needInit = false;
        _backwardsScanning = true;
        _timer.reset();
    }

    // If we're still reading backwards, keep trying until timing out.
    if (_backwardsScanning) {
        verify(!_extentHopping);
        // Still have time to succeed with reading backwards.
        if (_timer.seconds() < _backwardsScanTime) {
            return workBackwardsScan(out);
        }

        try {
            // If this throws WCE, it leave us in a state were the next call to work will retry.
            switchToExtentHopping();
        } catch (const WriteConflictException& wce) {
            _subIterators.clear();
            *out = WorkingSet::INVALID_ID;
            return NEED_YIELD;
        }
    }

    // Don't find it in time?  Swing from extent to extent like tarzan.com.
    verify(_extentHopping);
    return workExtentHopping(out);
}

PlanStage::StageState OplogStart::workExtentHopping(WorkingSetID* out) {
    if (_done || _subIterators.empty()) {
        return PlanStage::IS_EOF;
    }

    // we work from the back to the front since the back has the newest data.
    try {
        // TODO: should we ever check fetcherForNext()?
        if (auto record = _subIterators.back()->next()) {
            BSONObj obj = record->data.releaseToBson();
            if (!_filter->matchesBSON(obj)) {
                _done = true;
                WorkingSetID id = _workingSet->allocate();
                WorkingSetMember* member = _workingSet->get(id);
                member->loc = record->id;
                member->obj = {_txn->recoveryUnit()->getSnapshotId(), std::move(obj)};
                member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
                *out = id;
                return PlanStage::ADVANCED;
            }
        }
    } catch (const WriteConflictException& wce) {
        *out = WorkingSet::INVALID_ID;
        return PlanStage::NEED_YIELD;
    }

    _subIterators.pop_back();
    return PlanStage::NEED_TIME;
}

void OplogStart::switchToExtentHopping() {
    // Set up our extent hopping state.
    _subIterators = _collection->getManyCursors(_txn);

    // Transition from backwards scanning to extent hopping.
    _backwardsScanning = false;
    _extentHopping = true;

    // Toss the collection scan we were using.
    _cs.reset();
}

PlanStage::StageState OplogStart::workBackwardsScan(WorkingSetID* out) {
    PlanStage::StageState state = _cs->work(out);

    // EOF.  Just start from the beginning, which is where we've hit.
    if (PlanStage::IS_EOF == state) {
        _done = true;
        return state;
    }

    if (PlanStage::ADVANCED != state) {
        return state;
    }

    WorkingSetMember* member = _workingSet->get(*out);
    verify(member->hasObj());
    verify(member->hasLoc());

    if (!_filter->matchesBSON(member->obj.value())) {
        _done = true;
        // RecordId is returned in *out.
        return PlanStage::ADVANCED;
    } else {
        _workingSet->free(*out);
        return PlanStage::NEED_TIME;
    }
}

bool OplogStart::isEOF() {
    return _done;
}

void OplogStart::invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {
    if (_needInit) {
        return;
    }

    if (INVALIDATION_DELETION != type) {
        return;
    }

    if (_cs) {
        _cs->invalidate(txn, dl, type);
    }

    for (size_t i = 0; i < _subIterators.size(); i++) {
        _subIterators[i]->invalidate(dl);
    }
}

void OplogStart::saveState() {
    _txn = NULL;
    if (_cs) {
        _cs->saveState();
    }

    for (size_t i = 0; i < _subIterators.size(); i++) {
        _subIterators[i]->savePositioned();
    }
}

void OplogStart::restoreState(OperationContext* opCtx) {
    invariant(_txn == NULL);
    _txn = opCtx;
    if (_cs) {
        _cs->restoreState(opCtx);
    }

    for (size_t i = 0; i < _subIterators.size(); i++) {
        if (!_subIterators[i]->restore(opCtx)) {
            _subIterators.erase(_subIterators.begin() + i);
            // need to hit same i on next pass through loop
            i--;
        }
    }
}

PlanStageStats* OplogStart::getStats() {
    std::unique_ptr<PlanStageStats> ret(
        new PlanStageStats(CommonStats(kStageType), STAGE_OPLOG_START));
    ret->specific.reset(new CollectionScanStats());
    return ret.release();
}

vector<PlanStage*> OplogStart::getChildren() const {
    vector<PlanStage*> empty;
    return empty;
}

int OplogStart::_backwardsScanTime = 5;

}  // namespace mongo
