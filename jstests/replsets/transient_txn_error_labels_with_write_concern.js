// Test TransientTransactionError error label for commands in transactions with write concern.
// @tags: [uses_transactions]
(function() {
    "use strict";

    load("jstests/libs/check_log.js");
    load("jstests/libs/write_concern_util.js");
    load("jstests/replsets/rslib.js");

    const dbName = "test";
    const collName = "transient_txn_error_labels_with_write_concern";
    const rst = new ReplSetTest({name: collName, nodes: 2});
    const config = rst.getReplSetConfig();
    config.members[1].priority = 0;
    rst.startSet();
    rst.initiate(config);
    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    assert.eq(primary, rst.nodes[0]);
    const testDB = primary.getDB(dbName);

    const sessionOptions = {causalConsistency: false};
    const writeConcernMajority = {w: "majority", wtimeout: 500};

    assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));

    jsTest.log("Write concern errors should not have error labels");
    // Start a new session on the primary.
    let session = primary.startSession(sessionOptions);
    let sessionDb = session.getDatabase(dbName);
    let sessionColl = sessionDb.getCollection(collName);
    stopServerReplication(rst.getSecondaries());
    session.startTransaction({writeConcern: writeConcernMajority});
    assert.commandWorked(sessionColl.insert({_id: "write-with-write-concern"}));
    let res = session.commitTransaction_forTesting();
    checkWriteConcernTimedOut(res);
    assert(!res.hasOwnProperty("code"));
    assert(!res.hasOwnProperty("errorLabels"));
    restartServerReplication(rst.getSecondaries());

    jsTest.log(
        "commitTransaction should wait for write concern even if it returns NoSuchTransaction");
    rst.awaitReplication();
    stopServerReplication(rst.getSecondaries());
    // Use a txnNumber that is one higher than the server has tracked.
    res = sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(session.getTxnNumber_forTesting() + 1),
        autocommit: false,
        writeConcern: writeConcernMajority
    });
    checkWriteConcernTimedOut(res);
    assert.commandFailedWithCode(res, ErrorCodes.NoSuchTransaction);

    jsTest.log("NoSuchTransaction with write concern error is not transient");
    assert(!res.hasOwnProperty("errorLabels"));

    jsTest.log("NoSuchTransaction without write concern error is transient");
    restartServerReplication(rst.getSecondaries());
    // Use a txnNumber that is one higher than the server has tracked.
    res = sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(session.getTxnNumber_forTesting() + 1),
        autocommit: false,
        writeConcern: {w: "majority"}  // Wait with a long timeout.
    });
    assert.commandFailedWithCode(res, ErrorCodes.NoSuchTransaction);
    assert(!res.hasOwnProperty("writeConcernError"), res);
    assert.eq(res["errorLabels"], ["TransientTransactionError"], res);

    jsTest.log("If the noop write for NoSuchTransaction cannot occur, the error is not transient");
    assert.commandWorked(testDB.getSiblingDB("local").createCollection("todrop"));
    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: "hangDuringDropCollection", mode: "alwaysOn"}));
    // Create a pending drop on a collection in the local database. This will hold an X lock on
    // the local database.
    let awaitDrop =
        startParallelShell(() => assert(db.getSiblingDB("local")["todrop"].drop()), rst.ports[0]);
    checkLog.contains(testDB.getMongo(), "hangDuringDropCollection fail point enabled");
    // The server will attempt to perform a noop write, since the command returns
    // NoSuchTransaction. The noop write will time out acquiring a lock on the local database.
    // This should not be a TransientTransactionError, since the server has not successfully
    // replicated a write to confirm that it is primary.
    // Use a txnNumber that is one higher than the server has tracked.
    res = sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(session.getTxnNumber_forTesting() + 1),
        autocommit: false,
        writeConcern: writeConcernMajority,
        maxTimeMS: 1000
    });
    assert.commandFailedWithCode(res, ErrorCodes.MaxTimeMSExpired);
    assert(!res.hasOwnProperty("errorLabels"));
    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: "hangDuringDropCollection", mode: "off"}));
    awaitDrop();

    session.endSession();

    rst.stopSet();
}());
