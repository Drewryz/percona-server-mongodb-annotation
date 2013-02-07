// ensure $concat asserts on string

load('jstests/aggregation/extras/utils.js')

c = db.s6570;
c.drop();
c.save({x:"foo", y:"bar"});

assert.eq(c.aggregate({$project:{str:{$concat:["X", "$x", "Y", "$y"]}}}).result[0].str, "XfooYbar");

// Nullish (both with and without other strings)
assert.isnull(c.aggregate({$project:{str:{$concat: ["$missing"] }}}).result[0].str);
assert.isnull(c.aggregate({$project:{str:{$concat: [null] }}}).result[0].str);
assert.isnull(c.aggregate({$project:{str:{$concat: [undefined] }}}).result[0].str);
assert.isnull(c.aggregate({$project:{str:{$concat: ["$x", "$missing", "$y"] }}}).result[0].str);
assert.isnull(c.aggregate({$project:{str:{$concat: ["$x", null, "$y"] }}}).result[0].str);
assert.isnull(c.aggregate({$project:{str:{$concat: ["$x", undefined, "$y"] }}}).result[0].str);

// assert fail for all other types
assertErrorCode(c, {$project:{str:{$concat: [MinKey]}}}, 16702);
assertErrorCode(c, {$project:{str:{$concat: [1]}}}, 16702);
assertErrorCode(c, {$project:{str:{$concat: [NumberInt(1)]}}}, 16702);
assertErrorCode(c, {$project:{str:{$concat: [NumberLong(1)]}}}, 16702);
assertErrorCode(c, {$project:{str:{$concat: [true]}}}, 16702);
assertErrorCode(c, {$project:{str:{$concat: [function(){}]}}}, 16702);
assertErrorCode(c, {$project:{str:{$concat: [{}]}}}, 16702);
assertErrorCode(c, {$project:{str:{$concat: [[]]}}}, 16702);
assertErrorCode(c, {$project:{str:{$concat: [new Timestamp(0,0)]}}}, 16702);
assertErrorCode(c, {$project:{str:{$concat: [new Date(0)]}}}, 16702);
assertErrorCode(c, {$project:{str:{$concat: [new BinData(0,"")]}}}, 16702);
assertErrorCode(c, {$project:{str:{$concat: [/asdf/]}}}, 16702);
assertErrorCode(c, {$project:{str:{$concat: [MaxKey]}}}, 16702);
