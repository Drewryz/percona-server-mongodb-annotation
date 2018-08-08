'use strict';

/**
 * multi_statement_transaction_atomicity_isolation.js
 *
 * Inserts a handful of documents into a collection. Each thread then updates multiple documents
 * inside of a multi-statement transaction. The resulting documents are of the form
 *
 *   {_id: 0, order: [{tid: 0, iteration: 0, numUpdated: 3},
 *                    {tid: 1, iteration: 0, numUpdated: 2},
 *                    ...]}
 *
 *   {_id: 1, order: [{tid: 0, iteration: 0, numUpdated: 3},
 *                    {tid: 1, iteration: 0, numUpdated: 2},
 *                    ...]}
 *
 *   {_id: 2, order: [{tid: 0, iteration: 0, numUpdated: 3}, ...]}
 *
 * where the {tid: 0, iteration: 0, numUpdated: 3} element should occur 3 times and should always
 * come before the {tid: 1, iteration: 0, numUpdated: 2} element's 2 occurrences. In other words, we
 * track
 *
 *   (1) the relative order in which each of the transactions commit based on their position within
 *       the array, and
 *
 *   (2) the expected number of occurrences for each element in the array.
 *
 * An anomaly is detected if either
 *
 *   (a) transaction A's (tid, txnNumber, numToUpdate) element precedes transaction B's
 *       (tid, txnNumber, numToUpdate) element in one document and follows it in another. This would
 *       suggest that the database failed to detect a write-write conflict despite both transactions
 *       modifying the same document and is therefore not providing snapshot isolation.
 *
 *   (b) transaction C's (tid, txnNumber, numToUpdate) element doesn't appear numToUpdate times
 *       across a consistent snapshot of all of the documents. This would suggest that the database
 *       failed to atomically update all documents modified in a concurrent transaction.
 *
 * @tags: [uses_transactions]
 */

load('jstests/libs/cycle_detection.js');  // for Graph

// For withTxnAndAutoRetryOnWriteConflict.
load('jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js');

var $config = (function() {

    function checkTransactionCommitOrder(documents) {
        const graph = new Graph();

        for (let doc of documents) {
            const commitOrder = doc.order;
            for (let i = 1; i < commitOrder.length; ++i) {
                // We add an edge from commitOrder[i - 1] to commitOrder[i] because it being earlier
                // in the array that was $push'd to indicates that the commitOrder[i - 1]
                // transaction happened before the commit[i] transaction order.
                graph.addEdge(commitOrder[i - 1], commitOrder[i]);
            }
        }

        const result = graph.findCycle();

        if (result.length > 0) {
            const isOpPartOfCycle = (op) =>
                result.some(cyclicOp => bsonBinaryEqual({_: op}, {_: cyclicOp}));

            const filteredDocuments = documents.map(doc => {
                const filteredCommitOrder = doc.order.filter(isOpPartOfCycle);
                return Object.assign({}, doc, {order: filteredCommitOrder});
            });

            assertWhenOwnColl.eq([], result, tojson(filteredDocuments));
        }
    }

    function checkNumUpdatedByEachTransaction(documents) {
        const updateCounts = new Map();

        for (let doc of documents) {
            for (let op of doc.order) {
                // We store 'op' both as the key and as part of the value because the mongo shell's
                // Map type mangles the key and doesn't provide an API to retrieve the original
                // key-value pairs.
                const value = updateCounts.get(op) || {op, actual: 0};
                updateCounts.put(op, {op, actual: value.actual + 1});
            }
        }

        for (let {
                 op, actual
             } of updateCounts.values()) {
            assert.eq(op.numUpdated, actual, () => {
                return 'transaction ' + tojson(op) + ' should have updated ' + op.numUpdated +
                    ' documents, but ' + actual + ' were updated: ' + tojson(updateCounts.values());
            });
        }
    }

    const states = (function() {

        function getAllDocuments(collection, numDocs) {
            // We intentionally use a smaller batch size when fetching all of the documents in the
            // collection in order to stress the behavior of reading from the same snapshot over the
            // course of multiple network roundtrips.
            const batchSize = Math.max(2, Math.floor(numDocs / 5));

            collection.getDB().getSession().startTransaction();
            const documents =
                collection.find().batchSize(batchSize).readConcern('snapshot').toArray();
            collection.getDB().getSession().commitTransaction();

            assertWhenOwnColl.eq(numDocs, documents.length, () => tojson(documents));
            return documents;
        }

        function getDocIdsToUpdate(numDocs) {
            // Generate between [2, numDocs / 2] operations.
            const numOps = 2 + Random.randInt(Math.ceil(numDocs / 2) - 1);

            // Select 'numOps' document (without replacement) to update.
            let docIds = Array.from({length: numDocs}, (value, index) => index);
            return Array.shuffle(docIds).slice(0, numOps);
        }

        return {
            init: function init(db, collName) {
                this.iteration = 0;
                this.session = db.getMongo().startSession({causalConsistency: false});
            },

            update: function update(db, collName) {
                const collection = this.session.getDatabase(db.getName()).getCollection(collName);
                const docIds = getDocIdsToUpdate(this.numDocs);

                // We apply the following update to each of the 'docIds' documents to record the
                // number of times we expect to see the transaction being run in this execution of
                // the update() state function by this worker thread present across all documents.
                // Using the $push operator causes a transaction which commits after another
                // transaction to appear later in the array.
                const updateMods = {
                    $push: {
                        order: {
                            tid: this.tid,
                            iteration: this.iteration,
                            numUpdated: docIds.length,
                        }
                    }
                };

                withTxnAndAutoRetryOnWriteConflict(this.session, () => {
                    for (let [i, docId] of docIds.entries()) {
                        const res = collection.runCommand('update', {
                            updates: [{q: {_id: docId}, u: updateMods}],
                        });
                        assertAlways.commandWorked(res);
                        assertWhenOwnColl.eq(res.n, 1, () => tojson(res));
                        assertWhenOwnColl.eq(res.nModified, 1, () => tojson(res));
                    }
                });

                ++this.iteration;
            },

            checkConsistency: function checkConsistency(db, collName) {
                const collection = this.session.getDatabase(db.getName()).getCollection(collName);
                const documents = getAllDocuments(collection, this.numDocs);
                checkTransactionCommitOrder(documents);
                checkNumUpdatedByEachTransaction(documents);
            }
        };
    })();

    const transitions = {
        init: {update: 0.9, checkConsistency: 0.1},
        update: {update: 0.9, checkConsistency: 0.1},
        checkConsistency: {update: 1}
    };

    function setup(db, collName, cluster) {
        const bulk = db[collName].initializeUnorderedBulkOp();

        for (let i = 0; i < this.numDocs; ++i) {
            bulk.insert({_id: i, order: []});
        }

        const res = bulk.execute({w: 'majority'});
        assertWhenOwnColl.commandWorked(res);
        assertWhenOwnColl.eq(this.numDocs, res.nInserted);
    }

    function teardown(db, collName, cluster) {
        const documents = db[collName].find().toArray();
        checkTransactionCommitOrder(documents);
        checkNumUpdatedByEachTransaction(documents);
    }

    function skip(cluster) {
        if (cluster.isSharded() || cluster.isStandalone()) {
            return {skip: true, msg: 'only runs in a replica set.'};
        }
        return {skip: false};
    }

    return {
        threadCount: 10,
        iterations: 50,
        states: states,
        transitions: transitions,
        data: {numDocs: 10},
        setup: setup,
        teardown: teardown,
        skip: skip
    };

})();
