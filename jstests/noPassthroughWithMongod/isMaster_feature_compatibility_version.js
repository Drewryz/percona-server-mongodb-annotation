// Tests that when the featureCompatibilityVersion is not equal to the downgrade version, running
// isMaster with internalClient returns a response with minWireVersion == maxWireVersion. This
// ensures that an older version mongod/mongos will fail to connect to the node when it is upgraded,
// upgrading, or downgrading.
//
// TODO: Test that fcv 3.8 isMaster uses the latest wire version. (SERVER-32412) The wire version
// has been bumped, but fcv 3.8 is yet to be added.
(function() {
    "use strict";

    const adminDB = db.getSiblingDB("admin");
    const isMasterCommand = {
        isMaster: 1,
        internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)}
    };
    const upgradeVersion = "3.6";
    const downgradeVersion = "3.4";

    // When the featureCompatibilityVersion is equal to the upgrade version, running isMaster with
    // internalClient returns minWireVersion == maxWireVersion.
    // TODO: update to use fcv 3.8 here, and remove the "+ 1" below.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: upgradeVersion}));
    let res = adminDB.runCommand(isMasterCommand);
    assert.commandWorked(res);
    assert.eq(res.minWireVersion + 1, res.maxWireVersion, tojson(res));

    // When the featureCompatibilityVersion is upgrading, running isMaster with internalClient
    // returns minWireVersion == maxWireVersion.
    // TODO: update to use fcv 3.6/3.8 here, and remove the "+ 1" below.
    assert.writeOK(adminDB.system.version.update(
        {_id: "featureCompatibilityVersion"},
        {$set: {version: downgradeVersion, targetVersion: upgradeVersion}}));
    res = adminDB.runCommand(isMasterCommand);
    assert.commandWorked(res);
    assert.eq(res.minWireVersion + 1, res.maxWireVersion, tojson(res));

    // When the featureCompatibilityVersion is downgrading, running isMaster with internalClient
    // returns minWireVersion == maxWireVersion.
    // TODO: update to use fcv 3.6/3.8 here, and remove the "+ 1" below.
    assert.writeOK(adminDB.system.version.update(
        {_id: "featureCompatibilityVersion"},
        {$set: {version: downgradeVersion, targetVersion: downgradeVersion}}));
    res = adminDB.runCommand(isMasterCommand);
    assert.commandWorked(res);
    assert.eq(res.minWireVersion + 1, res.maxWireVersion, tojson(res));

    // When the featureCompatibilityVersion is equal to the downgrade version, running isMaster with
    // internalClient returns minWireVersion + 1 == maxWireVersion.
    // TODO: update to use fcv 3.6 here, and put it back to "+ 1" below.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: downgradeVersion}));
    res = adminDB.runCommand(isMasterCommand);
    assert.commandWorked(res);
    assert.eq(res.minWireVersion + 2, res.maxWireVersion, tojson(res));

    // When the internalClient field is missing from the isMaster command, the response returns the
    // full wire version range from minWireVersion == 0 to maxWireVersion == latest version, even if
    // the featureCompatibilityVersion is equal to the upgrade version.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: upgradeVersion}));
    res = adminDB.runCommand({isMaster: 1});
    assert.commandWorked(res);
    assert.eq(res.minWireVersion, 0, tojson(res));
    assert.lt(res.minWireVersion, res.maxWireVersion, tojson(res));
})();
