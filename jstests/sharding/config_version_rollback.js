/**
 * Tests that if the config.version document on a config server is rolled back, that config server
 * will detect the new config.version document when it gets recreated.
 * @tags: [requires_persistence]
 */

(function() {
    "use strict";
    load("jstests/libs/check_log.js");

    // The config.version document is written on transition to primary. We need to ensure this
    // config.version document is rolled back for this test.
    //
    // This means we have to guarantee the config.version document is not replicated by a secondary
    // during any of 1) initial sync, 2) steady state replication, or 3) catchup after election.
    //
    // 1) initial sync
    // We need non-primaries to finish initial sync so that they are electable, but without
    // replicating the config.version document. Since we can't control when the config.version
    // document is written (it's an internal write, not a client write), we turn on a failpoint
    // that stalls the write of the config.version document until we have ascertained that the
    // secondaries have finished initial sync.
    //
    // 2) steady state replication
    // Once the non-primaries have transitioned to secondary, we stop the secondaries from
    // replicating anything further by turning on a failpoint that stops the OplogFetcher. We then
    // allow the primary to write the config.verison document.
    //
    // 3) catchup after election
    // When the primary is stepped down and one of the secondaries is elected, the new primary will
    // notice that it is behind the original primary and try to catchup for a short period. So, we
    // also ensure that this short period is 0 by setting catchupTimeoutMillis to 0 earlier in the
    // ReplSetConfig passed to initiate().
    //
    // Thus, we guarantee the new primary will not have replicated the config.version document in
    // initial sync, steady state replication, or catchup, so the document will be rolled back.

    jsTest.log("Starting the replica set and waiting for secondaries to finish initial sync");
    var configRS = new ReplSetTest({nodes: 3});
    var nodes = configRS.startSet({
        configsvr: '',
        storageEngine: 'wiredTiger',
        setParameter: {
            "failpoint.transitionToPrimaryHangBeforeInitializingConfigDatabase":
                "{'mode':'alwaysOn'}"
        }
    });
    var conf = configRS.getReplSetConfig();
    conf.settings = {catchUpTimeoutMillis: 0};
    configRS.initiate(conf);

    var secondaries = configRS.getSecondaries();
    var origPriConn = configRS.getPrimary();

    // Ensure the primary is waiting to write the config.version document before stopping the oplog
    // fetcher on the secondaries.
    checkLog.contains(
        origPriConn,
        'transition to primary - transitionToPrimaryHangBeforeInitializingConfigDatabase fail point enabled.');

    jsTest.log("Stopping the OplogFetcher on the secondaries");
    secondaries.forEach(function(node) {
        assert.commandWorked(node.getDB('admin').runCommand(
            {configureFailPoint: 'stopOplogFetcher', mode: 'alwaysOn'}));
    });

    jsTest.log("Allowing the primary to write the config.version doc");
    // Note: since we didn't know which node would be elected to be the first primary, we had to
    // turn this failpoint on for all nodes earlier. Since we do want the all future primaries to
    // write the config.version doc immediately, we turn the failpoint off for all nodes now.
    nodes.forEach(function(node) {
        assert.commandWorked(node.adminCommand({
            configureFailPoint: "transitionToPrimaryHangBeforeInitializingConfigDatabase",
            mode: "off"
        }));
    });

    jsTest.log("Confirming that the primary has the config.version doc but the secondaries do not");
    var origConfigVersionDoc;
    assert.soon(function() {
        origConfigVersionDoc = origPriConn.getCollection('config.version').findOne();
        return null !== origConfigVersionDoc;
    });
    secondaries.forEach(function(secondary) {
        secondary.setSlaveOk();
        assert.eq(null, secondary.getCollection('config.version').findOne());
    });

    // Ensure manually deleting the config.version document is not allowed.
    assert.writeErrorWithCode(origPriConn.getCollection('config.version').remove({}), 40302);
    assert.commandFailedWithCode(origPriConn.getDB('config').runCommand({drop: 'version'}), 40303);

    jsTest.log("Stepping down original primary");
    try {
        origPriConn.adminCommand({replSetStepDown: 60, force: true});
    } catch (x) {
        // replSetStepDown closes all connections, thus a network exception is expected here.
    }

    jsTest.log("Waiting for new primary to be elected and write a new config.version document");
    var newPriConn = configRS.getPrimary();
    assert.neq(newPriConn, origPriConn);

    var newConfigVersionDoc = newPriConn.getCollection('config.version').findOne();
    assert.neq(null, newConfigVersionDoc);
    assert.neq(origConfigVersionDoc.clusterId, newConfigVersionDoc.clusterId);

    jsTest.log("Re-enabling replication on all nodes");
    secondaries.forEach(function(node) {
        assert.commandWorked(
            node.getDB('admin').runCommand({configureFailPoint: 'stopOplogFetcher', mode: 'off'}));
    });

    jsTest.log(
        "Waiting for original primary to rollback and replicate new config.version document");
    configRS.waitForState(origPriConn, ReplSetTest.State.SECONDARY);
    origPriConn.setSlaveOk();
    assert.soonNoExcept(function() {
        var foundClusterId = origPriConn.getCollection('config.version').findOne().clusterId;
        return friendlyEqual(newConfigVersionDoc.clusterId, foundClusterId);
    });

    jsTest.log("Forcing original primary to step back up and become primary again.");

    // Do prep work to make original primary transtion to primary again smoother by
    // waiting for all nodes to catch up to make them eligible to become primary and
    // step down the current primary to make it stop generating new oplog entries.
    configRS.awaitReplication();

    try {
        newPriConn.adminCommand({replSetStepDown: 60, force: true});
    } catch (x) {
        // replSetStepDown closes all connections, thus a network exception is expected here.
    }

    // Ensure former primary is eligible to become primary once more.
    assert.commandWorked(origPriConn.adminCommand({replSetFreeze: 0}));

    // Keep on trying until this node becomes the primary. One reason it can fail is when the other
    // nodes have newer oplog entries and will thus refuse to vote for this node.
    assert.soon(function() {
        return (origPriConn.adminCommand({replSetStepUp: 1})).ok;
    });

    assert.soon(function() {
        return origPriConn == configRS.getPrimary();
    });

    // Now we just need to start up a mongos and add a shard to confirm that the shard gets added
    // with the proper clusterId value.
    jsTest.log("Starting mongos");
    var mongos = MongoRunner.runMongos({configdb: configRS.getURL()});

    jsTest.log("Starting shard mongod");
    var shard = MongoRunner.runMongod({shardsvr: ""});

    jsTest.log("Adding shard to cluster");
    assert.commandWorked(mongos.adminCommand({addShard: shard.host}));

    jsTest.log("Verifying that shard was provided the proper clusterId");
    var shardIdentityDoc = shard.getDB('admin').system.version.findOne({_id: 'shardIdentity'});
    printjson(shardIdentityDoc);
    assert.eq(newConfigVersionDoc.clusterId,
              shardIdentityDoc.clusterId,
              "oldPriClusterId: " + origConfigVersionDoc.clusterId);
    configRS.stopSet();

})();
