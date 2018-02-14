// Tests that a client may discover a user's supported SASL mechanisms via isMaster.
(function() {
    "use strict";

    function runTest(conn) {
        var db = conn.getDB("admin");
        var externalDB = conn.getDB("$external");

        // Enable SCRAM-SHA-256.
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "4.0"}));

        function checkMechs(userid, mechs) {
            const res =
                assert.commandWorked(db.runCommand({isMaster: 1, saslSupportedMechs: userid}));
            assert.eq(mechs, res.saslSupportedMechs, tojson(res));
        }

        // Make users.
        assert.commandWorked(db.runCommand({createUser: "user", pwd: "pwd", roles: []}));
        assert.commandWorked(externalDB.runCommand({createUser: "user", roles: []}));
        assert.commandWorked(db.runCommand(
            {createUser: "IX", pwd: "pwd", roles: [], mechanisms: ["SCRAM-SHA-256"]}));

        // Internal users should support scram methods.
        checkMechs("admin.user", ["SCRAM-SHA-1", "SCRAM-SHA-256"]);

        // External users should support PLAIN, but not scram methods.
        checkMechs("$external.user", ["PLAIN"]);

        // Check non-normalized name finds normalized user.
        const IXchar = "\u2168";
        const IXuserid = "admin." + IXchar;
        checkMechs(IXuserid, ["SCRAM-SHA-256"]);

        // Check that names with compatibility equivalence collide.
        assert.commandWorked(db.runCommand(
            {createUser: IXchar, pwd: "pwd", roles: [], mechanisms: ["SCRAM-SHA-1"]}));
        assert.commandFailed(db.runCommand({isMaster: 1, saslSupportedMechs: IXuserid}),
                             ErrorCodes.BadValue,
                             "Two users exist with names exhibiting compatibility equivalence");
    }

    // Test standalone.
    var m = MongoRunner.runMongod(
        {setParameter: "authenticationMechanisms=SCRAM-SHA-1,SCRAM-SHA-256,PLAIN"});
    runTest(m);
    MongoRunner.stopMongod(m);

    // Test mongos.
    var st = new ShardingTest({
        shards: 0,
        other: {
            mongosOptions:
                {setParameter: "authenticationMechanisms=PLAIN,SCRAM-SHA-256,SCRAM-SHA-1"}
        }
    });
    runTest(st.s0);
    st.stop();
})();
