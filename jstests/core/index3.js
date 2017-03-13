// Cannot implicitly shard accessed collections because of extra shard key index in sharded
// collection.
// @tags: [assumes_no_implicit_index_creation]

t = db.index3;
t.drop();

assert(t.getIndexes().length == 0);

t.ensureIndex({name: 1});

t.save({name: "a"});

t.ensureIndex({name: 1});

assert(t.getIndexes().length == 2);

assert(t.validate().valid);
