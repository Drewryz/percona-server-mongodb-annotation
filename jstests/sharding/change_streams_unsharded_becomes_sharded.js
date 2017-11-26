// Tests the behavior of change streams on a collection that was initially unsharded but then
// becomes sharded. In particular, test that post-shardCollection inserts update their cached
// 'documentKey' to include the new shard key.
(function() {
    "use strict";

    load('jstests/libs/change_stream_util.js');  // For ChangeStreamTest.

    // For supportsMajorityReadConcern().
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    const testName = "change_streams_unsharded_becomes_sharded";
    const st = new ShardingTest({
        shards: 2,
        mongos: 1,
        rs: {
            nodes: 1,
            enableMajorityReadConcern: '',
            // Use a higher frequency for periodic noops to speed up the test.
            setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}
        }
    });

    const mongosDB = st.s0.getDB(testName);
    const mongosColl = mongosDB[testName];

    mongosDB.createCollection(testName);
    mongosColl.createIndex({x: 1});

    st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

    // Establish a change stream cursor on the unsharded collection.
    let cst = new ChangeStreamTest(mongosDB);
    let cursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: mongosColl});
    assert.eq(0, cursor.firstBatch.length, "Cursor had changes: " + tojson(cursor));

    // Verify that the cursor picks up documents inserted while the collection is unsharded. The
    // 'documentKey' at this point is simply the _id field.
    assert.writeOK(mongosColl.insert({_id: 0, x: 0}));
    cst.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: [{
            documentKey: {_id: 0},
            fullDocument: {_id: 0, x: 0},
            ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
            operationType: "insert",
        }]
    });

    // Enable sharding on the previously unsharded collection.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));

    // Shard the collection on x.
    assert.commandWorked(
        mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {x: 1}}));

    // Ensure that the primary shard has an up-to-date routing table.
    assert.commandWorked(st.rs0.getPrimary().getDB("admin").runCommand(
        {_flushRoutingTableCacheUpdates: mongosColl.getFullName()}));

    // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey).
    assert.commandWorked(mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {x: 0}}));

    // Verify that the cursor on the original shard is still valid and sees new inserted documents.
    // The 'documentKey' field should now include the shard key, even before a 'kNewShardDetected'
    // operation has been generated by the migration of a chunk to a new shard.
    assert.writeOK(mongosColl.insert({_id: 1, x: 1}));
    cst.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: [{
            documentKey: {x: 1, _id: 1},
            fullDocument: {_id: 1, x: 1},
            ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
            operationType: "insert",
        }]
    });

    // Move the [minKey, 0) chunk to shard1.
    assert.commandWorked(mongosDB.adminCommand({
        moveChunk: mongosColl.getFullName(),
        find: {x: -1},
        to: st.rs1.getURL(),
        _waitForDelete: true
    }));

    // Make sure the change stream cursor sees a document inserted on the recipient shard.
    assert.writeOK(mongosColl.insert({_id: -1, x: -1}));
    cst.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: [{
            documentKey: {x: -1, _id: -1},
            fullDocument: {_id: -1, x: -1},
            ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
            operationType: "insert",
        }]
    });

    st.stop();
})();
