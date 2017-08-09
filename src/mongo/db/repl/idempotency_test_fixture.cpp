/**
 *    Copyright 2017 (C) MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/idempotency_test_fixture.h"

#include <string>
#include <utility>
#include <vector>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/util/md5.hpp"

namespace mongo {
namespace repl {

/**
 * Compares BSON objects (BSONObj) in two sets of BSON objects (BSONObjSet) to see if the two
 * sets are equivalent.
 *
 * Two sets are equivalent if and only if their sizes are the same and all of their elements
 * that share the same index position are also equivalent in value.
 */
bool CollectionState::cmpIndexSpecs(const BSONObjSet& otherSpecs) const {
    if (indexSpecs.size() != otherSpecs.size()) {
        return false;
    }

    auto thisIt = this->indexSpecs.begin();
    auto otherIt = otherSpecs.begin();

    // thisIt and otherIt cannot possibly be out of sync in terms of progression through
    // their respective sets because we ensured earlier that their sizes are equal and we
    // increment both by 1 on each iteration. We can avoid checking both iterator positions and
    // only check one (thisIt).
    for (; thisIt != this->indexSpecs.end(); ++thisIt, ++otherIt) {
        // Since these are ordered sets, we expect that in the case of equivalent index specs,
        // each copy will be in the same order in both sets, therefore each loop step should be
        // true.

        if (!thisIt->binaryEqual(*otherIt)) {
            return false;
        }
    }

    return true;
}

/**
 * Returns a std::string representation of the CollectionState struct of which this is a member
 * function. Returns out its representation in the form:
 *
 * Collection options: {...}; Index options: [...]; MD5 hash: <md5 digest string>
 */
std::string CollectionState::toString() const {
    if (!this->exists) {
        return "Collection does not exist.";
    }

    BSONObj collectionOptionsBSON = this->collectionOptions.toBSON();
    StringBuilder sb;
    sb << "Collection options: " << collectionOptionsBSON.toString() << "; ";

    sb << "Index specs: [ ";
    bool firstIter = true;
    for (auto indexSpec : this->indexSpecs) {
        if (!firstIter) {
            sb << ", ";
        } else {
            firstIter = false;
        }
        sb << indexSpec.toString();
    }
    sb << " ]; ";

    sb << "MD5 Hash: ";
    // Be more explicit about CollectionState structs without a supplied MD5 hash string.
    sb << (this->dataHash.length() != 0 ? this->dataHash : "No hash");
    return sb.str();
}

CollectionState::CollectionState(CollectionOptions collectionOptions_,
                                 BSONObjSet indexSpecs_,
                                 std::string dataHash_)
    : collectionOptions(std::move(collectionOptions_)),
      indexSpecs(std::move(indexSpecs_)),
      dataHash(std::move(dataHash_)),
      exists(true){};

bool operator==(const CollectionState& lhs, const CollectionState& rhs) {
    if (!lhs.exists || !rhs.exists) {
        return lhs.exists == rhs.exists;
    }

    BSONObj lhsCollectionOptionsBSON = lhs.collectionOptions.toBSON();
    BSONObj rhsCollectionOptionsBSON = rhs.collectionOptions.toBSON();
    // Since collection options uses deferred comparison, we opt to binary compare its BSON
    // representations.
    bool collectionOptionsEqual = lhsCollectionOptionsBSON.binaryEqual(rhsCollectionOptionsBSON);
    bool indexSpecsEqual = lhs.cmpIndexSpecs(rhs.indexSpecs);
    bool dataHashEqual = lhs.dataHash == rhs.dataHash;
    bool existsEqual = lhs.exists == rhs.exists;
    return collectionOptionsEqual && indexSpecsEqual && dataHashEqual && existsEqual;
}

std::ostream& operator<<(std::ostream& stream, const CollectionState& state) {
    return stream << state.toString();
}

const auto kCollectionDoesNotExist = CollectionState();

/**
 * Creates a command oplog entry with given optime and namespace.
 */
OplogEntry makeCommandOplogEntry(OpTime opTime,
                                 const NamespaceString& nss,
                                 const BSONObj& command) {
    return OplogEntry(opTime, 1LL, OpTypeEnum::kCommand, nss.getCommandNS(), 2, command);
}

/**
 * Creates a create collection oplog entry with given optime.
 */
OplogEntry makeCreateCollectionOplogEntry(OpTime opTime,
                                          const NamespaceString& nss,
                                          const BSONObj& options) {
    BSONObjBuilder bob;
    bob.append("create", nss.coll());
    bob.appendElements(options);
    return makeCommandOplogEntry(opTime, nss, bob.obj());
}

/**
 * Creates an insert oplog entry with given optime and namespace.
 */
OplogEntry makeInsertDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToInsert) {
    return OplogEntry(opTime, 1LL, OpTypeEnum::kInsert, nss, documentToInsert);
}

/**
 * Creates an update oplog entry with given optime and namespace.
 */
OplogEntry makeUpdateDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToUpdate,
                                        const BSONObj& updatedDocument) {
    return OplogEntry(opTime, 1LL, OpTypeEnum::kUpdate, nss, updatedDocument, documentToUpdate);
}

/**
 * Creates an index creation entry with given optime and namespace.
 */
OplogEntry makeCreateIndexOplogEntry(OpTime opTime,
                                     const NamespaceString& nss,
                                     const std::string& indexName,
                                     const BSONObj& keyPattern) {
    BSONObjBuilder indexInfoBob;
    indexInfoBob.append("v", 2);
    indexInfoBob.append("key", keyPattern);
    indexInfoBob.append("name", indexName);
    indexInfoBob.append("ns", nss.ns());
    return makeInsertDocumentOplogEntry(
        opTime, NamespaceString(nss.getSystemIndexesCollection()), indexInfoBob.obj());
}

void appendSessionTransactionInfo(OplogEntry& entry,
                                  LogicalSessionId lsid,
                                  TxnNumber txnNum,
                                  StmtId stmtId) {
    auto info = entry.getOperationSessionInfo();
    info.setSessionId(lsid);
    info.setTxnNumber(txnNum);
    entry.setOperationSessionInfo(std::move(info));
    entry.setStatementId(stmtId);
}

Status IdempotencyTest::runOp(const OplogEntry& op) {
    return runOps({op});
}

Status IdempotencyTest::runOps(std::initializer_list<OplogEntry> ops) {
    SyncTail syncTail(nullptr, SyncTail::MultiSyncApplyFunc(), nullptr);
    MultiApplier::OperationPtrs opsPtrs;
    for (auto& op : ops) {
        opsPtrs.push_back(&op);
    }
    AtomicUInt32 fetchCount(0);
    return multiInitialSyncApply_noAbort(_opCtx.get(), &opsPtrs, &syncTail, &fetchCount);
}

void IdempotencyTest::testOpsAreIdempotent(std::initializer_list<OplogEntry> ops) {
    ASSERT_OK(runOps(ops));
    auto state = validate();
    ASSERT_OK(runOps(ops));
    ASSERT_EQUALS(state, validate());
}

OplogEntry IdempotencyTest::createCollection(CollectionUUID uuid) {
    return makeCreateCollectionOplogEntry(nextOpTime(), nss, BSON("uuid" << uuid));
}

OplogEntry IdempotencyTest::insert(const BSONObj& obj) {
    return makeInsertDocumentOplogEntry(nextOpTime(), nss, obj);
}

template <class IdType>
OplogEntry IdempotencyTest::update(IdType _id, const BSONObj& obj) {
    return makeUpdateDocumentOplogEntry(nextOpTime(), nss, BSON("_id" << _id), obj);
}

OplogEntry IdempotencyTest::buildIndex(const BSONObj& indexSpec, const BSONObj& options) {
    BSONObjBuilder bob;
    bob.append("v", 2);
    bob.append("key", indexSpec);
    bob.append("name", std::string(indexSpec.firstElementFieldName()) + "_index");
    bob.append("ns", nss.ns());
    bob.appendElementsUnique(options);
    return makeInsertDocumentOplogEntry(nextOpTime(), nssIndex, bob.obj());
}

OplogEntry IdempotencyTest::dropIndex(const std::string& indexName) {
    auto cmd = BSON("deleteIndexes" << nss.coll() << "index" << indexName);
    return makeCommandOplogEntry(nextOpTime(), nss, cmd);
}

CollectionState IdempotencyTest::validate() {
    AutoGetCollectionForReadCommand autoColl(_opCtx.get(), nss);
    auto collection = autoColl.getCollection();

    if (!collection) {
        // Return a mostly default initialized CollectionState struct with exists set to false to
        // indicate an unfound Collection (or a view).
        return kCollectionDoesNotExist;
    }
    ValidateResults validateResults;
    BSONObjBuilder bob;

    Lock::DBLock lk(_opCtx.get(), nss.db(), MODE_IX);
    auto lock = stdx::make_unique<Lock::CollectionLock>(_opCtx->lockState(), nss.ns(), MODE_X);
    ASSERT_OK(collection->validate(
        _opCtx.get(), kValidateFull, false, std::move(lock), &validateResults, &bob));
    ASSERT_TRUE(validateResults.valid);

    IndexDescriptor* desc = collection->getIndexCatalog()->findIdIndex(_opCtx.get());
    ASSERT_TRUE(desc);
    auto exec = InternalPlanner::indexScan(_opCtx.get(),
                                           collection,
                                           desc,
                                           BSONObj(),
                                           BSONObj(),
                                           BoundInclusion::kIncludeStartKeyOnly,
                                           PlanExecutor::NO_YIELD,
                                           InternalPlanner::FORWARD,
                                           InternalPlanner::IXSCAN_FETCH);
    ASSERT(NULL != exec.get());
    md5_state_t st;
    md5_init(&st);

    PlanExecutor::ExecState state;
    BSONObj c;
    while (PlanExecutor::ADVANCED == (state = exec->getNext(&c, NULL))) {
        md5_append(&st, (const md5_byte_t*)c.objdata(), c.objsize());
    }
    ASSERT_EQUALS(PlanExecutor::IS_EOF, state);
    md5digest d;
    md5_finish(&st, d);
    std::string dataHash = digestToString(d);

    auto collectionCatalog = collection->getCatalogEntry();
    auto collectionOptions = collectionCatalog->getCollectionOptions(_opCtx.get());
    std::vector<std::string> allIndexes;
    BSONObjSet indexSpecs = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    collectionCatalog->getAllIndexes(_opCtx.get(), &allIndexes);
    for (auto const& index : allIndexes) {
        indexSpecs.insert(collectionCatalog->getIndexSpec(_opCtx.get(), index));
    }
    ASSERT_EQUALS(indexSpecs.size(), allIndexes.size());

    CollectionState collectionState(collectionOptions, indexSpecs, dataHash);

    return collectionState;
}

template OplogEntry IdempotencyTest::update<int>(int _id, const BSONObj& obj);
template OplogEntry IdempotencyTest::update<const char*>(char const* _id, const BSONObj& obj);

}  // namespace repl
}  // namespace mongo
