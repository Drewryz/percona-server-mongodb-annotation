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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */


#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_test_lib.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/stdx/memory.h"

namespace mongo {

// How we access the external setParameter testing bool.
extern bool internalQueryForceIntersectionPlans;

}  // namespace mongo

namespace QueryMultiPlanRunner {

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

static const NamespaceString nss("unittests.QueryStageMultiPlanRunner");

/**
 * Create query solution.
 */
QuerySolution* createQuerySolution() {
    unique_ptr<QuerySolution> soln(new QuerySolution());
    soln->cacheData.reset(new SolutionCacheData());
    soln->cacheData->solnType = SolutionCacheData::COLLSCAN_SOLN;
    soln->cacheData->tree.reset(new PlanCacheIndexTree());
    return soln.release();
}

class MultiPlanRunnerBase {
public:
    MultiPlanRunnerBase() : _client(&_txn) {
        OldClientWriteContext ctx(&_txn, nss.ns());
        _client.dropCollection(nss.ns());
    }

    virtual ~MultiPlanRunnerBase() {
        OldClientWriteContext ctx(&_txn, nss.ns());
        _client.dropCollection(nss.ns());
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(&_txn, nss.ns(), obj));
    }

    void insert(const BSONObj& obj) {
        OldClientWriteContext ctx(&_txn, nss.ns());
        _client.insert(nss.ns(), obj);
    }

    void remove(const BSONObj& obj) {
        OldClientWriteContext ctx(&_txn, nss.ns());
        _client.remove(nss.ns(), obj);
    }

protected:
    OperationContextImpl _txn;
    DBDirectClient _client;
};


// Basic ranking test: collection scan vs. highly selective index scan.  Make sure we also get
// all expected results out as well.
class MPRCollectionScanVsHighlySelectiveIXScan : public MultiPlanRunnerBase {
public:
    void run() {
        const int N = 5000;
        for (int i = 0; i < N; ++i) {
            insert(BSON("foo" << (i % 10)));
        }

        addIndex(BSON("foo" << 1));

        AutoGetCollectionForRead ctx(&_txn, nss.ns());
        const Collection* coll = ctx.getCollection();

        // Plan 0: IXScan over foo == 7
        // Every call to work() returns something so this should clearly win (by current scoring
        // at least).
        IndexScanParams ixparams;
        ixparams.descriptor =
            coll->getIndexCatalog()->findIndexByKeyPattern(&_txn, BSON("foo" << 1));
        ixparams.bounds.isSimpleRange = true;
        ixparams.bounds.startKey = BSON("" << 7);
        ixparams.bounds.endKey = BSON("" << 7);
        ixparams.bounds.endKeyInclusive = true;
        ixparams.direction = 1;

        unique_ptr<WorkingSet> sharedWs(new WorkingSet());
        IndexScan* ix = new IndexScan(&_txn, ixparams, sharedWs.get(), NULL);
        unique_ptr<PlanStage> firstRoot(new FetchStage(&_txn, sharedWs.get(), ix, NULL, coll));

        // Plan 1: CollScan with matcher.
        CollectionScanParams csparams;
        csparams.collection = coll;
        csparams.direction = CollectionScanParams::FORWARD;

        // Make the filter.
        BSONObj filterObj = BSON("foo" << 7);
        StatusWithMatchExpression statusWithMatcher = MatchExpressionParser::parse(filterObj);
        verify(statusWithMatcher.isOK());
        unique_ptr<MatchExpression> filter = std::move(statusWithMatcher.getValue());
        // Make the stage.
        unique_ptr<PlanStage> secondRoot(
            new CollectionScan(&_txn, csparams, sharedWs.get(), filter.get()));

        // Hand the plans off to the runner.
        auto statusWithCQ = CanonicalQuery::canonicalize(nss, BSON("foo" << 7));
        verify(statusWithCQ.isOK());
        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        verify(NULL != cq.get());

        unique_ptr<MultiPlanStage> mps =
            make_unique<MultiPlanStage>(&_txn, ctx.getCollection(), cq.get());
        mps->addPlan(createQuerySolution(), firstRoot.release(), sharedWs.get());
        mps->addPlan(createQuerySolution(), secondRoot.release(), sharedWs.get());

        // Plan 0 aka the first plan aka the index scan should be the best.
        PlanYieldPolicy yieldPolicy(NULL, PlanExecutor::YIELD_MANUAL);
        mps->pickBestPlan(&yieldPolicy);
        ASSERT(mps->bestPlanChosen());
        ASSERT_EQUALS(0, mps->bestPlanIdx());

        // Takes ownership of arguments other than 'collection'.
        auto statusWithPlanExecutor = PlanExecutor::make(&_txn,
                                                         std::move(sharedWs),
                                                         std::move(mps),
                                                         std::move(cq),
                                                         coll,
                                                         PlanExecutor::YIELD_MANUAL);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        std::unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        // Get all our results out.
        int results = 0;
        BSONObj obj;
        while (PlanExecutor::ADVANCED == exec->getNext(&obj, NULL)) {
            ASSERT_EQUALS(obj["foo"].numberInt(), 7);
            ++results;
        }

        ASSERT_EQUALS(results, N / 10);
    }
};

// Case in which we select a blocking plan as the winner, and a non-blocking plan
// is available as a backup.
class MPRBackupPlan : public MultiPlanRunnerBase {
public:
    void run() {
        // Data is just a single {_id: 1, a: 1, b: 1} document.
        insert(BSON("_id" << 1 << "a" << 1 << "b" << 1));

        // Indices on 'a' and 'b'.
        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));

        AutoGetCollectionForRead ctx(&_txn, nss.ns());
        Collection* collection = ctx.getCollection();

        // Query for both 'a' and 'b' and sort on 'b'.
        auto statusWithCQ = CanonicalQuery::canonicalize(nss,
                                                         BSON("a" << 1 << "b" << 1),  // query
                                                         BSON("b" << 1),              // sort
                                                         BSONObj());                  // proj
        verify(statusWithCQ.isOK());
        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(NULL != cq.get());

        // Force index intersection.
        bool forceIxisectOldValue = internalQueryForceIntersectionPlans;
        internalQueryForceIntersectionPlans = true;

        // Get planner params.
        QueryPlannerParams plannerParams;
        fillOutPlannerParams(&_txn, collection, cq.get(), &plannerParams);
        // Turn this off otherwise it pops up in some plans.
        plannerParams.options &= ~QueryPlannerParams::KEEP_MUTATIONS;

        // Plan.
        vector<QuerySolution*> solutions;
        Status status = QueryPlanner::plan(*cq, plannerParams, &solutions);
        ASSERT(status.isOK());

        // We expect a plan using index {a: 1} and plan using index {b: 1} and
        // an index intersection plan.
        ASSERT_EQUALS(solutions.size(), 3U);

        // Fill out the MultiPlanStage.
        unique_ptr<MultiPlanStage> mps(new MultiPlanStage(&_txn, collection, cq.get()));
        unique_ptr<WorkingSet> ws(new WorkingSet());
        // Put each solution from the planner into the MPR.
        for (size_t i = 0; i < solutions.size(); ++i) {
            PlanStage* root;
            ASSERT(StageBuilder::build(&_txn, collection, *solutions[i], ws.get(), &root));
            // Takes ownership of 'solutions[i]' and 'root'.
            mps->addPlan(solutions[i], root, ws.get());
        }

        // This sets a backup plan.
        PlanYieldPolicy yieldPolicy(NULL, PlanExecutor::YIELD_MANUAL);
        mps->pickBestPlan(&yieldPolicy);
        ASSERT(mps->bestPlanChosen());
        ASSERT(mps->hasBackupPlan());

        // We should have picked the index intersection plan due to forcing ixisect.
        QuerySolution* soln = mps->bestSolution();
        ASSERT(QueryPlannerTestLib::solutionMatches(
            "{sort: {pattern: {b: 1}, limit: 0, node: "
            "{fetch: {node: {andSorted: {nodes: ["
            "{ixscan: {filter: null, pattern: {a:1}}},"
            "{ixscan: {filter: null, pattern: {b:1}}}]}}}}}}",
            soln->root.get()));

        // Get the resulting document.
        PlanStage::StageState state = PlanStage::NEED_TIME;
        WorkingSetID wsid;
        while (state != PlanStage::ADVANCED) {
            state = mps->work(&wsid);
        }
        WorkingSetMember* member = ws->get(wsid);

        // Check the document returned by the query.
        ASSERT(member->hasObj());
        BSONObj expectedDoc = BSON("_id" << 1 << "a" << 1 << "b" << 1);
        ASSERT(expectedDoc.woCompare(member->obj.value()) == 0);

        // The blocking plan became unblocked, so we should no longer have a backup plan,
        // and the winning plan should still be the index intersection one.
        ASSERT(!mps->hasBackupPlan());
        soln = mps->bestSolution();
        ASSERT(QueryPlannerTestLib::solutionMatches(
            "{sort: {pattern: {b: 1}, limit: 0, node: "
            "{fetch: {node: {andSorted: {nodes: ["
            "{ixscan: {filter: null, pattern: {a:1}}},"
            "{ixscan: {filter: null, pattern: {b:1}}}]}}}}}}",
            soln->root.get()));

        // Restore index intersection force parameter.
        internalQueryForceIntersectionPlans = forceIxisectOldValue;
    }
};

class All : public Suite {
public:
    All() : Suite("query_multi_plan_runner") {}

    void setupTests() {
        add<MPRCollectionScanVsHighlySelectiveIXScan>();
        add<MPRBackupPlan>();
    }
};

SuiteInstance<All> queryMultiPlanRunnerAll;

}  // namespace QueryMultiPlanRunner
