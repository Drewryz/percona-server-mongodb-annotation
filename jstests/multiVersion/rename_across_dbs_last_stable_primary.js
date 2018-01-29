(function() {
    'use strict';

    load("jstests/libs/feature_compatibility_version.js");
    load("jstests/replsets/libs/rename_across_dbs.js");

    const nodes = [{binVersion: 'last-stable'}, {binVersion: 'latest'}, {}];
    const options = {
        nodes: nodes,
        setFeatureCompatibilityVersion: lastStableFCV,
    };

    new RenameAcrossDatabasesTest(options).run();
}());
