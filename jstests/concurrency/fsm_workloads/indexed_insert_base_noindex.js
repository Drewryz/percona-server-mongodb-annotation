/**
 * indexed_insert_base_noindex.js
 *
 * Executes the indexed_insert_base.js workload after dropping its index.
 */
load('jstests/concurrency/fsm_libs/runner.js'); // for extendWorkload
load('jstests/concurrency/fsm_workloads/indexed_insert_base.js'); // for $config
load('jstests/concurrency/fsm_workload_helpers/indexed_noindex.js'); // for indexedNoindex

var $config = extendWorkload($config, indexedNoindex);
