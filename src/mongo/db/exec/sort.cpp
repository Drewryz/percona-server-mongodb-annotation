/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/exec/sort.h"

#include <algorithm>

#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/db/query/query_planner.h"

namespace {

    using mongo::DiskLoc;
    using mongo::WorkingSet;
    using mongo::WorkingSetID;
    using mongo::WorkingSetMember;

    /**
     * Returns expected memory usage of working set member
     */
    size_t getMemUsage(WorkingSet* ws, WorkingSetID wsid) {
        WorkingSetMember* member = ws->get(wsid);
        size_t memUsage = sizeof(DiskLoc) + member->obj.objsize();
        return memUsage;
    }

} // namespace

namespace mongo {

    using std::vector;

    const size_t kMaxBytes = 32 * 1024 * 1024;

    SortStageKeyGenerator::SortStageKeyGenerator(const BSONObj& sortSpec, const BSONObj& queryObj) {
        _hasBounds = false;
        _sortHasMeta = false;
        _rawSortSpec = sortSpec;

        // 'sortSpec' can be a mix of $meta and index key expressions.  We pick it apart so that
        // we only generate Btree keys for the index key expressions.

        // The Btree key fields go in here.  We pass this fake index key pattern to the Btree
        // key generator below as part of generating sort keys for the docs.
        BSONObjBuilder btreeBob;

        // The pattern we use to woCompare keys.  Each field in 'sortSpec' will go in here with
        // a value of 1 or -1.  The Btree key fields are verbatim, meta fields have a default.
        BSONObjBuilder comparatorBob;

        BSONObjIterator it(sortSpec);
        while (it.more()) {
            BSONElement elt = it.next();
            if (elt.isNumber()) {
                // Btree key.  elt (should be) foo: 1 or foo: -1.
                comparatorBob.append(elt);
                btreeBob.append(elt);
            }
            else if (LiteParsedQuery::isTextScoreMeta(elt)) {
                // Sort text score decreasing by default.  Field name doesn't matter but we choose
                // something that a user shouldn't ever have.
                comparatorBob.append("$metaTextScore", -1);
                _sortHasMeta = true;
            }
            else {
                // Sort spec. should have been validated before here.
                verify(false);
            }
        }

        // Our pattern for woComparing keys.
        _comparatorObj = comparatorBob.obj();

        // The fake index key pattern used to generate Btree keys.
        _btreeObj = btreeBob.obj();

        // If we're just sorting by meta, don't bother with all the key stuff.
        if (_btreeObj.isEmpty()) {
            return;
        }

        // We'll need to treat arrays as if we were to create an index over them. that is,
        // we may need to unnest the first level and consider each array element to decide
        // the sort order.
        std::vector<const char *> fieldNames;
        std::vector<BSONElement> fixed;
        BSONObjIterator btreeIt(_btreeObj);
        while (btreeIt.more()) {
            BSONElement patternElt = btreeIt.next();
            fieldNames.push_back(patternElt.fieldName());
            fixed.push_back(BSONElement());
        }

        _keyGen.reset(new BtreeKeyGeneratorV1(fieldNames, fixed, false /* not sparse */));

        // The bounds checker only works on the Btree part of the sort key.
        getBoundsForSort(queryObj, _btreeObj);

        if (_hasBounds) {
            _boundsChecker.reset(new IndexBoundsChecker(&_bounds, _btreeObj, 1 /* == order */));
        }
    }

    BSONObj SortStageKeyGenerator::getSortKey(const WorkingSetMember& member) const {
        BSONObj btreeKeyToUse = getBtreeKey(member.obj);

        if (!_sortHasMeta) {
            return btreeKeyToUse;
        }

        BSONObjBuilder mergedKeyBob;

        // Merge metadata into the key.
        BSONObjIterator it(_rawSortSpec);
        BSONObjIterator btreeIt(btreeKeyToUse);
        while (it.more()) {
            BSONElement elt = it.next();
            if (elt.isNumber()) {
                // Merge btree key elt.
                mergedKeyBob.append(btreeIt.next());
            }
            else if (LiteParsedQuery::isTextScoreMeta(elt)) {
                // Add text score metadata
                double score = 0.0;
                if (member.hasComputed(WSM_COMPUTED_TEXT_SCORE)) {
                    const TextScoreComputedData* scoreData
                        = static_cast<const TextScoreComputedData*>(
                                member.getComputed(WSM_COMPUTED_TEXT_SCORE));
                    score = scoreData->getScore();
                }
                mergedKeyBob.append("$metaTextScore", score);
            }
        }

        return mergedKeyBob.obj();
    }

    BSONObj SortStageKeyGenerator::getBtreeKey(const BSONObj& memberObj) const {
        if (_btreeObj.isEmpty()) {
            return BSONObj();
        }

        // We will sort '_data' in the same order an index over '_pattern' would have.  This is
        // tricky.  Consider the sort pattern {a:1} and the document {a:[1, 10]}. We have
        // potentially two keys we could use to sort on. Here we extract these keys.
        BSONObjCmp patternCmp(_btreeObj);
        BSONObjSet keys(patternCmp);

        // keyGen can throw on a "parallel array."  Previously we'd error out of sort.
        // For now we just accept the doc verbatim.  TODO: Do we want to error?
        try {
            _keyGen->getKeys(memberObj, &keys);
        }
        catch (...) {
            return memberObj;
        }

        if (keys.empty()) {
            // TODO: will this ever happen?  don't think it should.
            return memberObj;
        }

        // No bounds?  No problem!  Use the first key.
        if (!_hasBounds) {
            // Note that we sort 'keys' according to the pattern '_btreeObj'.
            return *keys.begin();
        }

        // To decide which key to use in sorting, we must consider not only the sort pattern but
        // the query.  Assume we have the query {a: {$gte: 5}} and a document {a:1}.  That
        // document wouldn't match the query.  As such, the key '1' in an array {a: [1, 10]}
        // should not be considered as being part of the result set and thus that array cannot
        // sort using the key '1'.  To ensure that the keys we sort by are valid w.r.t. the
        // query we use a bounds checker.
        verify(NULL != _boundsChecker.get());
        for (BSONObjSet::const_iterator it = keys.begin(); it != keys.end(); ++it) {
            if (_boundsChecker->isValidKey(*it)) {
                return *it;
            }
        }

        // No key in our bounds.
        // TODO: will this ever happen?  don't think it should.
        return *keys.begin();
    }

    void SortStageKeyGenerator::getBoundsForSort(const BSONObj& queryObj, const BSONObj& sortObj) {
        QueryPlannerParams params;
        params.options = QueryPlannerParams::NO_TABLE_SCAN;

        // We're creating a "virtual index" with key pattern equal to the sort order.
        IndexEntry sortOrder(sortObj, true, false, "doesnt_matter", BSONObj());
        params.indices.push_back(sortOrder);

        CanonicalQuery* rawQueryForSort;
        verify(CanonicalQuery::canonicalize("fake_ns",
                                            queryObj,
                                            &rawQueryForSort).isOK());
        auto_ptr<CanonicalQuery> queryForSort(rawQueryForSort);

        vector<QuerySolution*> solns;
        QueryPlanner::plan(*queryForSort, params, &solns);

        // TODO: are there ever > 1 solns?  If so, do we look for a specific soln?
        if (1 == solns.size()) {
            IndexScanNode* ixScan = NULL;
            QuerySolutionNode* rootNode = solns[0]->root.get();

            if (rootNode->getType() == STAGE_FETCH) {
                FetchNode* fetchNode = static_cast<FetchNode*>(rootNode);
                if (fetchNode->children[0]->getType() != STAGE_IXSCAN) {
                    delete solns[0];
                    // No bounds.
                    return;
                }
                ixScan = static_cast<IndexScanNode*>(fetchNode->children[0]);
            }
            else if (rootNode->getType() == STAGE_IXSCAN) {
                ixScan = static_cast<IndexScanNode*>(rootNode);
            }

            if (ixScan) {
                _bounds.fields.swap(ixScan->bounds.fields);
                _hasBounds = true;
            }
        }

        for (size_t i = 0; i < solns.size(); ++i) {
            delete solns[i];
        }
    }

    SortStage::WorkingSetComparator::WorkingSetComparator(BSONObj p) : pattern(p) { }

    bool SortStage::WorkingSetComparator::operator()(const SortableDataItem& lhs, const SortableDataItem& rhs) const {
        // False means ignore field names.
        int result = lhs.sortKey.woCompare(rhs.sortKey, pattern, false);
        if (0 != result) {
            return result < 0;
        }
        // Indices use DiskLoc as an additional sort key so we must as well.
        return lhs.loc < rhs.loc;
    }

    SortStage::SortStage(const SortStageParams& params, WorkingSet* ws, PlanStage* child)
        : _ws(ws),
          _child(child),
          _pattern(params.pattern),
          _query(params.query),
          _limit(params.limit),
          _sorted(false),
          _resultIterator(_data.end()),
          _memUsage(0) {
        dassert(_limit >= 0);
    }

    SortStage::~SortStage() { }

    bool SortStage::isEOF() {
        // We're done when our child has no more results, we've sorted the child's results, and
        // we've returned all sorted results.
        return _child->isEOF() && _sorted && (_data.end() == _resultIterator);
    }

    PlanStage::StageState SortStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        if (NULL == _sortKeyGen) {
            // This is heavy and should be done as part of work().
            _sortKeyGen.reset(new SortStageKeyGenerator(_pattern, _query));
            _sortKeyComparator.reset(new WorkingSetComparator(_sortKeyGen->getSortComparator()));
            // If limit > 1, we need to initialize _dataSet here to maintain ordered
            // set of data items while fetching from the child stage.
            if (_limit > 1) {
                const WorkingSetComparator& cmp = *_sortKeyComparator;
                _dataSet.reset(new SortableDataItemSet(cmp));
            }
            return PlanStage::NEED_TIME;
        }

        if (_memUsage > kMaxBytes) {
            return PlanStage::FAILURE;
        }

        if (isEOF()) { return PlanStage::IS_EOF; }

        // Still reading in results to sort.
        if (!_sorted) {
            WorkingSetID id;
            StageState code = _child->work(&id);

            if (PlanStage::ADVANCED == code) {
                // Add it into the map for quick invalidation if it has a valid DiskLoc.
                // A DiskLoc may be invalidated at any time (during a yield).  We need to get into
                // the WorkingSet as quickly as possible to handle it.
                WorkingSetMember* member = _ws->get(id);

                // Planner must put a fetch before we get here.
                verify(member->hasObj());

                // TODO: This should always be true...?
                if (member->hasLoc()) {
                    _wsidByDiskLoc[member->loc] = id;
                }


                // The data remains in the WorkingSet and we wrap the WSID with the sort key.
                SortableDataItem item;
                item.sortKey = _sortKeyGen->getSortKey(*member);
                item.wsid = id;
                if (member->hasLoc()) {
                    // The DiskLoc breaks ties when sorting two WSMs with the same sort key.
                    item.loc = member->loc;
                }

                addToBuffer(item);

                ++_commonStats.needTime;
                return PlanStage::NEED_TIME;
            }
            else if (PlanStage::IS_EOF == code) {
                // TODO: We don't need the lock for this.  We could ask for a yield and do this work
                // unlocked.  Also, this is performing a lot of work for one call to work(...)
                sortBuffer();
                _resultIterator = _data.begin();
                _sorted = true;
                ++_commonStats.needTime;
                return PlanStage::NEED_TIME;
            }
            else {
                if (PlanStage::NEED_FETCH == code) {
                    *out = id;
                    ++_commonStats.needFetch;
                }
                else if (PlanStage::NEED_TIME == code) {
                    ++_commonStats.needTime;
                }
                return code;
            }
        }

        // Returning results.
        verify(_resultIterator != _data.end());
        verify(_sorted);
        *out = _resultIterator->wsid;
        _resultIterator++;

        // If we're returning something, take it out of our DL -> WSID map so that future
        // calls to invalidate don't cause us to take action for a DL we're done with.
        WorkingSetMember* member = _ws->get(*out);
        if (member->hasLoc()) {
            _wsidByDiskLoc.erase(member->loc);
        }

        // If it was flagged, we just drop it on the floor, assuming the caller wants a DiskLoc.  We
        // could make this triggerable somehow.
        if (_ws->isFlagged(*out)) {
            _ws->free(*out);
            return PlanStage::NEED_TIME;
        }

        ++_commonStats.advanced;
        return PlanStage::ADVANCED;
    }

    void SortStage::prepareToYield() {
        ++_commonStats.yields;
        _child->prepareToYield();
    }

    void SortStage::recoverFromYield() {
        ++_commonStats.unyields;
        _child->recoverFromYield();
    }

    void SortStage::invalidate(const DiskLoc& dl) {
        ++_commonStats.invalidates;
        _child->invalidate(dl);

        // _data contains indices into the WorkingSet, not actual data.  If a WorkingSetMember in
        // the WorkingSet needs to change state as a result of a DiskLoc invalidation, it will still
        // be at the same spot in the WorkingSet.  As such, we don't need to modify _data.
        DataMap::iterator it = _wsidByDiskLoc.find(dl);

        // If we're holding on to data that's got the DiskLoc we're invalidating...
        if (_wsidByDiskLoc.end() != it) {
            // Grab the WSM that we're nuking.
            WorkingSetMember* member = _ws->get(it->second);
            verify(member->loc == dl);

            // Fetch, invalidate, and flag.
            WorkingSetCommon::fetchAndInvalidateLoc(member);
            _ws->flagForReview(it->second);

            // Remove the DiskLoc from our set of active DLs.
            _wsidByDiskLoc.erase(it);
            ++_specificStats.forcedFetches;
        }
    }

    PlanStageStats* SortStage::getStats() {
        _commonStats.isEOF = isEOF();

        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_SORT));
        ret->specific.reset(new SortStats(_specificStats));
        ret->children.push_back(_child->getStats());
        return ret.release();
    }

    /**
     * addToBuffer() and sortBuffer() work differently based on the
     * configured limit. addToBuffer() is also responsible for
     * performing some accounting on the overall memory usage to
     * make sure we're not using too much memory.
     *
     * limit == 0:
     *     addToBuffer() - Adds item to vector.
     *     sortBuffer() - Sorts vector.
     * limit == 1:
     *     addToBuffer() - Replaces first item in vector with max of
     *                     current and new item.
     *                     Updates memory usage if item was replaced.
     *     sortBuffer() - Does nothing.
     * limit > 1:
     *     addToBuffer() - Does not update vector. Adds item to set.
     *                     If size of set exceeds limit, remove item from set
     *                     with lowest key. Updates memory usage accordingly.
     *     sortBuffer() - Copies items from set to vectors.
     */
    void SortStage::addToBuffer(const SortableDataItem& item) {
        // Holds ID of working set member to be freed at end of this function.
        WorkingSetID wsidToFree = WorkingSet::INVALID_ID;

        if (_limit == 0) {
            _data.push_back(item);
            _memUsage += getMemUsage(_ws, item.wsid);
        }
        else if (_limit == 1) {
            if (_data.empty()) {
                _data.push_back(item);
                _memUsage = getMemUsage(_ws, item.wsid);
                return;
            }
            wsidToFree = item.wsid;
            const WorkingSetComparator& cmp = *_sortKeyComparator;
            // Compare new item with existing item in vector.
            if (cmp(item, _data[0])) {
                wsidToFree = _data[0].wsid;
                _data[0] = item;
                _memUsage = getMemUsage(_ws, item.wsid);
            }
        }
        else {
            // Update data item set instead of vector
            // Limit not reached - insert and return
            vector<SortableDataItem>::size_type limit(_limit);
            if (_dataSet->size() < limit) {
                _dataSet->insert(item);
                _memUsage += getMemUsage(_ws, item.wsid);
                return;
            }
            // Limit will be exceeded - compare with item with lowest key
            // If new item does not have a lower key value than last item,
            // do nothing.
            wsidToFree = item.wsid;
            SortableDataItemSet::const_iterator lastItemIt = --(_dataSet->end());
            const SortableDataItem& lastItem = *lastItemIt;
            const WorkingSetComparator& cmp = *_sortKeyComparator;
            if (cmp(item, lastItem)) {
                _memUsage += getMemUsage(_ws, item.wsid) - getMemUsage(_ws, lastItem.wsid);
                wsidToFree = lastItem.wsid;
                // According to std::set iterator validity rules,
                // it does not matter which of erase()/insert() happens first.
                // Here, we choose to erase first to release potential resources
                // used by the last item and to keep the scope of the iterator to a minimum.
                _dataSet->erase(lastItemIt);
                _dataSet->insert(item);
            }
        }

        // If the working set ID is valid, remove from
        // DiskLoc invalidation map and free from working set.
        if (wsidToFree != WorkingSet::INVALID_ID) {
            WorkingSetMember* member = _ws->get(wsidToFree);
            if (member->hasLoc()) {
                _wsidByDiskLoc.erase(member->loc);
            }
            _ws->free(wsidToFree);
        }
    }

    void SortStage::sortBuffer() {
        if (_limit == 0) {
            const WorkingSetComparator& cmp = *_sortKeyComparator;
            std::sort(_data.begin(), _data.end(), cmp);
        }
        else if (_limit == 1) {
            // Buffer contains either 0 or 1 item so it is already in a sorted state.
            return;
        }
        else {
            // Set already contains items in sorted order, so we simply copy the items
            // from the set to the vector.
            // Release the memory for the set after the copy.
            vector<SortableDataItem> newData(_dataSet->begin(), _dataSet->end());
            _data.swap(newData);
            _dataSet.reset();
        }
    }

}  // namespace mongo
