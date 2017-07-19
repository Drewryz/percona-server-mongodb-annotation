/**
 *    Copyright 2017 MongoDB Inc.
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

#include "mongo/db/jsobj.h"
#include "mongo/db/repl/rollback_impl_listener.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

/**
 * Unit test for rollback implementation introduced in 3.6.
 */
class RollbackImplListenerTest : public unittest::Test {
private:
    void setUp() override;
    void tearDown() override;

protected:
    /**
     * Generates a document representing an oplog entry with an unrecognized op type.
     * This is used to test cases where we are rolling back operations in the oplog generated by
     * a more recent version of the server.
     */
    BSONObj makeOpWithUnrecognizedOpType();

    /**
     * Generates a document representing an oplog entry that does not contain a collection UUID.
     */
    BSONObj makeOpWithMissingUuidField(char opType, const BSONObj& o);
    BSONObj makeOpWithMissingUuidField(char opType, const BSONObj& o, const BSONObj& o2);

    /**
     * Generates an applyOps oplog entry that contains the same operation as the provided entry.
     */
    BSONObj makeApplyOpsOplogEntry(const BSONObj& oplogEntryObj);

    /**
     * Tests the listener callback function's handling of oplog entries with unrecognized op types.
     */
    using ListenerMemberFn =
        Status (RollbackCommonPointResolver::Listener::*)(const BSONObj& oplogEntryObj);
    void testUnrecognizedOpType(ListenerMemberFn listenerMemberFn);

    /**
     * Tests the listener callback function's handling of missing collection UUIDs in certain oplog
     * entries that support them.
     */
    void testMissingUuidFieldInOplogEntry(ListenerMemberFn listenerMemberFn);

    // Instance of RollbackCommonPointResolver::Listener owned by this test fixture.
    std::unique_ptr<RollbackCommonPointResolver::Listener> _listener;
};

void RollbackImplListenerTest::setUp() {
    _listener = stdx::make_unique<RollbackImpl::Listener>();
}

void RollbackImplListenerTest::tearDown() {
    _listener = {};
}

const NamespaceString nss("test.x");

BSONObj RollbackImplListenerTest::makeOpWithUnrecognizedOpType() {
    return BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL << "op"
                     << "x"
                     << "ns"
                     << nss.ns()
                     << "ui"
                     << UUID::gen().toBSON().firstElement()
                     << "o"
                     << BSON("_id"
                             << "mydocid"
                             << "a"
                             << 1));
}

BSONObj RollbackImplListenerTest::makeOpWithMissingUuidField(char opType, const BSONObj& o) {
    return BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL << "op" << std::string(1U, opType)
                     << "ns"
                     << (opType == 'c' ? nss.getCommandNS() : nss).ns()
                     << "o"
                     << o);
}

BSONObj RollbackImplListenerTest::makeOpWithMissingUuidField(char opType,
                                                             const BSONObj& o,
                                                             const BSONObj& o2) {
    return BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL << "op" << std::string(1U, opType)
                     << "ns"
                     << (opType == 'c' ? nss.getCommandNS() : nss).ns()
                     << "o"
                     << o
                     << "o2"
                     << o2);
}

BSONObj RollbackImplListenerTest::makeApplyOpsOplogEntry(const BSONObj& oplogEntryObj) {
    // Technically, we should strip out the "ts" and "h" fields from 'oplogEntryObj' but this
    // doesn't affect the validation.
    auto result = BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL << "op"
                            << "c"
                            << "ns"
                            << "admin.$cmd"
                            << "o"
                            << BSON("applyOps" << BSON_ARRAY(oplogEntryObj)));
    return result;
}

void RollbackImplListenerTest::testUnrecognizedOpType(ListenerMemberFn listenerMemberFn) {
    auto listenerFn = stdx::bind(listenerMemberFn, _listener.get(), stdx::placeholders::_1);

    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError,
                  listenerFn(makeOpWithUnrecognizedOpType()));
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError,
                  listenerFn(makeApplyOpsOplogEntry(makeOpWithUnrecognizedOpType())));
}

void RollbackImplListenerTest::testMissingUuidFieldInOplogEntry(ListenerMemberFn listenerMemberFn) {
    auto listenerFn = stdx::bind(listenerMemberFn, _listener.get(), stdx::placeholders::_1);

    // Single document operations - insert, update and delete.
    ASSERT_EQUALS(ErrorCodes::IncompatibleRollbackAlgorithm,
                  listenerFn(makeOpWithMissingUuidField('i', BSON("_id" << 0 << "a" << 1))));
    ASSERT_EQUALS(ErrorCodes::IncompatibleRollbackAlgorithm,
                  listenerFn(makeOpWithMissingUuidField(
                      'u', BSON("_id" << 0 << "a" << 1), BSON("_id" << 0))));
    ASSERT_EQUALS(ErrorCodes::IncompatibleRollbackAlgorithm,
                  listenerFn(makeOpWithMissingUuidField('d', BSON("_id" << 0))));

    // Commands - oplog entries for these commands are generated with a "ui" field containing the
    // collection UUID starting in 3.6.
    ASSERT_EQUALS(ErrorCodes::IncompatibleRollbackAlgorithm,
                  listenerFn(makeOpWithMissingUuidField('c', BSON("create" << nss.coll()))));
    ASSERT_EQUALS(ErrorCodes::IncompatibleRollbackAlgorithm,
                  listenerFn(makeOpWithMissingUuidField('c',
                                                        BSON("renameCollection" << nss.ns() << "to"
                                                                                << "test.y"
                                                                                << "stayTemp"
                                                                                << false
                                                                                << "dropTarget"
                                                                                << false))));
    ASSERT_EQUALS(ErrorCodes::IncompatibleRollbackAlgorithm,
                  listenerFn(makeOpWithMissingUuidField('c', BSON("drop" << nss.coll()))));
    ASSERT_EQUALS(
        ErrorCodes::IncompatibleRollbackAlgorithm,
        listenerFn(makeOpWithMissingUuidField('c',
                                              BSON("collMod" << nss.coll() << "validationLevel"
                                                             << "off"),
                                              BSON("collectionOptions" << BSON("validationLevel"
                                                                               << "strict")))));
    ASSERT_EQUALS(ErrorCodes::IncompatibleRollbackAlgorithm,
                  listenerFn(makeOpWithMissingUuidField('c', BSON("emptycapped" << nss.coll()))));
    ASSERT_EQUALS(ErrorCodes::IncompatibleRollbackAlgorithm,
                  listenerFn(makeOpWithMissingUuidField(
                      'c', BSON("convertToCapped" << nss.coll() << "size" << 100000))));
    ASSERT_EQUALS(
        ErrorCodes::IncompatibleRollbackAlgorithm,
        listenerFn(makeOpWithMissingUuidField(
            'c',
            BSON("createIndex" << nss.coll() << "v" << 2 << "key" << BSON("x" << 1) << "name"
                               << "x_1"))));
    ASSERT_EQUALS(
        ErrorCodes::IncompatibleRollbackAlgorithm,
        listenerFn(makeOpWithMissingUuidField('c',
                                              BSON("dropIndexes" << nss.coll() << "index"
                                                                 << "x_1"),
                                              BSON("v" << 2 << "key" << BSON("x" << 1) << "name"
                                                       << "x_1"
                                                       << "ns"
                                                       << nss.ns()))));

    // Oplog entries for these commands/operations do not require/support collection UUIDs as top
    // level fields.
    ASSERT_OK(listenerFn(makeOpWithMissingUuidField('c', BSON("dropDatabase" << 1))));

    // Listener should recurse into operations contained in an applyOps oplog entry.
    ASSERT_EQUALS(ErrorCodes::IncompatibleRollbackAlgorithm,
                  listenerFn(makeApplyOpsOplogEntry(
                      makeOpWithMissingUuidField('i', BSON("_id" << 0 << "a" << 1)))));
    ASSERT_OK(listenerFn(
        makeApplyOpsOplogEntry(makeOpWithMissingUuidField('c', BSON("dropDatabase" << 1)))));
}

TEST_F(RollbackImplListenerTest,
       OnLocalOplogEntryReturnsUnrecoverableRollbackErrorOnUnrecognizedOpType) {
    testUnrecognizedOpType(&RollbackCommonPointResolver::Listener::onLocalOplogEntry);
}

TEST_F(RollbackImplListenerTest,
       OnLocalOplogEntryReturnsIncompatibleRollbackAlgorithmErrorOnMissingUuidFieldInOplogEntry) {
    testMissingUuidFieldInOplogEntry(&RollbackCommonPointResolver::Listener::onLocalOplogEntry);
}

TEST_F(RollbackImplListenerTest,
       OnRemoteOplogEntryReturnsUnrecoverableRollbackErrorOnUnrecognizedOpType) {
    testUnrecognizedOpType(&RollbackCommonPointResolver::Listener::onRemoteOplogEntry);
}

TEST_F(RollbackImplListenerTest,
       OnRemoteOplogEntryReturnsIncompatibleRollbackAlgorithmErrorOnMissingUuidFieldInOplogEntry) {
    testMissingUuidFieldInOplogEntry(&RollbackCommonPointResolver::Listener::onRemoteOplogEntry);
}

}  // namespace
