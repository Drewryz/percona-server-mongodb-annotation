// Check $redact pipeline stage.
load('jstests/aggregation/extras/utils.js');

t = db.jstests_aggregation_redact;
t.drop();

// this document will always be present but its content will change
t.save({ _id: 1,
         level: 1,
         // b will present on level 3, 4, and 5
         b: { level: 3,
              c: 5, // always included when b is included
              // the contents of d test that if we cannot see a document then we cannot see its
              // array-nested subdocument even if we have permissions to see the subdocument.
              // it also tests arrays containing documents we cannot see
              d: [ {level: 1, e: 4},
                   {f: 6},
                   {level: 5, g: 9},
                   "NOT AN OBJECT!!11!" // always included when b is included
                 ]
            },
         // the contents of h test that in order to see a subdocument (j) we must be able to see all
         // parent documents (h and i) even if we have permissions to see the subdocument
         h: { level: 2,
              i: { level: 4,
                   j: { level: 1,
                        k: 8
                      }
                 }
            },
         // l checks that we get an empty document when we can see a document but none of its fields
         l: {
              m: { level: 3,
                   n: 12
                 }
            },
         // o checks that we get an empty array when we can see a array but none of its entries
         o: [{ level: 5,
               p: 19
            }],
         // q is a basic field check and should always be included
         q: 14
      });

// this document will sometimes be missing
t.save({ _id: 2,
         level: 4,
      });

a1 = t.aggregate({$redact:  {$cond: [{$lte: ['$level', 1]}, "$$CONTINUE", "$$PRUNE"]}});
a2 = t.aggregate({$redact:  {$cond: [{$lte: ['$level', 2]}, "$$CONTINUE", "$$PRUNE"]}});
a3 = t.aggregate({$redact:  {$cond: [{$lte: ['$level', 3]}, "$$CONTINUE", "$$PRUNE"]}});
a4 = t.aggregate({$redact:  {$cond: [{$lte: ['$level', 4]}, "$$CONTINUE", "$$PRUNE"]}});
a5 = t.aggregate({$redact:  {$cond: [{$lte: ['$level', 5]}, "$$CONTINUE", "$$PRUNE"]}});

a1result = [{ _id: 1,
              level: 1,
              l: {},
              o: [],
              q: 14
           }];

a2result = [{ _id: 1,
              level: 1,
              h: { level: 2,
                 },
              l: {},
              o: [],
              q: 14
           }];

a3result = [{ _id: 1,
              level: 1,
              b: { level: 3,
                   c: 5,
                   d: [ {level: 1, e: 4},
                        {f: 6} ]
                 },
              h: { level: 2,
                 },
              l: {
                   m: { level: 3,
                         n: 12
                      }
                 },
              o: [],
              q: 14
           }];

a4result = [{ _id: 1,
              level: 1,
              b: { level: 3,
                   c: 5,
                   d: [ {level: 1, e: 4},
                        {f: 6} ]
                 },
              h: { level: 2,
                   i: { level: 4,
                        j: { level: 1,
                             k: 8
                           }
                      }
                 },
              l: {
                   m: { level: 3,
                         n: 12
                      }
                 },
              o: [],
              q: 14
           },
           { _id: 2,
             level: 4,
           }];

a5result = [{ _id: 1,
              level: 1,
              b: { level: 3,
                   c: 5,
                   d: [ {level: 1, e: 4},
                        {f: 6},
                        {level: 5, g: 9}
                      ]
                 },
              h: { level: 2,
                   i: { level: 4,
                        j: { level: 1,
                             k: 8
                           }
                      }
                 },
              l: {
                   m: { level: 3,
                         n: 12
                      }
                 },
              o: [{ level: 5,
                    p: 19
                 }],
              q: 14
           },
           { _id: 2,
             level: 4,
           }];

assert.eq(a1.result, a1result);
assert.eq(a2.result, a2result);
assert.eq(a3.result, a3result);
assert.eq(a4.result, a4result);
assert.eq(a5.result, a5result);
