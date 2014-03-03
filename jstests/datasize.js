// test end-to-end data allocation without powerOf2Sizes enabled
f = db.jstests_datasize;
f.drop();

// this test requires usePowerOf2Sizes to be off
db.createCollection( f.getName(), { usePowerOf2Sizes: false } );
assert.eq(0, f.stats().userFlags);

assert.eq( 0, db.runCommand( {datasize:"test.jstests_datasize"} ).size );
f.save( {qq:'c'} );
assert.eq( 32, db.runCommand( {datasize:"test.jstests_datasize"} ).size );
f.save( {qq:'fg'} );
assert.eq( 68, db.runCommand( {datasize:"test.jstests_datasize"} ).size );

f.drop();
db.createCollection( f.getName(), { usePowerOf2Sizes: false} );

f.ensureIndex( {qq:1} );
assert.eq( 0, db.runCommand( {datasize:"test.jstests_datasize"} ).size );
f.save( {qq:'c'} );
assert.eq( 32, db.runCommand( {datasize:"test.jstests_datasize"} ).size );
f.save( {qq:'fg'} );
assert.eq( 68, db.runCommand( {datasize:"test.jstests_datasize"} ).size );

assert.eq( 0, db.runCommand( {datasize:"test.jstests_datasize", min:{qq:'a'}} ).ok );

assert.eq( 68, db.runCommand( {datasize:"test.jstests_datasize", min:{qq:'a'}, max:{qq:'z' }} ).size );
assert.eq( 32, db.runCommand( {datasize:"test.jstests_datasize", min:{qq:'a'}, max:{qq:'d' }} ).size );
assert.eq( 32, db.runCommand( {datasize:"test.jstests_datasize", min:{qq:'a'}, max:{qq:'d' }, keyPattern:{qq:1}} ).size );
assert.eq( 36, db.runCommand( {datasize:"test.jstests_datasize", min:{qq:'d'}, max:{qq:'z' }, keyPattern:{qq:1}} ).size );

assert.eq( 0, db.runCommand( {datasize:"test.jstests_datasize", min:{qq:'c'}, max:{qq:'c' }} ).size );
assert.eq( 32, db.runCommand( {datasize:"test.jstests_datasize", min:{qq:'c'}, max:{qq:'d' }} ).size );

assert.eq( 0, db.runCommand( {datasize:"test.jstests_datasize", min:{qq:'a'}, max:{qq:'d' }, keyPattern:{a:1}} ).ok );
