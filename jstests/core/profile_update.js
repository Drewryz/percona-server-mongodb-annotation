// Confirms that profiled update execution contains all expected metrics with proper values.
// TODO SERVER-23259: Add planSummary.
// TODO SERVER-23264: Add execStats.

(function() {
    "use strict";

    load("jstests/libs/profiler.js");  // For getLatestProfilerEntry.

    // Setup test db and collection.
    var testDB = db.getSiblingDB("profile_update");
    assert.commandWorked(testDB.dropDatabase());
    var coll = testDB.getCollection("test");

    testDB.setProfilingLevel(2);

    //
    // Confirm metrics for single document update.
    //
    var i;
    for (i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i}));
    }
    assert.commandWorked(coll.createIndex({a: 1}));

    assert.writeOK(coll.update({a: {$gte: 2}}, {$set: {c: 1}}));

    var profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "update", tojson(profileObj));
    assert.eq(profileObj.keyUpdates, 0, tojson(profileObj));
    assert.eq(profileObj.keysExamined, 1, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 1, tojson(profileObj));
    assert.eq(profileObj.nMatched, 1, tojson(profileObj));
    assert.eq(profileObj.nModified, 1, tojson(profileObj));
    assert(profileObj.hasOwnProperty("millis"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("locks"), tojson(profileObj));

    //
    // Confirm metrics for multiple indexed document update.
    //
    coll.drop();
    for (i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i}));
    }
    assert.commandWorked(coll.createIndex({a: 1}));

    assert.writeOK(coll.update({a: {$gte: 5}}, {$set: {c: 1}}, {multi: true}));
    profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.keyUpdates, 0, tojson(profileObj));
    assert.eq(profileObj.keysExamined, 5, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 5, tojson(profileObj));
    assert.eq(profileObj.nMatched, 5, tojson(profileObj));
    assert.eq(profileObj.nModified, 5, tojson(profileObj));

    //
    // Confirm metrics for insert on update with "upsert: true".
    //
    coll.update({_id: "new value", a: 4}, {$inc: {b: 1}}, {upsert: true});
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.query, {_id: "new value", a: 4}, tojson(profileObj));
    assert.eq(profileObj.updateobj, {$inc: {b: 1}}, tojson(profileObj));
    assert.eq(profileObj.keysExamined, 0, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 0, tojson(profileObj));
    assert.eq(profileObj.nMatched, 0, tojson(profileObj));
    assert.eq(profileObj.nModified, 0, tojson(profileObj));
    assert.eq(profileObj.upsert, true, tojson(profileObj));

    //
    // Confirm "fromMultiPlanner" metric.
    //
    coll.drop();
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    for (i = 0; i < 5; ++i) {
        assert.writeOK(coll.insert({a: i, b: i}));
    }

    assert.writeOK(coll.update({a: 3, b: 3}, {$set: {c: 1}}));
    profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.fromMultiPlanner, true, tojson(profileObj));
})();