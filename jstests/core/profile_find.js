// @tags: [does_not_support_stepdowns, requires_profiling]

// Confirms that profiled find execution contains all expected metrics with proper values.

(function() {
    "use strict";

    // For getLatestProfilerEntry and getProfilerProtocolStringForCommand
    load("jstests/libs/profiler.js");

    var testDB = db.getSiblingDB("profile_find");
    assert.commandWorked(testDB.dropDatabase());
    var coll = testDB.getCollection("test");
    var isLegacyReadMode = (testDB.getMongo().readMode() === "legacy");

    testDB.setProfilingLevel(2);
    const profileEntryFilter = {op: "query"};

    //
    // Confirm most metrics on single document read.
    //
    var i;
    for (i = 0; i < 3; ++i) {
        assert.writeOK(coll.insert({a: i, b: i}));
    }
    assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: "fr"}}));

    if (!isLegacyReadMode) {
        assert.eq(coll.find({a: 1}).collation({locale: "fr"}).limit(1).itcount(), 1);
    } else {
        assert.neq(coll.findOne({a: 1}), null);
    }

    var profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);

    assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
    assert.eq(profileObj.keysExamined, 1, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 1, tojson(profileObj));
    assert.eq(profileObj.nreturned, 1, tojson(profileObj));
    assert.eq(profileObj.planSummary, "IXSCAN { a: 1 }", tojson(profileObj));
    assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
    assert.eq(profileObj.command.filter, {a: 1}, tojson(profileObj));
    if (isLegacyReadMode) {
        assert.eq(profileObj.command.ntoreturn, -1, tojson(profileObj));
    } else {
        assert.eq(profileObj.command.limit, 1, tojson(profileObj));
        assert.eq(profileObj.protocol,
                  getProfilerProtocolStringForCommand(testDB.getMongo()),
                  tojson(profileObj));
    }

    if (!isLegacyReadMode) {
        assert.eq(profileObj.command.collation, {locale: "fr"});
    }
    assert.eq(profileObj.cursorExhausted, true, tojson(profileObj));
    assert(!profileObj.hasOwnProperty("cursorid"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("responseLength"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("millis"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("locks"), tojson(profileObj));
    assert(profileObj.locks.hasOwnProperty("Global"), tojson(profileObj));
    assert(profileObj.locks.hasOwnProperty("Database"), tojson(profileObj));
    assert(profileObj.locks.hasOwnProperty("Collection"), tojson(profileObj));
    assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

    //
    // Confirm "cursorId" and "hasSortStage" metrics.
    //
    coll.drop();
    for (i = 0; i < 3; ++i) {
        assert.writeOK(coll.insert({a: i, b: i}));
    }
    assert.commandWorked(coll.createIndex({a: 1}));

    assert.neq(coll.findOne({a: 1}), null);

    assert.neq(coll.find({a: {$gte: 0}}).sort({b: 1}).batchSize(1).next(), null);
    profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);

    assert.eq(profileObj.hasSortStage, true, tojson(profileObj));
    assert(profileObj.hasOwnProperty("cursorid"), tojson(profileObj));
    assert(!profileObj.hasOwnProperty("cursorExhausted"), tojson(profileObj));
    assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

    //
    // Confirm "fromMultiPlanner" metric.
    //
    coll.drop();
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    for (i = 0; i < 5; ++i) {
        assert.writeOK(coll.insert({a: i, b: i}));
    }

    assert.neq(coll.findOne({a: 3, b: 3}), null);
    profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);

    assert.eq(profileObj.fromMultiPlanner, true, tojson(profileObj));
    assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

    //
    // Confirm "replanned" metric.
    // We should ideally be using a fail-point to trigger "replanned" rather than relying on
    // current query planner behavior knowledge to setup a scenario. SERVER-23620 has been entered
    // to add this fail-point and to update appropriate tests.
    //
    coll.drop();
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    for (i = 0; i < 20; ++i) {
        assert.writeOK(coll.insert({a: 5, b: i}));
        assert.writeOK(coll.insert({a: i, b: 10}));
    }
    assert.neq(coll.findOne({a: 5, b: 15}), null);
    assert.neq(coll.findOne({a: 15, b: 10}), null);
    profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);

    assert.eq(profileObj.replanned, true, profileObj);
    assert(profileObj.hasOwnProperty('replanReason'), profileObj);
    assert(
        profileObj.replanReason.match(
            /cached plan was less efficient than expected: expected trial execution to take [0-9]+ works but it took at least [0-9]+ works/),
        profileObj);
    assert.eq(profileObj.appName, "MongoDB Shell", profileObj);

    //
    // Confirm that query modifiers such as "hint" are in the profiler document.
    //
    coll.drop();
    assert.writeOK(coll.insert({_id: 2}));

    assert.eq(coll.find().hint({_id: 1}).itcount(), 1);
    profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
    assert.eq(profileObj.command.hint, {_id: 1}, tojson(profileObj));

    assert.eq(coll.find().comment("a comment").itcount(), 1);
    profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
    assert.eq(profileObj.command.comment, "a comment", tojson(profileObj));

    assert.eq(coll.find().maxScan(3000).itcount(), 1);
    profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
    assert.eq(profileObj.command.maxScan, 3000, tojson(profileObj));

    var maxTimeMS = 100000;
    assert.eq(coll.find().maxTimeMS(maxTimeMS).itcount(), 1);
    profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
    assert.eq(profileObj.command.maxTimeMS, maxTimeMS, tojson(profileObj));

    assert.eq(coll.find().max({_id: 3}).itcount(), 1);
    profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
    assert.eq(profileObj.command.max, {_id: 3}, tojson(profileObj));

    assert.eq(coll.find().min({_id: 0}).itcount(), 1);
    profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
    assert.eq(profileObj.command.min, {_id: 0}, tojson(profileObj));

    assert.eq(coll.find().returnKey().itcount(), 1);
    profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
    assert.eq(profileObj.command.returnKey, true, tojson(profileObj));

    //
    // Confirm that queries are truncated in the profiler as { $truncated: <string>, comment:
    // <string> }
    //
    let queryPredicate = {};

    for (let i = 0; i < 501; i++) {
        queryPredicate[i] = "a".repeat(150);
    }

    assert.eq(coll.find(queryPredicate).comment("profile_find").itcount(), 0);
    profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
    assert.eq((typeof profileObj.command.$truncated), "string", tojson(profileObj));
    assert.eq(profileObj.command.comment, "profile_find", tojson(profileObj));

    //
    // Confirm that a query whose filter contains a field named 'query' appears as expected in the
    // profiler. This test ensures that upconverting a legacy query correctly identifies this as a
    // user field rather than a wrapped filter spec.
    //
    coll.find({query: "foo"}).itcount();
    profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
    assert.eq(profileObj.command.filter, {query: "foo"}, tojson(profileObj));
})();
