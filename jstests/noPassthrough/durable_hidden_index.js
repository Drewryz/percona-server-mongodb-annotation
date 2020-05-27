/**
 * Test that hidden index status can be replicated by secondary nodes and will be persisted
 * into the index catalog, that is hidden index remains hidden after restart.
 *
 * @tags: [requires_journaling, requires_replication, requires_fcv_44]
 */

(function() {
"use strict";

load("jstests/libs/get_index_helpers.js");  // For GetIndexHelpers.findByName.

const dbName = "test";

function isIndexHidden(indexes, indexName) {
    const idx = GetIndexHelpers.findByName(indexes, indexName);
    return idx && idx.hidden;
}

//
// Test that hidden index status can be replicated by secondary nodes and will be persisted into the
// index catalog in a replica set.
//
const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
const primaryDB = rst.getPrimary().getDB(dbName);
primaryDB.coll.drop();

// Create a hidden index.
primaryDB.coll.createIndex({a: 1}, {hidden: true});
assert(isIndexHidden(primaryDB.coll.getIndexes(), "a_1"));

// Wait for the replication finishes before stopping the replica set.
rst.awaitReplication();

// Restart the replica set.
rst.stopSet(/* signal */ undefined, /* forRestart */ true);
rst.startSet(/* signal */ undefined, /* forRestart */ true);
const secondaryDB = rst.getSecondary().getDB(dbName);

assert(isIndexHidden(secondaryDB.coll.getIndexes(), "a_1"));

rst.stopSet();

//
// Test that hidden index status will be persisted into the index catalog in a standalone mongod.
//
// Start a mongod.
let conn = MongoRunner.runMongod();
assert.neq(null, conn, 'mongod was unable to start up');
let db = conn.getDB(dbName);
db.coll.drop();

// Create a hidden index.
db.coll.createIndex({a: 1}, {hidden: true});
assert(isIndexHidden(db.coll.getIndexes(), "a_1"));

// Restart the mongod.
MongoRunner.stopMongod(conn);
conn = MongoRunner.runMongod({restart: true, cleanData: false, dbpath: conn.dbpath});
db = conn.getDB(dbName);

// Test that after restart the index is still hidden.
assert(isIndexHidden(db.coll.getIndexes(), "a_1"));

MongoRunner.stopMongod(conn);
})();
