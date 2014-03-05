// test the newCollectionsUsePowerOf2Sizes param
function test(defaultMode) {
    // default should be server default
    db.a.drop();
    db.createCollection('a');
    assert.eq(db.a.stats().userFlags & 1, defaultMode);

    // explicitly turned off should be 0
    db.b.drop();
    db.createCollection('b', {usePowerOf2Sizes: false});
    assert.eq(db.b.stats().userFlags & 1, 0);

    // capped should be 0
    db.c.drop();
    db.createCollection('c', {capped:true, size: 10});
    assert.eq(db.c.stats().userFlags & 1, 0);

    // capped should be 0
    db.d.drop();
    db.createCollection('d', {capped:true, size: 10, usePowerOf2Sizes: false});
    assert.eq(db.d.stats().userFlags & 1, 0);

    // capped and ask explicitly for powerOf2 should be 0
    db.e.drop();
    db.createCollection('e', {capped:true, size: 10, usePowerOf2Sizes: true});
    assert.eq(db.e.stats().userFlags & 1, 0);
}

assert.eq(db.adminCommand({getParameter:1,
          newCollectionsUsePowerOf2Sizes: true}).newCollectionsUsePowerOf2Sizes, true);

test(1);
assert.commandWorked(db.adminCommand({setParameter:1, newCollectionsUsePowerOf2Sizes: false}));
test(0);

// reset the server to default value
assert.commandWorked(db.adminCommand({setParameter:1, newCollectionsUsePowerOf2Sizes: true}));
