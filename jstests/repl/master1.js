// Test handling of clock skew and optimes across mongod instances

var baseName = "jstests_repl_master1test";

oplog = function() {
    return m.getDB( "local" ).oplog.$main;
}

lastop = function() {
    return oplog().find().sort( {$natural:-1} ).next();
}

am = function() {
    return m.getDB( baseName ).a;
}

rt = new ReplTest( baseName );

m = rt.start( true );
m.forceWriteMode('legacy'); // for 2.6 only, see SERVER-13704

am().save( {} );
assert.eq( "i", lastop().op );

op = lastop();
printjson( op );
op.ts.t = op.ts.t + 600000 // 10 minutes
m.getDB( "local" ).runCommand( {godinsert:"oplog.$main", obj:op} );

rt.stop( true );
m = rt.start( true, null, true );

assert.eq( op.ts.t, lastop().ts.t );
am().save( {} );
assert.eq( op.ts.t, lastop().ts.t );
assert.eq( op.ts.i + 1, lastop().ts.i );

op = lastop();
printjson( op );
op.ts.i = Math.pow(2,31);
printjson( op );
m.getDB( "local" ).runCommand( {godinsert:"oplog.$main", obj:op} );

rt.stop( true );
m = rt.start( true, null, true );
assert.eq( op.ts.i, lastop().ts.i );
m.forceWriteMode('legacy'); // for 2.6 only, see SERVER-13704
am().save( {} );
sleep( 3000 ); // make sure dies on its own before stop() called

assert.neq( 0 /*EXIT_ABRUPT == 14 */, rt.stop( true ) );
