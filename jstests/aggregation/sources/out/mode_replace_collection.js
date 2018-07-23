// Tests the behavior of $out.
// @tags: [assumes_unsharded_collection]
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

    const coll = db.mode_replace_collection;
    coll.drop();

    const targetColl = db.mode_replace_collection_out;
    targetColl.drop();

    const pipeline = [{$out: {to: targetColl.getName(), mode: "replaceCollection"}}];

    //
    // Test $out with a non-existent output collection.
    //
    assert.commandWorked(coll.insert({_id: 0}));
    coll.aggregate(pipeline);
    assert.eq(1, targetColl.find().itcount());

    //
    // Test $out with an existing output collection.
    //
    coll.aggregate(pipeline);
    assert.eq(1, targetColl.find().itcount());

    //
    // Test that $out will preserve the indexes and options of the output collection.
    //
    targetColl.drop();
    assert.commandWorked(db.createCollection(targetColl.getName(), {validator: {a: {$gt: 0}}}));
    assert.commandWorked(targetColl.createIndex({a: 1}));

    coll.drop();
    assert.commandWorked(coll.insert({a: 1}));

    coll.aggregate(pipeline);
    assert.eq(1, targetColl.find().itcount());
    assert.eq(2, targetColl.getIndexes().length);

    const listColl = db.runCommand({listCollections: 1, filter: {name: targetColl.getName()}});
    assert.commandWorked(listColl);
    assert.eq({a: {$gt: 0}}, listColl.cursor.firstBatch[0].options["validator"]);

    //
    // Test that $out fails if it violates a unique index constraint.
    //
    coll.drop();
    assert.commandWorked(coll.insert([{_id: 0, a: 0}, {_id: 1, a: 0}]));
    targetColl.drop();
    assert.commandWorked(targetColl.createIndex({a: 1}, {unique: true}));

    assertErrorCode(coll, pipeline, 16996);

    // Rerun a similar test, except populate the target collection with a document that conflics
    // with one out of the pipeline. In this case, there is no unique key violation since the target
    // collection will be dropped before renaming the source collection.
    coll.drop();
    assert.commandWorked(coll.insert({_id: 0, a: 0}));
    targetColl.remove({});
    assert.commandWorked(targetColl.insert({_id: 1, a: 0}));

    coll.aggregate(pipeline);
    assert.eq(1, targetColl.find().itcount());
    assert.eq(2, targetColl.getIndexes().length);

}());
