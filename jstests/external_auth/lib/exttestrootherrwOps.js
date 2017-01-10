// note: run setup.js first with mongod --auth disabled
// then run this with mongod --auth enabled

// name: External user with read (only) on 'test' and readWrite on 'other'
// mode: auth

function exttestrootherrwOpsRun(){
  'use strict'
  var testUser='exttestrootherrw'

  var db = conn.getDB( '$external' )

  assert(
    db.auth({
      user: testUser,
      pwd: testUser+'9a5S',
      mechanism: 'PLAIN',
      digestPassword: false
    })
  )

  // check who we are authenticated as

  var res = db.runCommand({connectionStatus : 1})

  assert ( res.authInfo.authenticatedUsers[0].user == testUser)

  // test access

  load( 'jstests/external_auth/lib/_functions.js' )

  authuser_assertro( db.getSiblingDB('test') )
  authuser_assertrw( db.getSiblingDB('other') )
  authuser_assertnone( db.getSiblingDB('yetanother') )
}

exttestrootherrwOpsRun()
